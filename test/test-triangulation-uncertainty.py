#!/usr/bin/python3

r'''Triangulation uncertainty quantification test

We look at the triangulated position computed from a pixel observation in two
cameras. Calibration-time noise and triangulation-time noise both affect the
accuracy of the triangulated result. This tool samples both of these noise
sources to make sure the analytical uncertainty predictions are correct

'''

import sys
import argparse
import re
import os

cache_file = "/tmp/test-triangulation-uncertainty.pickle"

def parse_args():

    parser = \
        argparse.ArgumentParser(description = __doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument('--fixed',
                        type=str,
                        choices=('cam0','frames'),
                        default = 'cam0',
                        help='''Are we putting the origin at camera0, or are all the frames at a fixed (and
                        non-optimizeable) pose? One or the other is required.''')
    parser.add_argument('--model',
                        type=str,
                        choices=('opencv4','opencv8','splined'),
                        default = 'opencv4',
                        help='''Which lens model we're using. Must be one of
                        ('opencv4','opencv8','splined')''')
    parser.add_argument('--Nframes',
                        type=int,
                        default=50,
                        help='''How many chessboard poses to simulate. These are dense observations: every
                        camera sees every corner of every chessboard pose''')
    parser.add_argument('--Nsamples',
                        type=int,
                        default=100,
                        help='''How many random samples to evaluate''')
    parser.add_argument('--Ncameras',
                        type    = int,
                        default = 2,
                        help='''How many calibration-time cameras to simulate.
                        We will use 2 of these for triangulation, selected with
                        --cameras''')
    parser.add_argument('--cameras',
                        type = int,
                        nargs = 2,
                        default = (0,1),
                        help='''Which cameras we're using for the triangulation.
                        These need to be different, and in [0,Ncameras-1]. The
                        vanilla case will have Ncameras=2, so the default value
                        for this argument (0,1) is correct''')
    parser.add_argument('--no-sampling',
                        action='store_true',
                        help='''By default we check some things, and then
                        generate lots of statistical samples to compare the
                        empirical distributions with analytic predictions. This
                        is slow, so we may want to omit it''')
    parser.add_argument('--stabilize-coords',
                        action = 'store_true',
                        help='''Whether we report the triangulation in the camera-0 coordinate system (which
                        is moving due to noise) or in a stabilized coordinate
                        system based on the frame poses''')
    parser.add_argument('--cull-left-of-center',
                        action = 'store_true',
                        help='''If given, the calibration data in the left half of the imager is thrown
                        out''')
    parser.add_argument('--pixel-uncertainty-stdev-calibration',
                        type    = float,
                        default = 0.5,
                        help='''The observed_pixel_uncertainty of the chessboard observations at calibration
                        time''')
    parser.add_argument('--pixel-uncertainty-stdev-triangulation',
                        type    = float,
                        default = 0.5,
                        help='''The observed_pixel_uncertainty of the point observations at triangulation
                        time''')
    parser.add_argument('--pixel-uncertainty-triangulation-correlation',
                        type    = float,
                        default = 0.0,
                        help='''By default, the noise in the observation-time pixel observations is assumed
                        independent. This isn't entirely realistic: observations
                        of the same feature in multiple cameras originate from
                        an imager correlation operation, so they will have some
                        amount of correlation. If given, this argument specifies
                        how much correlation. This is a value in [0,1] scaling
                        the variance. 0 means "independent" (the default). 1.0
                        means "100% correlated".''')
    parser.add_argument('--baseline',
                        type    = float,
                        default = 2.,
                        help='''The baseline of the camera pair. This is the
                        horizontal distance between each pair of adjacent
                        cameras''')
    parser.add_argument('--observed-point',
                        type    = float,
                        nargs   = 3,
                        required = True,
                        help='''The world coordinate of the observed point. Usually this will be ~(small, 0,
                        large). The code will evaluate two points together: the
                        one passed here, and the same one with a negated x
                        coordinate''')
    parser.add_argument('--cache',
                        type=str,
                        choices=('read','write'),
                        help=f'''A cache file stores the recalibration results; computing these can take a
                        long time. This option allows us to or write the cache
                        instead of sampling. The cache file is hardcoded to
                        {cache_file}. By default, we do neither: we don't read
                        the cache (we sample instead), and we do not write it to
                        disk when we're done. This option is useful for tests
                        where we reprocess the same scenario repeatedly''')
    parser.add_argument('--make-documentation-plots',
                        type=str,
                        help='''If given, we produce plots for the documentation. Takes one argument: a
                        string describing this test. This will be used in the
                        filenames and titles of the resulting plots. Whitespace
                        and funny characters are allowed: will be replaced with
                        _ in the filenames. To make interactive plots, pass
                        ""''')
    parser.add_argument('--ellipse-plot-radius',
                        type=float,
                        help='''By default, the ellipse plot autoscale to show the data and the ellipses
                        nicely. But that means that plots aren't comparable
                        between runs. This option can be passed to select a
                        constant plot width, which allows such comparisons''')
    parser.add_argument('--terminal-pdf',
                        type=str,
                        help='''The gnuplotlib terminal for --make-documentation-plots .PDFs. Omit this
                        unless you know what you're doing''')
    parser.add_argument('--terminal-svg',
                        type=str,
                        help='''The gnuplotlib terminal for --make-documentation-plots .SVGs. Omit this
                        unless you know what you're doing''')
    parser.add_argument('--terminal-png',
                        type=str,
                        help='''The gnuplotlib terminal for --make-documentation-plots .PNGs. Omit this
                        unless you know what you're doing''')
    parser.add_argument('--explore',
                        action='store_true',
                        help='''If given, we drop into a REPL at the end''')

    args = parser.parse_args()

    if args.Ncameras < 2:
        raise Exception("--Ncameras must be given at least 2 cameras")
    if args.cameras[0] == args.cameras[1]:
        raise Exception("--cameras must select two different cameras")
    if args.cameras[0] < 0 or args.cameras[0] >= args.Ncameras:
        raise Exception("--cameras must select two different cameras, each in [0,Ncameras-1]")
    if args.cameras[1] < 0 or args.cameras[1] >= args.Ncameras:
        raise Exception("--cameras must select two different cameras, each in [0,Ncameras-1]")
    return args


args = parse_args()


terminal = dict(pdf = args.terminal_pdf,
                svg = args.terminal_svg,
                png = args.terminal_png,
                gp  = 'gp')
pointscale = dict(pdf = 1,
                  svg = 1,
                  png = 1,
                  gp  = 1)
pointscale[""] = 1.




def shorter_terminal(t):
    # Adjust the terminal string to be less tall. Makes the multiplots look
    # better: less wasted space
    m = re.match("(.*)( size.*?,)([0-9.]+)(.*?)$", t)
    if m is None: return t
    return m.group(1) + m.group(2) + str(float(m.group(3))*0.8) + m.group(4)

if args.make_documentation_plots:

    args.make_documentation_plots_extratitle = args.make_documentation_plots
    args.make_documentation_plots_filename   = re.sub(r"[^0-9a-zA-Z_\.\-]", "_", args.make_documentation_plots)

    print(f"Will write documentation plots to {args.make_documentation_plots_filename}-xxxx.pdf and .png and .svg")

    if terminal['svg'] is None: terminal['svg'] = 'svg size 800,600       noenhanced solid dynamic    font ",14"'
    if terminal['pdf'] is None: terminal['pdf'] = 'pdf size 8in,6in       noenhanced solid color      font ",12"'
    if terminal['png'] is None: terminal['png'] = 'pngcairo size 1024,768 transparent noenhanced crop font ",12"'
else:
    args.make_documentation_plots_extratitle = None

extraset = dict()
for k in pointscale.keys():
    extraset[k] = f'pointsize {pointscale[k]}'



testdir = os.path.dirname(os.path.realpath(__file__))

# I import the LOCAL mrcal since that's what I'm testing
sys.path[:0] = f"{testdir}/..",
import mrcal
import testutils
import copy
import numpy as np
import numpysane as nps
import pickle

from test_calibration_helpers import plot_args_points_and_covariance_ellipse,plot_arg_covariance_ellipse,calibration_baseline,calibration_sample,grad


############# Set up my world, and compute all the perfect positions, pixel
############# observations of everything
fixedframes = (args.fixed == 'frames')
object_spacing          = 0.1
object_width_n          = 10
object_height_n         = 9
calobject_warp_true     = np.array((0.002, -0.005))

# I want the RNG to be deterministic
np.random.seed(0)


extrinsics_rt_fromref_true = np.zeros((args.Ncameras,6), dtype=float)
extrinsics_rt_fromref_true[:,:3] = np.random.randn(args.Ncameras,3) * 0.1
extrinsics_rt_fromref_true[:, 3] = args.baseline * np.arange(args.Ncameras)
extrinsics_rt_fromref_true[:,4:] = np.random.randn(args.Ncameras,2) * 0.1

# cam0 is at the identity. This makes my life easy: I can assume that the
# optimization_inputs returned by calibration_baseline() use the same ref
# coordinate system as these transformations. I explicitly state this by passing
# calibration_baseline(allow_nonidentity_cam0_transform=False) later
extrinsics_rt_fromref_true[0] *= 0


# 1km straight ahead. In the cam0 coord system
# shape (Npoints,3)
p_triangulated_true0 = np.array((args.observed_point,
                                 args.observed_point),
                                dtype=float)
# first point has x<0
p_triangulated_true0[0,0] = -np.abs(p_triangulated_true0[0,0])
# second point is the same, but with a negated x: x>0
p_triangulated_true0[1,0] = -p_triangulated_true0[0,0]

Npoints = p_triangulated_true0.shape[0]



@nps.broadcast_define( (('Nintrinsics',),('Nintrinsics',),
                        (6,),(6,),
                        ('Nframes',6), ('Nframes',6),
                        ('Npoints',2,2)),
                       ('Npoints',3))
def triangulate_nograd( intrinsics_data0,intrinsics_data1,
                        rt_cam0_ref,rt_cam1_ref,
                        rt_ref_frame, rt_ref_frame_true,
                        q,
                        lensmodel,
                        stabilize_coords = True):
    return _triangulate( intrinsics_data0, intrinsics_data1,
                         rt_cam0_ref, rt_cam1_ref,
                         rt_ref_frame,
                         rt_ref_frame_true,
                         nps.atleast_dims(q,-3),
                         lensmodel,
                         stabilize_coords = stabilize_coords,
                         get_gradients = False)


@nps.broadcast_define( (('Nintrinsics',),('Nintrinsics',),
                        (6,),(6,),
                        ('Nframes',6), ('Nframes',6),
                        ('Npoints',2,2)),
                       (('Npoints',3),
                        (6,6),
                        (6,6),(6,6),
                        ('Npoints',3,'Nintrinsics'),
                        ('Npoints',3,'Nintrinsics'),
                        ('Npoints',3,3),('Npoints',3,3),
                        ('Npoints',3,3),('Npoints',3,3),('Npoints',3,3),
                        ('Npoints','Nframes',3,6),
                        ('Npoints',3, 2,2)))
def triangulate_grad( intrinsics_data0,intrinsics_data1,
                      rt_cam0_ref,rt_cam1_ref,
                      rt_ref_frame, rt_ref_frame_true,
                      q,
                      lensmodel,
                      stabilize_coords = True):
    return _triangulate( intrinsics_data0,intrinsics_data1,
                         rt_cam0_ref,rt_cam1_ref,
                         rt_ref_frame,
                         rt_ref_frame_true,
                         nps.atleast_dims(q,-3),
                         lensmodel,
                         stabilize_coords = stabilize_coords,
                         get_gradients    = True)


def _triangulate(# shape (Nintrinsics,)
                 intrinsics_data0, intrinsics_data1,
                 # shape (6,)
                 rt_cam0_ref, rt_cam1_ref,
                 # shape (Nframes,6),
                 rt_ref_frame, rt_ref_frame_true,
                 # shape (..., 2 (Ncameras), 2 (xy))
                 q,

                 lensmodel,
                 stabilize_coords,
                 get_gradients):

    r'''reports the triangulated position in the coords of cam0'''

    if not ( intrinsics_data0.ndim == 1 and \
             intrinsics_data1.ndim == 1 and \
             intrinsics_data0.shape[0] == intrinsics_data1.shape[0] and \
             rt_cam0_ref.shape == (6,) and \
             rt_cam1_ref.shape == (6,) and \
             rt_ref_frame.ndim == 2 and rt_ref_frame.shape[-1] == 6 and \
             q.shape[-2:] == (2,2 ) ):
        raise Exception("Arguments must consistent dimensions")

    # I now compute the same triangulation, but just at the un-perturbed baseline,
    # and keeping track of all the gradients

    if not get_gradients:

        rt01_baseline = mrcal.compose_rt(rt_cam0_ref,
                                         mrcal.invert_rt(rt_cam1_ref))

        # all the v have shape (...,3)
        vlocal0 = \
            mrcal.unproject(q[...,0,:],
                            lensmodel, intrinsics_data0)
        vlocal1 = \
            mrcal.unproject(q[...,1,:],
                            lensmodel, intrinsics_data1)

        v0 = vlocal0
        v1 = \
            mrcal.rotate_point_r(rt01_baseline[:3], vlocal1)

        # p_triangulated0 has shape (..., 3)
        p_triangulated0 = \
            mrcal.triangulate_leecivera_mid2(v0, v1, rt01_baseline[3:])

        p_triangulated_ref = mrcal.transform_point_rt(rt_cam0_ref, p_triangulated0,
                                                      inverted = True)

        if stabilize_coords:

            # shape (..., Nframes, 3)
            p_frames_new = \
                mrcal.transform_point_rt(rt_ref_frame,
                                         nps.dummy(p_triangulated_ref,-2),
                                         inverted = True)

            # shape (..., Nframes, 3)
            p_refs = mrcal.transform_point_rt(rt_ref_frame_true, p_frames_new)

            # shape (..., 3)
            p_triangulated_ref = np.mean(p_refs, axis=-2)
            p_triangulated0 = mrcal.transform_point_rt(rt_cam0_ref, p_triangulated_ref)

        return p_triangulated0
    else:
        rtr1,drtr1_drt1r = mrcal.invert_rt(rt_cam1_ref,
                                           get_gradients=True)
        rt01_baseline,drt01_drt0r, drt01_drtr1 = mrcal.compose_rt(rt_cam0_ref, rtr1, get_gradients=True)

        # all the v have shape (...,3)
        vlocal0, dvlocal0_dq0, dvlocal0_dintrinsics0 = \
            mrcal.unproject(q[...,0,:],
                            lensmodel, intrinsics_data0,
                            get_gradients = True)
        vlocal1, dvlocal1_dq1, dvlocal1_dintrinsics1 = \
            mrcal.unproject(q[...,1,:],
                            lensmodel, intrinsics_data1,
                            get_gradients = True)

        v0 = vlocal0
        v1, dv1_dr01, dv1_dvlocal1 = \
            mrcal.rotate_point_r(rt01_baseline[:3], vlocal1,
                                 get_gradients=True)

        # p_triangulated0 has shape (..., 3)
        p_triangulated0, dp_triangulated_dv0, dp_triangulated_dv1, dp_triangulated_dt01 = \
            mrcal.triangulate_leecivera_mid2(v0, v1, rt01_baseline[3:],
                                             get_gradients = True)

        shape_leading = dp_triangulated_dv0.shape[:-2]

        dp_triangulated_dq = np.zeros(shape_leading + (3,) + q.shape[-2:], dtype=float)
        nps.matmult( dp_triangulated_dv0,
                     dvlocal0_dq0,
                     out = dp_triangulated_dq[..., 0, :])
        nps.matmult( dp_triangulated_dv1,
                     dv1_dvlocal1,
                     dvlocal1_dq1,
                     out = dp_triangulated_dq[..., 1, :])

        Nframes = len(rt_ref_frame)

        if stabilize_coords:

            # shape (Nframes,6)
            rt_frame_ref, drtfr_drtrf = \
                mrcal.invert_rt(rt_ref_frame, get_gradients=True)

            # shape (Nframes,6)
            rt_true_shifted, _, drt_drtfr = \
                mrcal.compose_rt(rt_ref_frame_true, rt_frame_ref,
                                 get_gradients=True)

            # shape (..., Nframes, 3)
            p_refs,dprefs_drt,dprefs_dptriangulated = \
                mrcal.transform_point_rt(rt_true_shifted,
                                         nps.dummy(p_triangulated0,-2),
                                         get_gradients = True)

            # shape (..., 3)
            p_triangulated0 = np.mean(p_refs, axis=-2)

            # I have dpold/dx. dpnew/dx = dpnew/dpold dpold/dx

            # shape (...,3,3)
            dpnew_dpold = np.mean(dprefs_dptriangulated, axis=-3)
            dp_triangulated_dv0  = nps.matmult(dpnew_dpold, dp_triangulated_dv0)
            dp_triangulated_dv1  = nps.matmult(dpnew_dpold, dp_triangulated_dv1)
            dp_triangulated_dt01 = nps.matmult(dpnew_dpold, dp_triangulated_dt01)
            dp_triangulated_dq   = nps.xchg(nps.matmult( dpnew_dpold,
                                                         nps.xchg(dp_triangulated_dq,
                                                                  -2,-3)),
                                            -2,-3)

            # shape (..., Nframes,3,6)
            dp_triangulated_drtrf = \
                nps.matmult(dprefs_drt, drt_drtfr, drtfr_drtrf) / Nframes
        else:
            dp_triangulated_drtrf = np.zeros(shape_leading + (Nframes,3,6), dtype=float)

        return \
            p_triangulated0, \
            drtr1_drt1r, \
            drt01_drt0r, drt01_drtr1, \
            dvlocal0_dintrinsics0, dvlocal1_dintrinsics1, \
            dv1_dr01, dv1_dvlocal1, \
            dp_triangulated_dv0, dp_triangulated_dv1, dp_triangulated_dt01, \
            dp_triangulated_drtrf, \
            dp_triangulated_dq



################# Sampling
if args.cache is None or args.cache == 'write':
    optimization_inputs_baseline,                          \
    models_true, models_baseline,                          \
    indices_frame_camintrinsics_camextrinsics,             \
    lensmodel, Nintrinsics, imagersizes,                   \
    intrinsics_true, extrinsics_true_mounted, frames_true, \
    observations_true,                                     \
    args.Nframes =                                         \
        calibration_baseline(args.model,
                             args.Ncameras,
                             args.Nframes,
                             None,
                             args.pixel_uncertainty_stdev_calibration,
                             object_width_n,
                             object_height_n,
                             object_spacing,
                             extrinsics_rt_fromref_true,
                             calobject_warp_true,
                             fixedframes,
                             testdir,
                             cull_left_of_center = args.cull_left_of_center,
                             allow_nonidentity_cam0_transform = False)
else:
    with open(cache_file,"rb") as f:
        (optimization_inputs_baseline,
         models_true,
         models_baseline,
         indices_frame_camintrinsics_camextrinsics,
         lensmodel,
         Nintrinsics,
         imagersizes,
         intrinsics_true,
         extrinsics_true_mounted,
         frames_true,
         observations_true,
         intrinsics_sampled,
         extrinsics_sampled_mounted,
         frames_sampled,
         calobject_warp_sampled) = pickle.load(f)


icam0,icam1 = args.cameras

Rt01_true = mrcal.compose_Rt(mrcal.Rt_from_rt(extrinsics_rt_fromref_true[icam0]),
                             mrcal.invert_Rt(mrcal.Rt_from_rt(extrinsics_rt_fromref_true[icam1])))
Rt10_true = mrcal.invert_Rt(Rt01_true)

# shape (Npoints,Ncameras,3)
p_triangulated_true_local = nps.xchg( nps.cat( p_triangulated_true0,
                                               mrcal.transform_point_Rt(Rt10_true, p_triangulated_true0) ),
                                      0,1)

# Pixel coords at the perfect intersection
# shape (Npoints,Ncameras,2)
q_true = nps.xchg( np.array([ mrcal.project(p_triangulated_true_local[:,i,:],
                                            lensmodel,
                                            intrinsics_true[args.cameras[i]]) \
                            for i in range(2)]),
                 0,1)

# Sanity check. Without noise, the triangulation should report the test point exactly
p_triangulated0 = \
    triangulate_nograd(models_true[icam0].intrinsics()[1],         models_true[icam1].intrinsics()[1],
                       models_true[icam0].extrinsics_rt_fromref(), models_true[icam1].extrinsics_rt_fromref(),
                       frames_true, frames_true,
                       q_true,
                       lensmodel,
                       stabilize_coords = args.stabilize_coords)

testutils.confirm_equal(p_triangulated0, p_triangulated_true0,
                        eps = 1e-6,
                        msg = "Noiseless triangulation should be perfect")

################ At baseline, with gradients
p_triangulated0, \
drt_ref1_drt_1ref, \
drt01_drt_0ref, drt01_drt_ref1, \
dvlocal0_dintrinsics0, dvlocal1_dintrinsics1, \
dv1_dr01, dv1_dvlocal1, \
dp_triangulated_dv0, dp_triangulated_dv1, dp_triangulated_dt01, \
dp_triangulated_drtrf, \
dp_triangulated_dq = \
    triangulate_grad(models_baseline[icam0].intrinsics()[1],         models_baseline[icam1].intrinsics()[1],
                     models_baseline[icam0].extrinsics_rt_fromref(), models_baseline[icam1].extrinsics_rt_fromref(),
                     optimization_inputs_baseline['frames_rt_toref'], frames_true,
                     q_true,
                     lensmodel,
                     stabilize_coords = args.stabilize_coords)


testutils.confirm_equal(p_triangulated0, p_triangulated_true0,
                        worstcase   = True,
                        relative    = True,
                        eps         = 1e-4,
                        reldiff_eps = 1e-1,
                        msg = "Re-optimized triangulation should be close to the reference. This checks the regularization bias")

### Sensitivities
# The data flow:
#   q0,i0                       -> v0 (same as vlocal0; I'm working in the cam0 coord system)
#   q1,i1                       -> vlocal1
#   r_0ref,r_1ref               -> r01
#   r_0ref,r_1ref,t_0ref,t_1ref -> t01
#   vlocal1,r01                 -> v1
#   v0,v1,t01                   -> p_triangulated
ppacked,x,Jpacked,factorization = mrcal.optimizer_callback(**optimization_inputs_baseline)
Nstate = len(ppacked)

# I store dp_triangulated_dpstate initially, without worrying about the "packed"
# part. I'll scale the thing when done to pack it
dp_triangulated_dpstate = np.zeros((Npoints,3,Nstate), dtype=float)

istate_i0 = mrcal.state_index_intrinsics(icam0, **optimization_inputs_baseline)
istate_i1 = mrcal.state_index_intrinsics(icam1, **optimization_inputs_baseline)

# I'm expecting the layout of a vanilla calibration problem, and I assume that
# camera0 is at the reference below. Here I confirm that this assumption is
# correct
icam_extrinsics0 = mrcal.corresponding_icam_extrinsics(icam0, **optimization_inputs_baseline)
icam_extrinsics1 = mrcal.corresponding_icam_extrinsics(icam1, **optimization_inputs_baseline)

istate_e1 = mrcal.state_index_extrinsics(icam_extrinsics1, **optimization_inputs_baseline) \
    if icam_extrinsics1 >= 0 else None
istate_e0 = mrcal.state_index_extrinsics(icam_extrinsics0, **optimization_inputs_baseline) \
    if icam_extrinsics0 >= 0 else None

if not fixedframes:
    istate_f0     = mrcal.state_index_frames(0, **optimization_inputs_baseline)
    Nstate_frames = mrcal.num_states_frames(**optimization_inputs_baseline)
else:
    istate_f0     = None
    Nstate_frames = None


# dp_triangulated_di0 = dp_triangulated_dv0              dvlocal0_di0
# dp_triangulated_di1 = dp_triangulated_dv1 dv1_dvlocal1 dvlocal1_di1
nps.matmult( dp_triangulated_dv0,
             dvlocal0_dintrinsics0,
             out = dp_triangulated_dpstate[..., istate_i0:istate_i0+Nintrinsics])
nps.matmult( dp_triangulated_dv1,
             dv1_dvlocal1,
             dvlocal1_dintrinsics1,
             out = dp_triangulated_dpstate[..., istate_i1:istate_i1+Nintrinsics])

if istate_e1 is not None:
    # dp_triangulated_dr_0ref = dp_triangulated_dv1  dv1_dr01 dr01_dr_0ref +
    #                           dp_triangulated_dt01          dt01_dr_0ref
    # dp_triangulated_dr_1ref = dp_triangulated_dv1  dv1_dr01 dr01_dr_1ref +
    #                           dp_triangulated_dt01          dt01_dr_1ref
    # dp_triangulated_dt_0ref = dp_triangulated_dt01          dt01_dt_0ref
    # dp_triangulated_dt_1ref = dp_triangulated_dt01          dt01_dt_1ref
    dr01_dr_ref1    = drt01_drt_ref1[:3,:3]
    dr_ref1_dr_1ref = drt_ref1_drt_1ref[:3,:3]
    dr01_dr_1ref    = nps.matmult(dr01_dr_ref1, dr_ref1_dr_1ref)

    dt01_drt_ref1 = drt01_drt_ref1[3:,:]
    dt01_dr_1ref  = nps.matmult(dt01_drt_ref1, drt_ref1_drt_1ref[:,:3])
    dt01_dt_1ref  = nps.matmult(dt01_drt_ref1, drt_ref1_drt_1ref[:,3:])

    nps.matmult( dp_triangulated_dv1,
                 dv1_dr01,
                 dr01_dr_1ref,
                 out = dp_triangulated_dpstate[..., istate_e1:istate_e1+3])
    dp_triangulated_dpstate[..., istate_e1:istate_e1+3] += \
        nps.matmult(dp_triangulated_dt01, dt01_dr_1ref)

    nps.matmult( dp_triangulated_dt01,
                 dt01_dt_1ref,
                 out = dp_triangulated_dpstate[..., istate_e1+3:istate_e1+6])

if istate_e0 is not None:
    dr01_dr_0ref = drt01_drt_0ref[:3,:3]
    dt01_dr_0ref = drt01_drt_0ref[3:,:3]
    dt01_dt_0ref = drt01_drt_0ref[3:,3:]

    nps.matmult( dp_triangulated_dv1,
                 dv1_dr01,
                 dr01_dr_0ref,
                 out = dp_triangulated_dpstate[..., istate_e0:istate_e0+3])
    dp_triangulated_dpstate[..., istate_e0:istate_e0+3] += \
        nps.matmult(dp_triangulated_dt01, dt01_dr_0ref)

    nps.matmult( dp_triangulated_dt01,
                 dt01_dt_0ref,
                 out = dp_triangulated_dpstate[..., istate_e0+3:istate_e0+6])

if istate_f0 is not None:
    # dp_triangulated_drtrf has shape (Npoints,Nframes,3,6). I reshape to (Npoints,3,Nframes*6)
    dp_triangulated_dpstate[..., istate_f0:istate_f0+Nstate_frames] = \
        nps.clump(nps.xchg(dp_triangulated_drtrf,-2,-3), n=-2)


########## Gradient check
dp_triangulated_di0_empirical = grad(lambda i0: triangulate_nograd(i0, models_baseline[icam1].intrinsics()[1],
                                                                   models_baseline[icam0].extrinsics_rt_fromref(), models_baseline[icam1].extrinsics_rt_fromref(),
                                                                   optimization_inputs_baseline['frames_rt_toref'], frames_true,
                                                                   q_true,
                                                                   lensmodel,
                                                                   stabilize_coords=args.stabilize_coords),
                                     models_baseline[icam0].intrinsics()[1])
dp_triangulated_di1_empirical = grad(lambda i1: triangulate_nograd(models_baseline[icam0].intrinsics()[1],i1,
                                                                   models_baseline[icam0].extrinsics_rt_fromref(), models_baseline[icam1].extrinsics_rt_fromref(),
                                                                   optimization_inputs_baseline['frames_rt_toref'], frames_true,
                                                                   q_true,
                                                                   lensmodel,
                                                                   stabilize_coords=args.stabilize_coords),
                                     models_baseline[icam1].intrinsics()[1])
dp_triangulated_de1_empirical = grad(lambda e1: triangulate_nograd(models_baseline[icam0].intrinsics()[1], models_baseline[icam1].intrinsics()[1],
                                                                   models_baseline[icam0].extrinsics_rt_fromref(),e1,
                                                                   optimization_inputs_baseline['frames_rt_toref'], frames_true,
                                                                   q_true,
                                                                   lensmodel,
                                                                   stabilize_coords=args.stabilize_coords),
                                     models_baseline[icam1].extrinsics_rt_fromref())

dp_triangulated_de0_empirical = grad(lambda e0: triangulate_nograd(models_baseline[icam0].intrinsics()[1], models_baseline[icam1].intrinsics()[1],
                                                                   e0,models_baseline[icam1].extrinsics_rt_fromref(),
                                                                   optimization_inputs_baseline['frames_rt_toref'], frames_true,
                                                                   q_true,
                                                                   lensmodel,
                                                                   stabilize_coords=args.stabilize_coords),
                                     models_baseline[icam0].extrinsics_rt_fromref())

dp_triangulated_drtrf_empirical = grad(lambda rtrf: triangulate_nograd(models_baseline[icam0].intrinsics()[1], models_baseline[icam1].intrinsics()[1],
                                                                       models_baseline[icam0].extrinsics_rt_fromref(), models_baseline[icam1].extrinsics_rt_fromref(),
                                                                       rtrf, frames_true,
                                                                       q_true,
                                                                       lensmodel,
                                                                       stabilize_coords=args.stabilize_coords),
                                       optimization_inputs_baseline['frames_rt_toref'],
                                       step = 1e-5)

dp_triangulated_dq_empirical = grad(lambda q: triangulate_nograd(models_baseline[icam0].intrinsics()[1], models_baseline[icam1].intrinsics()[1],
                                                                 models_baseline[icam0].extrinsics_rt_fromref(), models_baseline[icam1].extrinsics_rt_fromref(),
                                                                 optimization_inputs_baseline['frames_rt_toref'], frames_true,
                                                                 q,
                                                                 lensmodel,
                                                                 stabilize_coords=args.stabilize_coords),
                                    q_true,
                                    step = 1e-3)

testutils.confirm_equal(dp_triangulated_dpstate[...,istate_i0:istate_i0+Nintrinsics],
                        dp_triangulated_di0_empirical,
                        relative = True,
                        worstcase = True,
                        eps = 0.1,
                        msg = "Gradient check: dp_triangulated_dpstate[intrinsics0]")
testutils.confirm_equal(dp_triangulated_dpstate[...,istate_i1:istate_i1+Nintrinsics],
                        dp_triangulated_di1_empirical,
                        relative = True,
                        worstcase = True,
                        eps = 0.1,
                        msg = "Gradient check: dp_triangulated_dpstate[intrinsics1]")
if istate_e0 is not None:
    testutils.confirm_equal(dp_triangulated_dpstate[...,istate_e0:istate_e0+6],
                            dp_triangulated_de0_empirical,
                            relative = True,
                            worstcase = True,
                            eps = 5e-4,
                            msg = "Gradient check: dp_triangulated_dpstate[extrinsics0]")
if istate_e1 is not None:
    testutils.confirm_equal(dp_triangulated_dpstate[...,istate_e1:istate_e1+6],
                            dp_triangulated_de1_empirical,
                            relative = True,
                            worstcase = True,
                            eps = 5e-4,
                            msg = "Gradient check: dp_triangulated_dpstate[extrinsics1]")
if istate_f0 is not None:
    testutils.confirm_equal(dp_triangulated_dpstate[...,istate_f0:istate_f0+Nstate_frames],
                            nps.clump(dp_triangulated_drtrf_empirical, n=-2),
                            relative = True,
                            worstcase = True,
                            eps = 0.1,
                            msg = "Gradient check: dp_triangulated_drtrf")

# dp_triangulated_dq_empirical has shape (Npoints,3,  Npoints,Ncameras,2)
# The cross terms (p_triangulated(point=A), q(point=B)) should all be zero
dp_triangulated_dq_empirical_cross_only = dp_triangulated_dq_empirical
dp_triangulated_dq_empirical = np.zeros((Npoints,3,2,2), dtype=float)

for ipt in range(Npoints):
    dp_triangulated_dq_empirical[ipt,...] = dp_triangulated_dq_empirical_cross_only[ipt,:,ipt,:,:]
    dp_triangulated_dq_empirical_cross_only[ipt,:,ipt,:,:] = 0
testutils.confirm_equal(dp_triangulated_dq_empirical_cross_only,
                        0,
                        eps = 1e-6,
                        msg = "Gradient check: dp_triangulated_dq: cross-point terms are 0")
testutils.confirm_equal(dp_triangulated_dq,
                        dp_triangulated_dq_empirical,
                        relative = True,
                        worstcase = True,
                        eps = 1e-4,
                        msg = "Gradient check: dp_triangulated_dq")

Nmeasurements_observations = mrcal.num_measurements_boards(**optimization_inputs_baseline)
if Nmeasurements_observations == mrcal.num_measurements(**optimization_inputs_baseline):
    # Note the special-case where I'm using all the observations
    Nmeasurements_observations = None

# I look at the two triangulated points together. This is a (6,) vector. And I
# pack the denominator by unpacking the numerator
# dp_triangulated_dpstate has shape (Npoints,3,Nstate)
dp0p1_triangulated_dppacked = copy.deepcopy(dp_triangulated_dpstate)
mrcal.unpack_state(dp0p1_triangulated_dppacked, **optimization_inputs_baseline)
dp0p1_triangulated_dppacked = nps.clump(dp0p1_triangulated_dppacked,n=2)


# My input vector, whose noise I'm propagating, is x = [q_calibration
# q_triangulation]: the calibration-time pixel observations and the
# observation-time pixel observations. These are independent, so Var(x) is
# block-diagonal. I want to propagate the noise in x to some function f(x). As
# usual, Var(f) = df/dx Var(x) (df/dx)T. I have x = [qc qt] and a block-diagonal
# Var(x) so Var(f) = df/dqc Var(qc) (df/dqc)T + df/dqt Var(qt) (df/dqt)T. So I
# can treat the two noise contributions separately, and add the two variances
# together

# Let's define the observation-time pixel noise. The noise vector
# q_true_sampled_noise has the same shape as q_true for each sample. so
# q_true_sampled_noise.shape = (Nsamples,Npoints,Ncameras,2). The covariance is
# a square matrix with each dimension of length Npoints*Ncameras*2
N_q_true_noise = Npoints*2*2
sigma_qt_sq = \
    args.pixel_uncertainty_stdev_triangulation * \
    args.pixel_uncertainty_stdev_triangulation
var_qt = np.diagflat( (sigma_qt_sq,) * N_q_true_noise )
var_qt_reshaped = var_qt.reshape( Npoints, 2, 2,
                                  Npoints, 2, 2 )

for ipt in range(Npoints):
    var_qt_reshaped[ipt,0,0, ipt,1,0] = sigma_qt_sq*args.pixel_uncertainty_triangulation_correlation
    var_qt_reshaped[ipt,1,0, ipt,0,0] = sigma_qt_sq*args.pixel_uncertainty_triangulation_correlation
    var_qt_reshaped[ipt,0,1, ipt,1,1] = sigma_qt_sq*args.pixel_uncertainty_triangulation_correlation
    var_qt_reshaped[ipt,1,1, ipt,0,1] = sigma_qt_sq*args.pixel_uncertainty_triangulation_correlation

# The variance due to calibration-time noise
Var_p0p1_triangulated = \
    mrcal.model_analysis._propagate_calibration_uncertainty(dp0p1_triangulated_dppacked,
                                                            factorization, Jpacked,
                                                            Nmeasurements_observations,
                                                            args.pixel_uncertainty_stdev_calibration,
                                                            what = 'covariance')

# The variance due to the observation-time noise can be simplified even further:
# the noise in each pixel observation is independent, and I can accumulate it
# independently
for ipt in range(Npoints):
    Var_p0p1_triangulated[3*ipt:3*ipt+3, 3*ipt:3*ipt+3] += \
        nps.matmult( nps.clump(dp_triangulated_dq[ipt], n=-2),

                 nps.clump( nps.clump(var_qt_reshaped[ipt,:,:,  ipt,:,:],
                                      n=2),
                            n=-2),

                 nps.transpose( nps.clump(dp_triangulated_dq[ipt], n=-2) ) )


if args.no_sampling:
    testutils.finish()
    sys.exit()

try:
    intrinsics_sampled
    did_sample = True
except:
    did_sample = False

if not did_sample:

    ( intrinsics_sampled,         \
      extrinsics_sampled_mounted, \
      frames_sampled,             \
      calobject_warp_sampled ) =  \
          calibration_sample( args.Nsamples, args.Ncameras, args.Nframes,
                              Nintrinsics,
                              optimization_inputs_baseline,
                              observations_true,
                              args.pixel_uncertainty_stdev_calibration,
                              fixedframes)

    if args.cache is not None and args.cache == 'write':
        with open(cache_file,"wb") as f:
            pickle.dump((optimization_inputs_baseline,
                         models_true,
                         models_baseline,
                         indices_frame_camintrinsics_camextrinsics,
                         lensmodel,
                         Nintrinsics,
                         imagersizes,
                         intrinsics_true,
                         extrinsics_true_mounted,
                         frames_true,
                         observations_true,
                         intrinsics_sampled,
                         extrinsics_sampled_mounted,
                         frames_sampled,
                         calobject_warp_sampled),
                        f)



# Let's actually apply the noise to compute var(distancep) empirically to compare
# against the var(distancep) prediction I just computed
# shape (Nsamples,Npoints,2,2)
qt_noise = \
    np.random.multivariate_normal( mean = np.zeros((N_q_true_noise,),),
                                   cov  = var_qt,
                                   size = args.Nsamples ).reshape(args.Nsamples,Npoints,2,2)
q_sampled = q_true + qt_noise


# I have the perfect observation pixel coords. I triangulate them through my
# sampled calibration
if fixedframes:
    extrinsics_sampled_cam0 = extrinsics_sampled_mounted[..., icam_extrinsics0,:]
    extrinsics_sampled_cam1 = extrinsics_sampled_mounted[..., icam_extrinsics1,:]
else:
    # if not fixedframes: extrinsics_sampled_mounted is prepended with the
    # extrinsics for cam0 (selected with icam_extrinsics==-1)
    extrinsics_sampled_cam0 = extrinsics_sampled_mounted[..., icam_extrinsics0+1,:]
    extrinsics_sampled_cam1 = extrinsics_sampled_mounted[..., icam_extrinsics1+1,:]

p_triangulated0_sampled = triangulate_nograd(intrinsics_sampled[...,icam0,:], intrinsics_sampled[...,icam1,:],
                                             extrinsics_sampled_cam0, extrinsics_sampled_cam1,
                                             frames_sampled, frames_true,
                                             q_sampled,
                                             lensmodel,
                                             stabilize_coords = args.stabilize_coords)


range0              = nps.mag(p_triangulated0[0])
range0_true         = nps.mag(p_triangulated0_true0[0])
range0_sampled      = nps.mag(p_triangulated0_sampled[:,0,:])
Mean_range0_sampled = nps.mag(range0_sampled).mean()
Var_range0_sampled  = nps.mag(range0_sampled).var()
# r = np.mag(p)
# dr_dp = p/r
# Var(r) = dr_dp var(p) dr_dpT
#        = p var(p) pT / norm2(p)
Var_range0 = nps.matmult(p_triangulated0[0],
                         Var_p0p1_triangulated[:3,:3],
                         nps.transpose(p_triangulated0[0]))[0] / nps.norm2(p_triangulated0[0])


diff                  = p_triangulated0[1] - p_triangulated0[0]
distance              = nps.mag(diff)
distance_true         = nps.mag(p_triangulated0_true0[:,0] - p_triangulated0_true0[:,1])
distance_sampled      = nps.mag(p_triangulated0_sampled[:,1,:] - p_triangulated0_sampled[:,0,:])
Mean_distance_sampled = nps.mag(distance_sampled).mean()
Var_distance_sampled  = nps.mag(distance_sampled).var()
# diff = p1-p0
# dist = np.mag(diff)
# ddist_dp01 = [-diff   diff] / dist
# Var(dist) = ddist_dp01 var(p01) ddist_dp01T
#           = [-diff   diff] var(p01) [-diff   diff]T / norm2(diff)
Var_distance = nps.matmult(nps.glue( -diff, diff, axis=-1),
                           Var_p0p1_triangulated,
                           nps.transpose(nps.glue( -diff, diff, axis=-1),))[0] / nps.norm2(diff)

if not (args.explore or \
        args.make_documentation_plots is not None):
    testutils.finish()
    sys.exit()



if args.make_documentation_plots is not None:

    import gnuplotlib as gp

    empirical_distributions_xz = \
        [ plot_args_points_and_covariance_ellipse(p_triangulated0_sampled[:,ipt,(0,2)],
                                                  'Observed') \
          for ipt in range(Npoints) ]
    # Individual covariances
    Var_p_diagonal = [Var_p0p1_triangulated[ipt*3:ipt*3+3,ipt*3:ipt*3+3][(0,2),:][:,(0,2)] \
                      for ipt in range(Npoints)]
    max_sigma_points = np.array([ np.max(np.sqrt(np.linalg.eig(V)[0])) for V in Var_p_diagonal ])
    max_sigma = np.max(max_sigma_points)

    if args.ellipse_plot_radius is not None:
        ellipse_plot_radius = args.ellipse_plot_radius
    else:
        ellipse_plot_radius = max_sigma*3

    title_triangulation = 'Triangulation uncertainty'
    title_covariance    = 'Covariance of the [p0,p1] vector (m^2)'
    title_range0        = 'Range to the left triangulated point'
    title_distance      = 'Distance between the two triangulated points'

    if args.make_documentation_plots_extratitle is not None:
        title_triangulation += f'. {args.make_documentation_plots_extratitle}'
        title_covariance    += f'. {args.make_documentation_plots_extratitle}'
        title_range0        += f'. {args.make_documentation_plots_extratitle}'
        title_distance      += f'. {args.make_documentation_plots_extratitle}'


    subplots = [ (empirical_distributions_xz[ipt][1], # points; plot first to not obscure the ellipses
                  plot_arg_covariance_ellipse(p_triangulated0[ipt][(0,2),],
                                              Var_p_diagonal[ipt],
                                              "Predicted"),
                  empirical_distributions_xz[ipt][0],
                  dict( square = True,
                        _xrange = [p_triangulated0[ipt,0] - ellipse_plot_radius,
                                   p_triangulated0[ipt,0] + ellipse_plot_radius],
                        _yrange = [p_triangulated0[ipt,2] - ellipse_plot_radius,
                                   p_triangulated0[ipt,2] + ellipse_plot_radius],
                        xlabel  = 'Triangulated point x (left/right) (m)',
                        ylabel  = 'Triangulated point z (forward/back) (m)',)
                  ) \
                 for ipt in range(Npoints) ]

    def makeplots(dohardcopy, processoptions_base):

        processoptions = copy.deepcopy(processoptions_base)
        if dohardcopy:
            processoptions['hardcopy'] = \
                f'{args.make_documentation_plots_filename}--ellipses.{extension}'
            processoptions['terminal'] = shorter_terminal(processoptions['terminal'])
        gp.plot( *subplots,
                 multiplot = f'title "{title_triangulation}" layout 1,2',
                 **processoptions )

        processoptions = copy.deepcopy(processoptions_base)
        if dohardcopy:
            processoptions['hardcopy'] = \
                f'{args.make_documentation_plots_filename}--p0-p1-magnitude-covariance.{extension}'
        processoptions['title'] = title_covariance
        gp.plotimage( np.abs(Var_p0p1_triangulated),
                      square = True,
                      xlabel = 'Variable index (left point x,y,z; right point x,y,z)',
                      ylabel = 'Variable index (left point x,y,z; right point x,y,z)',
                      **processoptions)


        processoptions = copy.deepcopy(processoptions_base)
        binwidth = np.sqrt(Var_range0) / 4.
        equation_range0_observed_gaussian = \
            mrcal.fitted_gaussian_equation(x        = range0_sampled,
                                           binwidth = binwidth,
                                           legend   = "Idealized gaussian fit to data")
        equation_range0_predicted_gaussian = \
            mrcal.fitted_gaussian_equation(mean     = range0,
                                           sigma    = np.sqrt(Var_range0),
                                           N        = len(range0_sampled),
                                           binwidth = binwidth,
                                           legend   = "Predicted")
        if dohardcopy:
            processoptions['hardcopy'] = \
                f'{args.make_documentation_plots_filename}--range-to-p0.{extension}'
        processoptions['title'] = title_range0
        gp.add_plot_option(processoptions, 'set', 'samples 1000')
        gp.plot(range0_sampled,
                histogram       = True,
                binwidth        = binwidth,
                equation_above  = (equation_range0_predicted_gaussian,
                                   equation_range0_observed_gaussian),
                xlabel          = "Range to the left triangulated point (m)",
                ylabel          = "Frequency",
                **processoptions)

        processoptions = copy.deepcopy(processoptions_base)
        binwidth = np.sqrt(Var_distance) / 4.
        equation_distance_observed_gaussian = \
            mrcal.fitted_gaussian_equation(x        = distance_sampled,
                                           binwidth = binwidth,
                                           legend   = "Idealized gaussian fit to data")
        equation_distance_predicted_gaussian = \
            mrcal.fitted_gaussian_equation(mean     = distance,
                                           sigma    = np.sqrt(Var_distance),
                                           N        = len(distance_sampled),
                                           binwidth = binwidth,
                                           legend   = "Predicted")
        if dohardcopy:
            processoptions['hardcopy'] = \
                f'{args.make_documentation_plots_filename}--distance-p1-p0.{extension}'
        processoptions['title'] = title_distance
        gp.add_plot_option(processoptions, 'set', 'samples 1000')
        gp.plot(distance_sampled,
                histogram       = True,
                binwidth        = binwidth,
                equation_above  = (equation_distance_predicted_gaussian,
                                   equation_distance_observed_gaussian),
                xlabel          = "Distance between triangulated points (m)",
                ylabel          = "Frequency",
                **processoptions)

    if args.make_documentation_plots:
        for extension in ('pdf','svg','png','gp'):
            makeplots(dohardcopy = True,
                      processoptions_base = dict(wait      = False,
                                                 terminal  = terminal[extension],
                                                 _set      = extraset[extension]))
    else:
        makeplots(dohardcopy = False,
                  processoptions_base = dict(wait = True))

if args.explore:
    import gnuplotlib as gp
    import IPython
    IPython.embed()




r'''
extend to work with non-ref camera

fixedframes?

triangulate_grad() should return less stuff. It can do more internally. It should
return ONLY dp_triangulated_d... gradients

What kind of nice API do I want for this? How much can I reuse the existing
uncertainty code? Can/should I have a separate rotation-only/at-infinity path?
Should finalize the API after I implement the deltapose-propagated uncertainty

Gnuplot: ellipses should move correctly when pressing "7"


tests:

look at distribution due to

- intrinsics/extrinsics and/or pixel observations

- look at mean, variance
'''
