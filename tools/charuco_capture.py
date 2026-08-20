"""
charuco_capture.py
capture ChArUco calibration images with live coverage
feedback.

Replaces capture_calibration_images.py for the ChArUco workflow. The key
addition is the coverage readout: the fisheye model is only trustworthy
where corners were actually detected, so the display accumulates every
detected corner into a heatmap and reports how far toward the image rim the
detections reach. Do not stop capturing until the outer radius bins are
green.

Usage: python charuco_capture.py            (board spec from charuco_board.json)

Keys: c capture (detects on several frames and keeps the best, so a
motion-blurred moment does not waste the shot; auto-rejected under
--min-corners), q / Esc quit.

The per-frame corner count flickering is normal - hand motion blurs frames
at ~15 fps and the detector is all-or-nothing per marker. The rolling peak
next to it is the number that matters, and captures use the best of several
frames anyway.

The radius bars span the USABLE image circle, not the frame corner: a 180
degree lens does not illuminate the corners of a 4:3 sensor, so the tool
estimates where the image actually ends from the session's brightness and
scales the bars to that. Fill those bars and the rim is covered.

Tips for reaching the rim: the board does NOT need to be fully visible -
half a board hanging off the edge of the frame still counts. Point the
camera so the TV slides toward the image border and beyond; keep enough
distance that individual markers stay reasonably undistorted locally
(strongly bent markers stop being detected - step back rather than closer
when rim detections die).
"""

import argparse
import glob
import json
import os
import sys
import time

import numpy as np
import cv2

import gxr_common as g
from make_charuco_board import get_dictionary, make_board

OUT_DIR = "charuco_images"


def make_detector(board, dictionary):
    """(detect(gray) -> (corners Nx2 or None, ids)) across aruco API versions."""
    if hasattr(cv2.aruco, "CharucoDetector"):
        det = cv2.aruco.CharucoDetector(board)

        def detect(gray):
            ch_corners, ch_ids, _, _ = det.detectBoard(gray)
            if ch_corners is None or ch_ids is None or len(ch_corners) == 0:
                return None, None
            return ch_corners.reshape(-1, 2), ch_ids.reshape(-1)
    else:
        params = cv2.aruco.DetectorParameters_create()

        def detect(gray):
            m_corners, m_ids, _ = cv2.aruco.detectMarkers(gray, dictionary, parameters=params)
            if m_ids is None or len(m_ids) == 0:
                return None, None
            n, ch_corners, ch_ids = cv2.aruco.interpolateCornersCharuco(m_corners, m_ids, gray, board)
            if ch_corners is None or n == 0:
                return None, None
            return ch_corners.reshape(-1, 2), ch_ids.reshape(-1)
    return detect


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="charuco_board.json", help="board spec from make_charuco_board.py")
    ap.add_argument("--min-corners", type=int, default=12, help="reject captures with fewer corners")
    ap.add_argument("--scale", type=float, default=0.5, help="display scale")
    args = ap.parse_args()

    with open(args.board) as f:
        spec = json.load(f)
    dictionary = get_dictionary(spec["dictionary"])
    board = make_board(spec["squares_x"], spec["squares_y"], spec["square_mm"], spec["marker_mm"], dictionary)
    detect = make_detector(board, dictionary)

    cap, cfg = g.open_camera()
    os.makedirs(OUT_DIR, exist_ok=True)
    existing = len(glob.glob(os.path.join(OUT_DIR, "*.png")))
    print(f"saving to {OUT_DIR}/ ({existing} images already there). c = capture, q = quit.")

    heat = None
    n_bins = 10
    saved = existing
    rmax = 1.0
    corner_radii = []          # px radii of every captured corner
    bright_max = None          # per-radius-bin session max brightness
    n_rad = 64
    peak = (0, 0.0)            # (count, at) rolling 1 s detection peak
    while True:
        ok, frame = cap.read()
        if not ok:
            continue
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        h, w = gray.shape
        if heat is None:
            heat = np.zeros((h // 8, w // 8), np.float32)
        corners, ids = detect(gray)

        # usable image circle from vignetting: session max brightness per
        # radius; the circle ends where it collapses (a 180 deg lens leaves
        # the 4:3 frame corners dark - corners can never be detected there)
        small = cv2.resize(gray, (w // 8, h // 8), interpolation=cv2.INTER_AREA)
        sy, sx = np.mgrid[0:small.shape[0], 0:small.shape[1]]
        rr_full = np.hypot(w / 2, h / 2)
        rs = np.hypot(sx * 8 - w / 2, sy * 8 - h / 2) / rr_full
        idx = np.minimum((rs * n_rad).astype(int), n_rad - 1)
        prof = np.zeros(n_rad)
        np.maximum.at(prof, idx.reshape(-1), small.reshape(-1).astype(float))
        bright_max = prof if bright_max is None else np.maximum(bright_max, prof)
        inner = bright_max[: n_rad // 3].max() + 1e-6
        usable = np.nonzero(bright_max > 0.25 * inner)[0]
        r_usable = (usable.max() + 1) / n_rad if len(usable) else 1.0

        view = frame.copy()
        if corners is not None:
            for x, y in corners:
                cv2.circle(view, (int(x), int(y)), 5, (0, 255, 0), 1, cv2.LINE_AA)
        # accumulated coverage as a green wash
        hm = cv2.resize((np.clip(heat, 0, 3) / 3 * 90).astype(np.uint8), (w, h))
        view[:, :, 1] = np.clip(view[:, :, 1].astype(int) + hm, 0, 255).astype(np.uint8)

        # radial coverage bars: fraction of the USABLE image circle
        cx, cy = w / 2, h / 2
        rmax = np.hypot(cx, cy) * r_usable
        radii = np.asarray(corner_radii)
        bin_hits = np.histogram(np.clip(radii / max(rmax, 1) * n_bins, 0, n_bins - 1e-6),
                                bins=n_bins, range=(0, n_bins))[0] if len(radii) else np.zeros(n_bins, int)
        bar_w = 30
        for i in range(n_bins):
            x0 = 10 + i * (bar_w + 4)
            col = (60, 200, 60) if bin_hits[i] >= 30 else ((60, 160, 220) if bin_hits[i] > 0 else (70, 70, 70))
            cv2.rectangle(view, (x0, h - 40), (x0 + bar_w, h - 15), col, -1)
            cv2.putText(view, str(bin_hits[i]), (x0 + 2, h - 22), cv2.FONT_HERSHEY_SIMPLEX, 0.45,
                        (0, 0, 0), 1, cv2.LINE_AA)
        cv2.putText(view, f"coverage by radius over the usable circle ({r_usable * 100:.0f}% of "
                    f"half-diagonal). saved: {saved}",
                    (10, h - 50), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2, cv2.LINE_AA)
        nc = 0 if corners is None else len(corners)
        now = time.time()
        if nc >= peak[0] or now - peak[1] > 1.0:
            peak = (nc, now)
        cv2.putText(view, f"corners now: {nc}  (peak 1s: {peak[0]})", (10, 40),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.9,
                    (0, 255, 0) if peak[0] >= args.min_corners else (0, 0, 255), 2, cv2.LINE_AA)

        cv2.imshow("charuco capture", cv2.resize(view, None, fx=args.scale, fy=args.scale))
        k = cv2.waitKey(1) & 0xFF
        if k in (ord('q'), 27):
            break
        if k == ord('c'):
            # best of several frames: detection is all-or-nothing per marker
            # under motion blur, so one press samples a few frames and keeps
            # the one with the most corners
            best = (0 if corners is None else len(corners), frame, corners)
            for _ in range(4):
                ok2, f2 = cap.read()
                if not ok2:
                    continue
                c2, _ = detect(cv2.cvtColor(f2, cv2.COLOR_BGR2GRAY))
                if c2 is not None and len(c2) > best[0]:
                    best = (len(c2), f2, c2)
            nbest, bframe, bcorners = best
            if nbest < args.min_corners:
                print(f"rejected: best of 5 frames had {nbest} corners (< {args.min_corners})")
                continue
            name = os.path.join(OUT_DIR, f"charuco_{int(time.time() * 1000)}.png")
            cv2.imwrite(name, bframe)
            saved += 1
            for x, y in bcorners:
                heat[min(int(y) // 8, heat.shape[0] - 1), min(int(x) // 8, heat.shape[1] - 1)] += 1
                corner_radii.append(float(np.hypot(x - cx, y - cy)))
            print(f"saved {name} ({nbest} corners)")

    cap.release()
    cv2.destroyAllWindows()
    radii = np.asarray(corner_radii)
    if len(radii):
        bin_hits = np.histogram(np.clip(radii / max(rmax, 1) * n_bins, 0, n_bins - 1e-6),
                                bins=n_bins, range=(0, n_bins))[0]
    else:
        bin_hits = np.zeros(n_bins, int)
    empty = [i for i in range(n_bins) if bin_hits[i] == 0]
    if empty:
        print(f"WARNING: radius bins {empty} (of 0..{n_bins - 1}, center to rim) got no corners this "
              "session. The model will extrapolate there. Capture more before calibrating.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
