"""
gxr_overlay.py 
live camera view through the lens with the ground-truth
angular grid overlaid, and manual editing of the displacement map.

The camera image is rectified into a virtual pinhole aligned with the panel's
optical axis (rotation from the last sweep/alignment run, or nudged by hand
with the arrow keys). On top of it the ideal sboys grid is drawn: where each
hue-coded line SHOULD be. The driver draws the same grid in content space
(capture mode), so after a correct map the two coincide. Where they don't,
drag the content toward the ground truth: click near the spot, drag; the
knots of the displacement lattice within the brush radius move, the driver
re-bakes within about a second, the camera shows the result.

Drag modifiers: hold Shift to constrain a drag to vertical only, Ctrl to
horizontal only, Alt to move in discrete 2-panel-pixel steps (combinable:
Shift+Alt = vertical in steps).

Editing model: every grey dot is a lattice knot drawn where its content
currently sits. Hover: the nearest dot highlights. Press and drag: that dot
sticks to the cursor and its neighbours follow with a falloff (radius = the
orange circle, mouse wheel or - = to change). Release: the driver rebakes,
about a second later the camera shows the result. Dots turn warmer the more
they are displaced.

Keys:
    c   toggle capture mode (driver draws the hue grid, other eye black)
    b   toggle blackout (panels black; camera goes dark, use between sessions)
    g   toggle correction gain 0 / 1 (A-B the map against the raw headset)
    e   switch eye (also switches the driver's calib.eye)
    k   toggle knot markers          [ ]  camera-side ground-truth grid opacity
    ; '  headset-side grid/pattern brightness (streamFrame.calib.patternBrightness)
    - = / wheel  falloff radius      a    alignment run (6 bit mini sweep -> rotation)
    arrows / , .   nudge the view rotation yaw / pitch / roll by 0.2 deg
    z   undo (last stroke)            r    reset knots inside the radius at cursor
    p   paint mode: stroke marks knots FREE (hollow: brush and smoothing
        ignore them; for regions the camera cannot see). Shift locks back.
        Shift+P constrains ALL knots at once (clears a speckled mask)
    l   align assist ('l' as in Lima, or press 1): white pattern + live
        margin readout per side, panel
        outline in green. Equalize L/R and T/B by translating the camera;
        set depth by pulling back until thin margins just appear all round
    v   toggle error vectors: measure the grid error once and draw arrows
        (x8) from where each line is to where it should be, with stats
    m   auto match once: detect the headset grid, nudge the map toward the
        ground truth (needs capture mode + gain 1)
    M   auto match loop (up to 5 iterations, stops when converged)
    f   smooth the map under the cursor (brush radius); F smooth the whole
        eye - the 2D line-smooth: irons out hand-tuning bumps, repeatable
    x   restore the eye to the session baseline (the map at overlay start)
    R   reset the whole eye's lattice s    save profile to the Distortion folder
    h   help overlay                  q    quit (blackout state is left as is)
"""

import argparse
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


def state_path():
    return os.path.join(g.tools_folder(), "gxr_state.json")


class OverlayApp:
    def __init__(self, args):
        self.args = args
        self.settings = g.Settings()
        self.link = g.DriverLink(self.settings, verbose=True)
        self.eye = args.eye
        self.link.require_active()
        self.cam_cfg = g.load_camera_config()
        self.cam = g.CameraModel.load(cfg=self.cam_cfg)
        self.cap, self.cam_cfg = g.open_camera(self.cam_cfg)
        self.size = (args.width, args.height)
        self.state = g.read_json(state_path(), default={}) or {}
        self.R = self.load_rotation()
        self.maps = None
        self.load_panel()
        self.Kv = self.make_view(args.fov)
        self.rebuild_maps()
        self.load_lattice()
        self.grid_opacity = args.grid_opacity
        self.show_knots = True
        # cached driver-side state for the HUD (settings.json is re-read at
        # 2 Hz, never per frame, and never from the mouse callback)
        self.state_cache = {"calib": {}, "gain": 1.0, "at": 0.0}
        self.error_vis = None
        self.align_assist = False
        self.paint_mode = False # 'p': paint knots free (Shift: locked)
        self.paint_down = False
        self.pick = None       # index of the hovered knot
        self.grab = None       # {"knot": i, "start": disp copy, "uv0": knot content uv}
        self.mouse = (0, 0)
        self.pending_stroke = None
        self.mod_flags = 0
        self.show_help = False
        self.brush = 0.06
        # Alt-drag step size in panel pixels
        self.snap_px = 2.0
        self.undo_stack = []
        self.drag = None
        self.dirty_at = None
        self.last_write = 0.0
        self.status = ""
        self.status_at = 0.0
        # grid step: follow the driver's setting unless overridden
        sf = self.settings.stream_frame()
        drv_step = (sf.get("eyeGaze", {}) or {}).get("gridAngularDeg")
        self.step_deg = args.step if args.step else (float(drv_step) if drv_step else 2.5)
        self.max_deg = args.max_deg
        self.grid_cache = None
        self.grid_cache_key = None
        self.angle_cache = None
        cv2.namedWindow("gxr overlay", cv2.WINDOW_NORMAL)
        cv2.resizeWindow("gxr overlay", self.size[0], self.size[1])
        cv2.setMouseCallback("gxr overlay", self.on_mouse)
        self.calib_sync()

    # driver state
    def calib_sync(self):
        """Point the driver's calibration eye at ours (does not touch blackout)."""
        self.settings.set_calib(eye=int(self.eye))

    def make_view(self, fov):
        """Virtual pinhole. fov None/auto: fit the whole eye texture (its
        asymmetric frustum) into the window with a small margin; the view
        stays centred on the optical axis, so the texture appears offset
        toward the temporal side, which is correct."""
        w, h = self.size
        if fov:
            return g.virtual_pinhole(self.size, float(fov))
        tx = max(abs(self.panel.l), abs(self.panel.r))
        ty = max(abs(self.panel.t), abs(self.panel.b))
        f = min((w / 2.0) / tx, (h / 2.0) / ty) * 0.94
        return np.array([[f, 0, w / 2.0], [0, f, h / 2.0], [0, 0, 1.0]])

    def load_panel(self):
        self.proj = self.link.projection(self.eye)
        self.panel = g.PanelModel(self.proj)
        self.tex_w, self.tex_h, _ = self.link.eye_texture()

    def load_lattice(self):
        lat = None
        if getattr(self.args, "profile", None):
            prof = g.read_json(self.args.profile)
            if not prof:
                raise RuntimeError(f"could not read profile {self.args.profile}")
            lat = g.DisplacementLattice.from_json((prof.get("distortion", {}) or {}).get("map"))
            if lat is None:
                raise RuntimeError(f"{self.args.profile} has no displacement map")
            self.settings.set_map(lat.to_json())
            print(f"loaded map from {self.args.profile}")
            self.args.profile = None  # only on first load; eye switches reload live state
        if lat is None:
            lat = g.DisplacementLattice.from_json(self.settings.get_map())
        if lat is None:
            lat = g.DisplacementLattice(self.args.cols, self.args.rows, "manual")
        self.lattice = lat
        self.knot_uv = self.lattice.knot_uv()
        # baseline for 'x': the map as it was when this eye's editing started
        # (i.e. the swept result, before any hand tuning this session)
        self.baseline = self.lattice.copy()

    def load_rotation(self):
        key = f"rotvec_{'left' if self.eye == 0 else 'right'}"
        rv = self.state.get(key)
        if isinstance(rv, list) and len(rv) == 3:
            return g.rotvec_to_matrix(rv)
        return np.eye(3)

    def save_rotation(self):
        key = f"rotvec_{'left' if self.eye == 0 else 'right'}"
        self.state[key] = g.matrix_to_rotvec(self.R).tolist()
        g.write_json_atomic(state_path(), self.state)

    def rebuild_maps(self):
        self.maps = self.cam.rectify_maps(self.R, self.Kv, self.size)
        self.grid_cache = None
        self.angle_cache = None

    def set_status(self, text):
        self.status = text
        self.status_at = time.time()
        print(text, flush=True)

    # geometry helpers
    def dirs_to_screen(self, dirs):
        return g.project_dirs(self.Kv, dirs)

    def screen_to_uv(self, x, y):
        d = np.array([(x - self.Kv[0, 2]) / self.Kv[0, 0], (y - self.Kv[1, 2]) / self.Kv[1, 1], 1.0])
        d /= np.linalg.norm(d)
        return self.panel.dirs_to_uv(d.reshape(1, 3))[0]

    def uv_to_screen(self, uv):
        return self.dirs_to_screen(self.panel.uv_to_dirs(uv))

    #  drawing
    def grid_layer(self):
        key = (self.step_deg, self.max_deg, self.eye)
        if self.grid_cache is not None and self.grid_cache_key == key:
            return self.grid_cache
        layer = np.zeros((self.size[1], self.size[0], 3), dtype=np.uint8)
        # axis cross (aH = 0 and aV = 0 lines) in white
        for aH, aV in (([0] * 64, np.linspace(-80, 80, 64)), (np.linspace(-80, 80, 64), [0] * 64)):
            d = self.panel.angles_to_dirs(np.asarray(aH, dtype=float), np.asarray(aV, dtype=float))
            pts = self.dirs_to_screen(d)
            self.polyline(layer, pts, (255, 255, 255), 1)
        # eye texture outline: everything the panel can show lies inside
        edge = np.concatenate([
            np.stack([np.linspace(0, 1, 40), np.zeros(40)], axis=1),
            np.stack([np.ones(40), np.linspace(0, 1, 40)], axis=1),
            np.stack([np.linspace(1, 0, 40), np.ones(40)], axis=1),
            np.stack([np.zeros(40), np.linspace(1, 0, 40)], axis=1)])
        self.polyline(layer, self.uv_to_screen(edge), (90, 90, 90), 1, closed=True)
        for n, pts_a in g.grid_polylines(self.step_deg, self.max_deg):
            aH = np.array([p[0] for p in pts_a])
            aV = np.array([p[1] for p in pts_a])
            d = self.panel.angles_to_dirs(aH, aV)
            # clip to the texture: the driver only draws the grid where the
            # panel shows content, so ground truth outside is meaningless
            uv = self.panel.dirs_to_uv(d)
            inside = np.all((uv >= -0.002) & (uv <= 1.002), axis=1)
            if not inside.any():
                continue
            pts = self.dirs_to_screen(d)
            color = g.hue_to_bgr((n / 6.0) % 1.0)
            # draw as segments so the clip does not connect across the gap
            run = []
            for p, ok in zip(pts, inside):
                if ok:
                    run.append(p)
                elif run:
                    self.polyline(layer, run, color, 1)
                    run = []
            if run:
                self.polyline(layer, run, color, 1, closed=inside.all())
            # label: half angle of the square, at the first visible point on
            # the right side (aH = +a) or wherever it first shows
            vis = np.nonzero(inside)[0]
            x, y = pts[vis[len(vis) // 2]]
            if 0 <= x < self.size[0] and 0 <= y < self.size[1]:
                cv2.putText(layer, f"{n * self.step_deg:g}", (int(x) + 3, int(y) - 3),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.4, color, 1, cv2.LINE_AA)
        self.grid_cache = layer
        self.grid_cache_key = key
        return layer

    def polyline(self, img, pts, color, thickness, closed=False):
        pts = np.asarray(pts)
        ok = np.isfinite(pts).all(axis=1) & (np.abs(pts) < 1e5).all(axis=1)
        if ok.sum() < 2:
            return
        p = pts[ok].astype(np.int32).reshape(-1, 1, 2)
        cv2.polylines(img, [p], closed, color, thickness, cv2.LINE_AA)

    def knot_content_uv(self):
        """Where each knot's content currently sits: uv - disp(knot)."""
        return self.knot_uv - self.lattice.eyes[self.eye].reshape(-1, 2)

    def knot_screen(self):
        return self.uv_to_screen(self.knot_content_uv())

    def free_mask(self):
        """Bool (n,): knots NOT constrained by data for the current eye.
        Free knots are fit extrapolation (or painted): the brush and the
        smoothers leave them alone so they cannot silently collect garbage;
        a direct grab promotes a knot back to constrained."""
        m = self.lattice.measured[self.eye]
        if m is None:
            return np.zeros(self.lattice.rows * self.lattice.cols, bool)
        return ~m.reshape(-1)

    def set_free(self, mask, free=True):
        lat = self.lattice
        if lat.measured[self.eye] is None:
            lat.measured[self.eye] = np.ones((lat.rows, lat.cols), bool)
        lat.measured[self.eye].reshape(-1)[mask] = not free

    def radius_screen(self, uv):
        p0 = self.uv_to_screen(uv.reshape(1, 2))[0]
        p1 = self.uv_to_screen((uv + np.array([self.brush, 0])).reshape(1, 2))[0]
        return max(4.0, float(np.linalg.norm(p1 - p0)))

    def draw_knots(self, img):
        pts = self.knot_screen()
        mags = np.linalg.norm(self.lattice.eyes[self.eye].reshape(-1, 2), axis=1)
        free = self.free_mask()
        for i, ((x, y), m) in enumerate(zip(pts, mags)):
            if not (0 <= x < self.size[0] and 0 <= y < self.size[1]):
                continue
            t = min(1.0, m / 0.004)
            color = (int(150 * (1 - t)), int(150 * (1 - t) + 80 * t), int(150 + 105 * t))
            r = 3
            if self.grab is not None and i == self.grab["knot"]:
                color, r = (0, 220, 255), 6
            elif self.grab is None and i == self.pick:
                color, r = (0, 255, 255), 5
            if free[i] and r == 3:
                # free (unmeasured / painted) knots: hollow and dim
                cv2.circle(img, (int(x), int(y)), r, (110, 110, 110), 1, cv2.LINE_AA)
            else:
                cv2.circle(img, (int(x), int(y)), r, color, -1, cv2.LINE_AA)
        if self.paint_mode:
            rr = self.radius_screen(self.screen_to_uv(*self.mouse))
            cv2.circle(img, self.mouse, int(rr), (80, 80, 255), 1, cv2.LINE_AA)
        # falloff circle at the hovered/grabbed knot
        idx = self.grab["knot"] if self.grab is not None else self.pick
        if idx is not None:
            cx, cy = pts[idx]
            if np.isfinite(cx) and np.isfinite(cy):
                rr = self.radius_screen(self.knot_content_uv()[idx])
                cv2.circle(img, (int(cx), int(cy)), int(rr), (0, 160, 255), 1, cv2.LINE_AA)
        if self.grab is not None:
            x0, y0 = self.grab["screen0"]
            cv2.line(img, (int(x0), int(y0)), self.mouse, (0, 220, 255), 1, cv2.LINE_AA)

    def refresh_state(self, force=False):
        now = time.time()
        if force or now - self.state_cache["at"] > 0.5:
            try:
                sf = self.settings.stream_frame()
                self.state_cache["calib"] = sf.get("calib", {}) or {}
                self.state_cache["gain"] = float((sf.get("distortion", {}) or {}).get("gain", 1.0))
                self.state_cache["diag"] = g.read_diagnostic() or {}
            except Exception as e:
                self.status = f"settings.json unreadable: {e}"
                self.status_at = now
            self.state_cache["at"] = now

    def hud(self, img):
        calib = self.state_cache["calib"]
        gain = self.state_cache["gain"]
        dmax, drms = self.lattice.stats(self.eye)
        yaw, pitch, roll = g.matrix_to_ypr_deg(self.R)
        lines = [
            f"eye {'LEFT' if self.eye == 0 else 'RIGHT'}   gain {gain:.2f}   capture {'ON' if calib.get('captureMode') else 'off'}"
            f"   blackout {'ON' if calib.get('blackout') else 'off'}   cam grid alpha {self.grid_opacity:.2f}"
            f"   hmd grid level {float(calib.get('patternBrightness', 1.0)):.2f}",
            f"map {self.lattice.cols}x{self.lattice.rows}  max {dmax:.5f} uv ({dmax * self.tex_w:.1f} px)  rms {drms:.5f}"
            f"   radius {self.brush:.3f} uv   view rot yaw {yaw:+.2f} pitch {pitch:+.2f} roll {roll:+.2f}"
            + (f"   free knots {int(self.free_mask().sum())}" if self.free_mask().any() else "")
            + ("   PAINT MODE" if self.paint_mode else ""),
        ]
        diag = self.state_cache.get("diag", {})
        if diag:
            drv = (f"driver: map {'ACTIVE' if diag.get('mapActive') else 'INACTIVE'} "
                   f"{diag.get('mapCols', 0)}x{diag.get('mapRows', 0)}  pass {'on' if diag.get('active') else 'OFF'}"
                   f"  frame {diag.get('frameCounter', 0)}  blackout {'ON' if diag.get('calibBlackout') else 'off'}")
        else:
            drv = "driver: no streamFrame block in diagnostic.json (old driver build?)"
        lines.append(drv)
        if self.status and time.time() - self.status_at < 6:
            lines.append(self.status)
        y = 22
        for line in lines:
            cv2.putText(img, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 3, cv2.LINE_AA)
            cv2.putText(img, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1, cv2.LINE_AA)
            y += 20
        if self.show_help:
            for line in __doc__.strip().splitlines()[-16:]:
                cv2.putText(img, line.rstrip(), (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 0), 3, cv2.LINE_AA)
                cv2.putText(img, line.rstrip(), (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 255, 200), 1, cv2.LINE_AA)
                y += 17

    # editing 
    #  all lattice math and file IO
    # happen in the main loop (a slow callback stalls the window's message
    # pump.)
    def on_mouse(self, event, x, y, flags, param):
        self.mouse = (x, y)
        self.mod_flags = flags
        if event == cv2.EVENT_LBUTTONDOWN:
            self.pending_stroke = ("down", x, y)
        elif event == cv2.EVENT_LBUTTONUP:
            self.pending_stroke = ("up", x, y)
        elif event == cv2.EVENT_MOUSEWHEEL:
            self.brush = min(0.5, self.brush * 1.15) if flags > 0 else max(0.01, self.brush / 1.15)

    def process_mouse(self):
        x, y = self.mouse
        pts = self.knot_screen()
        d2 = (pts[:, 0] - x) ** 2 + (pts[:, 1] - y) ** 2
        d2[~np.isfinite(d2)] = 1e18
        nearest = int(np.argmin(d2))
        self.pick = nearest if d2[nearest] < 30 ** 2 else None
        ev = self.pending_stroke
        self.pending_stroke = None
        if self.paint_mode:
            self.pick = None
            if ev is not None and ev[0] == "down":
                self.paint_down = True
            elif ev is not None and ev[0] == "up":
                self.paint_down = False
                self.dirty_at = time.time()
                self.flush(force=True)
            if self.paint_down:
                uv = self.screen_to_uv(x, y)
                inside = ((self.knot_uv - uv) ** 2).sum(axis=1) < self.brush ** 2
                if inside.any():
                    self.set_free(inside, free=not (self.mod_flags & cv2.EVENT_FLAG_SHIFTKEY))
            return
        if ev is not None and ev[0] == "down" and self.pick is not None:
            self.undo_stack.append(self.lattice.eyes[self.eye].copy())
            if len(self.undo_stack) > 50:
                self.undo_stack.pop(0)
            i = self.pick
            self.grab = {"knot": i, "start": self.lattice.eyes[self.eye].copy(),
                         "uv0": self.knot_content_uv()[i].copy(), "screen0": (float(pts[i][0]), float(pts[i][1]))}
            if self.free_mask()[i]:
                self.set_free(np.array([i]), free=False)   # a direct grab constrains the knot
        elif ev is not None and ev[0] == "up" and self.grab is not None:
            self.grab = None
            self.dirty_at = time.time()
            self.flush(force=True)
        if self.grab is not None:
            # dot follows the cursor: content at the grabbed knot moves from
            # uv0 to the uv under the cursor -> disp changes by -(delta)
            uv_now = self.screen_to_uv(x, y)
            delta = (uv_now - self.grab["uv0"]).copy()
            # drag modifiers: Shift = vertical only, Ctrl = horizontal only,
            # Alt = quantize to snap_px panel-pixel steps
            mods = self.mod_flags
            if mods & cv2.EVENT_FLAG_SHIFTKEY:
                delta[0] = 0.0
            if mods & cv2.EVENT_FLAG_CTRLKEY:
                delta[1] = 0.0
            if mods & cv2.EVENT_FLAG_ALTKEY:
                step_u = self.snap_px / self.tex_w
                step_v = self.snap_px / self.tex_h
                delta[0] = round(delta[0] / step_u) * step_u
                delta[1] = round(delta[1] / step_v) * step_v
            i = self.grab["knot"]
            dk = ((self.knot_uv - self.knot_uv[i]) ** 2).sum(axis=1)
            w = np.exp(-dk / (2 * self.brush ** 2))
            w[w < 0.02] = 0
            w[self.free_mask()] = 0.0   # falloff never drags free knots along
            w[i] = 1.0
            lat = self.grab["start"].reshape(-1, 2).copy()
            lat -= w[:, None] * delta[None, :]
            self.lattice.eyes[self.eye] = lat.reshape(self.lattice.rows, self.lattice.cols, 2)
            self.lattice.source = "manual"
            self.dirty_at = time.time()

    def flush(self, force=False):
        if self.dirty_at is None:
            return
        now = time.time()
        if force or now - self.last_write > 0.25:
            self.settings.set_map(self.lattice.to_json())
            self.last_write = now
            self.dirty_at = None

    def reset_under_cursor(self):
        self.undo_stack.append(self.lattice.eyes[self.eye].copy())
        uv = self.screen_to_uv(*self.mouse)
        d2 = ((self.knot_uv - uv) ** 2).sum(axis=1)
        w = np.exp(-d2 / (2 * self.brush ** 2))
        lat = self.lattice.eyes[self.eye].reshape(-1, 2)
        lat *= (1 - w)[:, None]
        self.set_free(w > 0.5, free=False)   # a reset knot is yours again
        self.dirty_at = time.time()
        self.flush(force=True)

    def undo(self):
        if not self.undo_stack:
            self.set_status("nothing to undo")
            return
        self.lattice.eyes[self.eye] = self.undo_stack.pop()
        self.dirty_at = time.time()
        self.flush(force=True)

    def nudge_rotation(self, yaw=0.0, pitch=0.0, roll=0.0):
        dR = g.rotvec_to_matrix([math.radians(pitch), math.radians(yaw), math.radians(roll)])
        self.R = self.R @ dR
        self.rebuild_maps()
        self.save_rotation()

    def alignment_run(self):
        """6 bit mini sweep to measure the camera rotation; updates the view."""
        import gxr_sweep
        calib_before = self.settings.get_calib()
        try:
            self.settings.set_calib(blackout=False, captureMode=False)
            show, grab, done = gxr_sweep.make_driver_io(self.cap, self.link, self.eye, 6, 2, 6)
            sweep = gxr_sweep.run_sweep(show, grab, done, self.eye, 6)
            dirs = self.cam.pixels_to_dirs(sweep["pix"])
            R, _ = g.solve_rotation(self.panel, sweep["uv"], dirs)
            self.R = R
            self.rebuild_maps()
            self.save_rotation()
            yaw, pitch, roll = g.matrix_to_ypr_deg(R)
            self.set_status(f"alignment: yaw {yaw:+.2f} pitch {pitch:+.2f} roll {roll:+.2f} deg. " +
                            gxr_sweep.alignment_hint(yaw, pitch, roll))
        except Exception as e:
            self.set_status(f"alignment failed: {e}")
        finally:
            self.settings.set_calib(blackout=bool(calib_before.get("blackout", False)),
                                    captureMode=bool(calib_before.get("captureMode", False)), pattern=-1)

    def save_profile(self):
        self.flush(force=True)
        sf = self.settings.stream_frame()
        name = f"Manual map {time.strftime('%Y-%m-%d %H:%M')}"
        path = g.save_profile(g.build_profile(sf, self.lattice, name, {"source": "manual"}), stem="gxr-map")
        self.set_status(f"saved {path}")

    def switch_eye(self):
        self.flush(force=True)
        self.save_rotation()
        self.eye = 1 - self.eye
        self.R = self.load_rotation()
        self.load_panel()
        self.Kv = self.make_view(self.args.fov)
        self.rebuild_maps()
        self.load_lattice()
        self.calib_sync()
        self.set_status(f"now editing the {'LEFT' if self.eye == 0 else 'RIGHT'} eye")

    # auto match (hue grid servo)
    def view_angles(self):
        """Per-pixel (aH, aV) in degrees of the rectified view, cached."""
        if self.angle_cache is None:
            w, h = self.size
            xs, ys = np.meshgrid(np.arange(w, dtype=np.float64), np.arange(h, dtype=np.float64))
            dx = (xs - self.Kv[0, 2]) / self.Kv[0, 0]
            dy = (ys - self.Kv[1, 2]) / self.Kv[1, 1]
            self.angle_cache = (np.degrees(np.arctan(dx)), np.degrees(np.arctan(-dy)))
        return self.angle_cache

    def detect_grid_errors(self, view_bgr, max_samples=25000):
        """Find the headset-drawn hue grid in the rectified view and return
        (uv_obs, uv_tgt): where each detected line point appears vs where its
        square should be. Only the component normal to the line is measured;
        the tangential part of the returned delta is zero by construction."""
        hsv = cv2.cvtColor(view_bgr, cv2.COLOR_BGR2HSV)
        H, S, V = hsv[:, :, 0].astype(np.float64), hsv[:, :, 1], hsv[:, :, 2]
        # colored lines: saturated and bright. the desaturated world, the
        # grey border and the white cross all fail the S test
        mask = (S > 110) & (V > 60)
        ys, xs = np.nonzero(mask)
        if len(ys) < 500:
            return None, None, "too few colored pixels (capture mode on? patternBrightness up? exposure?)"
        if len(ys) > max_samples:
            sel = np.random.default_rng(0).choice(len(ys), max_samples, replace=False)
            ys, xs = ys[sel], xs[sel]
        aH_map, aV_map = self.view_angles()
        aH, aV = aH_map[ys, xs], aV_map[ys, xs]
        # hue -> box class (shader hue = frac(n / 6), OpenCV hue 0..179)
        cls = np.round(H[ys, xs] / 180.0 * 6.0).astype(int) % 6
        # box estimate from the square metric max(|aH|, |aV|)
        m = np.maximum(np.abs(aH), np.abs(aV))
        n_est = m / self.step_deg
        n = cls + 6 * np.round((n_est - cls) / 6.0)
        ok = (n >= 1) & (np.abs(n - n_est) < 0.45)
        if ok.sum() < 500:
            return None, None, "lines found but could not be identified (step mismatch? huge error?)"
        aH, aV, n = aH[ok], aV[ok], n[ok]
        a = n * self.step_deg
        # closest point on the ideal square: move the dominant axis onto it
        h_dom = np.abs(aH) >= np.abs(aV)
        tH = np.where(h_dom, np.sign(aH) * a, aH)
        tV = np.where(h_dom, aV, np.sign(aV) * a)
        uv_obs = self.panel.dirs_to_uv(self.panel.angles_to_dirs(aH, aV))
        uv_tgt = self.panel.dirs_to_uv(self.panel.angles_to_dirs(tH, tV))
        inside = np.all((uv_obs > 0.005) & (uv_obs < 0.995) & (uv_tgt > 0.005) & (uv_tgt < 0.995), axis=1)
        if inside.sum() < 500:
            return None, None, "grid detected only outside the eye texture?"
        return uv_obs[inside], uv_tgt[inside], None

    def auto_match_iteration(self, damping=0.6):
        """One servo step: measure grid error, nudge the lattice toward zero."""
        calib = self.settings.get_calib()
        if not calib.get("captureMode"):
            self.settings.set_calib(captureMode=True)
            self.refresh_state(force=True)
            time.sleep(1.2)
        if self.settings.get_gain() < 0.5:
            self.set_status("auto match needs gain 1 (press g); the loop watches the corrected grid")
            return False
        frame = g.grab_average(self.cap, 3, gray=False)
        view = cv2.remap(frame.astype(np.uint8), self.maps[0], self.maps[1], cv2.INTER_LINEAR)
        uv_obs, uv_tgt, err = self.detect_grid_errors(view)
        if err:
            self.set_status(f"auto match: {err}")
            return False
        # never bake view misalignment into the map: remove the gauge first
        gauge, delta = self.split_view_gauge(uv_obs, uv_tgt - uv_obs)
        mag = np.linalg.norm(delta, axis=1)
        med_px = float(np.median(mag)) * self.tex_w
        # robust trim: drop the worst 2% (misclassified pixels, bleed)
        keep = mag < np.quantile(mag, 0.98)
        uv_obs, delta = uv_obs[keep], delta[keep]
        # content at uv_obs must move to uv_tgt -> disp changes by -delta
        dlat = g.DisplacementLattice(self.lattice.cols, self.lattice.rows)
        dlat.fit(self.eye, uv_obs, -delta * damping, smooth=0.3, prior=1e-3, taper=2.0, fade=1.0)
        self.lattice.eyes[self.eye] += dlat.eyes[self.eye]
        self.lattice.source = "grid-auto"
        self.dirty_at = time.time()
        self.flush(force=True)
        self.set_status(f"auto match: {len(uv_obs)} line points, median error {med_px:.1f} px, nudged (damping {damping})")
        return med_px

    def auto_match_loop(self, iters=5, settle=1.6):
        self.undo_stack.append(self.lattice.eyes[self.eye].copy())
        last = None
        for i in range(iters):
            r = self.auto_match_iteration()
            if r is False:
                return
            if last is not None and r > last * 0.95 and r < 1.5:
                self.set_status(f"auto match: converged at median {r:.1f} px after {i + 1} iterations")
                return
            last = r
            # wait for the driver to rebake and the camera to see it
            t0 = time.time()
            while time.time() - t0 < settle:
                ok, frame = self.cap.read()
                if ok:
                    v = cv2.remap(frame, self.maps[0], self.maps[1], cv2.INTER_LINEAR)
                    self.hud(v)
                    cv2.imshow("gxr overlay", v)
                cv2.waitKey(1)
        self.set_status(f"auto match: stopped after {iters} iterations, median {last:.1f} px (z undoes the whole run)")

    def panel_outline_screen(self):
        """The full uv square projected into the view (green ground truth)."""
        e = np.linspace(0, 1, 40)
        border = np.concatenate([
            np.stack([e, np.zeros_like(e)], 1), np.stack([np.ones_like(e), e], 1),
            np.stack([e[::-1], np.ones_like(e)], 1), np.stack([np.zeros_like(e), e[::-1]], 1)])
        return self.uv_to_screen(border)

    def draw_align(self, view):
        """Live translation-alignment readout: the bright (white pattern)
        panel region's margins against the full panel, in percent per side.
        Equal L/R and T/B margins = the camera pupil is centered on the eye
        box laterally. A side marked ? touches the camera image border, so
        its true margin is unknown (camera FOV clips before the panel edge).
        """
        gray = cv2.cvtColor(view, cv2.COLOR_BGR2GRAY)
        thr = max(40, int(gray.max()) * 0.4)
        mask = gray > thr
        cols = np.where(mask.any(axis=0))[0]
        rows = np.where(mask.any(axis=1))[0]
        lines = []
        if len(cols) < 20 or len(rows) < 20:
            lines.append("align: panel not visible (white pattern on? blackout off? exposure?)")
        else:
            x0, x1, y0, y1 = int(cols[0]), int(cols[-1]), int(rows[0]), int(rows[-1])
            cv2.rectangle(view, (x0, y0), (x1, y1), (0, 220, 255), 1)
            cy, cx = (y0 + y1) // 2, (x0 + x1) // 2
            uL = self.screen_to_uv(x0, cy)[0]
            uR = self.screen_to_uv(x1, cy)[0]
            vT = self.screen_to_uv(cx, y0)[1]
            vB = self.screen_to_uv(cx, y1)[1]
            h, w = mask.shape
            mL = "?" if x0 <= 2 else f"{uL * 100:.1f}"
            mR = "?" if x1 >= w - 3 else f"{(1 - uR) * 100:.1f}"
            mT = "?" if y0 <= 2 else f"{vT * 100:.1f}"
            mB = "?" if y1 >= h - 3 else f"{(1 - vB) * 100:.1f}"
            cov = (min(uR, 1) - max(uL, 0)) * (min(vB, 1) - max(vT, 0)) * 100
            lines.append(f"align margins %  L {mL}  R {mR}  T {mT}  B {mB}   coverage ~{cov:.0f}%")
            lines.append("translate to equalize L/R and T/B; for depth: pull back until thin, even")
            lines.append("margins just appear on all sides (~ design eye relief), then stop")
        pts = self.panel_outline_screen()
        self.polyline(view, pts, (0, 200, 0), 1, closed=True)
        y = self.size[1] - 14 - 20 * len(lines)
        for line in lines:
            cv2.putText(view, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 0), 3, cv2.LINE_AA)
            cv2.putText(view, line, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (100, 255, 100), 1, cv2.LINE_AA)
            y += 20

    def split_view_gauge(self, uv_obs, delta):
        """The grid detection measures only across lines, so a global view
        misalignment (stale rotation: uv shift + roll) shows up as large,
        axis-aligned, uniform arrows that are NOT map error. Solve the 3
        gauge parameters from the axis-decomposed samples, subtract them,
        and return (gauge_params, residual_delta). Consistent with the
        sweep, which removes the same 3 degrees of freedom as camera
        rotation."""
        au = np.abs(delta[:, 0]) >= np.abs(delta[:, 1])
        u = uv_obs[:, 0] - 0.5
        v = uv_obs[:, 1] - 0.5
        A = np.zeros((len(uv_obs), 3))
        t = np.zeros(len(uv_obs))
        A[au, 0] = 1
        A[au, 2] = -v[au]
        t[au] = delta[au, 0]
        A[~au, 1] = 1
        A[~au, 2] = u[~au]
        t[~au] = delta[~au, 1]
        g, *_ = np.linalg.lstsq(A, t, rcond=None)
        resid = delta.copy()
        resid[au, 0] -= g[0] - g[2] * v[au]
        resid[~au, 1] -= g[1] + g[2] * u[~au]
        return g, resid

    def measure_errors(self):
        """One detection pass without touching the map: arrows obs -> target."""
        calib = self.settings.get_calib()
        if not calib.get("captureMode"):
            self.set_status("error view needs capture mode (press c)")
            return None
        frame = g.grab_average(self.cap, 3, gray=False)
        view = cv2.remap(frame.astype(np.uint8), self.maps[0], self.maps[1], cv2.INTER_LINEAR)
        uv_obs, uv_tgt, err = self.detect_grid_errors(view)
        if err:
            self.set_status(f"error view: {err}")
            return None
        gauge, resid = self.split_view_gauge(uv_obs, uv_tgt - uv_obs)
        shift_px = (gauge[0] * self.tex_w, gauge[1] * self.tex_h)
        scr_o = self.uv_to_screen(uv_obs)
        scr_t = self.uv_to_screen(uv_obs + resid)
        mag_px = np.linalg.norm(resid, axis=1) * self.tex_w
        # thin out for drawing
        if len(scr_o) > 500:
            sel = np.random.default_rng(0).choice(len(scr_o), 500, replace=False)
            scr_o, scr_t, mag_show = scr_o[sel], scr_t[sel], mag_px[sel]
        else:
            mag_show = mag_px
        gmag = math.hypot(shift_px[0], shift_px[1])
        note = f"  <-- view misaligned, press a" if gmag > 5 else ""
        self.set_status(f"map error (view gauge removed): median {np.median(mag_px):.1f} px, "
                        f"p95 {np.quantile(mag_px, 0.95):.1f} px, max {mag_px.max():.1f} px  |  "
                        f"view offset ({shift_px[0]:+.1f}, {shift_px[1]:+.1f}) px, roll ~{math.degrees(gauge[2]):+.2f} deg{note}")
        return {"o": scr_o, "t": scr_t, "mag": mag_show, "at": time.time()}

    def draw_errors(self, img):
        ev = self.error_vis
        for (x0, y0), (x1, y1), m in zip(ev["o"], ev["t"], ev["mag"]):
            if not (np.isfinite(x0) and np.isfinite(x1)):
                continue
            # amplified 8x so pixel-scale errors are visible
            ex, ey = x0 + (x1 - x0) * 8, y0 + (y1 - y0) * 8
            t = min(1.0, m / 20.0)
            color = (int(255 * (1 - t)), int(255 * (1 - t)), 255)
            cv2.arrowedLine(img, (int(x0), int(y0)), (int(ex), int(ey)), color, 1, cv2.LINE_AA, tipLength=0.25)

    # main loop 
    def run(self):
        while True:
            ok, frame = self.cap.read()
            if not ok:
                time.sleep(0.02)
                continue
            view = cv2.remap(frame, self.maps[0], self.maps[1], cv2.INTER_LINEAR, borderValue=(40, 40, 40))
            grid = self.grid_layer()
            if self.grid_opacity > 0:
                mask = grid.any(axis=2)
                blended = cv2.addWeighted(view, 1 - self.grid_opacity, grid, self.grid_opacity, 0)
                view[mask] = blended[mask]
            self.process_mouse()
            if self.show_knots:
                self.draw_knots(view)
            if self.error_vis is not None:
                self.draw_errors(view)
            if self.align_assist:
                self.draw_align(view)
            self.refresh_state()
            self.hud(view)
            cv2.imshow("gxr overlay", view)
            self.flush()
            key = cv2.waitKeyEx(1)
            if key == -1:
                continue
            k = key & 0xFF if key < 256 else key
            if k in (ord('q'), 27):
                break
            elif k == ord('c'):
                cur = bool(self.settings.get_calib().get("captureMode", False))
                self.settings.set_calib(captureMode=not cur)
                self.refresh_state(force=True)
            elif k == ord('b'):
                cur = bool(self.settings.get_calib().get("blackout", False))
                self.settings.set_calib(blackout=not cur)
                self.refresh_state(force=True)
            elif k == ord('g'):
                gain = self.settings.get_gain()
                self.settings.set_gain(0.0 if gain > 0.5 else 1.0)
                self.refresh_state(force=True)
            elif k in (ord(';'), ord("'")):
                cur = float(self.settings.get_calib().get("patternBrightness", 1.0))
                cur = max(0.05, min(1.0, cur + (0.05 if k == ord("'") else -0.05)))
                self.settings.set_calib(patternBrightness=round(cur, 3))
                self.refresh_state(force=True)
            elif k == ord('e'):
                self.switch_eye()
            elif k == ord('k'):
                self.show_knots = not self.show_knots
            elif k == ord('h'):
                self.show_help = not self.show_help
            elif k == ord('['):
                self.grid_opacity = max(0.0, self.grid_opacity - 0.1)
            elif k == ord(']'):
                self.grid_opacity = min(1.0, self.grid_opacity + 0.1)
            elif k == ord('-'):
                self.brush = max(0.01, self.brush / 1.25)
            elif k == ord('='):
                self.brush = min(0.5, self.brush * 1.25)
            elif k == ord('z'):
                self.undo()
            elif k == ord('r'):
                self.reset_under_cursor()
            elif k == ord('f'):
                # local smooth under the cursor (brush-weighted anchor)
                self.undo_stack.append(self.lattice.eyes[self.eye].copy())
                uv = self.screen_to_uv(*self.mouse)
                d2 = ((self.knot_uv - uv) ** 2).sum(axis=1)
                gaussw = np.exp(-d2 / (2 * self.brush ** 2))
                # outside the brush the anchor must dominate the (global)
                # smoothness term, or "local" leaks: ~free inside, pinned
                # hard outside
                anchor = 0.05 + 4000.0 * (1.0 - gaussw) ** 2
                anchor[self.free_mask()] = 1e6
                ch, r0, r1 = self.lattice.smooth(self.eye, amount=0.8, anchor=anchor)
                self.lattice.source = "manual"
                self.dirty_at = time.time()
                self.flush(force=True)
                self.set_status(f"smoothed under cursor: max change {ch * self.tex_w:.1f} px, "
                                f"roughness {r0:.5f} -> {r1:.5f} (f again for more, z to undo)")
            elif k == ord('F'):
                # global smooth of the eye (line-smooth analog)
                self.undo_stack.append(self.lattice.eyes[self.eye].copy())
                anchor = np.ones(self.lattice.rows * self.lattice.cols)
                anchor[self.free_mask()] = 1e4
                ch, r0, r1 = self.lattice.smooth(self.eye, amount=0.6, anchor=anchor)
                self.lattice.source = "manual"
                self.dirty_at = time.time()
                self.flush(force=True)
                self.set_status(f"smoothed the whole eye: max change {ch * self.tex_w:.1f} px, "
                                f"roughness {r0:.5f} -> {r1:.5f} (F again for more, z to undo)")
            elif k == ord('x'):
                self.undo_stack.append(self.lattice.eyes[self.eye].copy())
                self.lattice.eyes[self.eye] = self.baseline.eyes[self.eye].copy()
                self.lattice.source = self.baseline.source
                self.dirty_at = time.time()
                self.flush(force=True)
                self.set_status("restored this eye to the session baseline (state at overlay start)")
            elif k == ord('R'):
                self.undo_stack.append(self.lattice.eyes[self.eye].copy())
                self.lattice.eyes[self.eye][:] = 0
                self.lattice.measured[self.eye] = None   # all constrained again
                self.dirty_at = time.time()
                self.flush(force=True)
                self.set_status("eye lattice reset to identity (all knots constrained)")
            elif k == ord('P'):
                self.set_free(np.ones(self.lattice.rows * self.lattice.cols, bool), free=False)
                self.dirty_at = time.time()
                self.flush(force=True)
                self.set_status("all knots constrained (free states cleared); values untouched")
            elif k == ord('s'):
                self.save_profile()
            elif k == ord('a'):
                self.alignment_run()
            elif k == ord('p'):
                self.paint_mode = not self.paint_mode
                self.paint_down = False
                if self.paint_mode:
                    self.set_status("paint: stroke = mark knots FREE (excluded), Shift+stroke = lock "
                                    "them back, wheel = radius, p = done")
                else:
                    self.set_status("paint off")
            elif k in (ord('l'), ord('1')):
                self.align_assist = not self.align_assist
                if self.align_assist:
                    self.settings.set_calib(pattern=1, eye=self.eye)
                    self.set_status("align assist on: white pattern up, equalize the margins")
                else:
                    self.settings.set_calib(pattern=-1)
            elif k == ord('v'):
                self.error_vis = None if self.error_vis is not None else self.measure_errors()
            elif k == ord('m'):
                self.undo_stack.append(self.lattice.eyes[self.eye].copy())
                self.auto_match_iteration()
            elif k == ord('M'):
                self.auto_match_loop()
            elif key in (2424832, 65361):   # left
                self.nudge_rotation(yaw=-0.2)
            elif key in (2555904, 65363):   # right
                self.nudge_rotation(yaw=0.2)
            elif key in (2490368, 65362):   # up
                self.nudge_rotation(pitch=-0.2)
            elif key in (2621440, 65364):   # down
                self.nudge_rotation(pitch=0.2)
            elif k == ord(','):
                self.nudge_rotation(roll=-0.2)
            elif k == ord('.'):
                self.nudge_rotation(roll=0.2)
        self.flush(force=True)
        self.save_rotation()
        self.cap.release()
        cv2.destroyAllWindows()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eye", type=lambda s: 0 if s.lower() in ("0", "l", "left") else 1, default=0)
    ap.add_argument("--fov", type=float, default=None, help="virtual view half FOV in degrees (default: fit the eye texture)")
    ap.add_argument("--width", type=int, default=1400)
    ap.add_argument("--height", type=int, default=1100)
    ap.add_argument("--step", type=float, default=None, help="grid step in degrees (default: the driver's eyeGaze.gridAngularDeg, 2.5)")
    ap.add_argument("--max-deg", type=float, default=60.0)
    ap.add_argument("--grid-opacity", type=float, default=0.6)
    ap.add_argument("--cols", type=int, default=33)
    ap.add_argument("--rows", type=int, default=33)
    ap.add_argument("--profile", metavar="JSON",
                    help="load the displacement map from a saved profile file first (restores a sweep)")
    args = ap.parse_args()
    if cv2 is None:
        print("opencv-python is required: pip install -r requirements.txt")
        return 2
    OverlayApp(args).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
