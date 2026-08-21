"""
gxr_sweep.py
automatic camera calibration of one eye's residual distortion
by a Gray-code sweep.

The driver renders binary Gray-code stripe patterns (settings.json
streamFrame.calib.pattern, echoed in diagnostic.json), the
calibrated fisheye camera looking into the lens captures each one, and the
decode gives a dense "which output uv is shown here" map over the camera
image. Together with the camera model (pixel -> direction) and the panel's
ideal mapping (uv -> direction from the projection tangents) that is a
direct measurement of the headset's real mapping F(uv). The residual camera
rotation is solved out, the correction disp(uv) = P^-1(R^T d_cam). uv is
fitted onto the displacement lattice, and the result is written into
settings.json (live) and saved as an importable profile.

Usage:
    python gxr_sweep.py --eye left            # full sweep, apply + save
    python gxr_sweep.py --eye right --bits 9  # coarser stripes (small camera)
    python gxr_sweep.py --eye left --align    # short sweep, rotation readout only
    python gxr_sweep.py --eye left --dry      # sweep + fit, print, do not apply

Before running: camera calibrated (charuco_calibrate.py), camera mounted in
front of the eye's lens roughly on axis, streamFrame enabled in the GUI.
Blackout is turned off for the duration of the sweep and restored after.
"""

import argparse
import json
import math
import os
import sys
import time

import numpy as np

import gxr_common as g

try:
    import cv2
except ImportError:
    cv2 = None


def eye_index(name):
    name = str(name).lower()
    if name in ("0", "l", "left"):
        return 0
    if name in ("1", "r", "right"):
        return 1
    raise argparse.ArgumentTypeError("eye must be left or right")


def decode_gray(bit_planes):
    """bit_planes: list of bool arrays MSB first -> integer code array."""
    gray = np.zeros(bit_planes[0].shape, dtype=np.int64)
    for b in bit_planes:
        gray = (gray << 1) | b.astype(np.int64)
    # gray -> binary
    code = gray.copy()
    shift = gray >> 1
    while np.any(shift):
        code ^= shift
        shift >>= 1
    return code


def make_driver_io(cap, link, eye, bits, frames_per_pattern=3, settle_frames=6):
    """(show, grab) callables for run_sweep on real hardware."""
    link.settings.set_calib(eye=int(eye), patternBits=int(bits))

    def show(index):
        link.show_pattern(index, settle_frames)

    def grab():
        return g.grab_average(cap, frames_per_pattern)

    return show, grab, link.pattern_off


def run_sweep(show, grab, done, eye, bits, contrast_min=20.0, bit_confidence=0.2, verbose=True,
              checkpoint_every=3, dump_dir=None):
    """Drive the pattern sequence through show(index)/grab() and decode.

    The tracked pose drifts while the headset is stationary (gyro bias) and
    the runtime reprojects the image by it, so the panel content moves a few
    pixels over a ~2 minute sweep even on a perfectly rigid mount. To make
    that harmless, a white reference is re-captured every `checkpoint_every`
    bit pairs; phase correlation against the first white gives the image
    shift and brightness over time, and every capture is warped/scaled back
    before decoding. All captures are buffered (uint8, ~100 MB at
    1600x1200, ~400 MB at 3264x2448) and decoded afterwards.

    Returns dict with: uv (N,2) decoded output uv per valid camera pixel,
    pix (N,2) camera pixel coords, valid mask (H,W), white/black images,
    per-axis usable_bits, drift path/report fields."""
    log = (lambda *a: print(*a, flush=True)) if verbose else (lambda *a: None)
    log(f"sweep: eye={'left' if eye == 0 else 'right'} bits={bits} -> {2 + 4 * bits} patterns")

    # ---- capture phase: buffer everything, interleave white checkpoints ----
    caps = []

    def cap(pattern_index):
        show(pattern_index)
        caps.append(np.clip(np.round(grab()), 0, 255).astype(np.uint8))
        return len(caps) - 1

    i_black = cap(0)
    i_white0 = cap(1)
    white_idx = [i_white0]
    plan = []
    idx = 2
    pair_n = 0
    for axis in (0, 1):
        for bit in range(bits):
            ip = cap(idx)
            ineg = cap(idx + 1)
            plan.append((axis, bit, ip, ineg))
            idx += 2
            pair_n += 1
            if pair_n % checkpoint_every == 0:
                white_idx.append(cap(1))
    if white_idx[-1] != len(caps) - 1:
        white_idx.append(cap(1))
    done()

    # exposure sanity on the raw captures
    w_raw = caps[i_white0]
    sat = float((w_raw >= 254).mean()) * 100.0
    panel_mask = w_raw > max(30, int(w_raw.max()) * 2 // 5)
    if sat > 1.0:
        log(f"  WARNING: {sat:.1f}% of the white reference is saturated (>=254). Clipped pixels")
        log("  decode garbage that passes the confidence gates. Pin the exposure (gxr_camera.json")
        log('  "mode": "pinned", "exposure": start around -6 and lower until this warning stops).')
    pat_meds = [float(np.median(caps[ip][panel_mask])) for _, _, ip, _ in plan] if panel_mask.any() else []
    if pat_meds:
        spread = (max(pat_meds) - min(pat_meds)) / max(np.median(pat_meds), 1.0)
        if spread > 0.12:
            log(f"  WARNING: pattern brightness varies {spread * 100:.0f}% across the sweep - auto")
            log("  exposure is pumping between patterns. Pin the exposure (see above); the drift")
            log("  gain correction cannot fix per-pattern level changes.")

    # drift + brightness estimation from the white checkpoints
    w0 = np.float32(caps[i_white0])
    rough = w0 > max(30.0, float(w0.max()) * 0.4)
    med0 = float(np.median(w0[rough])) if rough.any() else 1.0
    ck_i, ck_dx, ck_dy, ck_g = [], [], [], []
    for i in white_idx:
        wi = np.float32(caps[i])
        try:
            (dx, dy), _ = cv2.phaseCorrelate(w0, wi)
        except Exception:
            dx = dy = 0.0
        ck_i.append(i)
        ck_dx.append(dx)
        ck_dy.append(dy)
        medi = float(np.median(wi[rough])) if rough.any() else med0
        ck_g.append(med0 / max(medi, 1e-6))
    drift_path = max(math.hypot(a, b) for a, b in zip(ck_dx, ck_dy))
    log(f"  drift compensation: {len(white_idx)} white checkpoints, image path up to "
        f"{drift_path:.2f} cam px, brightness ratio down to {1.0 / max(ck_g):.3f} - corrected")

    def frame(i):
        f = np.float32(caps[i])
        g_ = np.interp(i, ck_i, ck_g)
        if abs(g_ - 1.0) > 0.005:
            f = f * g_
        dx = np.interp(i, ck_i, ck_dx)
        dy = np.interp(i, ck_i, ck_dy)
        if abs(dx) > 0.05 or abs(dy) > 0.05:
            M = np.float32([[1, 0, -dx], [0, 1, -dy]])
            f = cv2.warpAffine(f, M, (f.shape[1], f.shape[0]), flags=cv2.INTER_LINEAR,
                               borderMode=cv2.BORDER_REPLICATE)
        return f

    # decode phase, on compensated frames
    black = frame(i_black)
    white = frame(i_white0)
    contrast = white - black
    valid = contrast > contrast_min

    def dump(name, img):
        if dump_dir:
            os.makedirs(dump_dir, exist_ok=True)
            cv2.imwrite(os.path.join(dump_dir, name), img)
    dump("00_black.png", np.clip(black, 0, 255).astype(np.uint8))
    dump("01_white.png", np.clip(white, 0, 255).astype(np.uint8))
    dump("02_white_saturation.png", ((caps[i_white0] >= 254) * 255).astype(np.uint8))
    dump("03_contrast.png", np.clip(contrast / max(float(np.median(contrast[valid])) if valid.any() else 1, 1) * 128, 0, 255).astype(np.uint8))
    log(f"  contrast: median {np.median(contrast[valid]) if valid.any() else 0:.1f} codes, "
        f"{valid.mean() * 100:.1f}% of camera pixels see the panel")
    if valid.mean() < 0.02:
        raise RuntimeError("the camera barely sees the panel (check blackout, eye selection, mount, exposure)")

    planes = {0: [], 1: []}
    conf_planes = {0: [], 1: []}
    for axis, bit, ip, ineg in plan:
        diff = frame(ip) - frame(ineg)
        planes[axis].append(diff > 0)
        conf_planes[axis].append(np.abs(diff) / np.maximum(contrast, 1.0))
        if dump_dir:
            # sign+magnitude at half res: mid grey = no signal, black/white =
            # confident negative/positive. Stale frames show as a ghost of the
            # PREVIOUS pattern's stripes; saturation as flat grey patches on
            # the lit panel; reflections as coded stripes outside the panel.
            vis = np.clip(128 + diff / np.maximum(contrast, 1.0) * 127, 0, 255).astype(np.uint8)
            dump(f"10_diff_{'uv'[axis]}_bit{bit:02d}.png", cv2.resize(vis, None, fx=0.5, fy=0.5))
        log(f"  axis {'u' if axis == 0 else 'v'} bit {bit:2d}: median confidence "
            f"{np.median(np.abs(diff)[valid]) / max(np.median(contrast[valid]), 1):.2f}")

    result = {"black": black, "white": white, "contrast": contrast,
              "drift_cam_px": (float(ck_dx[-1]), float(ck_dy[-1])),
              "drift_path_cam_px": float(drift_path),
              "contrast_ratio": float(1.0 / max(ck_g)),
              "drift_compensated": True}
    codes = {}
    for axis in (0, 1):
        usable = 0
        for bit in range(bits):
            med = np.median(conf_planes[axis][bit][valid])
            if med < bit_confidence:
                break
            usable = bit + 1
        if usable < 3:
            raise RuntimeError(f"axis {axis}: only {usable} confident bits, the pattern is not readable "
                               "(exposure? focus? camera too far?)")
        kept = planes[axis][:usable]
        conf_ok = np.ones_like(valid)
        for bit in range(usable):
            conf_ok &= conf_planes[axis][bit] > bit_confidence * 0.5
        valid &= conf_ok
        code = decode_gray(kept)
        codes[axis] = (code.astype(np.float64) + 0.5) / float(1 << usable)
        result[f"usable_bits_{'uv'[axis]}"] = usable
        log(f"  axis {'u' if axis == 0 else 'v'}: {usable} usable bits ({1 << usable} cells)")

    if dump_dir:
        vis = np.zeros(valid.shape + (3,), np.uint8)
        vis[..., 2] = np.clip(codes[0] * 255, 0, 255).astype(np.uint8)   # u -> red
        vis[..., 1] = np.clip(codes[1] * 255, 0, 255).astype(np.uint8)   # v -> green
        vis[~valid] = 0
        dump("20_decoded_uv.png", vis)
        dump("21_valid.png", (valid * 255).astype(np.uint8))
        log(f"  debug dump written to {dump_dir}/")

    ys, xs = np.nonzero(valid)
    uv = np.stack([codes[0][ys, xs], codes[1][ys, xs]], axis=1)
    pix = np.stack([xs.astype(np.float64), ys.astype(np.float64)], axis=1)
    result.update({"uv": uv, "pix": pix, "valid": valid})
    log(f"  {len(uv)} valid samples")
    return result


def fit_from_sweep(sweep, cam, panel, lattice, eye, smooth, prior, verbose=True):
    """Solve rotation, fit the lattice for one eye. Returns a report dict."""
    log = (lambda *a: print(*a, flush=True)) if verbose else (lambda *a: None)
    uv = sweep["uv"]
    dirs = cam.pixels_to_dirs(sweep["pix"])
    R, res_rot = g.solve_rotation(panel, uv, dirs)
    yaw, pitch, roll = g.matrix_to_ypr_deg(R)
    log(f"  camera rotation vs panel axis: yaw {yaw:+.2f} deg, pitch {pitch:+.2f} deg, roll {roll:+.2f} deg")
    # displacement samples: content rendered for P(uv_s) must show at F(uv)
    back = (R.T @ dirs.T).T
    uv_true = panel.dirs_to_uv(back)
    disp = uv_true - uv
    # samples decoded outside the square are impossible, guard anyway
    ok = np.all((uv_true > -0.05) & (uv_true < 1.05), axis=1)
    uv, disp = uv[ok], disp[ok]
    # weight by local sample density inverse would be nicer; the camera's
    # pixel density over the panel is fairly uniform through the fisheye so
    # plain least squares is fine
    res = lattice.fit(eye, uv, disp, smooth=smooth, prior=prior,
                      taper=getattr(fit_from_sweep, "taper", 3.0),
                      fade=getattr(fit_from_sweep, "fade", 1.0),
                      edge=getattr(fit_from_sweep, "edge", "fade"))
    # robust loop: a least squares fit is dragged hard by gross outliers
    # (reflections that decode the panel's code from the wrong place, stale
    # or clipped pixels that slipped the gates) - and after a poisoned fit
    # the residuals themselves are poisoned, so one rejection pass is not
    # enough. Iterate: reject against the current fit's residual scale,
    # refit, repeat while it keeps finding outliers. Honest data loses
    # nothing (the threshold floors at 3 panel px above 6x the median).
    total_dropped = 0.0
    for _round in range(4):
        rmag = np.linalg.norm(res, axis=1)
        thresh = max(6.0 * float(np.median(rmag)), 3.0 / max(sweep["tex_size"][0], 1))
        keep = rmag < thresh
        newly = float((~keep).mean())
        if newly <= 0.001:
            break
        total_dropped = 1.0 - (1.0 - total_dropped) * (1.0 - newly)
        uv, disp = uv[keep], disp[keep]
        res = lattice.fit(eye, uv, disp, smooth=smooth, prior=prior,
                          taper=getattr(fit_from_sweep, "taper", 3.0),
                          fade=getattr(fit_from_sweep, "fade", 1.0),
                          edge=getattr(fit_from_sweep, "edge", "fade"))
    # speckle check on the measured mask: real coverage is contiguous, a
    # salt-and-pepper mask means the decode was noise. Isolated measured
    # knots are demoted to free so a later overlay session is not fighting
    # a speckled brush mask.
    m = lattice.measured[eye]
    if m is not None and m.any():
        nb = np.zeros(m.shape, int)
        nb[1:, :] += m[:-1, :]; nb[:-1, :] += m[1:, :]
        nb[:, 1:] += m[:, :-1]; nb[:, :-1] += m[:, 1:]
        isolated = m & (nb <= 1)
        if isolated.any():
            lattice.measured[eye] = m & ~isolated
            log(f"  demoted {int(isolated.sum())} isolated measured knots to free (speckle)")
    if total_dropped > 0.001:
        log(f"  robust refit: dropped {total_dropped * 100:.1f}% of samples as outliers")
        if total_dropped > 0.20:
            log("  WARNING: over 20% outliers - the sweep data is inconsistent (exposure, "
                "reflections, stale frames?). Rerun with --debug-dump and look at the images.")
    info = getattr(lattice, "last_fit_info", {})
    if info:
        log(f"  lattice: {info['knots_measured']}/{info['knots_total']} knots measured "
            f"({info['measured_fraction'] * 100:.0f}%), the rest fade to identity over {info['taper_knots']:.0f} knots")
    tex_w, tex_h = sweep.get("tex_size", (1, 1))
    rms_uv = math.sqrt((res ** 2).sum(axis=1).mean())
    max_uv = float(np.linalg.norm(res, axis=1).max())
    # residual in visual angle: uv -> tan scale
    tan_per_u = (panel.r - panel.l)
    tan_per_v = (panel.b - panel.t)
    rms_deg = math.degrees(math.sqrt(((res[:, 0] * tan_per_u) ** 2 + (res[:, 1] * tan_per_v) ** 2).mean()))
    dmax, drms = lattice.stats_measured(eye)
    cov_u = (uv[:, 0].min(), uv[:, 0].max())
    cov_v = (uv[:, 1].min(), uv[:, 1].max())
    log(f"  correction: max |disp| {dmax:.5f} uv ({dmax * tex_w:.1f} px), rms {drms:.5f} uv")
    log(f"  fit residual: rms {rms_uv:.5f} uv ({rms_deg:.3f} deg), max {max_uv:.5f} uv")
    log(f"  camera covers u [{cov_u[0]:.2f}, {cov_u[1]:.2f}] v [{cov_v[0]:.2f}, {cov_v[1]:.2f}] of the eye texture")
    return {
        "rotation_ypr_deg": [yaw, pitch, roll],
        "rotvec": g.matrix_to_rotvec(R).tolist(),
        "fit_rms_uv": rms_uv,
        "fit_rms_deg": rms_deg,
        "fit_max_uv": max_uv,
        "disp_max_uv": dmax,
        "disp_rms_uv": drms,
        "coverage_u": [float(cov_u[0]), float(cov_u[1])],
        "coverage_v": [float(cov_v[0]), float(cov_v[1])],
        "samples": int(len(uv)),
    }


def alignment_hint(yaw, pitch, roll, tol=1.0):
    hints = []
    if abs(yaw) > tol:
        hints.append(f"turn the camera {'left' if yaw > 0 else 'right'} by ~{abs(yaw):.1f} deg (yaw)")
    if abs(pitch) > tol:
        hints.append(f"tilt the camera {'up' if pitch > 0 else 'down'} by ~{abs(pitch):.1f} deg (pitch)")
    if abs(roll) > tol * 2:
        hints.append(f"roll the camera {'clockwise' if roll > 0 else 'counter-clockwise'} by ~{abs(roll):.1f} deg")
    if not hints:
        return "rotation is within tolerance; the solve removes the rest"
    return "; ".join(hints) + " (rotation is solved out anyway, but a small angle keeps the field of view centered)"


def compare_raws(args):
    """Refit two raw sweeps (saved with --save-raw, ideally taken with the
    camera at two different rotations) using the CURRENT calibration, and
    report the disagreement of the two maps. The headset's field is the
    same in both; the per-fit rotation solve removes the pose; so on the
    jointly covered region the two maps must agree, and their difference is
    the camera model error carried into the panel. Runs without the driver
    or the camera. Re-run after every recalibration - this is the
    acceptance test for the camera model."""
    cam = g.CameraModel.load()
    lats, covers, tex = [], [], None
    for path in args.compare:
        raw = np.load(path)
        proj = raw["proj"]
        tex = tuple(int(x) for x in raw["tex"])
        panel = g.PanelModel(proj)
        lat = g.DisplacementLattice(args.cols, args.rows)
        fit_from_sweep.taper = args.taper
        fit_from_sweep.edge = args.edge
        print(f"{path}: {len(raw['uv'])} samples")
        fit_from_sweep({"uv": raw["uv"], "pix": raw["pix"], "tex_size": tex},
                       cam, panel, lat, 0, args.smooth, args.prior)
        lats.append(lat)
        covers.append(lat.measured[0])
    both = covers[0] & covers[1]
    if not both.any():
        print("the two sweeps share no covered knots; same eye and mount position?")
        return 1
    uvk = lats[0].knot_uv().reshape(args.rows, args.cols, 2)[both]
    d = (lats[0].eyes[0] - lats[1].eyes[0])[both]
    # a residual 3-parameter gauge (shift + roll) can survive if the two
    # rotation solves anchored slightly differently; remove it like the
    # overlay does before reporting
    u = uvk[:, 0] - 0.5
    v = uvk[:, 1] - 0.5
    A = np.zeros((len(d) * 2, 3))
    A[0::2, 0] = 1; A[0::2, 2] = -v
    A[1::2, 1] = 1; A[1::2, 2] = u
    gpar, *_ = np.linalg.lstsq(A, d.reshape(-1), rcond=None)
    d[:, 0] -= gpar[0] - gpar[2] * v
    d[:, 1] -= gpar[1] + gpar[2] * u
    mag_px = np.linalg.norm(d, axis=1) * tex[0]
    r = np.linalg.norm(uvk - 0.5, axis=1) / math.sqrt(0.5)
    print(f"\nmap disagreement over {both.sum()} shared knots (gauge removed):")
    print(f"  median {np.median(mag_px):.2f} px, p95 {np.quantile(mag_px, 0.95):.2f} px, "
          f"max {mag_px.max():.2f} px")
    for a, b, name in ((0.0, 0.4, "center"), (0.4, 0.75, "mid"), (0.75, 1.01, "edge")):
        m = (r >= a) & (r < b)
        if m.any():
            print(f"  {name:6s} (r {a:.2f}-{b:.2f}): median {np.median(mag_px[m]):.2f} px, "
                  f"p95 {np.quantile(mag_px[m], 0.95):.2f} px  ({m.sum()} knots)")
    print("\nread: this is the camera-model error budget of your maps. A few px at the")
    print("edge band = calibration is good enough; tens of px = recalibrate (coverage!).")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eye", type=eye_index, help="left or right (required except with --compare)")
    ap.add_argument("--bits", type=int, default=10, help="Gray-code bits per axis (1..12), default 10")
    ap.add_argument("--cols", type=int, default=33, help="lattice columns (default 33)")
    ap.add_argument("--rows", type=int, default=33, help="lattice rows (default 33)")
    ap.add_argument("--smooth", type=float, default=0.05, help="lattice smoothness weight (default 0.05)")
    ap.add_argument("--prior", type=float, default=1e-3, help="pull toward identity inside the measured region")
    ap.add_argument("--taper", type=float, default=3.0,
                    help="knots over which unmeasured areas fade to identity (default 3)")
    ap.add_argument("--edge", choices=("fade", "extend"), default="fade",
                    help="unmeasured knots: 'fade' to identity (safe default) or 'extend' the "
                         "boundary value outward (no magnification band at the coverage edge)")
    ap.add_argument("--compare", nargs=2, metavar=("A.NPZ", "B.NPZ"),
                    help="no sweep: refit two saved raw captures with the CURRENT camera "
                         "calibration and report how much the resulting maps differ. Two sweeps "
                         "with the camera rotated ~10 deg between them must produce the same map; "
                         "the difference is end-to-end camera-model error in panel pixels.")
    ap.add_argument("--frames", type=int, default=3, help="camera frames averaged per pattern")
    ap.add_argument("--settle", type=int, default=6, help="driver frames a pattern must be shown before capture")
    ap.add_argument("--debug-dump", metavar="DIR",
                    help="write decode post-mortem images: black/white refs, contrast, per-bit "
                         "difference images, the validity mask and the decoded uv as color. When a "
                         "sweep produces garbage, these show WHERE it went wrong (saturation, "
                         "stale frames, reflections, compression).")
    ap.add_argument("--align", action="store_true", help="alignment check only: 6 bit sweep, print rotation, no fit applied")
    ap.add_argument("--dry", action="store_true", help="do everything but write nothing to settings.json")
    ap.add_argument("--reset-other-eye", action="store_true", help="zero the other eye's lattice in the written map")
    ap.add_argument("--save-raw", metavar="NPZ", help="save the decoded samples for offline refits")
    ap.add_argument("--load-raw", metavar="NPZ", help="skip the camera, refit from a saved npz")
    ap.add_argument("--name", default=None, help="profile name")
    args = ap.parse_args()

    if cv2 is None:
        print("opencv-python is required: pip install -r requirements.txt")
        return 2

    if args.compare:
        return compare_raws(args)
    if args.eye is None:
        ap.error("--eye is required")

    settings = g.Settings()
    link = g.DriverLink(settings)
    eye = args.eye
    bits = 6 if args.align else max(1, min(12, args.bits))

    diag = link.require_active()
    proj = link.projection(eye)
    tex_w, tex_h, aspect = link.eye_texture()
    panel = g.PanelModel(proj)
    print(f"driver: eye texture {tex_w}x{tex_h} aspect {aspect:.4f}, proj {['%.4f' % p for p in proj]}")

    cam_cfg = g.load_camera_config()
    cam = g.CameraModel.load(cfg=cam_cfg)

    # remember and neutralise the calibration state we touch
    calib_before = settings.get_calib()
    restore = {
        "blackout": bool(calib_before.get("blackout", False)),
        "captureMode": bool(calib_before.get("captureMode", False)),
        "pattern": -1,
    }
    # stationary dimming fades the pattern during the sweep on older driver
    # builds (new ones suspend it while a pattern shows); disable it for the
    # run and restore afterwards
    sf_before = settings.stream_frame()
    dim_was_on = bool((sf_before.get("stationaryDimming", {}) or {}).get("enable", False))
    if dim_was_on:
        settings.update(lambda sf: sf.setdefault("stationaryDimming", {}).__setitem__("enable", False))
        print("stationary dimming disabled for the sweep (restored after)")
    if args.load_raw:
        raw = np.load(args.load_raw)
        sweep = {"uv": raw["uv"], "pix": raw["pix"], "tex_size": (tex_w, tex_h)}
        print(f"loaded {len(sweep['uv'])} samples from {args.load_raw}")
    else:
        cap, cam_cfg = g.open_camera(cam_cfg)
        try:
            settings.set_calib(blackout=False, captureMode=False)
            show, grab, done = make_driver_io(cap, link, eye, bits, args.frames, args.settle)
            sweep = run_sweep(show, grab, done, eye, bits, dump_dir=args.debug_dump)
            sweep["tex_size"] = (tex_w, tex_h)
        finally:
            settings.set_calib(**restore)
            if dim_was_on:
                settings.update(lambda sf: sf.setdefault("stationaryDimming", {}).__setitem__("enable", True))
            cap.release()
        if args.save_raw:
            np.savez_compressed(args.save_raw, uv=sweep["uv"], pix=sweep["pix"], proj=np.array(proj),
                                tex=np.array([tex_w, tex_h]))
            print(f"raw samples saved to {args.save_raw}")

    # lattice: start from the live one so the other eye is preserved
    existing = g.DisplacementLattice.from_json(settings.get_map())
    if existing and existing.cols == args.cols and existing.rows == args.rows and not args.reset_other_eye:
        lattice = existing.copy()
    else:
        lattice = g.DisplacementLattice(args.cols, args.rows)
        if existing and not args.reset_other_eye:
            # resample the other eye onto the new lattice size
            other = 1 - eye
            lattice.eyes[other] = existing.evaluate(other, lattice.knot_uv()).reshape(args.rows, args.cols, 2)
            if existing.measured[other] is not None:
                m = existing.measured[other].astype(np.float64)
                lattice.measured[other] = cv2.resize(m, (args.cols, args.rows),
                                                     interpolation=cv2.INTER_NEAREST) > 0.5
    lattice.source = "graycode"

    # virgin panel enforcement
    # the sweep pattern bypasses the driver's warp, so the map it produces
    # is the TOTAL correction. The shader composes the map ON TOP of the
    # radial curves and alignment shifts: if those are non-identity when the
    # map is applied, the radial part gets corrected twice. So: back the
    # current radial tune up as a profile, then zero it live, write neutral
    # profiles, and pin gain to 1 (the map is absolute displacement).
    sf_now = settings.stream_frame()
    issues = g.radial_neutrality(sf_now)
    if issues and not (args.dry or args.align):
        print("live radial/alignment is not identity: " + "; ".join(issues))
        backup = g.save_profile(
            g.build_profile(sf_now, g.DisplacementLattice.from_json(settings.get_map())
                            or g.DisplacementLattice(args.cols, args.rows),
                            f"Pre-sweep radial backup {time.strftime('%Y-%m-%d %H:%M')}"),
            stem="gxr-radial-backup")
        g.neutralize_radial(settings)
        print(f"  backed up to {backup} and zeroed (a camera map is the TOTAL correction;")
        print("  radial curves underneath it would be applied twice).")
    if not (args.dry or args.align) and abs(settings.get_gain() - 1.0) > 1e-6:
        settings.set_gain(1.0)
        print("distortion gain set to 1.0 (the sweep measures absolute displacement)")

    fit_from_sweep.taper = args.taper
    fit_from_sweep.edge = args.edge
    if "drift_cam_px" in sweep and len(sweep.get("uv", [])):
        du = sweep["uv"][:, 0].max() - sweep["uv"][:, 0].min()
        dxs = sweep["pix"][:, 0].max() - sweep["pix"][:, 0].min()
        dv = sweep["uv"][:, 1].max() - sweep["uv"][:, 1].min()
        dys = sweep["pix"][:, 1].max() - sweep["pix"][:, 1].min()
        sx = du / max(dxs, 1) * tex_w
        sy = dv / max(dys, 1) * tex_h
        path_px = sweep.get("drift_path_cam_px", 0.0) * max(sx, sy)
        print(f"stability: image drifted up to {path_px:.2f} panel px during the sweep "
              f"(pose drift; compensated in the decode), brightness ratio {sweep['contrast_ratio']:.3f}"
              + ("  <-- large: check the mount and disable stationary dimming" if path_px > 8.0 else ""))
    report = fit_from_sweep(sweep, cam, panel, lattice, eye, args.smooth, args.prior)
    yaw, pitch, roll = report["rotation_ypr_deg"]
    print("alignment: " + alignment_hint(yaw, pitch, roll))

    if args.align:
        print("alignment run only, nothing written.")
        return 0
    if args.dry:
        print("dry run, nothing written. Re-run without --dry to apply.")
        return 0

    settings.set_map(lattice.to_json())
    sf = settings.stream_frame()
    name = args.name or f"Camera fit {'L' if eye == 0 else 'R'} {time.strftime('%Y-%m-%d %H:%M')}"
    meta = {"source": "graycode", "eye": "left" if eye == 0 else "right", "bits": bits,
            "camera": {"width": cam_cfg["width"], "height": cam_cfg["height"]},
            "report": report, "proj": proj, "eyeTexture": [tex_w, tex_h]}
    path = g.save_profile(g.build_profile(sf, lattice, name, meta, neutral=True), stem="gxr-map")
    print(f"map applied live (gain {settings.get_gain():.2f}) and saved as {path}")
    print("verify with: python gxr_overlay.py --eye " + ("left" if eye == 0 else "right"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
