#define NPY_NO_DEPRECATED_API NPY_API_VERSION

#include <stdbool.h>
#include <Python.h>
#include <structmember.h>
#include <numpy/arrayobject.h>
#include <signal.h>
#include <dogleg.h>

#include <suitesparse/cholmod.h>

#include "mrcal.h"


#if PY_MAJOR_VERSION == 3
#define PyString_FromString PyUnicode_FromString
#define PyString_FromFormat PyUnicode_FromFormat
#define PyInt_FromLong      PyLong_FromLong
#define PyString_AsString   PyUnicode_AsUTF8
#define PyInt_Check         PyLong_Check
#define PyInt_AsLong        PyLong_AsLong
#define STRING_OBJECT       "U"
#else
#define STRING_OBJECT       "S"
#endif

#define IS_NULL(x) ((x) == NULL || (PyObject*)(x) == Py_None)
#define IS_TRUE(x) ((x) != NULL && PyObject_IsTrue(x))

// Python is silly. There's some nuance about signal handling where it sets a
// SIGINT (ctrl-c) handler to just set a flag, and the python layer then reads
// this flag and does the thing. Here I'm running C code, so SIGINT would set a
// flag, but not quit, so I can't interrupt the solver. Thus I reset the SIGINT
// handler to the default, and put it back to the python-specific version when
// I'm done
#define SET_SIGINT() struct sigaction sigaction_old;                    \
do {                                                                    \
    if( 0 != sigaction(SIGINT,                                          \
                       &(struct sigaction){ .sa_handler = SIG_DFL },    \
                       &sigaction_old) )                                \
    {                                                                   \
        PyErr_SetString(PyExc_RuntimeError, "sigaction() failed");      \
        goto done;                                                      \
    }                                                                   \
} while(0)
#define RESET_SIGINT() do {                                             \
    if( 0 != sigaction(SIGINT,                                          \
                       &sigaction_old, NULL ))                          \
        PyErr_SetString(PyExc_RuntimeError, "sigaction-restore failed"); \
} while(0)

#define PERCENT_S_COMMA(s,n) "'%s',"
#define COMMA_LENSMODEL_NAME(s,n) , mrcal_lensmodel_name( (lensmodel_t){.type = s} )
#define VALID_LENSMODELS_FORMAT  "(" LENSMODEL_LIST(PERCENT_S_COMMA) ")"
#define VALID_LENSMODELS_ARGLIST LENSMODEL_LIST(COMMA_LENSMODEL_NAME)

#define CHECK_CONTIGUOUS(x) do {                                        \
    if( !PyArray_IS_C_CONTIGUOUS(x) )                                   \
    {                                                                   \
        PyErr_SetString(PyExc_RuntimeError, "All inputs must be c-style contiguous arrays (" #x ")"); \
        return false;                                                   \
    } } while(0)


#define COMMA ,
#define ARG_DEFINE(     name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) pytype name = initialvalue;
#define ARG_LIST_DEFINE(name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) pytype name,
#define ARG_LIST_CALL(  name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) name,
#define NAMELIST(       name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) #name ,
#define PARSECODE(      name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) parsecode
#define PARSEARG(       name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) parseprearg &name,
#define FREE_PYARRAY(   name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) Py_XDECREF(name_pyarrayobj);
#define CHECK_LAYOUT(   name, pytype, initialvalue, parsecode, parseprearg, name_pyarrayobj, npy_type, dims_ref) \
    if(!IS_NULL(name_pyarrayobj)) {                                     \
        int dims[] = dims_ref;                                          \
        int ndims = (int)sizeof(dims)/(int)sizeof(dims[0]);             \
                                                                        \
        if( ndims > 0 )                                                 \
        {                                                               \
            if( PyArray_NDIM((PyArrayObject*)name_pyarrayobj) != ndims )          \
            {                                                           \
                PyErr_Format(PyExc_RuntimeError, "'" #name "' must have exactly %d dims; got %d", ndims, PyArray_NDIM((PyArrayObject*)name_pyarrayobj)); \
                return false;                                           \
            }                                                           \
            for(int i=0; i<ndims; i++)                                  \
                if(dims[i] >= 0 && dims[i] != PyArray_DIMS((PyArrayObject*)name_pyarrayobj)[i]) \
                {                                                       \
                    PyErr_Format(PyExc_RuntimeError, "'" #name "' must have dimensions '" #dims_ref "' where <0 means 'any'. Dims %d got %ld instead", i, PyArray_DIMS((PyArrayObject*)name_pyarrayobj)[i]); \
                    return false;                                       \
                }                                                       \
        }                                                               \
        if( (int)npy_type >= 0 )                                        \
        {                                                               \
            if( PyArray_TYPE((PyArrayObject*)name_pyarrayobj) != npy_type )       \
            {                                                           \
                PyErr_SetString(PyExc_RuntimeError, "'" #name "' must have type: " #npy_type); \
                return false;                                           \
            }                                                           \
            if( !PyArray_IS_C_CONTIGUOUS((PyArrayObject*)name_pyarrayobj) )       \
            {                                                           \
                PyErr_SetString(PyExc_RuntimeError, "'" #name "' must be c-style contiguous"); \
                return false;                                           \
            }                                                           \
        }                                                               \
    }
#define PYMETHODDEF_ENTRY(function_prefix, name, args) {#name,          \
                                                        (PyCFunction)function_prefix ## name, \
                                                        args,           \
                                                        function_prefix ## name ## _docstring}


// A wrapper around a solver context and various solver metadata. I need the
// optimization to be able to keep this, and I need Python to free it as
// necessary when the refcount drops to 0
typedef struct {
    PyObject_HEAD
    dogleg_solverContext_t* ctx;

    lensmodel_t lensmodel;
    mrcal_problem_details_t problem_details;

    int Ncameras_intrinsics, Ncameras_extrinsics;
    int Nframes, Npoints;
    int NobservationsBoard;
    int calibration_object_width_n;

} SolverContext;
static void SolverContext_dealloc(SolverContext* self)
{
    mrcal_free_context((void**)&self->ctx);
    Py_TYPE(self)->tp_free((PyObject*)self);
}
static PyObject* SolverContext_str(SolverContext* self)
{
    if(self->ctx == NULL)
        return PyString_FromString("Empty context");
    char lensmodel_name[1024];
    const char* p_lensmodel_name = lensmodel_name;
    if(!mrcal_lensmodel_name_full( lensmodel_name, sizeof(lensmodel_name), self->lensmodel ))
    {
        // Couldn't get the model string for some reason. This really should
        // never happen. But instead of barfing I can return a static string
        // that will be right much of the time (configuration-free models) and
        // good-enough most of the time
        p_lensmodel_name = mrcal_lensmodel_name( self->lensmodel );
    }

    return PyString_FromFormat("Non-empty context made with        %s\n"
                               "Ncameras_intrinsics:               %d\n"
                               "Ncameras_extrinsics:               %d\n"
                               "Nframes:                           %d\n"
                               "Npoints:                           %d\n"
                               "NobservationsBoard:                %d\n"
                               "calibration_object_width_n:        %d\n"
                               "do_optimize_intrinsic_core:        %d\n"
                               "do_optimize_intrinsic_distortions: %d\n",
                               p_lensmodel_name,
                               self->Ncameras_intrinsics,
                               self->Ncameras_extrinsics,
                               self->Nframes, self->Npoints,
                               self->NobservationsBoard,
                               self->calibration_object_width_n,
                               self->problem_details.do_optimize_intrinsic_core,
                               self->problem_details.do_optimize_intrinsic_distortions);
}

static PyObject* csr_from_cholmod_sparse( cholmod_sparse* Jt,

                                          // These are allowed to be NULL; If
                                          // so, I'll use the data in Jt. THE Jt
                                          // STILL OWNS THE DATA
                                          PyObject* P,
                                          PyObject* I,
                                          PyObject* X)
{
    // I do the Python equivalent of this;
    // scipy.sparse.csr_matrix((data, indices, indptr))


    PyObject* result = NULL;

    PyObject* module = NULL;
    PyObject* method = NULL;
    PyObject* args   = NULL;
    if(NULL == (module = PyImport_ImportModule("scipy.sparse")))
    {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't import scipy.sparse. I need that to represent J");
        goto done;
    }
    if(NULL == (method = PyObject_GetAttrString(module, "csr_matrix")))
    {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't find 'csr_matrix' in scipy.sparse");
        goto done;
    }

    // Here I'm assuming specific types in my cholmod arrays. I tried to
    // static_assert it, but internally cholmod uses void*, so I can't do that
    if(P == NULL) P = PyArray_SimpleNewFromData(1, ((npy_intp[]){Jt->ncol + 1}), NPY_INT32,  Jt->p);
    else Py_INCREF(P);

    if(I == NULL) I = PyArray_SimpleNewFromData(1, ((npy_intp[]){Jt->nzmax   }), NPY_INT32,  Jt->i);
    else Py_INCREF(I);

    if(X == NULL) X = PyArray_SimpleNewFromData(1, ((npy_intp[]){Jt->nzmax   }), NPY_DOUBLE, Jt->x);
    else Py_INCREF(X);

    PyObject* MatrixDef = PyTuple_Pack(3, X, I, P);
    args                = PyTuple_Pack(1, MatrixDef);
    Py_DECREF(P);
    Py_DECREF(I);
    Py_DECREF(X);
    Py_DECREF(MatrixDef);

    if(NULL == (result = PyObject_CallObject(method, args)))
        goto done; // reuse already-set error

    // Testing code to dump out a dense representation of this matrix to a file.
    // Can compare that file to what this function returns like this:
    //   Jf = np.fromfile("/tmp/J_17014_444.dat").reshape(17014,444)
    //   np.linalg.norm( Jf - J.toarray() )
    // {
    // #define P(A, index) ((unsigned int*)((A)->p))[index]
    // #define I(A, index) ((unsigned int*)((A)->i))[index]
    // #define X(A, index) ((double*      )((A)->x))[index]
    //         char logfilename[128];
    //         sprintf(logfilename, "/tmp/J_%d_%d.dat",(int)Jt->ncol,(int)Jt->nrow);
    //         FILE* fp = fopen(logfilename, "w");
    //         double* Jrow;
    //         Jrow = malloc(Jt->nrow*sizeof(double));
    //         for(unsigned int icol=0; icol<Jt->ncol; icol++)
    //         {
    //             memset(Jrow, 0, Jt->nrow*sizeof(double));
    //             for(unsigned int i=P(Jt, icol); i<P(Jt, icol+1); i++)
    //             {
    //                 int irow = I(Jt,i);
    //                 double x = X(Jt,i);
    //                 Jrow[irow] = x;
    //             }
    //             fwrite(Jrow,sizeof(double),Jt->nrow,fp);
    //         }
    //         fclose(fp);
    //         free(Jrow);
    // #undef P
    // #undef I
    // #undef X
    // }

 done:
    Py_XDECREF(module);
    Py_XDECREF(method);
    Py_XDECREF(args);

    return result;
}

static PyObject* SolverContext_J(SolverContext* self)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }

    return csr_from_cholmod_sparse(self->ctx->beforeStep->Jt, NULL,NULL,NULL);
}

static PyObject* SolverContext_p(SolverContext* self)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }

    cholmod_sparse* Jt = self->ctx->beforeStep->Jt;
    return PyArray_SimpleNewFromData(1, ((npy_intp[]){Jt->nrow}), NPY_DOUBLE, self->ctx->beforeStep->p);
}

static PyObject* SolverContext_x(SolverContext* self)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }

    cholmod_sparse* Jt = self->ctx->beforeStep->Jt;
    return PyArray_SimpleNewFromData(1, ((npy_intp[]){Jt->ncol}), NPY_DOUBLE, self->ctx->beforeStep->x);
}

static PyObject* SolverContext_state_index_intrinsics(SolverContext* self,
                                                      PyObject* args)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }
    PyObject* result = NULL;
    int i_cam_intrinsics = -1;
    if(!PyArg_ParseTuple( args, "i", &i_cam_intrinsics )) goto done;
    if( i_cam_intrinsics < 0 || i_cam_intrinsics >= self->Ncameras_intrinsics )
    {
        PyErr_Format(PyExc_RuntimeError,
                     "i_cam_intrinsics must refer to a valid camera, i.e. be in the range [0,%d] inclusive. Instead I got %d",
                     self->Ncameras_intrinsics-1,i_cam_intrinsics);
        goto done;
    }
    result = Py_BuildValue("i",
                           mrcal_state_index_intrinsics(i_cam_intrinsics,
                                                        self->problem_details,
                                                        self->lensmodel));
 done:
    return result;
}

static PyObject* SolverContext_state_index_camera_rt(SolverContext* self,
                                                     PyObject* args)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }
    PyObject* result = NULL;
    int i_cam_extrinsics = -1;
    if(!PyArg_ParseTuple( args, "i", &i_cam_extrinsics )) goto done;
    if( i_cam_extrinsics < 0 || i_cam_extrinsics >= self->Ncameras_extrinsics )
    {
        PyErr_Format(PyExc_RuntimeError,
                     "i_cam_extrinsics must refer to a valid camera, i.e. be in the range [0,%d] inclusive. Instead I got %d",
                     self->Ncameras_extrinsics-1,i_cam_extrinsics);
        goto done;
    }
    result = Py_BuildValue("i",
                           mrcal_state_index_camera_rt(i_cam_extrinsics,
                                                       self->Ncameras_intrinsics,
                                                       self->problem_details,
                                                       self->lensmodel));
 done:
    return result;
}
static PyObject* SolverContext_state_index_frame_rt(SolverContext* self,
                                                    PyObject* args)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }
    PyObject* result = NULL;
    int i_frame = -1;
    if(!PyArg_ParseTuple( args, "i", &i_frame )) goto done;
    if( i_frame < 0 || i_frame >= self->Nframes )
    {
        PyErr_Format(PyExc_RuntimeError,
                     "i_frame must refer to a valid frame i.e. be in the range [0,%d] inclusive. Instead I got %d",
                     self->Nframes-1,i_frame);
        goto done;
    }
    result = Py_BuildValue("i",
                           mrcal_state_index_frame_rt(i_frame,
                                                      self->Ncameras_intrinsics,
                                                      self->Ncameras_extrinsics,
                                                      self->problem_details,
                                                      self->lensmodel));
 done:
    return result;
}
static PyObject* SolverContext_state_index_point(SolverContext* self,
                                                 PyObject* args)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }
    PyObject* result = NULL;
    int i_point = -1;
    if(!PyArg_ParseTuple( args, "i", &i_point )) goto done;
    if( i_point < 0 || i_point >= self->Nframes )
    {
        PyErr_Format(PyExc_RuntimeError,
                     "i_point must refer to a valid point i.e. be in the range [0,%d] inclusive. Instead I got %d",
                     self->Npoints-1,i_point);
        goto done;
    }
    result = Py_BuildValue("i",
                           mrcal_state_index_point(i_point,
                                                   self->Nframes,
                                                   self->Ncameras_intrinsics,
                                                   self->Ncameras_extrinsics,
                                                   self->problem_details,
                                                   self->lensmodel));
 done:
    return result;
}
static PyObject* SolverContext_state_index_calobject_warp(SolverContext* self,
                                                          PyObject* args)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }

    return Py_BuildValue("i",
                         mrcal_state_index_calobject_warp(self->Npoints,
                                                          self->Nframes,
                                                          self->Ncameras_intrinsics,
                                                          self->Ncameras_extrinsics,
                                                          self->problem_details,
                                                          self->lensmodel));
}

static PyObject* SolverContext_num_measurements_dict(SolverContext* self)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }

    int Nmeasurements_all = self->ctx->beforeStep->Jt->ncol;
    int Nmeasurements_regularization =
        mrcal_getNmeasurements_regularization(self->Ncameras_intrinsics,
                                              self->problem_details,
                                              self->lensmodel);
    int Nmeasurements_boards =
        mrcal_getNmeasurements_boards( self->NobservationsBoard,
                                       self->calibration_object_width_n);
    int Nmeasurements_points =
        Nmeasurements_all - Nmeasurements_regularization - Nmeasurements_boards;

    PyObject* result = PyDict_New();
    PyObject* x;

    x = PyInt_FromLong(Nmeasurements_regularization);
    PyDict_SetItemString(result, "regularization", x);
    Py_DECREF(x);

    x = PyInt_FromLong(Nmeasurements_boards);
    PyDict_SetItemString(result, "boards", x);
    Py_DECREF(x);

    x = PyInt_FromLong(Nmeasurements_points);
    PyDict_SetItemString(result, "points", x);
    Py_DECREF(x);

    x = PyInt_FromLong(Nmeasurements_all);
    PyDict_SetItemString(result, "all", x);
    Py_DECREF(x);

    return result;
}

static PyObject* SolverContext_pack_unpack(SolverContext* self,
                                           PyObject* args,
                                           bool pack)
{
    if( self->ctx == NULL )
    {
        PyErr_SetString(PyExc_RuntimeError, "I need a non-empty context");
        return NULL;
    }

    PyObject*      result = NULL;
    PyArrayObject* p      = NULL;
    if(!PyArg_ParseTuple( args, "O&", PyArray_Converter, &p )) goto done;

    if( PyArray_TYPE(p) != NPY_DOUBLE )
    {
        PyErr_SetString(PyExc_RuntimeError, "The input array MUST have values of type 'float'");
        goto done;
    }

    if( !PyArray_IS_C_CONTIGUOUS(p) )
    {
        PyErr_SetString(PyExc_RuntimeError, "The input array MUST be a C-style contiguous array");
        goto done;
    }

    int       ndim       = PyArray_NDIM(p);
    npy_intp* dims       = PyArray_DIMS(p);
    if( ndim <= 0 || dims[ndim-1] <= 0 )
    {
        PyErr_SetString(PyExc_RuntimeError, "The input array MUST have non-degenerate data in it");
        goto done;
    }

    int Nstate = self->ctx->beforeStep->Jt->nrow;
    if( dims[ndim-1] != Nstate )
    {
        PyErr_Format(PyExc_RuntimeError, "The input array MUST have last dimension of size Nstate=%d; instead got %ld",
                     Nstate, dims[ndim-1]);
        goto done;
    }

    double* x = (double*)PyArray_DATA(p);
    if(pack)
        for(int i=0; i<PyArray_SIZE(p)/Nstate; i++)
        {
            mrcal_pack_solver_state_vector( x,
                                            self->lensmodel, self->problem_details,
                                            self->Ncameras_intrinsics, self->Ncameras_extrinsics,
                                            self->Nframes, self->Npoints );
            x = &x[Nstate];
        }
    else
        for(int i=0; i<PyArray_SIZE(p)/Nstate; i++)
        {
            mrcal_unpack_solver_state_vector( x,
                                              self->lensmodel, self->problem_details,
                                              self->Ncameras_intrinsics, self->Ncameras_extrinsics,
                                              self->Nframes, self->Npoints );
            x = &x[Nstate];
        }


    Py_INCREF(Py_None);
    result = Py_None;

 done:
    Py_XDECREF(p);
    return result;
}
static PyObject* SolverContext_pack(SolverContext* self, PyObject* args)
{
    return SolverContext_pack_unpack(self, args, true);
}
static PyObject* SolverContext_unpack(SolverContext* self, PyObject* args)
{
    return SolverContext_pack_unpack(self, args, false);
}




static const char SolverContext_J_docstring[] =
#include "SolverContext_J.docstring.h"
    ;
static const char SolverContext_p_docstring[] =
#include "SolverContext_p.docstring.h"
    ;
static const char SolverContext_x_docstring[] =
#include "SolverContext_x.docstring.h"
    ;
static const char SolverContext_state_index_intrinsics_docstring[] =
#include "SolverContext_state_index_intrinsics.docstring.h"
    ;
static const char SolverContext_state_index_camera_rt_docstring[] =
#include "SolverContext_state_index_camera_rt.docstring.h"
    ;
static const char SolverContext_state_index_frame_rt_docstring[] =
#include "SolverContext_state_index_frame_rt.docstring.h"
    ;
static const char SolverContext_state_index_point_docstring[] =
#include "SolverContext_state_index_point.docstring.h"
    ;
static const char SolverContext_state_index_calobject_warp_docstring[] =
#include "SolverContext_state_index_calobject_warp.docstring.h"
    ;
static const char SolverContext_num_measurements_dict_docstring[] =
#include "SolverContext_num_measurements_dict.docstring.h"
    ;
static const char SolverContext_pack_docstring[] =
#include "SolverContext_pack.docstring.h"
    ;
static const char SolverContext_unpack_docstring[] =
#include "SolverContext_unpack.docstring.h"
    ;

static PyMethodDef SolverContext_methods[] =
    { PYMETHODDEF_ENTRY(SolverContext_, J,                          METH_NOARGS),
      PYMETHODDEF_ENTRY(SolverContext_, p,                          METH_NOARGS),
      PYMETHODDEF_ENTRY(SolverContext_, x,                          METH_NOARGS),
      PYMETHODDEF_ENTRY(SolverContext_, state_index_intrinsics,     METH_VARARGS),
      PYMETHODDEF_ENTRY(SolverContext_, state_index_camera_rt,      METH_VARARGS),
      PYMETHODDEF_ENTRY(SolverContext_, state_index_frame_rt,       METH_VARARGS),
      PYMETHODDEF_ENTRY(SolverContext_, state_index_point,          METH_VARARGS),
      PYMETHODDEF_ENTRY(SolverContext_, state_index_calobject_warp, METH_NOARGS),
      PYMETHODDEF_ENTRY(SolverContext_, num_measurements_dict,      METH_NOARGS),
      PYMETHODDEF_ENTRY(SolverContext_, pack,                       METH_VARARGS),
      PYMETHODDEF_ENTRY(SolverContext_, unpack,                     METH_VARARGS),
      {}
    };

static PyTypeObject SolverContextType =
{
     PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "mrcal.SolverContext",
    .tp_basicsize = sizeof(SolverContext),
    .tp_new       = PyType_GenericNew,
    .tp_dealloc   = (destructor)SolverContext_dealloc,
    .tp_methods   = SolverContext_methods,
    .tp_str       = (reprfunc)SolverContext_str,
    .tp_repr      = (reprfunc)SolverContext_str,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Opaque solver context used by mrcal",
};

static bool parse_lensmodel_from_arg(// output
                                     lensmodel_t* lensmodel,
                                     // input
                                     PyObject* lensmodel_string)
{
    const char* lensmodel_cstring = PyString_AsString(lensmodel_string);
    if( lensmodel_cstring == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "The lens model must be given as a string");
        return false;
    }

    *lensmodel = mrcal_lensmodel_from_name(lensmodel_cstring);
    if( !mrcal_lensmodel_type_is_valid(lensmodel->type) )
    {
        if(lensmodel->type == LENSMODEL_INVALID_BADCONFIG)
        {
            PyErr_Format(PyExc_RuntimeError, "Couldn't parse the configuration of the given lens model '%s'",
                         lensmodel_cstring);
            return false;
        }
        PyErr_Format(PyExc_RuntimeError, "Invalid lens model was passed in: '%s'. Must be one of " VALID_LENSMODELS_FORMAT,
                     lensmodel_cstring
                     VALID_LENSMODELS_ARGLIST);
        return false;
    }
    return true;
}

static PyObject* getLensModelMeta(PyObject* NPY_UNUSED(self),
                                  PyObject* args)
{
    PyObject* result = NULL;
    SET_SIGINT();

    PyObject* lensmodel_string = NULL;
    if(!PyArg_ParseTuple( args, STRING_OBJECT, &lensmodel_string ))
        goto done;
    lensmodel_t lensmodel;
    if(!parse_lensmodel_from_arg(&lensmodel, lensmodel_string))
        goto done;

    mrcal_lensmodel_meta_t meta = mrcal_lensmodel_meta(lensmodel);

#define MRCAL_ITEM_BUILDVALUE_DEF(  name, type, pybuildvaluecode, PRIcode,SCNcode, bitfield, cookie) " s "pybuildvaluecode
#define MRCAL_ITEM_BUILDVALUE_VALUE(name, type, pybuildvaluecode, PRIcode,SCNcode, bitfield, cookie) , #name, cookie name

    if(lensmodel.type == LENSMODEL_SPLINED_STEREOGRAPHIC )
        result = Py_BuildValue("{"
                               MRCAL_LENSMODEL_META_LIST(MRCAL_ITEM_BUILDVALUE_DEF, )
                               MRCAL_LENSMODEL_SPLINED_STEREOGRAPHIC_CONFIG_LIST(MRCAL_ITEM_BUILDVALUE_DEF, )
                               "}"
                               MRCAL_LENSMODEL_META_LIST(MRCAL_ITEM_BUILDVALUE_VALUE, meta.)
                               MRCAL_LENSMODEL_SPLINED_STEREOGRAPHIC_CONFIG_LIST(MRCAL_ITEM_BUILDVALUE_VALUE, lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.));
    else
        result = Py_BuildValue("{"
                               MRCAL_LENSMODEL_META_LIST(MRCAL_ITEM_BUILDVALUE_DEF, )
                               "}"
                               MRCAL_LENSMODEL_META_LIST(MRCAL_ITEM_BUILDVALUE_VALUE, meta.));

    Py_INCREF(result);

 done:
    RESET_SIGINT();
    return result;
}

static PyObject* getKnotsForSplinedModels(PyObject* NPY_UNUSED(self),
                                          PyObject* args)
{
    PyObject*      result = NULL;
    PyArrayObject* py_ux  = NULL;
    PyArrayObject* py_uy  = NULL;
    SET_SIGINT();

    PyObject* lensmodel_string = NULL;
    if(!PyArg_ParseTuple( args, STRING_OBJECT, &lensmodel_string ))
        goto done;
    lensmodel_t lensmodel;
    if(!parse_lensmodel_from_arg(&lensmodel, lensmodel_string))
        goto done;

    if(lensmodel.type != LENSMODEL_SPLINED_STEREOGRAPHIC)
    {
        PyErr_Format(PyExc_RuntimeError,
                     "This function works only with the LENSMODEL_SPLINED_STEREOGRAPHIC model. %S passed in",
                     lensmodel_string);
        goto done;
    }

    {
        double ux[lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.Nx];
        double uy[lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.Ny];
        if(!mrcal_get_knots_for_splined_models(ux,uy, lensmodel))
        {
            PyErr_SetString(PyExc_RuntimeError,
                            "mrcal_get_knots_for_splined_models() failed");
            goto done;
        }


        npy_intp dims[1];

        dims[0] = lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.Nx;
        py_ux = (PyArrayObject*)PyArray_SimpleNew(1, dims, NPY_DOUBLE);
        if(py_ux == NULL)
        {
            PyErr_SetString(PyExc_RuntimeError, "Couldn't allocate ux");
            goto done;
        }

        dims[0] = lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.Ny;
        py_uy = (PyArrayObject*)PyArray_SimpleNew(1, dims, NPY_DOUBLE);
        if(py_uy == NULL)
        {
            PyErr_SetString(PyExc_RuntimeError, "Couldn't allocate uy");
            goto done;
        }

        memcpy(PyArray_DATA(py_ux), ux, lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.Nx*sizeof(double));
        memcpy(PyArray_DATA(py_uy), uy, lensmodel.LENSMODEL_SPLINED_STEREOGRAPHIC__config.Ny*sizeof(double));
    }

    result = Py_BuildValue("OO", py_ux, py_uy);

 done:
    Py_XDECREF(py_ux);
    Py_XDECREF(py_uy);
    RESET_SIGINT();
    return result;
}

static PyObject* getNlensParams(PyObject* NPY_UNUSED(self),
                                PyObject* args)
{
    PyObject* result = NULL;
    SET_SIGINT();

    PyObject* lensmodel_string = NULL;
    if(!PyArg_ParseTuple( args, STRING_OBJECT, &lensmodel_string ))
        goto done;
    lensmodel_t lensmodel;
    if(!parse_lensmodel_from_arg(&lensmodel, lensmodel_string))
        goto done;

    int Nparams = mrcal_getNlensParams(lensmodel);

    result = Py_BuildValue("i", Nparams);

 done:
    RESET_SIGINT();
    return result;
}

static PyObject* getSupportedLensModels(PyObject* NPY_UNUSED(self),
                                        PyObject* NPY_UNUSED(args))
{
    PyObject* result = NULL;
    SET_SIGINT();
    const char* const* names = mrcal_getSupportedLensModels();

    // I now have a NULL-terminated list of NULL-terminated strings. Get N
    int N=0;
    while(names[N] != NULL)
        N++;

    result = PyTuple_New(N);
    if(result == NULL)
    {
        PyErr_Format(PyExc_RuntimeError, "Failed PyTuple_New(%d)", N);
        goto done;
    }

    for(int i=0; i<N; i++)
    {
        PyObject* name = Py_BuildValue("s", names[i]);
        if( name == NULL )
        {
            PyErr_Format(PyExc_RuntimeError, "Failed Py_BuildValue...");
            Py_DECREF(result);
            result = NULL;
            goto done;
        }
        PyTuple_SET_ITEM(result, i, name);
    }

 done:
    RESET_SIGINT();
    return result;
}

static PyObject* getNextLensModel(PyObject* NPY_UNUSED(self),
                                  PyObject* args)
{
    PyObject* result = NULL;
    SET_SIGINT();

    PyObject* lensmodel_now_string   = NULL;
    PyObject* lensmodel_final_string = NULL;
    if(!PyArg_ParseTuple( args, STRING_OBJECT STRING_OBJECT,
                          &lensmodel_now_string,
                          &lensmodel_final_string))
        goto done;

    lensmodel_t lensmodel_now, lensmodel_final;
    if(!parse_lensmodel_from_arg(&lensmodel_now,   lensmodel_now_string) ||
       !parse_lensmodel_from_arg(&lensmodel_final, lensmodel_final_string))
        goto done;

    lensmodel_t lensmodel = mrcal_getNextLensModel(lensmodel_now, lensmodel_final);
    if(!mrcal_lensmodel_type_is_valid(lensmodel.type))
    {
        PyErr_Format(PyExc_RuntimeError, "Couldn't figure out the 'next' lens model from '%S' to '%S'",
                     lensmodel_now_string, lensmodel_final_string);
        goto done;
    }

    result = Py_BuildValue("s", mrcal_lensmodel_name(lensmodel));

 done:
    RESET_SIGINT();
    return result;
}

// just like PyArray_Converter(), but leave None as None
static
int PyArray_Converter_leaveNone(PyObject* obj, PyObject** address)
{
    if(obj == Py_None)
    {
        *address = Py_None;
        Py_INCREF(Py_None);
        return 1;
    }
    return PyArray_Converter(obj,address);
}


// project(), and unproject() have very similar arguments and operation, so the
// logic is consolidated as much as possible in these functions. The first arg
// is called "points" in both cases, but is 2d in one case, and 3d in the other

#define PROJECT_ARGUMENTS_REQUIRED(_)                                   \
    _(points,     PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, points,     NPY_DOUBLE, {} ) \
    _(lensmodel,  PyObject*,      NULL,    STRING_OBJECT,                         , NULL,       -1,         {} ) \
    _(intrinsics, PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, intrinsics, NPY_DOUBLE, {} ) \

#define PROJECT_ARGUMENTS_OPTIONAL(_) \
    _(get_gradients,    PyObject*,  Py_False,    "O",                                   , NULL,      -1, {})

#define UNPROJECT_ARGUMENTS_REQUIRED(_) PROJECT_ARGUMENTS_REQUIRED(_)
#define UNPROJECT_ARGUMENTS_OPTIONAL(_)

static bool _un_project_validate_args( // out
                                      lensmodel_t* lensmodel_type,

                                      // in
                                      int dim_points_in, // 3 for project(), 2 for unproject()
                                      PROJECT_ARGUMENTS_REQUIRED(ARG_LIST_DEFINE)
                                      PROJECT_ARGUMENTS_OPTIONAL(ARG_LIST_DEFINE)
                                      void* dummy __attribute__((unused)))
{
    if( PyArray_NDIM(intrinsics) != 1 )
    {
        PyErr_SetString(PyExc_RuntimeError, "'intrinsics' must have exactly 1 dim");
        return false;
    }

    if( PyArray_NDIM(points) < 1 )
    {
        PyErr_SetString(PyExc_RuntimeError, "'points' must have ndims >= 1");
        return false;
    }
    if( dim_points_in != PyArray_DIMS(points)[ PyArray_NDIM(points)-1 ] )
    {
        PyErr_Format(PyExc_RuntimeError, "points.shape[-1] MUST be %d. Instead got %ld",
                     dim_points_in,
                     PyArray_DIMS(points)[PyArray_NDIM(points)-1] );
        return false;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    PROJECT_ARGUMENTS_REQUIRED(CHECK_LAYOUT);
    PROJECT_ARGUMENTS_OPTIONAL(CHECK_LAYOUT);
#pragma GCC diagnostic pop

    if(!parse_lensmodel_from_arg(lensmodel_type, lensmodel))
        return false;

    int NlensParams = mrcal_getNlensParams(*lensmodel_type);
    if( NlensParams != PyArray_DIMS(intrinsics)[0] )
    {
        PyErr_Format(PyExc_RuntimeError, "intrinsics.shape[0] MUST be %d. Instead got %ld",
                     NlensParams,
                     PyArray_DIMS(intrinsics)[0] );
        return false;
    }

    return true;
}

#define _UN_PROJECT_PREAMBLE(ARGUMENTS_REQUIRED,ARGUMENTS_OPTIONAL,ARGUMENTS_OPTIONAL_VALIDATE,dim_points_in,dim_points_out) \
    PyObject*      result          = NULL;                              \
                                                                        \
    SET_SIGINT();                                                       \
    PyArrayObject* out             = NULL;                              \
    __attribute__((unused)) PyArrayObject* dq_dintrinsics = NULL;       \
    __attribute__((unused)) PyArrayObject* dq_dp          = NULL;       \
                                                                        \
    ARGUMENTS_REQUIRED(ARG_DEFINE);                                     \
    ARGUMENTS_OPTIONAL(ARG_DEFINE);                                     \
                                                                        \
    char* keywords[] = { ARGUMENTS_REQUIRED(NAMELIST)                   \
                         ARGUMENTS_OPTIONAL(NAMELIST)                   \
                         NULL};                                         \
    if(!PyArg_ParseTupleAndKeywords( args, kwargs,                      \
                                     ARGUMENTS_REQUIRED(PARSECODE) "|"  \
                                     ARGUMENTS_OPTIONAL(PARSECODE),     \
                                                                        \
                                     keywords,                          \
                                                                        \
                                     ARGUMENTS_REQUIRED(PARSEARG)       \
                                     ARGUMENTS_OPTIONAL(PARSEARG) NULL)) \
        goto done;                                                      \
                                                                        \
    /* if the input points array is degenerate, return a degenerate thing */ \
    if( IS_NULL(points) )                                               \
    {                                                                   \
        result = Py_None;                                               \
        Py_INCREF(result);                                              \
        goto done;                                                      \
    }                                                                   \
                                                                        \
    lensmodel_t lensmodel_type;                                         \
    if(!_un_project_validate_args( &lensmodel_type,                     \
                                   dim_points_in,                       \
                                   ARGUMENTS_REQUIRED(ARG_LIST_CALL)    \
                                   ARGUMENTS_OPTIONAL_VALIDATE(ARG_LIST_CALL) \
                                   NULL))                               \
        goto done;                                                      \
                                                                        \
    int Nintrinsics = PyArray_DIMS(intrinsics)[0];                      \
                                                                        \
    /* poor man's broadcasting of the inputs. I compute the total number of */ \
    /* points by multiplying the extra broadcasted dimensions. And I set up the */ \
    /* outputs to have the appropriate broadcasted dimensions        */ \
    const npy_intp* leading_dims  = PyArray_DIMS(points);               \
    int             Nleading_dims = PyArray_NDIM(points)-1;             \
    int Npoints = PyArray_SIZE(points) / leading_dims[Nleading_dims];   \
    bool get_gradients_bool = IS_TRUE(get_gradients);                   \
                                                                        \
    {                                                                   \
        npy_intp dims[Nleading_dims+2]; /* one extra for the gradients */ \
        memcpy(dims, leading_dims, Nleading_dims*sizeof(dims[0]));      \
                                                                        \
        dims[Nleading_dims + 0] =  dim_points_out;                      \
        out = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+1,        \
                                                dims,                   \
                                                NPY_DOUBLE);            \
        if( get_gradients_bool )                                        \
        {                                                               \
            dims[Nleading_dims + 0] = 2;                                \
            dims[Nleading_dims + 1] = Nintrinsics;                      \
            dq_dintrinsics = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+2, \
                                                               dims,    \
                                                               NPY_DOUBLE); \
                                                                        \
            dims[Nleading_dims + 0] = 2;                                \
            dims[Nleading_dims + 1] = dim_points_in;                    \
            dq_dp          = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+2, \
                                                               dims,    \
                                                               NPY_DOUBLE); \
        }                                                               \
    }


static PyObject* project(PyObject* NPY_UNUSED(self),
                         PyObject* args,
                         PyObject* kwargs)
{
    _UN_PROJECT_PREAMBLE(PROJECT_ARGUMENTS_REQUIRED,
                         PROJECT_ARGUMENTS_OPTIONAL,
                         PROJECT_ARGUMENTS_OPTIONAL,
                         3, 2);

    bool mrcal_result =
        mrcal_project((point2_t*)PyArray_DATA(out),
                      get_gradients_bool ? (double*)PyArray_DATA(dq_dintrinsics) : NULL,
                      get_gradients_bool ? (point3_t*)PyArray_DATA(dq_dp)  : NULL,

                      (const point3_t*)PyArray_DATA(points),
                      Npoints,
                      lensmodel_type,
                      // core, distortions concatenated
                      (const double*)PyArray_DATA(intrinsics));


    if(!mrcal_result)
    {
        PyErr_SetString(PyExc_RuntimeError, "mrcal_project() failed!");
        Py_DECREF((PyObject*)out);
        goto done;
    }

    if( get_gradients_bool )
    {
        result = PyTuple_Pack(3, out, dq_dp, dq_dintrinsics);
        Py_DECREF(out);
        Py_DECREF(dq_dp);
        Py_DECREF(dq_dintrinsics);
    }
    else
        result = (PyObject*)out;

 done:
    PROJECT_ARGUMENTS_REQUIRED(FREE_PYARRAY);
    PROJECT_ARGUMENTS_OPTIONAL(FREE_PYARRAY);
    RESET_SIGINT();
    return result;
}

static PyObject* _unproject(PyObject* NPY_UNUSED(self),
                            PyObject* args,
                            PyObject* kwargs)
{
    // unproject() has the same arguments as project(), except no gradient
    // reporting. The first arg is called "points" in both cases, but is 2d in
    // one case, and 3d in the other
    PyObject* get_gradients = NULL;

    _UN_PROJECT_PREAMBLE(UNPROJECT_ARGUMENTS_REQUIRED,
                         UNPROJECT_ARGUMENTS_OPTIONAL,
                         PROJECT_ARGUMENTS_OPTIONAL,
                         2, 3);
    bool mrcal_result =
        mrcal_unproject((point3_t*)PyArray_DATA(out),

                        (const point2_t*)PyArray_DATA(points),
                        Npoints,
                        lensmodel_type,
                        /* core, distortions concatenated */
                        (const double*)PyArray_DATA(intrinsics));
    if(!mrcal_result)
    {
        PyErr_SetString(PyExc_RuntimeError, "mrcal_unproject() failed!");
        Py_DECREF((PyObject*)out);
        goto done;
    }

    result = (PyObject*)out;

 done:
    UNPROJECT_ARGUMENTS_REQUIRED(FREE_PYARRAY);
    UNPROJECT_ARGUMENTS_OPTIONAL(FREE_PYARRAY);
    RESET_SIGINT();
    return result;
}

// project_stereographic(), and unproject_stereographic() have very similar
// arguments and operation, so the logic is consolidated as much as possible in
// these functions. The first arg is called "points" in both cases, but is 2d in
// one case, and 3d in the other
#define UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(_)                                   \
    _(points,     PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, points,     NPY_DOUBLE, {} )
#define UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(_)                                   \
    _(fx,                  double,       1.0,    "d",  , NULL, -1, {} ) \
    _(fy,                  double,       1.0,    "d",  , NULL, -1, {} ) \
    _(cx,                  double,       0.0,    "d",  , NULL, -1, {} ) \
    _(cy,                  double,       0.0,    "d",  , NULL, -1, {} ) \
    _(get_gradients,    PyObject*,  Py_False,    "O",  , NULL, -1, {} )

static bool _un_project_stereographic_validate_args(// in
                                                    int dim_points_in, // 3 for project(), 2 for unproject()
                                                    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(ARG_LIST_DEFINE)
                                                    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(ARG_LIST_DEFINE)
                                                    void* dummy __attribute__((unused)))
{
    if( PyArray_NDIM(points) < 1 )
    {
        PyErr_SetString(PyExc_RuntimeError, "'points' must have ndims >= 1");
        return false;
    }
    if( dim_points_in != PyArray_DIMS(points)[ PyArray_NDIM(points)-1 ] )
    {
        PyErr_Format(PyExc_RuntimeError, "points.shape[-1] MUST be %d. Instead got %ld",
                     dim_points_in,
                     PyArray_DIMS(points)[PyArray_NDIM(points)-1] );
        return false;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(CHECK_LAYOUT);
    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(CHECK_LAYOUT);
#pragma GCC diagnostic pop

    return true;
}

static PyObject* _un_project_stereographic(PyObject* NPY_UNUSED(self),
                                           PyObject* args,
                                           PyObject* kwargs,
                                           bool projecting)
{
    int dim_points_in, dim_points_out;
    if(projecting)
    {
        dim_points_in  = 3;
        dim_points_out = 2;
    }
    else
    {
        dim_points_in  = 2;
        dim_points_out = 3;
    }

    PyObject* result = NULL;

    SET_SIGINT();
    PyArrayObject* out  = NULL;
    PyArrayObject* grad = NULL;

    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(ARG_DEFINE);
    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(ARG_DEFINE);

    char* keywords[] = { UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(NAMELIST)
                         UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(NAMELIST)
                         NULL};
    if(!PyArg_ParseTupleAndKeywords( args, kwargs,
                                     UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(PARSECODE) "|"
                                     UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(PARSECODE),

                                     keywords,

                                     UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(PARSEARG)
                                     UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(PARSEARG) NULL))
        goto done;

    /* if the input points array is degenerate, return a degenerate thing */
    if( IS_NULL(points) )
    {
        result = Py_None;
        Py_INCREF(result);
        goto done;
    }

    if(!_un_project_stereographic_validate_args( dim_points_in,
                                                 UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(ARG_LIST_CALL)
                                                 UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(ARG_LIST_CALL)
                                                 NULL))
        goto done;

    /* poor man's broadcasting of the inputs. I compute the total number of */
    /* points by multiplying the extra broadcasted dimensions. And I set up the */
    /* outputs to have the appropriate broadcasted dimensions        */
    const npy_intp* leading_dims  = PyArray_DIMS(points);
    int             Nleading_dims = PyArray_NDIM(points)-1;
    int Npoints = PyArray_SIZE(points) / leading_dims[Nleading_dims];
    bool get_gradients_bool = IS_TRUE(get_gradients);

    {
        npy_intp dims[Nleading_dims+2]; /* one extra for the gradients */
        memcpy(dims, leading_dims, Nleading_dims*sizeof(dims[0]));

        dims[Nleading_dims + 0] = dim_points_out;
        out = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+1,
                                                dims,
                                                NPY_DOUBLE);
        if( get_gradients_bool )
        {
            dims[Nleading_dims + 0] = dim_points_out;
            dims[Nleading_dims + 1] = dim_points_in;
            grad = (PyArrayObject*)PyArray_SimpleNew(Nleading_dims+2,
                                                     dims,
                                                     NPY_DOUBLE);
        }
    }

    if(projecting)
        mrcal_project_stereographic((point2_t*)PyArray_DATA(out),
                                    get_gradients_bool ? (point3_t*)PyArray_DATA(grad)  : NULL,
                                    (const point3_t*)PyArray_DATA(points),
                                    Npoints,
                                    fx,fy,cx,cy);
    else
        mrcal_unproject_stereographic((point3_t*)PyArray_DATA(out),
                                      get_gradients_bool ? (point2_t*)PyArray_DATA(grad) : NULL,
                                      (const point2_t*)PyArray_DATA(points),
                                      Npoints,
                                      fx,fy,cx,cy);

    if( get_gradients_bool )
    {
        result = PyTuple_Pack(2, out, grad);
        Py_DECREF(out);
        Py_DECREF(grad);
    }
    else
        result = (PyObject*)out;

 done:
    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_REQUIRED(FREE_PYARRAY) ;
    UN_PROJECT_STEREOGRAPHIC_ARGUMENTS_OPTIONAL(FREE_PYARRAY) ;
    RESET_SIGINT();
    return result;
}

static PyObject* project_stereographic(PyObject* self,
                                       PyObject* args,
                                       PyObject* kwargs)
{
    return _un_project_stereographic(self, args, kwargs, true);
}
static PyObject* unproject_stereographic(PyObject* self,
                                         PyObject* args,
                                         PyObject* kwargs)
{
    return _un_project_stereographic(self, args, kwargs, false);
}



#define OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(_)                                  \
    _(intrinsics,                         PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, intrinsics,                  NPY_DOUBLE, {-1 COMMA -1       } ) \
    _(extrinsics,                         PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, extrinsics,                  NPY_DOUBLE, {-1 COMMA  6       } ) \
    _(frames,                             PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, frames,                      NPY_DOUBLE, {-1 COMMA  6       } ) \
    _(points,                             PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, points,                      NPY_DOUBLE, {-1 COMMA  3       } ) \
    _(observations_board,                 PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, observations_board,          NPY_DOUBLE, {-1 COMMA -1 COMMA -1 COMMA 3 } ) \
    _(indices_frame_camintrinsics_camextrinsics,PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, indices_frame_camintrinsics_camextrinsics,  NPY_INT,    {-1 COMMA  3       } ) \
    _(observations_point,                 PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, observations_point,                   NPY_DOUBLE, {-1 COMMA  3       } ) \
    _(indices_point_camintrinsics_camextrinsics_flags,PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, indices_point_camintrinsics_camextrinsics_flags, NPY_INT,    {-1 COMMA  4       } ) \
    _(lensmodel,                         PyObject*,      NULL,    STRING_OBJECT,  ,                        NULL,                        -1,         {}                   ) \
    _(imagersizes,                        PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, imagersizes,                 NPY_INT,    {-1 COMMA 2        } )

#define OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(_) \
    _(calobject_warp,                     PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, calobject_warp,              NPY_DOUBLE, {2}                  ) \
    _(do_optimize_intrinsic_core,         PyObject*,      Py_True, "O",  ,                                  NULL,           -1,         {})  \
    _(do_optimize_intrinsic_distortions,  PyObject*,      Py_True, "O",  ,                                  NULL,           -1,         {})  \
    _(do_optimize_extrinsics,             PyObject*,      Py_True, "O",  ,                                  NULL,           -1,         {})  \
    _(do_optimize_frames,                 PyObject*,      Py_True, "O",  ,                                  NULL,           -1,         {})  \
    _(do_optimize_calobject_warp,         PyObject*,      Py_False,"O",  ,                                  NULL,           -1,         {})  \
    _(skipped_observations_board,         PyObject*,      NULL,    "O",  ,                                  NULL,           -1,         {})  \
    _(skipped_observations_point,         PyObject*,      NULL,    "O",  ,                                  NULL,           -1,         {})  \
    _(calibration_object_spacing,         double,         -1.0,    "d",  ,                                  NULL,           -1,         {})  \
    _(calibration_object_width_n,         int,            -1,      "i",  ,                                  NULL,           -1,         {})  \
    _(outlier_indices,                    PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, outlier_indices,NPY_INT,    {-1} ) \
    _(roi,                                PyArrayObject*, NULL,    "O&", PyArray_Converter_leaveNone COMMA, roi,            NPY_DOUBLE, {-1 COMMA 4} ) \
    _(verbose,                            PyObject*,      NULL,    "O",  ,                                  NULL,           -1,         {})  \
    _(skip_regularization,                PyObject*,      NULL,    "O",  ,                                  NULL,           -1,         {})

#define OPTIMIZERCALLBACK_ARGUMENTS_ALL(_) \
    OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(_) \
    OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(_)

#define OPTIMIZE_ARGUMENTS_REQUIRED(_) OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(_)
#define OPTIMIZE_ARGUMENTS_OPTIONAL(_) OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(_) \
    _(get_covariances,                    PyObject*,      NULL,    "O",  ,                                  NULL,           -1,         {})  \
    _(skip_outlier_rejection,             PyObject*,      NULL,    "O",  ,                                  NULL,           -1,         {})  \
    _(observed_pixel_uncertainty,         double,         -1.0,    "d",  ,                                  NULL,           -1,         {})  \
    _(solver_context,                     SolverContext*, NULL,    "O",  (PyObject*),                       NULL,           -1,         {})

#define OPTIMIZE_ARGUMENTS_ALL(_) \
    OPTIMIZE_ARGUMENTS_REQUIRED(_) \
    OPTIMIZE_ARGUMENTS_OPTIONAL(_)

// Using this for both optimize() and optimizerCallback()
static bool optimize_validate_args( // out
                                    lensmodel_t* lensmodel_type,

                                    // in
                                    OPTIMIZE_ARGUMENTS_REQUIRED(ARG_LIST_DEFINE)
                                    OPTIMIZE_ARGUMENTS_OPTIONAL(ARG_LIST_DEFINE)

                                    void* dummy __attribute__((unused)))
{
    if(PyObject_IsTrue(do_optimize_calobject_warp) &&
       IS_NULL(calobject_warp))
    {
        PyErr_SetString(PyExc_RuntimeError, "if(do_optimize_calobject_warp) then calobject_warp MUST be given as an array to seed the optimization and to receive the results");
        return false;
    }

    static_assert( sizeof(pose_t)/sizeof(double) == 6, "pose_t is assumed to contain 6 elements");

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    OPTIMIZERCALLBACK_ARGUMENTS_ALL(CHECK_LAYOUT) ;
#pragma GCC diagnostic pop

    int Ncameras_intrinsics = PyArray_DIMS(intrinsics)[0];
    int Ncameras_extrinsics = PyArray_DIMS(extrinsics)[0];
    if( PyArray_DIMS(imagersizes)[0] != Ncameras_intrinsics )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent Ncameras: 'intrinsics' says %ld, 'imagersizes' says %ld",
                     Ncameras_intrinsics,
                     PyArray_DIMS(imagersizes)[0]);
        return false;
    }
    if( !IS_NULL(roi) && PyArray_DIMS(roi)[0] != Ncameras_intrinsics )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent Ncameras: 'intrinsics' says %ld, 'roi' says %ld",
                     Ncameras_intrinsics,
                     PyArray_DIMS(roi)[0]);
        return false;
    }

    long int NobservationsBoard = PyArray_DIMS(observations_board)[0];
    if( PyArray_DIMS(indices_frame_camintrinsics_camextrinsics)[0] != NobservationsBoard )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent NobservationsBoard: 'observations_board' says %ld, 'indices_frame_camintrinsics_camextrinsics' says %ld",
                     NobservationsBoard,
                     PyArray_DIMS(indices_frame_camintrinsics_camextrinsics)[0]);
        return false;
    }

    // calibration_object_spacing and calibration_object_width_n must be > 0 OR
    // we have to not be using a calibration board
    if( NobservationsBoard > 0 )
    {
        if( calibration_object_spacing <= 0.0 )
        {
            PyErr_Format(PyExc_RuntimeError, "We have board observations, so calibration_object_spacing MUST be a valid float > 0");
            return false;
        }

        if( calibration_object_width_n <= 0 )
        {
            PyErr_Format(PyExc_RuntimeError, "We have board observations, so calibration_object_width_n MUST be a valid int > 0");
            return false;
        }


        if( calibration_object_width_n != PyArray_DIMS(observations_board)[1] ||
            calibration_object_width_n != PyArray_DIMS(observations_board)[2] )
        {
            PyErr_Format(PyExc_RuntimeError, "observations_board.shape[1:] MUST be (%d,%d,3). Instead got (%ld,%ld,%ld)",
                         calibration_object_width_n, calibration_object_width_n,
                         PyArray_DIMS(observations_board)[1],
                         PyArray_DIMS(observations_board)[2],
                         PyArray_DIMS(observations_board)[3]);
            return false;
        }
    }

    int NobservationsPoint = PyArray_DIMS(observations_point)[0];
    if( PyArray_DIMS(indices_point_camintrinsics_camextrinsics_flags)[0] != NobservationsPoint )
    {
        PyErr_Format(PyExc_RuntimeError, "Inconsistent NobservationsPoint: 'observations_point...' says %ld, 'indices_point_camintrinsics_camextrinsics_flags' says %ld",
                     NobservationsPoint,
                     PyArray_DIMS(indices_point_camintrinsics_camextrinsics_flags)[0]);
        return false;
    }

    if(!parse_lensmodel_from_arg(lensmodel_type, lensmodel))
        return false;

    int NlensParams = mrcal_getNlensParams(*lensmodel_type);
    if( NlensParams != PyArray_DIMS(intrinsics)[1] )
    {
        PyErr_Format(PyExc_RuntimeError, "intrinsics.shape[1] MUST be %d. Instead got %ld",
                     NlensParams,
                     PyArray_DIMS(intrinsics)[1] );
        return false;
    }

    if( !IS_NULL(skipped_observations_board) )
    {
        if( !PySequence_Check(skipped_observations_board) )
        {
            PyErr_Format(PyExc_RuntimeError, "skipped_observations_board MUST be None or an iterable of monotonically-increasing integers >= 0");
            return false;
        }

        int Nskipped_observations = (int)PySequence_Size(skipped_observations_board);
        long iskip_last = -1;
        for(int i=0; i<Nskipped_observations; i++)
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_board, i);
            if(!PyInt_Check(nextskip))
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_board MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            long iskip = PyInt_AsLong(nextskip);
            if(iskip <= iskip_last)
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_board MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            iskip_last = iskip;
        }
    }

    if( !IS_NULL(skipped_observations_point))
    {
        if( !PySequence_Check(skipped_observations_point) )
        {
            PyErr_Format(PyExc_RuntimeError, "skipped_observations_point MUST be None or an iterable of monotonically-increasing integers >= 0");
            return false;
        }

        int Nskipped_observations = (int)PySequence_Size(skipped_observations_point);
        long iskip_last = -1;
        for(int i=0; i<Nskipped_observations; i++)
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_point, i);
            if(!PyInt_Check(nextskip))
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_point MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            long iskip = PyInt_AsLong(nextskip);
            if(iskip <= iskip_last)
            {
                PyErr_Format(PyExc_RuntimeError, "skipped_observations_point MUST be None or an iterable of monotonically-increasing integers >= 0");
                return false;
            }
            iskip_last = iskip;
        }
    }

    // make sure the indices arrays are valid: the data is monotonic and
    // in-range
    int Nframes = 0;
    if( !IS_NULL(frames) )
        Nframes = PyArray_DIMS(frames)[0];
    int i_frame_last  = -1;
    int i_cam_intrinsics_last = -1;
    int i_cam_extrinsics_last = -1;
    for(int i_observation=0; i_observation<NobservationsBoard; i_observation++)
    {
        // check for monotonicity and in-rangeness
        int i_frame          = ((int*)PyArray_DATA(indices_frame_camintrinsics_camextrinsics))[i_observation*3 + 0];
        int i_cam_intrinsics = ((int*)PyArray_DATA(indices_frame_camintrinsics_camextrinsics))[i_observation*3 + 1];
        int i_cam_extrinsics = ((int*)PyArray_DATA(indices_frame_camintrinsics_camextrinsics))[i_observation*3 + 2];

        // First I make sure everything is in-range
        if(i_frame < 0 || i_frame >= Nframes)
        {
            PyErr_Format(PyExc_RuntimeError, "i_frame MUST be in [0,%d], instead got %d in row %d of indices_frame_camintrinsics_camextrinsics",
                         Nframes-1, i_frame, i_observation);
            return false;
        }
        if(i_cam_intrinsics < 0 || i_cam_intrinsics >= Ncameras_intrinsics)
        {
            PyErr_Format(PyExc_RuntimeError, "i_cam_intrinsics MUST be in [0,%d], instead got %d in row %d of indices_frame_camintrinsics_camextrinsics",
                         Ncameras_intrinsics-1, i_cam_intrinsics, i_observation);
            return false;
        }
        if(i_cam_extrinsics < -1 || i_cam_extrinsics >= Ncameras_extrinsics)
        {
            PyErr_Format(PyExc_RuntimeError, "i_cam_extrinsics MUST be in [-1,%d], instead got %d in row %d of indices_frame_camintrinsics_camextrinsics",
                         Ncameras_extrinsics-1, i_cam_extrinsics, i_observation);
            return false;
        }
        // And then I check monotonicity
        if(i_frame == i_frame_last)
        {
            if( i_cam_intrinsics <= i_cam_intrinsics_last )
            {
                PyErr_Format(PyExc_RuntimeError, "i_cam_intrinsics MUST be monotonically increasing in indices_frame_camintrinsics_camextrinsics. Instead row %d (frame %d) of indices_frame_camintrinsics_camextrinsics has i_cam_intrinsics=%d after previously seeing i_cam_intrinsics=%d",
                             i_observation, i_frame, i_cam_intrinsics, i_cam_intrinsics_last);
                return false;
            }
            if( i_cam_extrinsics <= i_cam_extrinsics_last )
            {
                PyErr_Format(PyExc_RuntimeError, "i_cam_extrinsics MUST be monotonically increasing in indices_frame_camintrinsics_camextrinsics. Instead row %d (frame %d) of indices_frame_camintrinsics_camextrinsics has i_cam_extrinsics=%d after previously seeing i_cam_extrinsics=%d",
                             i_observation, i_frame, i_cam_extrinsics, i_cam_extrinsics_last);
                return false;
            }
        }
        else if( i_frame < i_frame_last )
        {
            PyErr_Format(PyExc_RuntimeError, "i_frame MUST be monotonically increasing in indices_frame_camintrinsics_camextrinsics. Instead row %d of indices_frame_camintrinsics_camextrinsics has i_frame=%d after previously seeing i_frame=%d",
                         i_observation, i_frame, i_frame_last);
            return false;
        }

        i_frame_last          = i_frame;
        i_cam_intrinsics_last = i_cam_intrinsics;
        i_cam_extrinsics_last = i_cam_extrinsics;
    }
    int Npoints = 0;
    if( !IS_NULL(points) )
        Npoints = PyArray_DIMS(points)[0];
    int i_point_last = -1;
    i_cam_intrinsics_last = -1;
    i_cam_extrinsics_last = -1;
    for(int i_observation=0; i_observation<NobservationsPoint; i_observation++)
    {
        int i_point          = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 0];
        int i_cam_intrinsics = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 1];
        int i_cam_extrinsics = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 2];

        // First I make sure everything is in-range
        if(i_point < 0 || i_point >= Npoints)
        {
            PyErr_Format(PyExc_RuntimeError, "i_point MUST be in [0,%d], instead got %d in row %d of indices_point_camintrinsics_camextrinsics_flags",
                         Npoints-1, i_point, i_observation);
            return false;
        }
        if(i_cam_intrinsics < 0 || i_cam_intrinsics >= Ncameras_intrinsics)
        {
            PyErr_Format(PyExc_RuntimeError, "i_cam_intrinsics MUST be in [0,%d], instead got %d in row %d of indices_point_camintrinsics_camextrinsics_flags",
                         Ncameras_intrinsics-1, i_cam_intrinsics, i_observation);
            return false;
        }
        if(i_cam_extrinsics < -1 || i_cam_extrinsics >= Ncameras_extrinsics)
        {
            PyErr_Format(PyExc_RuntimeError, "i_cam_extrinsics MUST be in [-1,%d], instead got %d in row %d of indices_point_camintrinsics_camextrinsics_flags",
                         Ncameras_extrinsics-1, i_cam_extrinsics, i_observation);
            return false;
        }
        // And then I check monotonicity
        if(i_point == i_point_last)
        {
            if( i_cam_intrinsics <= i_cam_intrinsics_last )
            {
                PyErr_Format(PyExc_RuntimeError, "i_cam_intrinsics MUST be monotonically increasing in indices_point_camintrinsics_camextrinsics_flags. Instead row %d (point %d) of indices_point_camintrinsics_camextrinsics_flags has i_cam_intrinsics=%d after previously seeing i_cam_intrinsics=%d",
                             i_observation, i_point, i_cam_intrinsics, i_cam_intrinsics_last);
                return false;
            }
            if( i_cam_extrinsics <= i_cam_extrinsics_last )
            {
                PyErr_Format(PyExc_RuntimeError, "i_cam_extrinsics MUST be monotonically increasing in indices_point_camintrinsics_camextrinsics_flags. Instead row %d (point %d) of indices_point_camintrinsics_camextrinsics_flags has i_cam_extrinsics=%d after previously seeing i_cam_extrinsics=%d",
                             i_observation, i_point, i_cam_extrinsics, i_cam_extrinsics_last);
                return false;
            }
        }
        else if( i_point < i_point_last )
        {
            PyErr_Format(PyExc_RuntimeError, "i_point MUST be monotonically increasing in indices_point_camintrinsics_camextrinsics_flags. Instead row %d of indices_point_camintrinsics_camextrinsics_flags has i_point=%d after previously seeing i_point=%d",
                         i_observation, i_point, i_point_last);
            return false;
        }

        i_point_last          = i_point;
        i_cam_intrinsics_last = i_cam_intrinsics;
        i_cam_extrinsics_last = i_cam_extrinsics;
    }

    if(skip_outlier_rejection && PyObject_IsTrue(skip_outlier_rejection) )
    {
        // skipping outlier rejection. The pixel uncertainty isn't used and
        // doesn't matter
    }
    else
    {
        // not skipping outlier rejection. The pixel uncertainty is used and
        // must be valid
        if( observed_pixel_uncertainty <= 0.0 )
        {
            PyErr_Format(PyExc_RuntimeError, "observed_pixel_uncertainty MUST be a valid float > 0");
            return false;
        }
    }

    if( !(IS_NULL(solver_context) ||
          Py_TYPE(solver_context) == &SolverContextType) )
    {
        PyErr_Format(PyExc_RuntimeError, "solver_context must be None or of type mrcal.SolverContext");
        return false;
    }

    return true;
}

static
PyObject* _optimize(bool is_optimize, // or optimizerCallback
                    PyObject* args,
                    PyObject* kwargs)
{
    PyObject* result = NULL;

    PyArrayObject* x_final                    = NULL;
    PyArrayObject* covariance_intrinsics      = NULL;
    PyArrayObject* covariance_extrinsics      = NULL;
    PyArrayObject* outlier_indices_final      = NULL;
    PyArrayObject* outside_ROI_indices_final  = NULL;
    PyObject*      pystats                    = NULL;

    SET_SIGINT();

    // define a superset of the variables: the ones used in optimize()
    OPTIMIZE_ARGUMENTS_ALL(ARG_DEFINE) ;

    if( is_optimize )
    {
        char* keywords[] = { OPTIMIZE_ARGUMENTS_REQUIRED(NAMELIST)
                             OPTIMIZE_ARGUMENTS_OPTIONAL(NAMELIST)
                             NULL};
        if(!PyArg_ParseTupleAndKeywords( args, kwargs,
                                         OPTIMIZE_ARGUMENTS_REQUIRED(PARSECODE) "|"
                                         OPTIMIZE_ARGUMENTS_OPTIONAL(PARSECODE),

                                         keywords,

                                         OPTIMIZE_ARGUMENTS_REQUIRED(PARSEARG)
                                         OPTIMIZE_ARGUMENTS_OPTIONAL(PARSEARG) NULL))
            goto done;
    }
    else
    {
        char* keywords[] = { OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(NAMELIST)
                             OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(NAMELIST)
                             NULL};
        if(!PyArg_ParseTupleAndKeywords( args, kwargs,
                                         OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(PARSECODE) "|"
                                         OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(PARSECODE),

                                         keywords,

                                         OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(PARSEARG)
                                         OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(PARSEARG) NULL))
            goto done;

        skip_outlier_rejection = Py_True;
    }


    // Some of my input arguments can be empty (None). The code all assumes that
    // everything is a properly-dimensioned numpy array, with "empty" meaning
    // some dimension is 0. Here I make this conversion. The user can pass None,
    // and we still do the right thing.
    //
    // There's a silly implementation detail here: if you have a preprocessor
    // macro M(x), and you pass it M({1,2,3}), the preprocessor see 3 separate
    // args, not 1. That's why I have a __VA_ARGS__ here and why I instantiate a
    // separate dims[] (PyArray_SimpleNew is a macro too)
#define SET_SIZE0_IF_NONE(x, type, ...)                                 \
    ({                                                                  \
        if( IS_NULL(x) )                                                \
        {                                                               \
            if( x != NULL ) Py_DECREF(x);                               \
            npy_intp dims[] = {__VA_ARGS__};                            \
            x = (PyArrayObject*)PyArray_SimpleNew(sizeof(dims)/sizeof(dims[0]), \
                                                  dims, type);          \
        }                                                               \
    })

    SET_SIZE0_IF_NONE(extrinsics,                 NPY_DOUBLE, 0,6);

    SET_SIZE0_IF_NONE(frames,                     NPY_DOUBLE, 0,6);
    SET_SIZE0_IF_NONE(observations_board,         NPY_DOUBLE, 0,179,171,3); // arbitrary numbers; shouldn't matter
    SET_SIZE0_IF_NONE(indices_frame_camintrinsics_camextrinsics, NPY_INT,    0,3);

    SET_SIZE0_IF_NONE(points,                     NPY_DOUBLE, 0,3);
    SET_SIZE0_IF_NONE(observations_point,         NPY_DOUBLE, 0,3);
    SET_SIZE0_IF_NONE(indices_point_camintrinsics_camextrinsics_flags,NPY_INT, 0,4);
    SET_SIZE0_IF_NONE(imagersizes,                NPY_INT,    0,2);
#undef SET_NULL_IF_NONE




    lensmodel_t lensmodel_type;
    // Check the arguments for optimize(). If optimizerCallback, then the other
    // stuff is defined, but it all has valid, default values
    if( !optimize_validate_args(&lensmodel_type,
                                OPTIMIZE_ARGUMENTS_REQUIRED(ARG_LIST_CALL)
                                OPTIMIZE_ARGUMENTS_OPTIONAL(ARG_LIST_CALL)
                                NULL))
        goto done;

    {
        int Ncameras_intrinsics= PyArray_DIMS(intrinsics)[0];
        int Ncameras_extrinsics= PyArray_DIMS(extrinsics)[0];
        int Nframes            = PyArray_DIMS(frames)[0];
        int Npoints            = PyArray_DIMS(points)[0];
        int NobservationsBoard = PyArray_DIMS(observations_board)[0];

        // The checks in optimize_validate_args() make sure these casts are kosher
        double*       c_intrinsics     = (double*)  PyArray_DATA(intrinsics);
        pose_t*       c_extrinsics     = (pose_t*)  PyArray_DATA(extrinsics);
        pose_t*       c_frames         = (pose_t*)  PyArray_DATA(frames);
        point3_t*     c_points         = (point3_t*)PyArray_DATA(points);
        point2_t*     c_calobject_warp =
            IS_NULL(calobject_warp) ?
            NULL : (point2_t*)PyArray_DATA(calobject_warp);


        observation_board_t c_observations_board[NobservationsBoard];
        const point3_t* c_observations_board_pool = (point3_t*)PyArray_DATA(observations_board); // must be contiguous; made sure above

        int Nskipped_observations_board =
            IS_NULL(skipped_observations_board) ?
            0 :
            (int)PySequence_Size(skipped_observations_board);
        int i_skipped_observation_board = 0;
        int i_observation_board_next_skip = -1;
        if( i_skipped_observation_board < Nskipped_observations_board )
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_board, i_skipped_observation_board);
            i_observation_board_next_skip = (int)PyInt_AsLong(nextskip);
        }

        int i_frame_current_skipped = -1;
        int i_frame_last            = -1;
        for(int i_observation=0; i_observation<NobservationsBoard; i_observation++)
        {
            int i_frame          = ((int*)PyArray_DATA(indices_frame_camintrinsics_camextrinsics))[i_observation*3 + 0];
            int i_cam_intrinsics = ((int*)PyArray_DATA(indices_frame_camintrinsics_camextrinsics))[i_observation*3 + 1];
            int i_cam_extrinsics = ((int*)PyArray_DATA(indices_frame_camintrinsics_camextrinsics))[i_observation*3 + 2];

            c_observations_board[i_observation].i_cam_intrinsics = i_cam_intrinsics;
            c_observations_board[i_observation].i_cam_extrinsics = i_cam_extrinsics;
            c_observations_board[i_observation].i_frame          = i_frame;

            // I skip this frame if I skip ALL observations of this frame
            if( i_frame_current_skipped >= 0 &&
                i_frame_current_skipped != i_frame )
            {
                // Ooh! We moved past the frame where we skipped all
                // observations. So I need to go back, and mark all of those as
                // skipping that frame
                for(int i_observation_skip_frame = i_observation-1;
                    i_observation_skip_frame >= 0 && c_observations_board[i_observation_skip_frame].i_frame == i_frame_current_skipped;
                    i_observation_skip_frame--)
                {
                    c_observations_board[i_observation_skip_frame].skip_frame = true;
                }
            }
            else
                c_observations_board[i_observation].skip_frame = false;

            if( i_observation == i_observation_board_next_skip )
            {
                if( i_frame_last != i_frame )
                    i_frame_current_skipped = i_frame;

                c_observations_board[i_observation].skip_observation = true;

                i_skipped_observation_board++;
                if( i_skipped_observation_board < Nskipped_observations_board )
                {
                    PyObject* nextskip = PySequence_GetItem(skipped_observations_board, i_skipped_observation_board);
                    i_observation_board_next_skip = (int)PyInt_AsLong(nextskip);
                }
                else
                    i_observation_board_next_skip = -1;
            }
            else
            {
                c_observations_board[i_observation].skip_observation = false;
                i_frame_current_skipped = -1;
            }

            i_frame_last = i_frame;
        }
        // check for frame-skips on the last observation
        if( i_frame_current_skipped >= 0 )
        {
            for(int i_observation_skip_frame = NobservationsBoard - 1;
                i_observation_skip_frame >= 0 && c_observations_board[i_observation_skip_frame].i_frame == i_frame_current_skipped;
                i_observation_skip_frame--)
            {
                c_observations_board[i_observation_skip_frame].skip_frame = true;
            }
        }




        int NobservationsPoint = PyArray_DIMS(observations_point)[0];

        observation_point_t c_observations_point[NobservationsPoint];
        int Nskipped_observations_point =
            IS_NULL(skipped_observations_point) ?
            0 :
            (int)PySequence_Size(skipped_observations_point);
        int i_skipped_observation_point = 0;
        int i_observation_point_next_skip = -1;
        if( i_skipped_observation_point < Nskipped_observations_point )
        {
            PyObject* nextskip = PySequence_GetItem(skipped_observations_point, i_skipped_observation_point);
            i_observation_point_next_skip = (int)PyInt_AsLong(nextskip);
        }

        int i_point_current_skipped = -1;
        int i_point_last            = -1;
        for(int i_observation=0; i_observation<NobservationsPoint; i_observation++)
        {
            int i_point          = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 0];
            int i_cam_intrinsics = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 1];
            int i_cam_extrinsics = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 2];
            int flags            = ((int*)PyArray_DATA(indices_point_camintrinsics_camextrinsics_flags))[i_observation*4 + 3];

            c_observations_point[i_observation].i_cam_intrinsics = i_cam_intrinsics;
            c_observations_point[i_observation].i_cam_extrinsics = i_cam_extrinsics;
            c_observations_point[i_observation].i_point          = i_point;
            c_observations_point[i_observation].has_ref_range    = flags & (1U << MRCAL_POINT_HAS_REF_RANGE_BIT);
            c_observations_point[i_observation].has_ref_position = flags & (1U << MRCAL_POINT_HAS_REF_POSITION_BIT);;

            c_observations_point[i_observation].px = ((point3_t*)PyArray_DATA(observations_point))[i_observation];


            // I skip this point if I skip ALL observations of this point
            if( i_point_current_skipped >= 0 &&
                i_point_current_skipped != i_point )
            {
                // Ooh! We moved past the point where we skipped all
                // observations. So I need to go back, and mark all of those as
                // skipping that point
                for(int i_observation_skip_point = i_observation-1;
                    i_observation_skip_point >= 0 && c_observations_point[i_observation_skip_point].i_point == i_point_current_skipped;
                    i_observation_skip_point--)
                {
                    c_observations_point[i_observation_skip_point].skip_point = true;
                }
            }
            else
                c_observations_point[i_observation].skip_point = false;

            if( i_observation == i_observation_point_next_skip )
            {
                if( i_point_last != i_point )
                    i_point_current_skipped = i_point;

                c_observations_point[i_observation].skip_observation = true;

                i_skipped_observation_point++;
                if( i_skipped_observation_point < Nskipped_observations_point )
                {
                    PyObject* nextskip = PySequence_GetItem(skipped_observations_point, i_skipped_observation_point);
                    i_observation_point_next_skip = (int)PyInt_AsLong(nextskip);
                }
                else
                    i_observation_point_next_skip = -1;
            }
            else
            {
                c_observations_point[i_observation].skip_observation = false;
                i_point_current_skipped = -1;
            }

            i_point_last = i_point;
        }
        // check for point-skips on the last observation
        if( i_point_current_skipped >= 0 )
        {
            for(int i_observation_skip_point = NobservationsPoint - 1;
                i_observation_skip_point >= 0 && c_observations_point[i_observation_skip_point].i_point == i_point_current_skipped;
                i_observation_skip_point--)
            {
                c_observations_point[i_observation_skip_point].skip_point = true;
            }
        }




        mrcal_problem_details_t problem_details =
            { .do_optimize_intrinsic_core        = PyObject_IsTrue(do_optimize_intrinsic_core),
              .do_optimize_intrinsic_distortions = PyObject_IsTrue(do_optimize_intrinsic_distortions),
              .do_optimize_extrinsics            = PyObject_IsTrue(do_optimize_extrinsics),
              .do_optimize_frames                = PyObject_IsTrue(do_optimize_frames),
              .do_optimize_calobject_warp        = PyObject_IsTrue(do_optimize_calobject_warp),
              .do_skip_regularization            = skip_regularization && PyObject_IsTrue(skip_regularization)
            };

        int Nmeasurements = mrcal_getNmeasurements_all(Ncameras_intrinsics, NobservationsBoard,
                                                       c_observations_point, NobservationsPoint,
                                                       calibration_object_width_n,
                                                       problem_details,
                                                       lensmodel_type);

        int Nintrinsics_all = mrcal_getNlensParams(lensmodel_type);

        double* c_covariance_intrinsics = NULL;
        if(Nintrinsics_all != 0 &&
           get_covariances && PyObject_IsTrue(get_covariances))
        {
            covariance_intrinsics =
                (PyArrayObject*)PyArray_SimpleNew(3,
                                                  ((npy_intp[]){Ncameras_intrinsics,
                                                                Nintrinsics_all,Nintrinsics_all}), NPY_DOUBLE);
            c_covariance_intrinsics = PyArray_DATA(covariance_intrinsics);
        }

        double* c_covariance_extrinsics = NULL;
        if(Ncameras_extrinsics > 1 &&
           get_covariances && PyObject_IsTrue(get_covariances))
        {
            covariance_extrinsics =
                (PyArrayObject*)PyArray_SimpleNew(2,
                                                  ((npy_intp[]){Ncameras_extrinsics*6,Ncameras_extrinsics*6}), NPY_DOUBLE);
            c_covariance_extrinsics = PyArray_DATA(covariance_extrinsics);
        }

        // input
        int* c_outlier_indices;
        int Noutlier_indices;
        if(IS_NULL(outlier_indices))
        {
            c_outlier_indices = NULL;
            Noutlier_indices  = 0;
        }
        else
        {
            c_outlier_indices = PyArray_DATA(outlier_indices);
            Noutlier_indices = PyArray_DIMS(outlier_indices)[0];
        }

        double* c_roi;
        if(IS_NULL(roi))
            c_roi = NULL;
        else
            c_roi = PyArray_DATA(roi);

        int* c_imagersizes = PyArray_DATA(imagersizes);

        dogleg_solverContext_t** solver_context_optimizer = NULL;
        if(!IS_NULL(solver_context))
        {
            solver_context_optimizer                   = &solver_context->ctx;
            solver_context->lensmodel                  = lensmodel_type;
            solver_context->problem_details            = problem_details;
            solver_context->Ncameras_intrinsics        = Ncameras_intrinsics;
            solver_context->Ncameras_extrinsics        = Ncameras_extrinsics;
            solver_context->Nframes                    = Nframes;
            solver_context->Npoints                    = Npoints;
            solver_context->NobservationsBoard         = NobservationsBoard;
            solver_context->calibration_object_width_n = calibration_object_width_n;

        }

        // both optimize() and optimizerCallback() use this
        x_final = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Nmeasurements}), NPY_DOUBLE);
        double* c_x_final = PyArray_DATA(x_final);

        if( is_optimize )
        {
            const int Npoints_fromBoards =
                NobservationsBoard *
                calibration_object_width_n*calibration_object_width_n;
            outlier_indices_final     = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Npoints_fromBoards}), NPY_INT);
            outside_ROI_indices_final = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Npoints_fromBoards}), NPY_INT);

            // output
            int* c_outlier_indices_final     = PyArray_DATA(outlier_indices_final);
            int* c_outside_ROI_indices_final = PyArray_DATA(outside_ROI_indices_final);

            mrcal_stats_t stats =
                mrcal_optimize( c_x_final,
                                NULL,
                                c_covariance_intrinsics,
                                c_covariance_extrinsics,
                                c_outlier_indices_final,
                                c_outside_ROI_indices_final,
                                (void**)solver_context_optimizer,
                                c_intrinsics,
                                c_extrinsics,
                                c_frames,
                                c_points,
                                c_calobject_warp,

                                Ncameras_intrinsics, Ncameras_extrinsics,
                                Nframes, Npoints,

                                c_observations_board,
                                c_observations_board_pool,
                                NobservationsBoard,
                                c_observations_point,
                                NobservationsPoint,

                                false,
                                Noutlier_indices,
                                c_outlier_indices,
                                c_roi,
                                verbose &&                PyObject_IsTrue(verbose),
                                skip_outlier_rejection && PyObject_IsTrue(skip_outlier_rejection),
                                lensmodel_type,
                                observed_pixel_uncertainty,
                                c_imagersizes,
                                problem_details,

                                calibration_object_spacing,
                                calibration_object_width_n);

            if(stats.rms_reproj_error__pixels < 0.0)
            {
                // Error! I throw an exception
                PyErr_SetString(PyExc_RuntimeError, "mrcal.optimize() failed!");
                goto done;
            }

            pystats = PyDict_New();
            if(pystats == NULL)
            {
                PyErr_SetString(PyExc_RuntimeError, "PyDict_New() failed!");
                goto done;
            }
#define MRCAL_STATS_ITEM_POPULATE_DICT(type, name, pyconverter)         \
            {                                                           \
                PyObject* obj = pyconverter( (type)stats.name);         \
                if( obj == NULL)                                        \
                {                                                       \
                    PyErr_SetString(PyExc_RuntimeError, "Couldn't make PyObject for '" #name "'"); \
                    goto done;                                          \
                }                                                       \
                                                                        \
                if( 0 != PyDict_SetItemString(pystats, #name, obj) )    \
                {                                                       \
                    PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict '" #name "'"); \
                    Py_DECREF(obj);                                     \
                    goto done;                                          \
                }                                                       \
            }
            MRCAL_STATS_ITEM(MRCAL_STATS_ITEM_POPULATE_DICT);

            if( 0 != PyDict_SetItemString(pystats, "x",
                                          (PyObject*)x_final) )
            {
                PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'x'");
                goto done;
            }
            if( covariance_intrinsics &&
                0 != PyDict_SetItemString(pystats, "covariance_intrinsics",
                                          (PyObject*)covariance_intrinsics) )
            {
                PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'covariance_intrinsics'");
                goto done;
            }
            if( covariance_extrinsics &&
                0 != PyDict_SetItemString(pystats, "covariance_extrinsics",
                                          (PyObject*)covariance_extrinsics) )
            {
                PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'covariance_extrinsics'");
                goto done;
            }
            // The outlier_indices_final numpy array has Nfeatures elements,
            // but I want to return only the first Noutliers elements
            if( NULL == PyArray_Resize(outlier_indices_final,
                                       &(PyArray_Dims){ .ptr = ((npy_intp[]){stats.Noutliers}),
                                           .len = 1 },
                                       true,
                                       NPY_ANYORDER))
            {
                PyErr_Format(PyExc_RuntimeError, "Couldn't resize outlier_indices_final to %d elements",
                             stats.Noutliers);
                goto done;
            }
            if( 0 != PyDict_SetItemString(pystats, "outlier_indices",
                                          (PyObject*)outlier_indices_final) )
            {
                PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'outlier_indices'");
                goto done;
            }
            // The outside_ROI_indices_final numpy array has Nfeatures elements,
            // but I want to return only the first NoutsideROI elements
            if( NULL == PyArray_Resize(outside_ROI_indices_final,
                                       &(PyArray_Dims){ .ptr = ((npy_intp[]){stats.NoutsideROI}),
                                           .len = 1 },
                                       true,
                                       NPY_ANYORDER))
            {
                PyErr_Format(PyExc_RuntimeError, "Couldn't resize outside_ROI_indices_final to %d elements",
                             stats.NoutsideROI);
                goto done;
            }
            if( 0 != PyDict_SetItemString(pystats, "outside_ROI_indices",
                                          (PyObject*)outside_ROI_indices_final) )
            {
                PyErr_SetString(PyExc_RuntimeError, "Couldn't add to stats dict 'outside_ROI_indices'");
                goto done;
            }

            result = pystats;
            Py_INCREF(result);
        }
        else
        {
            int N_j_nonzero = mrcal_getN_j_nonzero(Ncameras_intrinsics, Ncameras_extrinsics,
                                                   c_observations_board, NobservationsBoard,
                                                   c_observations_point, NobservationsPoint,
                                                   problem_details,
                                                   lensmodel_type,
                                                   calibration_object_width_n);
            int Nintrinsics = mrcal_getNlensParams(lensmodel_type);

            int Nstate = mrcal_getNstate(Ncameras_intrinsics, Ncameras_extrinsics,
                                         Nframes, Npoints,
                                         problem_details, lensmodel_type);

            PyArrayObject* P = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){Nmeasurements + 1}), NPY_INT32);
            PyArrayObject* I = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){N_j_nonzero      }), NPY_INT32);
            PyArrayObject* X = (PyArrayObject*)PyArray_SimpleNew(1, ((npy_intp[]){N_j_nonzero      }), NPY_DOUBLE);

            cholmod_sparse Jt = {
                .nrow   = Nstate,
                .ncol   = Nmeasurements,
                .nzmax  = N_j_nonzero,
                .stype  = 0,
                .itype  = CHOLMOD_INT,
                .xtype  = CHOLMOD_REAL,
                .dtype  = CHOLMOD_DOUBLE,
                .sorted = 1,
                .packed = 1,
                .p = PyArray_DATA(P),
                .i = PyArray_DATA(I),
                .x = PyArray_DATA(X) };

            mrcal_optimizerCallback( c_x_final,
                                     &Jt,

                                     c_intrinsics,
                                     c_extrinsics,
                                     c_frames,
                                     c_points,
                                     c_calobject_warp,

                                     Ncameras_intrinsics, Ncameras_extrinsics,
                                     Nframes, Npoints,

                                     c_observations_board,
                                     c_observations_board_pool,
                                     NobservationsBoard,
                                     c_observations_point,
                                     NobservationsPoint,

                                     Noutlier_indices,
                                     c_outlier_indices,
                                     c_roi,
                                     verbose && PyObject_IsTrue(verbose),
                                     lensmodel_type,
                                     c_imagersizes,
                                     problem_details,

                                     calibration_object_spacing,
                                     calibration_object_width_n,
                                     Nintrinsics, Nmeasurements, N_j_nonzero);

            result = PyTuple_Pack(2, x_final, csr_from_cholmod_sparse(&Jt,
                                                                      (PyObject*)P,
                                                                      (PyObject*)I,
                                                                      (PyObject*)X));
            Py_DECREF(P);
            Py_DECREF(I);
            Py_DECREF(X);
        }
    }

 done:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
    OPTIMIZERCALLBACK_ARGUMENTS_REQUIRED(FREE_PYARRAY) ;
    OPTIMIZERCALLBACK_ARGUMENTS_OPTIONAL(FREE_PYARRAY) ;
#pragma GCC diagnostic pop

    if(x_final)               Py_DECREF(x_final);
    if(covariance_intrinsics)
        Py_DECREF(covariance_intrinsics);
    if(covariance_extrinsics)
        Py_DECREF(covariance_extrinsics);
    if(outlier_indices_final) Py_DECREF(outlier_indices_final);
    if(pystats)               Py_DECREF(pystats);

    RESET_SIGINT();
    return result;
}

static PyObject* optimizerCallback(PyObject* NPY_UNUSED(self),
                                   PyObject* args,
                                   PyObject* kwargs)
{
    return _optimize(false, args, kwargs);
}
static PyObject* optimize(PyObject* NPY_UNUSED(self),
                          PyObject* args,
                          PyObject* kwargs)
{
    return _optimize(true, args, kwargs);
}




static const char optimize_docstring[] =
#include "optimize.docstring.h"
    ;
static const char optimizerCallback_docstring[] =
#include "optimizerCallback.docstring.h"
    ;
static const char getLensModelMeta_docstring[] =
#include "getLensModelMeta.docstring.h"
    ;
static const char getNlensParams_docstring[] =
#include "getNlensParams.docstring.h"
    ;
static const char getSupportedLensModels_docstring[] =
#include "getSupportedLensModels.docstring.h"
    ;
static const char getNextLensModel_docstring[] =
#include "getNextLensModel.docstring.h"
    ;
static const char getKnotsForSplinedModels_docstring[] =
#include "getKnotsForSplinedModels.docstring.h"
    ;
static const char project_docstring[] =
#include "project.docstring.h"
    ;
static const char _unproject_docstring[] =
#include "_unproject.docstring.h"
    ;
static const char project_stereographic_docstring[] =
#include "project_stereographic.docstring.h"
    ;
static const char unproject_stereographic_docstring[] =
#include "unproject_stereographic.docstring.h"
    ;
static PyMethodDef methods[] =
    { PYMETHODDEF_ENTRY(,optimize,                 METH_VARARGS | METH_KEYWORDS),
      PYMETHODDEF_ENTRY(,optimizerCallback,        METH_VARARGS | METH_KEYWORDS),
      PYMETHODDEF_ENTRY(,getLensModelMeta,         METH_VARARGS),
      PYMETHODDEF_ENTRY(,getNlensParams,           METH_VARARGS),
      PYMETHODDEF_ENTRY(,getSupportedLensModels,   METH_NOARGS),
      PYMETHODDEF_ENTRY(,getNextLensModel,         METH_VARARGS),
      PYMETHODDEF_ENTRY(,getKnotsForSplinedModels, METH_VARARGS),
      PYMETHODDEF_ENTRY(,project,                  METH_VARARGS | METH_KEYWORDS),
      PYMETHODDEF_ENTRY(,_unproject,               METH_VARARGS | METH_KEYWORDS),
      PYMETHODDEF_ENTRY(,project_stereographic,    METH_VARARGS | METH_KEYWORDS),
      PYMETHODDEF_ENTRY(,unproject_stereographic,  METH_VARARGS | METH_KEYWORDS),
      {}
    };


static void _init_mrcal_common(PyObject* module)
{
    Py_INCREF(&SolverContextType);
    PyModule_AddObject(module, "SolverContext", (PyObject *)&SolverContextType);

    PyModule_AddIntConstant(module,
                            "POINT_HAS_REF_RANGE",
                            1U << MRCAL_POINT_HAS_REF_RANGE_BIT);
    PyModule_AddIntConstant(module,
                            "POINT_HAS_REF_POSITION",
                            1U << MRCAL_POINT_HAS_REF_POSITION_BIT);
}

#if PY_MAJOR_VERSION == 2

PyMODINIT_FUNC init_mrcal(void)
{
    if (PyType_Ready(&SolverContextType) < 0)
        return;

    PyObject* module =
        Py_InitModule3("_mrcal", methods,
                       "Calibration and SFM routines");
    _init_mrcal_common(module);
    import_array();
}

#else

static struct PyModuleDef module_def =
    {
     PyModuleDef_HEAD_INIT,
     "_mrcal",
     "Calibration and SFM routines",
     -1,
     methods
    };

PyMODINIT_FUNC PyInit__mrcal(void)
{
    if (PyType_Ready(&SolverContextType) < 0)
        return NULL;

    PyObject* module =
        PyModule_Create(&module_def);

    _init_mrcal_common(module);
    import_array();

    return module;
}

#endif
