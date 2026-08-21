# Adapted from https://github.com/sboys3/camera-calibration (MIT, see
# LICENSE-sboys3-camera-calibration). Changes: the image size is stored in
# calibration_data.pkl (the tools refuse a calibration made at another
# resolution than the camera delivers), and the calibration file path follows
# gxr_camera.json. Additionally: --fisheye calibrates with cv2.fisheye
# (equidistant base + k1..k4), the right model for 150+ degree lenses where
# the rational/pinhole model biases the edges; it stores model="fisheye" in
# the pkl and the tools branch on it automatically. The fisheye solver is
# fragile about degenerate board poses, so images that break it
# (CALIB_CHECK_COND) are dropped automatically with a message.
import numpy as np
import cv2
import glob
import os
import pickle
import gxr_common as g
import sys

FISHEYE = "--fisheye" in sys.argv

# Camera calibration parameters
# You can modify these variables as needed
CHESSBOARD_SIZE = (10, 7)  # Number of inner corners per chessboard row and column
SQUARE_SIZE = 2.5         # Size of a square in centimeters
CALIBRATION_IMAGES_PATH = 'calibration_images/*.jpg'  # Path to calibration images
OUTPUT_DIRECTORY = 'output'  # Directory to save calibration results
SAVE_UNDISTORTED = True   # Whether to save undistorted images

def calibrate_camera():
    """
    Calibrate the camera using chessboard images.
    
    Returns:
        ret: The RMS re-projection error
        mtx: Camera matrix
        dist: Distortion coefficients
        rvecs: Rotation vectors
        tvecs: Translation vectors
    """
    # Prepare object points (0,0,0), (1,0,0), (2,0,0) ... (8,5,0)
    objp = np.zeros((CHESSBOARD_SIZE[0] * CHESSBOARD_SIZE[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0:CHESSBOARD_SIZE[0], 0:CHESSBOARD_SIZE[1]].T.reshape(-1, 2)
    
    # Scale object points by square size (for real-world measurements)
    objp = objp * SQUARE_SIZE
    
    # Arrays to store object points and image points from all images
    objpoints = []  # 3D points in real world space
    imgpoints = []  # 2D points in image plane
    used_images = []  # filenames matching objpoints entries
    
    # Get list of calibration images
    images = glob.glob(CALIBRATION_IMAGES_PATH)
    
    if not images:
        print(f"No calibration images found at {CALIBRATION_IMAGES_PATH}")
        return None, None, None, None, None
    
    # Create output directory if it doesn't exist
    if not os.path.exists(OUTPUT_DIRECTORY):
        os.makedirs(OUTPUT_DIRECTORY)
    
    print(f"Found {len(images)} calibration images")
    
    # Process each calibration image
    for idx, fname in enumerate(images):
        img = cv2.imread(fname)
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        
        # Find the chessboard corners
        ret, corners = cv2.findChessboardCorners(gray, CHESSBOARD_SIZE, None)
        
        # If found, add object points and image points
        if ret:
            objpoints.append(objp)
            
            # Refine corner positions
            criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
            corners2 = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
            imgpoints.append(corners2)
            used_images.append(fname)
            
            # Draw and display the corners
            cv2.drawChessboardCorners(img, CHESSBOARD_SIZE, corners2, ret)
            
            # Save image with corners drawn
            output_img_path = os.path.join(OUTPUT_DIRECTORY, f'corners_{os.path.basename(fname)}')
            cv2.imwrite(output_img_path, img)
            
            print(f"Processed image {idx+1}/{len(images)}: {fname} - Chessboard found")
        else:
            print(f"Processed image {idx+1}/{len(images)}: {fname} - Chessboard NOT found")
    
    if not objpoints:
        print("No chessboard patterns were detected in any images.")
        return None, None, None, None, None
    
    print(f"Calibrating camera ({'fisheye/equidistant' if FISHEYE else 'rational'} model)...")
    
    if FISHEYE:
        # cv2.fisheye wants (N,1,3)/(N,1,2) float64 and refuses degenerate
        # poses with an "ill-conditioned" error naming the image index;
        # drop the offender and retry until it converges
        # OpenCV 4.x wants point arrays as (N,1,d), OpenCV 5.x as (1,N,d)
        # (anything else dies inside with "Sizes of input arguments do not
        # match"); probe which convention this build accepts
        shape41 = lambda a, d: np.asarray(a, np.float64).reshape(-1, 1, d)
        shape15 = lambda a, d: np.asarray(a, np.float64).reshape(1, -1, d)
        reshape = shape15 if int(cv2.__version__.split(".")[0]) >= 5 else shape41
        convention_flipped = False
        objp_f = [reshape(o, 3) for o in objpoints]
        imgp_f = [reshape(i, 2) for i in imgpoints]
        names = [os.path.basename(f) for f in used_images]
        # the fisheye flag constants moved between OpenCV builds (cv2.fisheye
        # vs cv2 top level vs missing from the Python bindings entirely);
        # the numeric values are fixed in fisheye.hpp, so fall back to them
        def fisheye_flag(name, value):
            return getattr(cv2.fisheye, name, getattr(cv2, "fisheye_" + name, getattr(cv2, name, value)))
        flags = (fisheye_flag("CALIB_RECOMPUTE_EXTRINSIC", 1 << 1)
                 | fisheye_flag("CALIB_FIX_SKEW", 1 << 3)
                 | fisheye_flag("CALIB_CHECK_COND", 1 << 2))
        criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-8)
        while True:
            if len(objp_f) < 8:
                print("too few usable images left for a fisheye calibration; capture more varied shots")
                return None, None, None, None, None
            K = np.zeros((3, 3))
            D = np.zeros((4, 1))
            try:
                ret, mtx, dist, rvecs, tvecs = cv2.fisheye.calibrate(
                    objp_f, imgp_f, gray.shape[::-1], K, D, None, None, flags, criteria)
                break
            except cv2.error as e:
                msg = str(e)
                if "Sizes of input arguments do not match" in msg and not convention_flipped:
                    # wrong convention guess for this build: flip it once
                    convention_flipped = True
                    reshape = shape41 if reshape is shape15 else shape15
                    objp_f = [reshape(np.asarray(o).reshape(-1, 3), 3) for o in objp_f]
                    imgp_f = [reshape(np.asarray(i).reshape(-1, 2), 2) for i in imgp_f]
                    print("  point-array convention flipped for this OpenCV build, retrying")
                    continue
                # "CALIB_CHECK_COND - Ill-conditioned matrix for input array 12"
                bad = None
                for tok in msg.replace(")", " ").split():
                    if tok.isdigit():
                        bad = int(tok)
                if bad is None or bad >= len(objp_f):
                    print(f"fisheye calibration failed: {msg}")
                    return None, None, None, None, None
                print(f"  dropping degenerate image {names[bad]} and retrying ({len(objp_f) - 1} left)")
                for lst in (objp_f, imgp_f, names):
                    lst.pop(bad)
        objpoints, imgpoints = objp_f, imgp_f
    else:
        flags = cv2.CALIB_RATIONAL_MODEL
        ret, mtx, dist, rvecs, tvecs = cv2.calibrateCamera(
            objpoints, imgpoints, gray.shape[::-1], None, None, flags=flags
        )
    
    # Save calibration results
    calibration_data = {
        'camera_matrix': mtx,
        'distortion_coefficients': dist,
        'rotation_vectors': rvecs,
        'translation_vectors': tvecs,
        'reprojection_error': ret,
        'flags': flags,
        'model': 'fisheye' if FISHEYE else 'rational',
        'image_size': (int(gray.shape[1]), int(gray.shape[0])),
        'images_used': len(objpoints),
    }
    cfg = g.load_camera_config()
    if (int(cfg["width"]), int(cfg["height"])) != (int(gray.shape[1]), int(gray.shape[0])):
        print(f"WARNING: calibration images are {gray.shape[1]}x{gray.shape[0]} but gxr_camera.json pins "
              f"{cfg['width']}x{cfg['height']}. Recapture, or change the config, they must match.")
    
    with open(os.path.join(OUTPUT_DIRECTORY, 'calibration_data.pkl'), 'wb') as f:
        pickle.dump(calibration_data, f)
    
    # Save camera matrix and distortion coefficients as text files
    np.savetxt(os.path.join(OUTPUT_DIRECTORY, 'camera_matrix.txt'), mtx)
    np.savetxt(os.path.join(OUTPUT_DIRECTORY, 'distortion_coefficients.txt'), dist)
    
    print(f"Calibration complete! RMS re-projection error: {ret}")
    print(f"Results saved to {OUTPUT_DIRECTORY}")
    
    return ret, mtx, dist, rvecs, tvecs

def undistort_images(mtx, dist):
    """
    Undistort all calibration images using the calibration results.
    
    Args:
        mtx: Camera matrix
        dist: Distortion coefficients
    """
    if not SAVE_UNDISTORTED:
        return
    
    images = glob.glob(CALIBRATION_IMAGES_PATH)
    
    if not images:
        print(f"No images found at {CALIBRATION_IMAGES_PATH}")
        return
    
    undistorted_dir = os.path.join(OUTPUT_DIRECTORY, 'undistorted')
    if not os.path.exists(undistorted_dir):
        os.makedirs(undistorted_dir)
    
    print(f"Undistorting {len(images)} images...")
    
    for idx, fname in enumerate(images):
        img = cv2.imread(fname)
        h, w = img.shape[:2]
        
        # Refine camera matrix based on free scaling parameter
        newcameramtx, roi = cv2.getOptimalNewCameraMatrix(mtx, dist, (w, h), 1, (w, h))
        
        # Undistort image
        dst = cv2.undistort(img, mtx, dist, None, newcameramtx)
        
        # Crop the image (optional)
        x, y, w, h = roi
        dst = dst[y:y+h, x:x+w]
        
        # Save undistorted image
        output_img_path = os.path.join(undistorted_dir, f'undistorted_{os.path.basename(fname)}')
        cv2.imwrite(output_img_path, dst)
        
        print(f"Undistorted image {idx+1}/{len(images)}: {fname}")
    
    print(f"Undistorted images saved to {undistorted_dir}")

def calculate_reprojection_error(objpoints, imgpoints, mtx, dist, rvecs, tvecs):
    """
    Calculate the reprojection error for each calibration image.
    
    Args:
        objpoints: 3D points in real world space
        imgpoints: 2D points in image plane
        mtx: Camera matrix
        dist: Distortion coefficients
        rvecs: Rotation vectors
        tvecs: Translation vectors
    
    Returns:
        mean_error: Mean reprojection error
    """
    total_error = 0
    for i in range(len(objpoints)):
        imgpoints2, _ = cv2.projectPoints(objpoints[i], rvecs[i], tvecs[i], mtx, dist)
        error = cv2.norm(imgpoints[i], imgpoints2, cv2.NORM_L2) / len(imgpoints2)
        total_error += error
        print(f"Reprojection error for image {i+1}: {error}")
    
    mean_error = total_error / len(objpoints)
    print(f"Mean reprojection error: {mean_error}")
    
    return mean_error

def main():
    """
    Main function to run the camera calibration process.
    """
    print("Starting camera calibration...")
    
    # Calibrate camera
    ret, mtx, dist, rvecs, tvecs = calibrate_camera()
    
    if mtx is None:
        print("Calibration failed. Exiting.")
        return
    
    # Undistort images
    undistort_images(mtx, dist)
    
    print("Camera calibration completed successfully!")

if __name__ == "__main__":
    main()