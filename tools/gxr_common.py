"""
gxr_common.py 

shared pieces of the Galaxy XR camera calibration tools.

"""

import json
import math
import os
import re
import sys
import time
import pickle
import tempfile

import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover
    cv2 = None


# paths / files

def config_folder():
    """%APPDATA%/CustomHeadset (or ~/.config/CustomHeadset elsewhere)."""
    if os.name == "nt":
        base = os.environ.get("APPDATA")
        if not base:
            base = os.path.expanduser("~")
        return os.path.join(base, "CustomHeadset")
    return os.path.join(os.path.expanduser("~"), ".config", "CustomHeadset")


def settings_path():
    return os.path.join(config_folder(), "settings.json")


def diagnostic_path():
    return os.path.join(config_folder(), "diagnostic.json")


def distortion_folder():
    return os.path.join(config_folder(), "Distortion")


def tools_folder():
    return os.path.dirname(os.path.abspath(__file__))


def strip_json_comments(text):
    """Remove // and /* */ comments outside strings (mirrors the GUI's cleaner)."""
    out = []
    i = 0
    n = len(text)
    in_str = False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] not in "\r\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def read_json(path, default=None, retries=0, retry_delay=0.06, on_error="raise"):
    """Read JSON with comment stripping. Files that another process rewrites
    in place (the driver's diagnostic.json at 4 Hz) can legitimately be
    caught empty or half-written; `retries` re-reads through that window,
    and on_error="default" makes a persistent parse failure return `default`
    instead of raising (right for read-only polling, WRONG for settings.json,
    where treating a corrupt file as empty would clobber it on write)."""
    for attempt in range(retries + 1):
        try:
            with open(path, "r", encoding="utf-8-sig") as f:
                text = f.read()
        except FileNotFoundError:
            return default
        except OSError:
            # transient share violation while the writer holds the file
            if attempt < retries:
                time.sleep(retry_delay)
                continue
            if on_error == "default":
                return default
            raise
        try:
            return json.loads(strip_json_comments(text))
        except json.JSONDecodeError as e:
            if attempt < retries:
                time.sleep(retry_delay)
                continue
            if on_error == "default":
                return default
            raise RuntimeError(f"{path}: not valid JSON ({e})")


def write_json_atomic(path, data):
    """Write to a temp file in the same folder, then replace. The driver's
    directory watcher fires on the final rename, so it never sees a half
    written file."""
    folder = os.path.dirname(path)
    os.makedirs(folder, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".gxr-", suffix=".json", dir=folder)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(json.dumps(data, indent=1))
        for attempt in range(20):
            try:
                os.replace(tmp, path)
                return
            except PermissionError:
                # someone (GUI/driver) has it open for a moment
                time.sleep(0.05)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass


class Settings:
    """Read-modify-write access to settings.json with a streamFrame focus.

    Every mutation goes through `update`, which re-reads the file first so
    concurrent GUI edits are not clobbered (last write wins per key, not per
    file). Comments in the file are lost on the first write; the GUI does not
    write comments either.
    """

    def __init__(self, path=None):
        self.path = path or settings_path()

    def read(self):
        # retries ride out a concurrent GUI/driver write; a persistent parse
        # failure still raises so a corrupt settings.json is never clobbered
        data = read_json(self.path, default={}, retries=10)
        if not isinstance(data, dict):
            data = {}
        return data

    def stream_frame(self):
        return self.read().get("streamFrame", {}) or {}

    def update(self, mutate):
        """mutate(streamFrameDict) -> None. Writes back IN PLACE (like the GUI):
        one directory change event, which is what the driver's watcher reacts
        to most reliably; a temp file + rename produces several events."""
        data = self.read()
        sf = data.get("streamFrame")
        if not isinstance(sf, dict):
            sf = {}
            data["streamFrame"] = sf
        mutate(sf)
        text = json.dumps(data, indent=1)
        for attempt in range(40):
            try:
                with open(self.path, "w", encoding="utf-8") as f:
                    f.write(text)
                return
            except PermissionError:
                time.sleep(0.05)
        raise RuntimeError(f"could not write {self.path} (locked?)")

    # convenience setters
    def set_calib(self, **kwargs):
        def m(sf):
            calib = sf.setdefault("calib", {})
            for k, v in kwargs.items():
                calib[k] = v
        self.update(m)

    def get_calib(self):
        return self.stream_frame().get("calib", {}) or {}

    def set_gain(self, gain):
        def m(sf):
            sf.setdefault("distortion", {})["gain"] = float(gain)
        self.update(m)

    def get_gain(self):
        d = self.stream_frame().get("distortion", {}) or {}
        return float(d.get("gain", 1.0))

    def get_map(self):
        d = self.stream_frame().get("distortion", {}) or {}
        m = d.get("map")
        return m if isinstance(m, dict) else None

    def set_map(self, lattice_map):
        """lattice_map: dict with cols, rows, left, right, source (see
        DisplacementLattice.to_json)."""
        def m(sf):
            sf.setdefault("distortion", {})["map"] = lattice_map
        self.update(m)


def read_diagnostic():
    # torn/mid-write reads come back as None; every caller polls, so the
    # next cycle picks it up
    d = read_json(diagnostic_path(), default=None, retries=4, on_error="default")
    if not isinstance(d, dict):
        return None
    return d.get("streamFrame")


class DriverLink:
    """Handshake with the driver through settings.json + diagnostic.json."""

    def __init__(self, settings=None, verbose=True):
        self.settings = settings or Settings()
        self.verbose = verbose

    def log(self, *a):
        if self.verbose:
            print(*a, flush=True)

    def diag(self):
        return read_diagnostic()

    def require_active(self, timeout=6.0):
        """Wait until the driver reports the stream frame pass running and
        return the diagnostic block. Raises with a readable hint otherwise."""
        t0 = time.time()
        last = None
        while time.time() - t0 < timeout:
            d = self.diag()
            if d and d.get("active") and d.get("projValid"):
                return d
            last = d
            time.sleep(0.25)
        if last is None:
            raise RuntimeError(
                "diagnostic.json has no streamFrame block. Is SteamVR running with the "
                "GxR driver build that includes the camera calibration support?")
        if not last.get("active"):
            raise RuntimeError(
                "streamFrame pass not active: enable it on the Streamed Headset page "
                "and make sure something is being rendered.")
        raise RuntimeError("projection tangents not available yet, wait for the headset to connect.")

    def projection(self, eye):
        d = self.require_active()
        key = "projLeft" if eye == 0 else "projRight"
        return [float(x) for x in d[key]]

    def eye_texture(self):
        d = self.require_active()
        return int(d.get("eyeTexWidth", 0)), int(d.get("eyeTexHeight", 0)), float(d.get("eyeAspect", 1.0))

    def show_pattern(self, index, settle_frames=6, timeout=8.0):
        """Write the pattern index and wait until the driver has drawn it for
        settle_frames consecutive frames. Returns the diag block."""
        self.settings.set_calib(pattern=int(index))
        t0 = time.time()
        while time.time() - t0 < timeout:
            d = self.diag()
            if d and d.get("calibPatternShown") == int(index) and d.get("calibPatternFrames", 0) >= settle_frames:
                return d
            time.sleep(0.05)
        raise RuntimeError(f"driver did not confirm pattern {index} within {timeout}s "
                           "(settings.json hot reload broken? stream frame pass off?)")

    def pattern_off(self):
        self.settings.set_calib(pattern=-1)


# camera

DEFAULT_CAMERA_CONFIG = {
    "camera_id": 0,
    # "bypass": open the camera exactly like sboys3's scripts (default OpenCV
    # backend, set width/height, touch nothing else, auto exposure). This is
    # the default and what you want for the fisheye calibration and while
    # getting the rig going. "pinned": force backend/fourcc/manual exposure
    # (for the sweep, once a stable exposure matters).
    "mode": "bypass",
    "width": 1600,
    "height": 1200,
    # everything below is only used in "pinned" mode
    "backend": "dshow",         # "dshow", "msmf" or "auto"
    "fps": 30,
    "fourcc": "MJPG",
    # DirectShow manual exposure, log2 seconds (e.g. -6 = 1/64 s). null =
    # leave the camera's setting.
    "exposure": None,
    "gain": None,
    "auto_white_balance": None,
    # frames to discard after opening (auto exposure settling, MJPG keyframes)
    "warmup_frames": 5,
    "calibration_file": "output/calibration_data.pkl",
}


def camera_config_path():
    return os.path.join(tools_folder(), "gxr_camera.json")


def load_camera_config():
    cfg = dict(DEFAULT_CAMERA_CONFIG)
    user = read_json(camera_config_path(), default=None)
    if isinstance(user, dict):
        cfg.update(user)
    return cfg


def save_camera_config(cfg):
    write_json_atomic(camera_config_path(), cfg)


def open_camera(cfg=None):
    """Open the camera. Returns (cap, cfg). Default is bypass mode: identical
    to sboys3's scripts (cv2.VideoCapture(id); set width, height; done)."""
    if cv2 is None:
        raise RuntimeError("opencv-python is required (pip install -r requirements.txt)")
    cfg = cfg or load_camera_config()
    mode = str(cfg.get("mode", "bypass")).lower()
    if mode == "bypass":
        cap = cv2.VideoCapture(int(cfg["camera_id"]))
        if not cap.isOpened():
            raise RuntimeError(f"could not open camera {cfg['camera_id']}")
        # the high resolution modes of these sensors only exist in MJPEG
        # (USB2 bandwidth); the default YUY2 negotiation silently caps the
        # resolution (e.g. the 8MP IMX179 falls back to 1600x1200), so the
        # fourcc must be selected BEFORE the resolution is requested
        if cfg.get("fourcc"):
            cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*cfg["fourcc"]))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, int(cfg["width"]))
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, int(cfg["height"]))
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    else:
        backends = {"dshow": getattr(cv2, "CAP_DSHOW", cv2.CAP_ANY),
                    "msmf": getattr(cv2, "CAP_MSMF", cv2.CAP_ANY),
                    "auto": cv2.CAP_ANY}
        backend = backends.get(str(cfg.get("backend", "dshow")).lower(), cv2.CAP_ANY) if os.name == "nt" else cv2.CAP_ANY
        cap = cv2.VideoCapture(int(cfg["camera_id"]), backend)
        if not cap.isOpened():
            raise RuntimeError(f"could not open camera {cfg['camera_id']}")
        fourcc = cfg.get("fourcc")
        if fourcc:
            cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*fourcc))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, int(cfg["width"]))
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, int(cfg["height"]))
        if cfg.get("fps"):
            cap.set(cv2.CAP_PROP_FPS, float(cfg["fps"]))
        if cfg.get("exposure") is not None:
            # DirectShow: 0.25 = manual, 0.75 = auto (V4L2 uses 1/3)
            cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.25 if os.name == "nt" else 1)
            cap.set(cv2.CAP_PROP_EXPOSURE, float(cfg["exposure"]))
        if cfg.get("gain") is not None:
            cap.set(cv2.CAP_PROP_GAIN, float(cfg["gain"]))
        if cfg.get("auto_white_balance") is False:
            cap.set(cv2.CAP_PROP_AUTO_WB, 0)
        for _ in range(int(cfg.get("warmup_frames", 5))):
            cap.read()
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if (w, h) != (int(cfg["width"]), int(cfg["height"])):
        print(f"warning: camera delivers {w}x{h}, config asked for {cfg['width']}x{cfg['height']}. "
              "The fisheye calibration is only valid at the resolution it was made at.")
    return cap, cfg


def grab_average(cap, count=3, gray=True):
    """Average `count` FRESH frames (float32). The capture pipeline buffers
    several frames internally, so after a pattern change the next reads can
    still return the previous pattern (at 8MP/15fps the buffer is ~300 ms
    deep - enough to poison every capture). Flush by timing: buffered
    frames return back-to-back, a live frame takes about a frame period to
    arrive, so read until the inter-read gap looks live (or a hard cap of
    10 flushed frames), then start averaging."""
    fps = cap.get(cv2.CAP_PROP_FPS) or 0
    period = 1.0 / fps if fps and fps > 1 else 1.0 / 30.0
    last = time.time()
    for _ in range(10):
        cap.read()
        now = time.time()
        gap, last = now - last, now
        if gap > 0.5 * period:
            break
    acc = None
    got = 0
    for _ in range(count * 3):
        ok, frame = cap.read()
        if not ok:
            continue
        if gray:
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        f = frame.astype(np.float32)
        acc = f if acc is None else acc + f
        got += 1
        if got >= count:
            break
    if acc is None:
        raise RuntimeError("camera returned no frames")
    return acc / got

# fisheye calibration and camera model

class CameraModel:
    """Camera model from calibration_data.pkl: either the OpenCV pinhole +
    rational distortion model, or the cv2.fisheye (equidistant + k1..k4)
    model, selected by the pkl's "model" field. The fisheye model is the
    right one for 150+ degree lenses: the rational model's pinhole base
    diverges toward 90 degrees and biases the outer image even when its RMS
    looks fine.

    pixel -> unit direction in camera frame (x right, y down, z forward),
    direction -> pixel, and rectified (virtual pinhole) view helpers.
    """

    def __init__(self, K, dist, image_size, model="rational"):
        self.K = np.asarray(K, dtype=np.float64)
        self.model = model
        n = 4 if model == "fisheye" else -1
        self.dist = np.asarray(dist, dtype=np.float64).reshape(-1, 1)
        if model == "fisheye":
            self.dist = self.dist[:4].reshape(4, 1)
        self.size = (int(image_size[0]), int(image_size[1]))

    @staticmethod
    def load(path=None, cfg=None):
        cfg = cfg or load_camera_config()
        path = path or cfg["calibration_file"]
        if not os.path.isabs(path):
            path = os.path.join(tools_folder(), path)
        if not os.path.exists(path):
            raise RuntimeError(f"fisheye calibration not found at {path}. Run make_charuco_board.py, "
                               "charuco_capture.py and charuco_calibrate.py first "
                               "(Docs/TunerUsage.md, Step 1).")
        with open(path, "rb") as f:
            data = pickle.load(f)
        size = data.get("image_size") or (int(cfg["width"]), int(cfg["height"]))
        return CameraModel(data["camera_matrix"], data["distortion_coefficients"], size,
                           data.get("model", "rational"))

    def pixels_to_dirs(self, pts):
        """pts: (N,2) pixel coords -> (N,3) unit directions."""
        pts = np.asarray(pts, dtype=np.float64).reshape(-1, 1, 2)
        if self.model == "fisheye":
            # the default inverse iteration is too shallow near the rim of
            # a 170 degree lens; ask for a tight tolerance where supported
            try:
                norm = cv2.fisheye.undistortPoints(
                    pts, self.K, self.dist,
                    criteria=(cv2.TERM_CRITERIA_MAX_ITER + cv2.TERM_CRITERIA_EPS, 100, 1e-12)).reshape(-1, 2)
            except TypeError:
                norm = cv2.fisheye.undistortPoints(pts, self.K, self.dist).reshape(-1, 2)
        else:
            norm = cv2.undistortPoints(pts, self.K, self.dist).reshape(-1, 2)
        d = np.concatenate([norm, np.ones((norm.shape[0], 1))], axis=1)
        d /= np.linalg.norm(d, axis=1, keepdims=True)
        return d

    def dirs_to_pixels(self, dirs):
        dirs = np.asarray(dirs, dtype=np.float64).reshape(-1, 3)
        rvec = np.zeros(3)
        tvec = np.zeros(3)
        if self.model == "fisheye":
            # fisheye.projectPoints wants (N,1,3)
            pts, _ = cv2.fisheye.projectPoints(dirs.reshape(-1, 1, 3), rvec, tvec, self.K, self.dist)
        else:
            pts, _ = cv2.projectPoints(dirs, rvec, tvec, self.K, self.dist)
        return pts.reshape(-1, 2)

    def rectify_maps(self, R, K_virt, out_size):
        """Maps for cv2.remap that render the camera image as seen by a
        virtual pinhole K_virt whose frame is R^T applied to the camera frame
        (i.e. R = rotation from panel/eye frame to camera frame; the virtual
        view is then the panel frame)."""
        Rt = np.asarray(R, dtype=np.float64).T
        Kv = np.asarray(K_virt, dtype=np.float64)
        if self.model == "fisheye":
            return cv2.fisheye.initUndistortRectifyMap(self.K, self.dist, Rt, Kv,
                                                       tuple(out_size), cv2.CV_32FC1)
        return cv2.initUndistortRectifyMap(self.K, self.dist, Rt, Kv, tuple(out_size), cv2.CV_32FC1)


def virtual_pinhole(out_size, half_fov_deg):
    """K for a virtual pinhole with symmetric half FOV (deg) horizontally."""
    w, h = out_size
    f = (w / 2.0) / math.tan(math.radians(half_fov_deg))
    return np.array([[f, 0, w / 2.0], [0, f, h / 2.0], [0, 0, 1.0]])


def project_dirs(K, dirs):
    """Pinhole projection of (N,3) directions (z>0) with matrix K -> (N,2)."""
    dirs = np.asarray(dirs, dtype=np.float64).reshape(-1, 3)
    z = dirs[:, 2:3]
    z = np.where(np.abs(z) < 1e-9, 1e-9, z)
    x = dirs[:, 0:1] / z
    y = dirs[:, 1:2] / z
    return np.concatenate([K[0, 0] * x + K[0, 2], K[1, 1] * y + K[1, 2]], axis=1)


# panel model

class PanelModel:
    """Ideal uv <-> direction mapping of one eye from raw projection tangents."""

    def __init__(self, proj):
        self.l, self.r, self.t, self.b = [float(x) for x in proj]

    def uv_to_tan(self, uv):
        uv = np.asarray(uv, dtype=np.float64).reshape(-1, 2)
        tx = self.l + uv[:, 0] * (self.r - self.l)
        ty = self.t + uv[:, 1] * (self.b - self.t)
        return np.stack([tx, ty], axis=1)

    def tan_to_uv(self, tan):
        tan = np.asarray(tan, dtype=np.float64).reshape(-1, 2)
        u = (tan[:, 0] - self.l) / (self.r - self.l)
        v = (tan[:, 1] - self.t) / (self.b - self.t)
        return np.stack([u, v], axis=1)

    def uv_to_dirs(self, uv):
        tan = self.uv_to_tan(uv)
        d = np.concatenate([tan, np.ones((tan.shape[0], 1))], axis=1)
        return d / np.linalg.norm(d, axis=1, keepdims=True)

    def dirs_to_uv(self, dirs):
        dirs = np.asarray(dirs, dtype=np.float64).reshape(-1, 3)
        z = dirs[:, 2:3]
        z = np.where(z < 1e-6, 1e-6, z)
        tan = np.concatenate([dirs[:, 0:1] / z, dirs[:, 1:2] / z], axis=1)
        return self.tan_to_uv(tan)

    def angles_deg(self, uv):
        """sboys grid angles: aH = atan(tx), aV = atan(-ty) (up positive)."""
        tan = self.uv_to_tan(uv)
        return np.degrees(np.arctan(tan[:, 0])), np.degrees(np.arctan(-tan[:, 1]))

    def angles_to_dirs(self, aH_deg, aV_deg):
        tx = np.tan(np.radians(np.asarray(aH_deg, dtype=np.float64)))
        ty = -np.tan(np.radians(np.asarray(aV_deg, dtype=np.float64)))
        d = np.stack([tx, ty, np.ones_like(tx)], axis=-1)
        return d / np.linalg.norm(d, axis=-1, keepdims=True)


# rotation helpers

def rotvec_to_matrix(rv):
    rv = np.asarray(rv, dtype=np.float64).reshape(3)
    R, _ = cv2.Rodrigues(rv)
    return R


def matrix_to_rotvec(R):
    rv, _ = cv2.Rodrigues(np.asarray(R, dtype=np.float64))
    return rv.reshape(3)


def matrix_to_ypr_deg(R):
    """Yaw (about y, +right), pitch (about x, +down), roll (about z) in
    degrees for a y-down, z-forward frame. Small angle friendly readout."""
    R = np.asarray(R, dtype=np.float64)
    yaw = math.degrees(math.atan2(R[0, 2], R[2, 2]))
    pitch = math.degrees(-math.asin(max(-1.0, min(1.0, R[1, 2]))))
    roll = math.degrees(math.atan2(R[1, 0], R[1, 1]))
    return yaw, pitch, roll


def solve_rotation(panel, uv, cam_dirs, iters=8):
    """Find R (panel -> camera frame) minimising the uv residual
    |P^-1(R^T d) - uv|^2 in the least squares sense. Kabsch on the ideal vs
    observed directions gives a closed form; a couple of reweighting passes
    then favour the central region (where distortion is smallest) so the
    gauge is anchored where the eye looks. Returns (R, residual_uv (N,2))."""
    ideal = panel.uv_to_dirs(uv)
    obs = np.asarray(cam_dirs, dtype=np.float64)
    w = np.ones(len(uv))
    R = np.eye(3)
    for it in range(iters):
        H = (ideal * w[:, None]).T @ obs
        U, _, Vt = np.linalg.svd(H)
        D = np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))])
        R = Vt.T @ D @ U.T  # maps ideal -> obs
        # residual in uv after removing the rotation
        back = (R.T @ obs.T).T
        res = panel.dirs_to_uv(back) - uv
        # downweight large residual (real distortion) samples so they do not
        # drag the rotation; robust-ish IRLS with a floor
        r = np.linalg.norm(res, axis=1)
        s = max(np.median(r) * 3.0, 1e-4)
        w = 1.0 / (1.0 + (r / s) ** 2)
    return R, res


# displacement lattice

def _catmull_rom_weights(t):
    t2 = t * t
    t3 = t2 * t
    return np.stack([
        0.5 * (-t3 + 2 * t2 - t),
        0.5 * (3 * t3 - 5 * t2 + 2),
        0.5 * (-3 * t3 + 4 * t2 + t),
        0.5 * (t3 - t2),
    ], axis=-1)


class DisplacementLattice:
    """cols x rows lattice of (du, dv) per eye, identical semantics to the
    driver's StreamFrameDisplacementMap. Values in bounds uv units."""

    def __init__(self, cols=33, rows=33, source="manual"):
        self.cols = int(cols)
        self.rows = int(rows)
        self.eyes = [np.zeros((self.rows, self.cols, 2)), np.zeros((self.rows, self.cols, 2))]
        # per-eye bool mask of knots actually constrained by data (sweep
        # support or user edit); None = unknown = treat all as constrained.
        # travels through the json as leftMeasured/rightMeasured (0/1); the
        # driver ignores the extra keys, the GUI drops them on a re-save
        # (the overlay then falls back to all-constrained, which is safe).
        self.measured = [None, None]
        self.source = source

    # json
    @staticmethod
    def from_json(m):
        if not isinstance(m, dict):
            return None
        cols, rows = int(m.get("cols", 0)), int(m.get("rows", 0))
        if cols < 2 or rows < 2:
            return None
        lat = DisplacementLattice(cols, rows, m.get("source", ""))
        for eye, key in enumerate(("left", "right")):
            arr = m.get(key)
            if isinstance(arr, list) and len(arr) == cols * rows * 2:
                lat.eyes[eye] = np.asarray(arr, dtype=np.float64).reshape(rows, cols, 2)
            marr = m.get(key + "Measured")
            if isinstance(marr, list) and len(marr) == cols * rows:
                lat.measured[eye] = np.asarray(marr, dtype=bool).reshape(rows, cols)
        return lat

    def to_json(self, enable=True):
        out = {
            "enable": bool(enable),
            "cols": self.cols,
            "rows": self.rows,
            "left": [round(float(v), 7) for v in self.eyes[0].reshape(-1)],
            "right": [round(float(v), 7) for v in self.eyes[1].reshape(-1)],
            "source": self.source,
        }
        for eye, key in enumerate(("leftMeasured", "rightMeasured")):
            if self.measured[eye] is not None:
                out[key] = [int(v) for v in self.measured[eye].reshape(-1)]
        return out

    def copy(self):
        lat = DisplacementLattice(self.cols, self.rows, self.source)
        lat.eyes = [e.copy() for e in self.eyes]
        lat.measured = [None if m is None else m.copy() for m in self.measured]
        return lat

    # geometry
    def knot_uv(self):
        """(rows*cols, 2) uv positions of the knots, row major."""
        us = np.linspace(0, 1, self.cols)
        vs = np.linspace(0, 1, self.rows)
        uu, vv = np.meshgrid(us, vs)
        return np.stack([uu.reshape(-1), vv.reshape(-1)], axis=1)

    def basis(self, uv):
        """(N, rows*cols) Catmull-Rom interpolation matrix (scipy sparse CSR),
        exactly the weights the driver bake uses (clamped edges)."""
        from scipy import sparse
        uv = np.asarray(uv, dtype=np.float64).reshape(-1, 2)
        n = uv.shape[0]
        fx = np.clip(uv[:, 0], 0, 1) * (self.cols - 1)
        fy = np.clip(uv[:, 1], 0, 1) * (self.rows - 1)
        c0 = np.minimum(fx.astype(int), self.cols - 2)
        r0 = np.minimum(fy.astype(int), self.rows - 2)
        wx = _catmull_rom_weights(fx - c0)
        wy = _catmull_rom_weights(fy - r0)
        rows_idx = np.arange(n)
        ri, ci, vals = [], [], []
        for j in range(4):
            rr = np.clip(r0 - 1 + j, 0, self.rows - 1)
            for i in range(4):
                cc = np.clip(c0 - 1 + i, 0, self.cols - 1)
                ri.append(rows_idx)
                ci.append(rr * self.cols + cc)
                vals.append(wx[:, i] * wy[:, j])
        B = sparse.coo_matrix((np.concatenate(vals), (np.concatenate(ri), np.concatenate(ci))),
                              shape=(n, self.rows * self.cols))
        return B.tocsr()

    def evaluate(self, eye, uv):
        B = self.basis(uv)
        vals = self.eyes[eye].reshape(-1, 2)
        return np.asarray(B @ vals)

    def fit(self, eye, uv, disp, smooth=0.05, prior=1e-3, weights=None,
            taper=3.0, fade=1.0, edge="fade"):
        """Regularised least squares fit of the eye's lattice to samples.

        min |W (B c - disp)|^2 + smooth * |L c|^2 + sum_i p_i |c_i|^2

        L is the lattice Laplacian (smoothness). p_i is a per-knot pull
        toward identity (zero): `prior` where the samples support the knot,
        ramping up to `fade` (both relative to the data term's scale) over
        `taper` knots of distance outside the covered region. So the field
        holds where measured and relaxes smoothly to identity where not,
        instead of extrapolating. Returns the per-sample residual (N, 2);
        stores a summary in self.last_fit_info.

        edge: what unmeasured knots become.
          "fade"   - ramp to identity over `taper` knots (guaranteed safe,
                     but the ramp itself is a local magnification error:
                     disp/ramp-width of stretch in a band along the
                     coverage boundary).
          "extend" - continue the nearest measured knot's VALUE outward
                     with a slowly decaying magnitude; the gradient dies at
                     the boundary, so there is no magnification band, at
                     the cost of a small constant shift in the (unseen)
                     periphery. Clamped so the sampled source can never
                     leave [0,1], and lightly relaxed for continuity."""
        from scipy import sparse
        from scipy import ndimage
        uv = np.asarray(uv, dtype=np.float64).reshape(-1, 2)
        disp = np.asarray(disp, dtype=np.float64).reshape(-1, 2)
        n_knots = self.rows * self.cols
        B = self.basis(uv)
        if weights is not None:
            w = np.asarray(weights, dtype=np.float64).reshape(-1)
            Bw = sparse.diags(np.sqrt(w)) @ B
            dw = disp * np.sqrt(w)[:, None]
        else:
            Bw, dw = B, disp
        A = (Bw.T @ Bw).toarray()
        data_scale = max(np.trace(A) / n_knots, 1e-12)
        # per-knot data support: how much sample mass lands on each knot
        # (|basis| column sums; Catmull-Rom weights can be negative, the
        # magnitude is what indicates presence of nearby samples)
        support = np.asarray(np.abs(B).sum(axis=0)).reshape(self.rows, self.cols)
        ref = np.median(support[support > 0]) if (support > 0).any() else 1.0
        measured = support > 0.02 * ref
        # distance (in knot units) from the measured region
        if measured.any():
            dist = ndimage.distance_transform_edt(~measured)
        else:
            dist = np.full((self.rows, self.cols), taper)
        if edge == "extend":
            # no outward ramp: the ramp would drag the (weakly supported)
            # boundary fringe toward zero before the harmonic extension can
            # take its Dirichlet values from it. Uniform gentle prior; the
            # extension below replaces everything outside solid support.
            p_knot = np.full(self.rows * self.cols, prior)
        else:
            ramp = np.clip(dist / max(taper, 1e-6), 0.0, 1.0) ** 2
            p_knot = (prior + (fade - prior) * ramp).reshape(-1)
        L = self._laplacian()
        A = A + smooth * data_scale * (L.T @ L) + data_scale * np.diag(p_knot)
        rhs = np.asarray(Bw.T @ dw)
        c = np.linalg.solve(A, rhs)
        self.eyes[eye] = c.reshape(self.rows, self.cols, 2)
        if edge == "extend" and measured.any() and not measured.all():
            # harmonic extension: free knots solve min |L c|^2 with the
            # measured knots as a Dirichlet boundary (plus a tiny far-field
            # pull to zero so fully detached regions cannot drift). A
            # constant boundary extends as a constant, so the coverage edge
            # gets NO artificial magnification band; whatever gradient the
            # data implies at the boundary relaxes away smoothly instead.
            # solid = knots with real sample mass (not just the Catmull-Rom
            # fringe of the last samples, which the solve extrapolates)
            solid = support > 0.2 * ref
            if not solid.any():
                solid = measured
            e = self.eyes[eye].reshape(-1, 2)
            free = ~solid.reshape(-1)
            L = self._laplacian()
            Lf = L[np.ix_(free, free)]
            Lb = L[np.ix_(free, ~free)]
            # Laplace equation at the free knots, Dirichlet boundary at the
            # solid ones. lstsq (not solve + ridge): a ridge crushes the
            # near-null constant mode and drags the extension to zero; for a
            # free component fully detached from any solid knot, Lf is
            # singular in that mode and lstsq's min-norm answer (zero) is
            # exactly the sensible fallback.
            e[free] = np.linalg.lstsq(Lf, -(Lb @ e[~free]), rcond=None)[0]
            # never let the sampled source leave the texture. Where this
            # clamp binds (the correction wants content that does not exist
            # past the texture border), a step is physically unavoidable;
            # relax the free knots around the bind so it spreads over ~2
            # knots instead of 1, then clamp again.
            e = e.reshape(self.rows, self.cols, 2)
            ku = np.linspace(0, 1, self.cols)[None, :].repeat(self.rows, 0)
            kv = np.linspace(0, 1, self.rows)[:, None].repeat(self.cols, 1)

            def clamp(arr):
                arr[:, :, 0] = np.clip(arr[:, :, 0], -ku, 1 - ku)
                arr[:, :, 1] = np.clip(arr[:, :, 1], -kv, 1 - kv)
            before = e.copy()
            clamp(e)
            bound = np.abs(e - before).max(axis=2) > 1e-7
            if bound.any():
                near = ndimage.distance_transform_edt(~bound) <= 2.0
                anchor = np.where((near & ~solid).reshape(-1), 1.0, 1e4)
                self.eyes[eye] = e
                self.smooth(eye, amount=1.0, anchor=anchor)
                e = self.eyes[eye]
                clamp(e)
            self.eyes[eye] = e
        self.measured[eye] = measured.copy()
        self.last_fit_measured = measured
        self.last_fit_info = {
            "knots_measured": int(measured.sum()),
            "knots_total": int(n_knots),
            "measured_fraction": float(measured.mean()),
            "taper_knots": float(taper),
        }
        return np.asarray(B @ c) - disp

    def _laplacian(self):
        n = self.rows * self.cols
        L = np.zeros((n, n))
        for r in range(self.rows):
            for c in range(self.cols):
                i = r * self.cols + c
                nb = []
                if r > 0: nb.append(i - self.cols)
                if r < self.rows - 1: nb.append(i + self.cols)
                if c > 0: nb.append(i - 1)
                if c < self.cols - 1: nb.append(i + 1)
                L[i, i] = len(nb)
                for j in nb:
                    L[i, j] = -1
        return L

    def smooth(self, eye, amount=0.6, anchor=None):
        """Variational smoothing (the 2D analog of sboys' line-smooth):
        min sum a_i |c_i - c0_i|^2 + amount * |L c|^2, i.e. the lattice
        closest to the current one under a Laplacian smoothness penalty.
        `anchor` is a per-knot fidelity weight (rows*cols,), default 1
        everywhere; small anchor = free to relax (used for local smoothing
        under a brush). Returns (max_change_uv, roughness_before,
        roughness_after) where roughness is rms |L c|."""
        n = self.rows * self.cols
        L = self._laplacian()
        a = np.ones(n) if anchor is None else np.asarray(anchor, dtype=np.float64).reshape(n)
        A = np.diag(a) + amount * (L.T @ L)
        c0 = self.eyes[eye].reshape(-1, 2)
        rough0 = float(np.sqrt(((L @ c0) ** 2).sum(axis=1).mean()))
        c = np.linalg.solve(A, a[:, None] * c0)
        rough1 = float(np.sqrt(((L @ c) ** 2).sum(axis=1).mean()))
        change = float(np.abs(c - c0).max())
        self.eyes[eye] = c.reshape(self.rows, self.cols, 2)
        return change, rough0, rough1

    def stats(self, eye):
        e = self.eyes[eye]
        mag = np.linalg.norm(e, axis=2)
        return float(mag.max()), float(np.sqrt((mag ** 2).mean()))

    def stats_measured(self, eye):
        """Like stats but only over knots the last fit marked as measured
        (falls back to all knots if no fit info is available)."""
        info = getattr(self, "last_fit_info", None)
        mask = getattr(self, "last_fit_measured", None)
        e = self.eyes[eye]
        mag = np.linalg.norm(e, axis=2)
        if mask is not None and mask.shape == mag.shape and mask.any():
            mag = mag[mask]
        return float(mag.max()), float(np.sqrt((mag ** 2).mean()))

# profile output

def build_profile(settings_sf, lattice, name, meta=None, neutral=False):
    """A version 2 streamFrameDistortionProfile. A profile is a complete
    correction, not a patch.

    neutral=False: radial curves and centers copied from the live settings
    (the pre-camera workflow, where the map refines a radial tune).

    neutral=True: the map IS the whole correction (camera sweeps measure the
    total, and the shader composes map on top of radial - so a camera
    profile must carry an identity radial or the radial part gets applied
    twice). k1/k2, curves, per-eye/per-axis, segments and centers are all
    written as identity; gain 1."""
    d = dict(settings_sf.get("distortion", {}) or {})
    for k in ("annulus", "tune", "centerTune"):
        d.pop(k, None)
    if neutral:
        d.update({"mode": "k1k2", "gain": 1.0, "perEye": False, "perAxis": False,
                  "segments": 1, "curves": {}})
    d["map"] = lattice.to_json()
    profile = {
        "type": "streamFrameDistortionProfile",
        "version": 2,
        "name": name,
        "distortion": d,
        "k1": 0 if neutral else settings_sf.get("k1", 0),
        "k2": 0 if neutral else settings_sf.get("k2", 0),
        "centerOffsetXLeft": 0 if neutral else settings_sf.get("centerOffsetXLeft", 0),
        "centerOffsetXRight": 0 if neutral else settings_sf.get("centerOffsetXRight", 0),
        "centerOffsetY": 0 if neutral else settings_sf.get("centerOffsetY", 0),
    }
    if meta:
        profile["meta"] = meta
    return profile


def radial_neutrality(sf):
    """List of human-readable reasons the live radial/alignment state is not
    identity (empty list = virgin panel apart from the map)."""
    issues = []
    d = sf.get("distortion", {}) or {}
    if abs(float(sf.get("k1", 0) or 0)) > 1e-9 or abs(float(sf.get("k2", 0) or 0)) > 1e-9:
        issues.append(f"k1/k2 = {sf.get('k1', 0)}/{sf.get('k2', 0)}")
    if str(d.get("mode", "k1k2")) != "k1k2":
        issues.append(f"distortion mode '{d.get('mode')}'")
    for name, c in (d.get("curves", {}) or {}).items():
        if not isinstance(c, dict):
            continue
        if abs(float(c.get("k1", 0) or 0)) > 1e-9 or abs(float(c.get("k2", 0) or 0)) > 1e-9 \
                or (isinstance(c.get("points"), list) and len(c["points"]) > 0):
            issues.append(f"curve '{name}' non-identity")
    if bool(d.get("perEye")) or bool(d.get("perAxis")) or int(d.get("segments", 1) or 1) > 1:
        issues.append("perEye/perAxis/segments active")
    al = sf.get("alignment", {}) or {}
    for k in ("leftH", "leftV", "rightH", "rightV"):
        if abs(float(al.get(k, 0) or 0)) > 1e-9:
            issues.append(f"alignment.{k} = {al.get(k)}")
    return issues


def neutralize_radial(settings):
    """Zero the live radial curves and alignment shifts (map untouched).
    Callers should back the previous state up as a profile first."""
    def mutate(sf):
        sf["k1"] = 0
        sf["k2"] = 0
        d = sf.setdefault("distortion", {})
        d.update({"mode": "k1k2", "perEye": False, "perAxis": False, "segments": 1, "curves": {}})
        al = sf.setdefault("alignment", {})
        for k in ("leftH", "leftV", "rightH", "rightV"):
            al[k] = 0
    settings.update(mutate)


def save_profile(profile, stem="gxr-map"):
    os.makedirs(distortion_folder(), exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = os.path.join(distortion_folder(), f"{stem}-{stamp}.json")
    write_json_atomic(path, profile)
    return path

# sboys grid geometry (ground truth overlay), matches vrlink_layer_ps.hlsl

def hue_to_bgr(hue):
    r = max(0.0, min(1.0, abs(hue * 6 - 3) - 1))
    g = max(0.0, min(1.0, 2 - abs(hue * 6 - 2)))
    b = max(0.0, min(1.0, 2 - abs(hue * 6 - 4)))
    return (int(b * 255), int(g * 255), int(r * 255))


def grid_polylines(step_deg, max_deg):
    """Yield (box_index, list_of_(aH, aV)_polylines) for the sboys pattern:
    box n is the square |aH| = n*step (where |aH| >= |aV|) and |aV| = n*step
    (where |aV| >= |aH|), i.e. simply the square of half-size n*step in
    (aH, aV) angle space. Hue = frac(n / 6)."""
    n = 1
    while n * step_deg <= max_deg + 1e-9:
        a = n * step_deg
        pts = []
        # square, densely sampled so it projects as a smooth curve
        for s in np.linspace(-a, a, 32):
            pts.append((a, s))
        for s in np.linspace(a, -a, 32):
            pts.append((s, a))
        for s in np.linspace(a, -a, 32):
            pts.append((-a, s))
        for s in np.linspace(-a, a, 32):
            pts.append((s, -a))
        yield n, pts
        n += 1
