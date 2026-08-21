"""
make_charuco_board.py
generate a ChArUco calibration board sized for a TV.

Why ChArUco instead of the plain chessboard (board.svg): the chessboard
detector needs the WHOLE board visible, so on a fisheye it never yields
corners near the image rim - exactly where the camera model must be right
for the panel edges. ChArUco markers identify each corner individually, so
a board half out of frame still contributes corners, and the rim becomes
calibratable.

The board is emitted as a PNG at the TV's exact native resolution, to be
shown fullscreen at 1:1. The physical square size then follows from the
TV's pixel pitch, which is what gives the calibration its scale.

Usage:
    python make_charuco_board.py --tv-diag 55 --tv-res 3840x2160
    python make_charuco_board.py --tv-diag 55 --tv-res 1920x1080

Writes charuco_board.png (display this) and charuco_board.json (the board
spec; charuco_capture.py and charuco_calibrate.py read it).

TV setup - all of these matter for the mm scale and detection:
  - show the PNG fullscreen with NO scaling (1:1 pixel mapping). A photo
    viewer's "actual size" fullscreen, or a browser at exactly 100% zoom.
  - set the TV to PC / Just Scan / 1:1 mode. Overscan silently rescales
    the image and breaks the square size.
  - disable motion smoothing / dynamic contrast / eco dimming; fix the
    backlight at a constant level.
  - matte-screen TVs are easier; on glossy panels angle the rig so the
    camera does not see its own reflection.
"""

import argparse
import json
import math
import os
import sys

import numpy as np
import cv2

DICT_NAME = "DICT_5X5_1000"


def get_dictionary(name=DICT_NAME):
    d = getattr(cv2.aruco, name)
    if hasattr(cv2.aruco, "getPredefinedDictionary"):
        return cv2.aruco.getPredefinedDictionary(d)
    return cv2.aruco.Dictionary_get(d)


def make_board(squares_x, squares_y, square_len, marker_len, dictionary):
    """Board object across the OpenCV aruco API generations."""
    if hasattr(cv2.aruco, "CharucoBoard") and not hasattr(cv2.aruco, "CharucoBoard_create"):
        return cv2.aruco.CharucoBoard((squares_x, squares_y), square_len, marker_len, dictionary)
    try:
        return cv2.aruco.CharucoBoard((squares_x, squares_y), square_len, marker_len, dictionary)
    except Exception:
        return cv2.aruco.CharucoBoard_create(squares_x, squares_y, square_len, marker_len, dictionary)


def board_image(board, size_px):
    if hasattr(board, "generateImage"):
        return board.generateImage(size_px, marginSize=0, borderBits=1)
    return board.draw(size_px, marginSize=0, borderBits=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tv-diag", type=float, required=True, help="TV diagonal in inches (e.g. 55)")
    ap.add_argument("--tv-res", default="3840x2160", help="TV native resolution WxH (default 3840x2160)")
    ap.add_argument("--squares", default="14x8", help="board squares columns x rows (default 14x8)")
    ap.add_argument("--marker-ratio", type=float, default=0.75, help="marker side / square side (default 0.75)")
    ap.add_argument("--margin-px", type=int, default=60, help="white margin around the board in TV pixels")
    ap.add_argument("--out", default="charuco_board", help="output stem (default charuco_board)")
    args = ap.parse_args()

    W, H = (int(x) for x in args.tv_res.lower().split("x"))
    sx, sy = (int(x) for x in args.squares.lower().split("x"))
    px_per_mm = math.hypot(W, H) / (args.tv_diag * 25.4)

    square_px = int(min((W - 2 * args.margin_px) / sx, (H - 2 * args.margin_px) / sy))
    square_mm = square_px / px_per_mm
    marker_mm = square_mm * args.marker_ratio

    dictionary = get_dictionary()
    # board geometry in mm; the image is rendered separately at exact pixels
    board = make_board(sx, sy, square_mm, marker_mm, dictionary)
    bw, bh = sx * square_px, sy * square_px
    img = board_image(board, (bw, bh))

    canvas = np.full((H, W), 255, np.uint8)
    x0, y0 = (W - bw) // 2, (H - bh) // 2
    canvas[y0:y0 + bh, x0:x0 + bw] = img

    png = args.out + ".png"
    meta = args.out + ".json"
    cv2.imwrite(png, canvas)
    with open(meta, "w") as f:
        json.dump({
            "type": "charuco",
            "dictionary": DICT_NAME,
            "squares_x": sx, "squares_y": sy,
            "square_mm": round(square_mm, 4),
            "marker_mm": round(marker_mm, 4),
            "square_px": square_px,
            "tv_res": [W, H], "tv_diag_in": args.tv_diag,
            "png": os.path.basename(png),
        }, f, indent=2)

    print(f"board: {sx}x{sy} squares, {square_px} px = {square_mm:.2f} mm per square "
          f"({px_per_mm:.3f} px/mm on this TV)")
    print(f"wrote {png} ({W}x{H}) and {meta}")
    print("display the PNG fullscreen at 1:1 (100% zoom), TV in PC / Just Scan mode,")
    print("motion smoothing and dynamic dimming OFF. See the header of this file.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
