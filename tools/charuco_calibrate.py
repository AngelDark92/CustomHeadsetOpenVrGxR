"""
charuco_calibrate.py
fisheye (and rational) calibration from ChArUco
captures, with the honesty reports the old script lacked.

Differences from camera_calibration.py:
  - partial boards count: corners are identified individually, so shots
    with the board half out of frame constrain the image rim.
  - no CALIB_CHECK_COND: instead of OpenCV silently discarding the most
    oblique (= most valuable) views as "ill-conditioned", both models are
    fitted with an explicit outlier loop that drops whole images by their
    reprojection RMS and says which and why.
  - coverage report: corner count and residual binned by field angle, and
    a hard warning where the model is extrapolating. The overall RMS of a
    calibration says NOTHING about the rim if no corners reached it.
  - both models are fitted and their disagreement is printed by angle;
    where they diverge, at least one of them is wrong there.

Usage:
    python charuco_calibrate.py                 # fisheye stored (default)
    python charuco_calibrate.py --store rational

Writes tools/output/calibration_data.pkl in the same format as before, so
gxr_sweep.py / gxr_overlay.py need no changes.
"""

import argparse
import glob
import json
import math
import os
import pickle
import sys

import numpy as np
import cv2

from make_charuco_board import get_dictionary, make_board
from charuco_capture import make_detector, OUT_DIR

OUTPUT_DIRECTORY = "output"


def detect_all(images, board, dictionary, min_corners=12, verbose=True):
    """-> list of (name, obj (N,3), img (N,2)) per usable image."""
    detect = make_detector(board, dictionary)
    all_obj = board.getChessboardCorners() if hasattr(board, "getChessboardCorners") else board.chessboardCorners
    all_obj = np.asarray(all_obj, np.float64).reshape(-1, 3)
    out = []
    size = None
    for fname in images:
        img = cv2.imread(fname)
        if img is None:
            continue
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        size = (gray.shape[1], gray.shape[0])
        corners, ids = detect(gray)
        n = 0 if corners is None else len(corners)
        if n < min_corners:
            if verbose:
                print(f"  {os.path.basename(fname)}: {n} corners, skipped")
            continue
        # reject views where the corners are nearly collinear (degenerate pose)
        c = corners - corners.mean(axis=0)
        s = np.linalg.svd(c, compute_uv=False)
        if s[1] < 3.0:
            if verbose:
                print(f"  {os.path.basename(fname)}: corners nearly collinear, skipped")
            continue
        out.append((os.path.basename(fname), all_obj[ids].copy(), corners.astype(np.float64)))
        if verbose:
            print(f"  {os.path.basename(fname)}: {n} corners")
    return out, size


def _cv_flag(name, default=0):
    """OpenCV moved the fisheye calibration flags from cv2.fisheye.CALIB_*
    (4.x) to top-level cv2.CALIB_* (5.x) - with DIFFERENT numeric values
    (5.x unified the enum), so the lookup must be by name, never numeric.
    Missing flag -> 0: the calibration still runs, just without that
    refinement."""
    for mod in (cv2.fisheye, cv2):
        if hasattr(mod, name):
            return getattr(mod, name)
    return default


def _per_image_rms_fisheye(K, D, rvecs, tvecs, det):
    rms = []
    for (name, obj, img), rv, tv in zip(det, rvecs, tvecs):
        proj, _ = cv2.fisheye.projectPoints(obj.reshape(-1, 1, 3), rv, tv, K, D)
        rms.append(float(np.sqrt(((proj.reshape(-1, 2) - img) ** 2).sum(axis=1).mean())))
    return np.array(rms)


def _per_image_rms_rational(K, D, rvecs, tvecs, det):
    rms = []
    for (name, obj, img), rv, tv in zip(det, rvecs, tvecs):
        proj, _ = cv2.projectPoints(obj, rv, tv, K, D)
        rms.append(float(np.sqrt(((proj.reshape(-1, 2) - img) ** 2).sum(axis=1).mean())))
    return np.array(rms)


def fit_with_outlier_loop(det, size, model, verbose=True, max_rounds=6):
    """Fit one model, dropping whole images whose reprojection RMS is an
    outlier (> max(2.5 * median, 1.5 px)). Returns (K, D, rms, det_used) or
    raises RuntimeError."""
    det = list(det)
    for round_i in range(max_rounds):
        if len(det) < 8:
            raise RuntimeError(f"{model}: only {len(det)} usable images left, capture more")
        if model == "fisheye":
            # (1, N, ch): the only per-view layout OpenCV 5's
            # fisheye.calibrate accepts (4.x took (N, 1, ch) too)
            objp = [o.reshape(1, -1, 3) for _, o, _ in det]
            imgp = [i.reshape(1, -1, 2) for _, _, i in det]
            flags = _cv_flag("CALIB_RECOMPUTE_EXTRINSIC") | _cv_flag("CALIB_FIX_SKEW")
            K = np.zeros((3, 3)); D = np.zeros((4, 1))
            try:
                rms, K, D, rvecs, tvecs = cv2.fisheye.calibrate(
                    objp, imgp, size, K, D, None, None, flags,
                    (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-9))
            except cv2.error as e:
                # a genuinely singular view can still abort the solver; drop
                # the image the message names, if it names one
                bad = None
                for tok in str(e).replace(")", " ").split():
                    if tok.isdigit() and int(tok) < len(det):
                        bad = int(tok)
                if bad is None:
                    raise RuntimeError(f"fisheye solver failed: {e}")
                if verbose:
                    print(f"  fisheye: solver rejected {det[bad][0]}, dropping it")
                det.pop(bad)
                continue
            per = _per_image_rms_fisheye(K, D, rvecs, tvecs, det)
        else:
            objp = [o.astype(np.float32) for _, o, _ in det]
            imgp = [i.astype(np.float32).reshape(-1, 1, 2) for _, _, i in det]
            rms, K, D, rvecs, tvecs = cv2.calibrateCamera(
                objp, imgp, size, None, None, flags=_cv_flag("CALIB_RATIONAL_MODEL"))
            per = _per_image_rms_rational(K, D, rvecs, tvecs, det)
        thresh = max(2.5 * np.median(per), 1.5)
        bad = np.nonzero(per > thresh)[0]
        if len(bad) == 0:
            if verbose:
                print(f"  {model}: {len(det)} images, RMS {rms:.3f} px "
                      f"(per-image median {np.median(per):.3f}, worst {per.max():.3f})")
            return K, D, float(rms), det
        for i in sorted(bad, reverse=True):
            if verbose:
                print(f"  {model}: dropping {det[i][0]} (image RMS {per[i]:.2f} px > {thresh:.2f})")
            det.pop(i)
    raise RuntimeError(f"{model}: outlier loop did not settle in {max_rounds} rounds")


def pixel_theta(K, D, model, pts):
    """Field angle (deg) of image points under a model."""
    pts = np.asarray(pts, np.float64).reshape(-1, 1, 2)
    if model == "fisheye":
        norm = cv2.fisheye.undistortPoints(
            pts, K, D, criteria=(cv2.TERM_CRITERIA_MAX_ITER + cv2.TERM_CRITERIA_EPS, 200, 1e-12)).reshape(-1, 2)
    else:
        norm = cv2.undistortPoints(pts, K, D).reshape(-1, 2)
    return np.degrees(np.arctan(np.linalg.norm(norm, axis=1)))


def theta_report(det, K, D, model, rvecs=None, verbose=True):
    """Corner count + residual binned by field angle. Returns max theta."""
    # residuals per corner via a fresh per-image pose solve, so the report
    # works for either model without keeping the calibrate() extrinsics
    thetas, errs = [], []
    for name, obj, img in det:
        # pose from undistorted points + identity-pinhole solvePnP works for
        # both models, so the report does not depend on calibrate() extrinsics
        if model == "fisheye":
            und = cv2.fisheye.undistortPoints(
                img.reshape(-1, 1, 2), K, D,
                criteria=(cv2.TERM_CRITERIA_MAX_ITER + cv2.TERM_CRITERIA_EPS, 200, 1e-12)).reshape(-1, 2)
        else:
            und = cv2.undistortPoints(img.reshape(-1, 1, 2), K, D).reshape(-1, 2)
        ok, rv, tv = cv2.solvePnP(obj, und.reshape(-1, 1, 2), np.eye(3), np.zeros(5))
        if not ok:
            continue
        if model == "fisheye":
            proj, _ = cv2.fisheye.projectPoints(obj.reshape(-1, 1, 3), rv, tv, K, D)
        else:
            proj, _ = cv2.projectPoints(obj, rv, tv, K, D)
        e = np.linalg.norm(proj.reshape(-1, 2) - img, axis=1)
        thetas.append(pixel_theta(K, D, model, img))
        errs.append(e)
    th = np.concatenate(thetas)
    er = np.concatenate(errs)
    edges = np.arange(0, 95, 10)
    if verbose:
        print(f"  {model}: corner coverage and residual by field angle")
        print("    theta      corners   rms px")
        for a, b in zip(edges[:-1], edges[1:]):
            m = (th >= a) & (th < b)
            if m.sum() == 0:
                print(f"    {a:2d}-{b:2d} deg   {0:7d}   -        <-- NO DATA: the model extrapolates here")
            else:
                print(f"    {a:2d}-{b:2d} deg   {m.sum():7d}   {np.sqrt((er[m] ** 2).mean()):.3f}")
    return float(th.max())


def model_disagreement(Kf, Df, Kr, Dr, size, verbose=True):
    """Angular disagreement of the two fitted models along the image radius."""
    w, h = size
    cx, cy = Kf[0, 2], Kf[1, 2]
    rmax = min(cx, cy, w - cx, h - cy) * 0.98
    if verbose:
        print("  model cross-check (fisheye vs rational unprojection; where they diverge,")
        print("  at least one is wrong there):")
        print("    image radius   theta_fisheye   disagreement")
    out = []
    for frac in (0.2, 0.4, 0.6, 0.7, 0.8, 0.9, 0.98):
        pts = np.stack([cx + np.array([1, 0, -1, 0]) * rmax * frac,
                        cy + np.array([0, 1, 0, -1]) * rmax * frac], axis=1)
        nf = cv2.fisheye.undistortPoints(pts.reshape(-1, 1, 2), Kf, Df,
                                         criteria=(cv2.TERM_CRITERIA_MAX_ITER + cv2.TERM_CRITERIA_EPS, 200, 1e-12)).reshape(-1, 2)
        nr = cv2.undistortPoints(pts.reshape(-1, 1, 2), Kr, Dr).reshape(-1, 2)
        df = np.concatenate([nf, np.ones((4, 1))], 1); df /= np.linalg.norm(df, axis=1, keepdims=True)
        dr = np.concatenate([nr, np.ones((4, 1))], 1); dr /= np.linalg.norm(dr, axis=1, keepdims=True)
        ang = np.degrees(np.arccos(np.clip((df * dr).sum(axis=1), -1, 1)))
        thf = np.degrees(np.arctan(np.linalg.norm(nf, axis=1))).mean()
        out.append((frac, thf, ang.mean()))
        if verbose:
            print(f"    {frac * 100:5.0f}%        {thf:6.1f} deg     {ang.mean():7.3f} deg")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="charuco_board.json")
    ap.add_argument("--images", default=os.path.join(OUT_DIR, "*.png"))
    ap.add_argument("--store", choices=("fisheye", "rational"), default="fisheye",
                    help="which model to write into calibration_data.pkl (default fisheye)")
    ap.add_argument("--min-corners", type=int, default=12)
    args = ap.parse_args()

    with open(args.board) as f:
        spec = json.load(f)
    dictionary = get_dictionary(spec["dictionary"])
    board = make_board(spec["squares_x"], spec["squares_y"], spec["square_mm"], spec["marker_mm"], dictionary)

    images = sorted(glob.glob(args.images))
    if not images:
        print(f"no images at {args.images}; run charuco_capture.py first")
        return 1
    print(f"detecting in {len(images)} images...")
    det, size = detect_all(images, board, dictionary, args.min_corners)
    if len(det) < 8:
        print("fewer than 8 usable images, capture more")
        return 1
    total = sum(len(i) for _, _, i in det)
    print(f"{len(det)} usable images, {total} corners at {size[0]}x{size[1]}")

    print("fitting fisheye (equidistant, k1..k4)...")
    Kf, Df, rms_f, det_f = fit_with_outlier_loop(det, size, "fisheye")
    print("fitting rational (for the cross-check)...")
    Kr, Dr, rms_r, det_r = fit_with_outlier_loop(det, size, "rational")

    max_th = theta_report(det_f, Kf, Df, "fisheye")
    theta_report(det_r, Kr, Dr, "rational")
    model_disagreement(Kf, Df, Kr, Dr, size)
    print(f"  corner coverage reaches {max_th:.1f} deg of field angle.")
    print("  the panel edges typically sit at 50-60 deg through the lens: everything the")
    print("  headset needs must be INSIDE the covered range, or the map inherits guesswork.")

    K, D, rms, model = (Kf, Df, rms_f, "fisheye") if args.store == "fisheye" else (Kr, Dr, rms_r, "rational")
    os.makedirs(OUTPUT_DIRECTORY, exist_ok=True)
    data = {
        "camera_matrix": K,
        "distortion_coefficients": D,
        "reprojection_error": rms,
        "model": model,
        "image_size": (int(size[0]), int(size[1])),
        "images_used": len(det_f if model == "fisheye" else det_r),
        "board": spec,
        "method": "charuco",
        "max_theta_deg": max_th,
    }
    try:
        import gxr_common as g
        cfg = g.load_camera_config()
        if (int(cfg["width"]), int(cfg["height"])) != (int(size[0]), int(size[1])):
            print(f"WARNING: images are {size[0]}x{size[1]} but gxr_camera.json pins "
                  f"{cfg['width']}x{cfg['height']}. They must match; recapture or fix the config.")
    except Exception:
        pass
    path = os.path.join(OUTPUT_DIRECTORY, "calibration_data.pkl")
    with open(path, "wb") as f:
        pickle.dump(data, f)
    np.savetxt(os.path.join(OUTPUT_DIRECTORY, "camera_matrix.txt"), K)
    np.savetxt(os.path.join(OUTPUT_DIRECTORY, "distortion_coefficients.txt"), D)
    print(f"stored the {model} model in {path} (RMS {rms:.3f} px)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
