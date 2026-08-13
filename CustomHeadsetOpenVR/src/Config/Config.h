#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <tuple>

struct ConfigColor{
	double r = 1.0;
	double g = 1.0;
	double b = 1.0;
};

struct HiddenAreaMeshConfig {
	bool enable = false;
	bool testMode = false;
	int detailLevel = 8;
	double radiusTopOuter = 0.25;
	double radiusTopInner = 0.25;
	double radiusBottomInner = 0.25;
	double radiusBottomOuter = 0.25;

	constexpr bool operator==(const HiddenAreaMeshConfig& other) const {
		return std::tie(this->enable, this->testMode, this->detailLevel, this->radiusTopOuter, this->radiusTopInner, this->radiusBottomInner, this->radiusBottomOuter) ==
		       std::tie(other.enable, other.testMode, other.detailLevel, other.radiusTopOuter, other.radiusTopInner, other.radiusBottomInner, other.radiusBottomOuter);
	}
	constexpr bool operator!=(const HiddenAreaMeshConfig& other) const {
		return !(this->operator==(other));
	}
};

struct StationaryDimmingConfig{
	// if the display should be dimmed when the headset is stationary
	bool enable = true;
	// the angle that the headset has to rotate for it to be considered as moved
	double movementThreshold = 0.4;
	// the time in seconds that the headset has to be stationary for it to be dimmed
	double movementTime = 15.0;
	// the amount to dim the display to when stationary
	double dimBrightnessPercent = 2;
	// the amount per second to dim the display when stationary
	double dimSeconds = 10;
	// the amount per second to brighten the display when moving
	double brightenSeconds = 5;
};


// one control point of the spline distortion curve
struct StreamFrameDistortionPoint{
	// radius, 0 at the optical center, roughly 0.5 at the edge midpoints
	double r = 0;
	// radial scale multiplier at that radius, 1.0 = no change
	double scale = 1;
};

// diagnostic band that limits the distortion correction to a radius range so
// one region of the curve can be tuned against untouched surroundings
struct StreamFrameAnnulusConfig{
	bool enable = false;
	double rMin = 0.0;
	double rMax = 0.75;
	// width of the smooth ramp at both edges of the band
	double feather = 0.05;
};

// interactive in-headset distortion tuner: the human eye as the null
// detector. while enabled the driver takes over the distortion curves with a
// per-band working copy edited live from the controllers (joystick y adjusts
// the highlighted band's scale, a/b step bands outward/inward, x cycles
// linked/left/right eye editing, y resets the band, holding either grip
// saves an importable profile). the tuner forces the angular grid and warped
// overlays on so the nulling task is ready the moment the toggle flips.
struct StreamFrameDistortionTuneConfig{
	bool enable = false;
	// scale units per second at full stick deflection (response is squared,
	// so half deflection moves at a quarter rate for fine work)
	double rate = 0.08;
	// band radii, in the same aspect-corrected radius space as the spline r
	std::vector<double> bands = {0.15, 0.22, 0.30, 0.38, 0.46, 0.55, 0.65};
	// stepped adjustment: when > 0, the stick applies exactly this scale
	// step every 100ms while deflected past halfway, instead of the analog
	// rate. deterministic fine nulling ("one click at a time").
	double stepSize = 0.0;
	// opacity of the band highlight ring (0 hides it entirely)
	double ringOpacity = 0.55;
	// force the angular grid + warped overlays on while tuning. off = the
	// tuner leaves the overlays to the user's own toggles (e.g. tuning
	// against real game content, or the world-locked grid variant).
	bool forceGrid = true;
	// band segments for the tuner session: 1 = radial editing as before,
	// 4 or 8 adds a segment walk (joystick click) so each band can be
	// nudged per angular sector. the ALL position (walk start) still
	// edits the whole band; segments carry deltas on top of it.
	// tune segments last: center first, radial bands second — a wrong
	// center masquerades as exactly the asymmetry segments would absorb.
	int segments = 1;
	// per-band segment counts, one entry per band (inner to outer). outer
	// bands cover far more circumference, so they can carry far more
	// segments than inner ones (e.g. 4,4,8,8,12,16,16). empty = uniform
	// `segments` everywhere; shorter than the band list = last entry
	// repeats; entries clamp to 1..32. the tuner flattens whatever layout
	// into uniform max-count segment curves on save, so profiles and the
	// baked lut are unchanged in shape.
	std::vector<int> segmentLayout = {};
};

// center-offset tuning mode: distinct from the band tuner and used
// independently. while enabled the distortion is replaced by a small
// "breathing" radial pulse (sinusoidal k1) whose stationary point makes the
// currently configured optical center directly visible; the sticks then
// drag it onto the lens's true center (the fringe-free sharpest point of
// the fine grid). results are the centerOffset values, saved independently.
struct StreamFrameCenterTuneConfig{
	bool enable = false;
	// amplitude of the breathing pulse (k1 peak). 0.05 = +-1.25% scale at r=0.5
	double breatheAmp = 0.05;
};

// one distortion curve: k1/k2 polynomial coefficients and/or spline points,
// which of the two is evaluated follows the global distortion mode
struct StreamFrameCurve{
	double k1 = 0;
	double k2 = 0;
	std::vector<StreamFrameDistortionPoint> points = {};
};

struct StreamFrameDistortionConfig{
	// "k1k2" evaluates 1 + k1 r^2 + k2 r^4, "spline" interpolates the points
	std::string mode = "k1k2";
	// spline control points of the base curve, sorted by r internally. flat
	// outside the range. the base k1/k2 live at the streamFrame top level.
	std::vector<StreamFrameDistortionPoint> points = {};
	// global multiplier on the correction: baked scale becomes
	// 1 + gain * (scale - 1). gain 1 = the curve as authored, 0 = off,
	// -1 = the exact inverse. one knob for the perceptual 1d search:
	// sweep gain while watching the warped angular grid during a slow
	// head rotation and keep whatever swims least (settles curve sign
	// AND amplitude in one pass, scaling out any measurement bias).
	double gain = 1.0;
	// separate curves per eye and/or per axis. per axis blends a horizontal and
	// a vertical curve around the ring, capturing elliptic/astigmatic error.
	bool perEye = false;
	bool perAxis = false;
	// named curves used when the toggles are active. expected keys:
	// perEye: "left", "right". perAxis: "horizontal", "vertical".
	// both: "leftHorizontal", "leftVertical", "rightHorizontal", "rightVertical".
	// a missing key falls back to the base curve.
	std::map<std::string, StreamFrameCurve> curves = {};
	// angular band segments: 1 = purely radial curves (default). 2..32
	// splits every band into that many angular segments with their own
	// scale, interpolated periodically around the ring — positional
	// correction for top/bottom/nasal/temporal asymmetry that radial
	// bands cannot express. segment curves live in `curves` under keys
	// "left#0".."left#N-1" / "right#0".. and fall back to the plain
	// per-eye curve when missing. segments > 1 takes precedence over
	// perAxis (it is a superset of the elliptic blend).
	int segments = 1;
	StreamFrameAnnulusConfig annulus = {};
	StreamFrameDistortionTuneConfig tune = {};
	StreamFrameCenterTuneConfig centerTune = {};
};

struct StreamFrameCASConfig{
	// contrast adaptive sharpening applied before encoding
	bool enable = false;
	// 0 to 1
	double strength = 0.5;
	// per-eye override: when enabled, strengthLeft/strengthRight replace the
	// shared strength. lets one eye be sharpened harder (e.g. masking mild
	// off-axis lens blur from facial asymmetry) without over-sharpening the
	// good eye.
	bool perEye = false;
	double strengthLeft = 0.5;
	double strengthRight = 0.5;
};

// fade the streamed frames to black when the headset has not moved for a
// while, e.g. left on a desk with SteamVR running. uniform full fade, so no
// uneven oled wear. brightness returns quickly once movement is detected.
struct StreamFrameDimmingConfig{
	bool enable = false;
	// the angle in degrees that the headset has to rotate to count as moved
	double movementThreshold = 0.4;
	// seconds of stillness before dimming starts
	double movementTime = 15.0;
	// seconds to fade fully to black
	double dimSeconds = 10.0;
	// seconds to fade back to full brightness on movement
	double brightenSeconds = 1.0;
};

struct StreamFrameConfig{
	// process direct mode layer textures before the streaming driver consumes them
	bool enable = false;
	// saturation with 50 being normal, same semantics as customShader.saturation
	double saturation = 50;
	// contrast with 50 being normal, same semantics as customShader.contrast
	double contrast = 50;
	// the point from 0-100% of white that the contrast is centered around
	double contrastMidpoint = 50;
	// if the contrast should be done in linear space instead of gamma
	bool contrastLinear = false;
	// gamma of the output, 2.2 is neutral
	double gamma = 2.2;
	// per channel tint multiplier
	ConfigColor colorMultiplier = {};
	// 3x3 linear rgb color matrix, row major. active when exactly 9 values.
	std::vector<double> srgbMatrix = {};
	// FXAA-class single pass AA integrated into the layer shader, applied
	// BEFORE CAS so sharpening acts on resolved edges (off by default:
	// costs up to ~8 extra taps per pixel on edges and softens text
	// slightly; intended for titles with heavy specular/geometry shimmer)
	// 0 off, 1 fast (in-pass, CAS sharpens raw neighbors around the AA
	// resolved center), 2 quality (separate FXAA pre-pass into an fx
	// intermediate; CAS then sees fully resolved neighborhoods, at the
	// cost of one extra full-region pass and one extra scratch texture)
	int fxaaMode = 0;
	StreamFrameCASConfig cas = {};
	// add low amplitude noise before encoding to reduce banding in dark scenes
	bool dither = false;
	StreamFrameDimmingConfig stationaryDimming = {};
	// radial distortion pre perturbation, applied to the streamed eye images to
	// compensate an imperfect distortion profile on the standalone headset.
	double k1 = 0;
	double k2 = 0;
	StreamFrameDistortionConfig distortion = {};
	// optical center offset from the texture center, in uv units, per eye
	double centerOffsetXLeft = 0;
	double centerOffsetXRight = 0;
	double centerOffsetY = 0;
	// per-eye whole-image alignment shift (prism correction), in fractions
	// of the eye's image (bounds-normalized uv). corrects the RELATIVE
	// alignment between the two eyes' images when an eye sits off its lens
	// axis (the lens then acts as a weak prism and fusion strains — the
	// vertical direction especially, fusional range there is tiny).
	// positive h moves that eye's image right, positive v moves it up.
	// values are small: 0.002 is already a strong vertical correction.
	struct {
		double leftH = 0;
		double leftV = 0;
		double rightH = 0;
		double rightV = 0;
	} alignment = {};
	// skip the color adjustment while the dashboard is open, in case the
	// compositor shader replacement also applies it to the flattened scene in
	// that state. off by default: the recommended setup is to leave the custom
	// shader disabled or neutral for streamed headsets and let this pass be the
	// single source of truth in every state. does not affect cas/dither.
	bool skipColorWhileDashboardOpen = false;
	// process during SubmitLayer (using the previous frame's sync texture)
	// instead of during Present. try this if Present time processing has no
	// visible effect because the driver already consumes the layer at submit.
	bool processAtSubmitLayer = false;
	// gaze consumption (eye tracking tap must be receiving valid data).
	// debugRing draws a small ring at the mapped gaze point per eye — the
	// live calibration tool for the direction->viewport mapping that the
	// dynamic pupil swim pass will reuse. tanHalfFov are the assumed
	// symmetric projection half-angle tangents used for the mapping; tune
	// until the ring lands where you look (live reload, shader hot reload).
	struct {
		bool debugRing = false;
		// fallback mapping only (used when the HMD display component's real
		// projection frusta are unavailable)
		double tanHalfFovX = 1.19;
		double tanHalfFovY = 1.19;
		// lead the gaze by extrapolating recent gaze motion this many ms
		// forward, compensating capture->link->publish latency. 0 disables.
		double predictionMs = 30;
		// overlay a calibration grid: the straight-line reference for pupil
		// swim calibration. mode "uv" = lines every 0.1 uv; mode "angular"
		// = lines every gridAngularDeg degrees of visual angle computed
		// from the real projection frusta (sboy-style distortion photos:
		// each rendered line has a known angular position, so a photo
		// through the lens directly measures distortion error)
		bool debugGrid = false;
		std::string gridMode = "uv";
		double gridAngularDeg = 2.5;
		// world-locked fixation dot for VOR-based swim probing: latched to
		// the current view direction when enabled (toggle off/on to
		// re-center). the user fixates the dot and slowly rotates their
		// head in place; VOR keeps the eye on target, so any systematic
		// gaze-vs-dot residual measures the optics/tracking chain.
		bool calibDot = false;
		// one-switch probe capture for scoring runs: acts as calibDot +
		// swimProbe + overlayWarped together, so an A/B scoring session is
		// a single toggle in the GUI with no ordering to get wrong
		bool probeCapture = false;
		// draw the angular grid at fixed WORLD azimuth/elevation instead of
		// head-locked lens angles: the grid then stays put while the head
		// rotates, which is exactly the stimulus the swim nulling task
		// wants (angular mode only; needs the head pose, on automatically)
		bool gridWorldLocked = false;
		// while the dot is on, log throttled SwimProbe lines: angular
		// residual (raw + smoothed gaze), head angular velocity, and
		// per-eye lens UVs of dot and gaze — the raw data for empirical
		// static-profile and pupil-swim fitting
		bool swimProbe = false;
		// draw the calibration grid and fixation dot in content space so
		// the distortion profile warps them like scene content. use for
		// profile validation: grid straightness + probe scoring runs.
		bool overlayWarped = false;
	} eyeGaze = {};
	// dynamic pupil swim correction (requires gaze). phase A: the
	// distortion center follows the gaze point by these fractions per
	// axis; 0 = static behavior, correction vanishes at center gaze by
	// construction. tune with the debug grid: fixate an intersection,
	// move gaze around it, raise until nearby lines stop
	// bending/shifting with gaze. shift clamped to +-0.15 uv.
	struct {
		double centerStrengthX = 0;
		double centerStrengthY = 0;
	} pupilSwim = {};
	// keyed mutex acquire timeout for the frame sync texture, in ms. when it
	// expires the frame passes through unprocessed (a visible "flash" of
	// ungraded color), which happens under heavy load (shader compilation,
	// level streaming). after a skip the timeout escalates (3x, min 15ms) to
	// break flash streaks, and resets on the next acquired frame.
	int syncTimeoutMs = 10;
	// passive recon logger: opt-in, off by default. installs observation-only
	// vtable hooks on vrlink's D3D11 context to map its layer-consumption
	// point (zero-copy v3 feasibility), NVENC module, and copy/bind shape.
	// intended for ONE disposable session; never substitutes or alters
	// anything. see ReconLogger.h.
	bool reconLogger = false;
	// render-side hitch instrumentation, the HITCHDIAG analog of KALDIAG:
	// every 2s a summary of the frame-callback cadence (dt mean/max, counts
	// over 16.7/33ms, AcquireSync wait, our own work time, skip/create/evict
	// counters), plus a one-shot HITCH line whenever the gap since the
	// previous frame callback exceeds 25ms, tagged with what the previous
	// frame did (scratch create, lut bake, shader compile, sync skip) so
	// outliers self-attribute. cost is a few clock reads per frame.
	bool hitchDiag = true;
	// scratch LRU evictions are moved to a deferred list and released a few
	// frames later, one per frame, AFTER the keyed mutex is released - so a
	// resolution/layer change never pays release cost inside the same
	// mutex-held frame that already pays the (unavoidable) creation stall.
	// off = legacy synchronous evict-in-frame, kept for A/B.
	bool deferredEviction = true;
	// render the processed frame directly into the layer texture (slice
	// aware RTV) instead of drawing into a scratch target and copying the
	// bounds region back. cuts per-eye traffic from ~6x to ~4x of the
	// texture size (the field stutter in heavy titles at 5000x5400+ per eye
	// was bandwidth, not shader math) and halves scratch VRAM. per-texture
	// automatic fallback to the copy-back path if the layer refuses an RTV.
	bool directRender = true;
	// zero-copy path: instead of warping the layer in place, warp into our
	// own shared shadow textures and hand vrlink the SHADOW handles at
	// SubmitLayer. the whole frame path becomes one draw (sample app,
	// write shadow): ~2x traffic vs 4x for directRender and 6x legacy.
	// costs a triple-buffered shadow ring per layer size (same VRAM as one
	// extra swap set). array-layer (single-pass instanced) apps still copy
	// into scratch first (the shader samples Texture2D, not an array), so
	// they run at ~4x into the shadow. experimental: vrlink accepting
	// handles outside its own swap sets is the one assumption we cannot
	// verify from this side, hence default OFF until field-confirmed; if a
	// session shows black/frozen frames, turn this off.
	bool zeroCopy = false;
	// experimental throw/velocity fix mode: 0 = off, 1 = classic (the v3
	// estimator: position-derived linear velocity substituted via a smooth
	// speed-ramped blend, nothing else), 2 = full (adds angular velocity
	// substitution, wrist-flick blend term, peak/direction holds and the
	// release-gesture anchor). classic preserved because field testing
	// rated it the best-feeling iteration; full is the later heuristic
	// stack. json values: "off" / "classic" / "full".
	// 3 = derive: DISCARD the runtime's velocity entirely for streamed
	// (vrlink) controllers and always report the pose-derived estimate —
	// no engage gate, no blend, no peak hold: one consistent self
	// coherent signal, the same method SteamVR itself would use on the
	// poses. all modes apply ONLY to streamed controllers (serials
	// VRLINK*/SamsungVST*); lighthouse devices (LHR-*) have native
	// velocity and are never touched.
	// zero-copy v3: consumption-point source substitution. the frame is
	// warped into a rotating SHARED shadow set and vrlink's per-frame
	// staging copy (the recon-verified single consumption point) is
	// redirected to read the fresh shadow. the layer keeps the app's
	// unprocessed frame, so every failure (stale shadow, open failure,
	// toggle off) degrades to a passthrough flash — never a freeze (v1
	// wall: handle bookkeeping untouched) and never an encoder reset (v2
	// wall: NVENC surfaces untouched). our per-eye traffic 4x -> 2x.
	// EXPERIMENTAL: one dedicated toggle-on test in a disposable session.
	bool zeroCopyV3 = false;
	// NVENC tap: OBSERVE-ONLY recon of vrlink's encoder (init params, rate
	// control surface, registered resources). answers whether NVENC
	// consumes the layer directly (the v3c site) and exposes the parameter
	// surface for the black-floor work. modifies nothing. enable BEFORE
	// launching SteamVR so the encoder creation is not missed.
	bool nvencTap = false;
	int velocityFixMode = 4; // kalman: consolidation default 2026-08-11
	// derive-mode speed-adaptive smoothing: the estimator is a low lag
	// endpoint derivative, so its noise shows fully in derive mode (the
	// old modes' 1 m/s engage gate was hiding it). the filter time
	// constant slides from tauSlow (held still: kill trembling, latency
	// invisible) to tauFast (throw speeds: near raw so peak and phase
	// survive) as effective speed (|v| + 0.15|w|) crosses speedLow..High.
	double deriveSmoothTauSlowMs = 90.0;
	double deriveSmoothTauFastMs = 6.0;
	double deriveSmoothSpeedLow = 0.25;
	double deriveSmoothSpeedHigh = 1.6;
	// separate ANGULAR smoothing (off = original behavior: one alpha from
	// combined speed drives both channels, keeping v and w phase locked).
	// field data 2026-08-10: reported |w| swings +-40% around raw with 32%
	// per-sample jitter tails — the shared alpha tuned for linear speeds
	// under-serves the angular channel. when enabled, the angular channel
	// gets its own speed-adaptive alpha from these knobs (angular speeds
	// in rad/s; defaults chosen to match the old 0.15 rad/s-per-m/s
	// conversion, so enabling with defaults is nearly behavior neutral).
	bool deriveSmoothAngSeparate = false;
	double deriveSmoothAngTauSlowMs = 90.0;
	double deriveSmoothAngTauFastMs = 6.0;
	double deriveSmoothAngSpeedLow = 1.7;
	double deriveSmoothAngSpeedHigh = 10.5;
	// derive-mode split-channel output. the axis-wise EMA smooths
	// MAGNITUDE well, but smoothing each axis independently does not
	// stabilize DIRECTION when components sit near zero crossings: field
	// data (2026-08-09 burst log) shows the filtered vector's direction
	// swinging 73-83 deg/sample (median) at ~0.5 m/s and 23-50 deg/sample
	// inside the 40ms release zone — the "objects fly off in random
	// directions" residual. split mode keeps the EMA for magnitude only
	// and takes direction from a speed^weightPow weighted vector sum of
	// the RAW estimator outputs over a short trailing window: fast,
	// high-SNR samples pin the direction, slow noisy ones contribute
	// ~nothing. independent per-channel toggles keep A/B single-variable.
	bool deriveSplitDirLinear = false;
	bool deriveSplitDirAngular = false;
	double deriveDirWindowMs = 50.0;
	double deriveDirWeightPow = 2.0;
	// direction REFERENCE for split mode. field data (2026-08-10) showed
	// the "window" average of raw estimates barely helps: consecutive SG
	// estimates share 7/8 of their input positions, so their noise is
	// almost fully correlated and averaging them does not cancel it.
	// 1 = secant: direction of the raw position DISPLACEMENT across the
	//     derive ring (newest - oldest). displacement over ~25-70ms at
	//     throw speed is 5-20cm against ~1-4mm position noise, so its
	//     direction is clean to a few degrees; lag is ~half the ring
	//     span of arc curvature (deterministic and small). DEFAULT.
	// 2 = runtime: direction of vrlink's own reported velocity (device
	//     side sensor fusion: smooth and consistent, magnitude heavily
	//     smoothed — which does not matter, we only take its direction).
	// 0 = window: the original speed^pow weighted average (kept for A/B).
	int deriveDirSource = 1;
	// MAGNITUDE source for split mode. 0 = vector: |vector EMA| (original;
	// under-reads and jitters during direction change because opposing
	// components cancel inside the average). 1 = scalar: EMA of |raw|
	// itself with the same adaptive tau — smooths the speed without the
	// cancellation loss. field direction is solved by the secant (BURSTDIR
	// 2026-08-10: 2.8-3.5 deg median from raw), so with direction
	// decoupled, tauFast can also simply be raised (15-20ms) for less
	// magnitude jitter with no direction penalty.
	int deriveMagSource = 0;
	// release latch: field data 2026-08-10 (202 ReleaseSnap events) shows a
	// tail problem — the input release event trails the motion, and ~25% of
	// throws sample the output AFTER the hand slowed (release/peak ratio
	// p25 = 0.80, long tail to near zero). when enabled, the moment a
	// trigger/grip RELEASE arrives from the input tap, the output replays
	// the peak (v, w) of the last latchWindowMs for latchHoldMs (full
	// strength for the first half, linear decay after) so late-sampling
	// games still read the throw. median throws (already at peak) are
	// unaffected. derive mode only; off by default for a clean A/B.
	bool deriveReleaseLatch = false;
	double deriveLatchWindowMs = 150.0;
	double deriveLatchHoldMs = 120.0;
	double deriveLatchMinSpeed = 0.8;
	// per-channel latch peaks (field 2026-08-10: a single effective-speed
	// peak key picked the WINDUP moment for arm throws — |w| spikes while v
	// points backward — and the latch replayed that poisoned vector at
	// release: ratio p90 2.22, direction 56 deg off. flicks improved with
	// the same key because their true peak IS angular dominant. so: v
	// replays from the linear-peak moment, w from the angular-peak moment,
	// each behind its own gate.)
	double deriveLatchAngMinSpeed = 6.0;
	// input position prefilter feeding the derive fit AND the secant:
	// per-axis median of the last 3 raw positions kills single-sample
	// network spikes (the p90 18%/sample jitter tail) at ~1 sample lag.
	// "off" or "median3".
	int derivePreFilter = 0;
	// input pre-smoothing (the adjustable-strength version of the
	// prefilter idea): EMA over raw positions/orientations BEFORE any
	// derivation, strength in ms (0 = off). scope selects what consumes
	// the smoothed stream: "direction" = only the secant (direction is
	// cleaned, magnitude still derived from the exact positions);
	// "both" = the fit AND the secant (maximum smoothness, some peak lag).
	double derivePreSmoothMs = 0.0;
	int derivePreSmoothScope = 0; // 0=direction 1=both
	// CONSUMER DISCRIMINATOR (diagnostic): many engines ignore the driver's
	// reported velocity entirely and estimate throws from rendered pose
	// history (Unity XR toolkit, VRTK, custom rigs). across sessions our
	// radically different velocity outputs produced near identical felt
	// results — the signature of exactly that. "zero" reports zero
	// velocity: if throwing still works AT ALL, the game does not read
	// vecVelocity and the pose stream is the real battlefield. 2-minute
	// test, then turn it off.
	int deriveDiagVelocity = 0; // 0=off 1=zero
	// pose-assist: if the game derives throws from pose deltas, make the
	// POSE tell the throw's story too — during the latch hold, the
	// reported position is forward-integrated along the latched velocity
	// (same decay), so pose-history estimators read the clean release
	// instead of the snap-back. brief visual hand overshoot at release is
	// the price; opt-in.
	bool deriveLatchPoseAssist = false;
	// KALMAN mode (velocityFixMode "kalman"): replicate the native
	// lighthouse ARCHITECTURE rather than patching symptoms. native
	// controllers report one coherent fused kinematic state — pose,
	// velocity, angular velocity all from a single estimator, so the
	// runtime's forward prediction and every game-side pose-history
	// estimator agree by construction. this mode runs a per-controller
	// constant-velocity Kalman filter over the incoming stream and reports
	// THE FILTER STATE as the pose: position, orientation, v and w are
	// self consistent; no splits, no latches, no replays.
	// kalmanProcessAccel (m/s^2) is THE responsiveness knob: high = trusts
	// motion (snappy, noisier), low = trusts smoothness (calm, laggier).
	// RATIFIED 2026-08-11 (campaign close, §3/§4): A=1 P=2.7 W=400 O=1.25
	// L=0. these ARE the consolidation defaults — the 1.6.0 commit updated
	// mode/dup/coast but missed this trio, so the driver published the
	// pre-campaign 40/2.0/0.5 and every GUI reset restored untuned values
	// (field incident 2026-08-11, cost one capture).
	double kalmanProcessAccel = 1.0;
	double kalmanPosNoiseMm = 2.7;
	double kalmanProcessAngAccel = 400.0;
	double kalmanOriNoiseDeg = 1.25;
	// optional fixed forward prediction of the reported state (native
	// drivers do this to counter transport latency); 0 = off
	double kalmanLeadMs = 0.0;
	// EXPERIMENT B — fixed-skew release rewind (single-session test,
	// default OFF). the input release event travels a slower path than the
	// pose stream: it lands 50-150ms after the true release, so games
	// sample the snap-back. this reports, for a short hold after the
	// release event arrives, the velocity from rewindMs EARLIER in the
	// kalman history — pure time re-alignment by one physical constant
	// (the transport skew), no peak picking, no heuristics. if the right
	// rewind exists, opposite throws vanish at one setting; if no setting
	// works, the hypothesis is falsified and the experiment ends. the
	// pose is never touched.
	double kalmanReleaseRewindMs = 0.0;
	double kalmanRewindHoldMs = 100.0;
	// direction/magnitude split reporting (field 2026-08-10 tuning session:
	// the user's hands found A=1 best DESPITE weak throws — direction
	// stability dominates felt quality, but magnitude lag at A=1 makes
	// items fall out of the hand. the two channels want different
	// smoothing, exactly the derive-era split finding. when set, the
	// REPORTED velocity direction comes from an EMA of the state velocity
	// with this time constant, while magnitude stays live from the state —
	// run A back at 40-60 for full-strength snappy throws with A=1-like
	// direction calm. continuous and phase agnostic: no events, no moment
	// picking. 0 = off. pose untouched.
	double kalmanDirSmoothMs = 0.0;
	double kalmanAngDirSmoothMs = 0.0;
	// magnitude channel (field 2026-08-10: A=1 + raised P/O is the user
	// verified sweet spot for DIRECTION, but that configuration's lag
	// under-reports throw SPEED — "strength feels low", items falling out.
	// the inverse of naive splitting: direction stays with the calm state;
	// MAGNITUDE comes from a parallel fast estimator over the same
	// measurements (kalmanMagSource "fast", accel knob below). magScale is
	// a plain always-on trim multiplier on top (1.0 = neutral).)
	int kalmanMagSource = 0; // 0=state 1=fast
	double kalmanMagAccel = 60.0;
	double kalmanMagScale = 1.0;
	double kalmanAngMagScale = 1.0;
	// duplicate-sample skip (field 2026-08-10: raw-step telemetry caught
	// 12,121 frozen steps in one session — vrlink repeats the last pose
	// whenever fresh tracking data has not arrived, and every repeat tells
	// the filter "the hand stopped dead". this drags throw velocity down
	// (the calm state's chronic weakness) and makes fast estimators
	// oscillate stop/jump (the flip engine). a duplicate is a MISSING
	// measurement, not a measurement of stillness: while the state is
	// moving, duplicates now coast the filter (predict only) instead of
	// braking it. genuine stillness keeps normal updates. textbook
	// missing-data handling; toggle for A/B.
	// 0=off 1=coast 2=drop. field 2026-08-11: with device-time active,
	// COAST is feel-rejected (extrapolated positions + snap poison the
	// pose history the game fits throws from; NIS 100-500 spikes), and
	// OFF pays a velocity drag (every repeat says "stopped"; the chronic
	// ~0.80 strength). DROP treats a detected repeat as never having
	// arrived: no measurement, no prediction, no clock advance — the
	// next real sample predicts across the full accumulated device-time
	// gap in one honest step. no fake stillness, no invented positions;
	// reported pose holds (runtime still animates from v). the run cap
	// below applies to coast AND drop. default off = field champion.
	// 3=soft: the repeat IS processed as a measurement, but with R
	// inflated by dupRScale^2 — honest noise model for a sample of
	// unknown age (its true uncertainty at hand speed v is v*sigma_age,
	// not the sensor floor). gain on repeats shrinks ~k^2 while
	// covariance keeps accumulating through the run, so fresh-sample
	// catch-up self-schedules. dupRScale=1 in soft is bit-identical to
	// off; k -> inf converges toward coast/drop. the run cap applies:
	// repeats sustained past it are accepted at full weight.
	int kalmanDupMode = 3;
	double kalmanDupRScale = 3.0;
	// teleport guard: reinit instead of innovating when an accepted step
	// exceeds this floor AND implies >25 m/s (physically impossible hand
	// speed = unflagged tracking reacquire). floor ignores freeze
	// catch-ups (~0.1m). 0 disables. config-only this slice; GUI knob
	// rides the next GUI-touching slice.
	double kalmanTeleportM = 0.75;
	// measurement timestamping (estimator correctness pass 2026-08-10):
	// vrlink stamps every pose with poseTimeOffset, and this session's
	// field data shows it is real and VARYING — median +13.8ms, stdev
	// 3.8ms, sample-to-sample swings of ~6ms during throws, and a stale
	// tail down to -69ms. the filter previously treated every sample as
	// "now": at 5 m/s a 6ms timing swing masquerades as 30mm of position
	// noise against a 4mm R, which is exactly the unmodeled noise that
	// forced A=1 and its weak throws. with deviceTime on, each
	// measurement is stamped tMeas = receipt + poseTimeOffset and dt is
	// the device-time delta; out-of-order samples (dt <= 0) are DROPPED,
	// never reinit (the old dt<=0 reinit would zero velocity mid-throw
	// once device time is in play). |offset| > 100ms falls back to
	// receipt time. toggle off = previous behavior exactly, for A/B.
	bool kalmanDeviceTime = true;
	// dup run cap (bug fix 2026-08-10; rationale sharpened 2026-08-11):
	// no human hand holds a position BIT-IDENTICALLY for tens of ms —
	// real stillness shows micro-tremor above the 0.3mm gate. a repeat
	// sustained past this cap therefore means the TRACKER stopped
	// producing (set-down controller, long dropout), and in both cases
	// believing the repeat (velocity to zero, hold position) beats
	// extrapolating or distrusting blind. also closes the runaway loop:
	// skipping repeats blocks the very measurements that update the
	// speed the dup gate tests. applies to coast, drop, and soft.
	double kalmanDupCoastMaxMs = 90.0;
	// ET gaze aim assist (plan C): people fixate throw targets BEFORE the
	// hand releases, so gaze carries the intended direction through the
	// one channel immune to the input-timing problem that produces the
	// opposite-direction tail (~8% of throws sample the snap-back). when
	// enabled, the reported velocity direction is bent toward the gaze
	// ray by assist fraction of the angle between them, capped at maxDeg,
	// only above minSpeed, only with fresh valid gaze (<100ms). direction
	// only — magnitude and spin untouched; the rendered hand untouched.
	double kalmanGazeAssist = 0.0;   // 0..1
	double kalmanGazeMaxDeg = 30.0;
	double kalmanGazeMinSpeed = 1.2; // m/s
	// fixed-lag smoothing (field 2026-08-10: the full-lock gaze test proved
	// this game IGNORES driver vecVelocity — 18k bends up to 177 deg with
	// zero effect on throws — and derives throws from POSE history. the
	// battlefield is the reported position stream. a filter estimates the
	// present from the past; a smoother estimates L ms ago using samples
	// from BOTH sides — calm like A=1 AND amplitude-accurate like high A,
	// which filtering fundamentally cannot combine. the entire reported
	// state (pose + velocities, coherent) shifts to t-L; the one honest
	// cost is L ms of added hand latency. 0 = off. implemented as a
	// two-estimate fusion: stored forward state at t-L fused with the
	// current state backcast to t-L.
	double kalmanSmoothLagMs = 0.0;
	// experimental throw/velocity fix. vrlink's reported controller velocity
	// is heavily smoothed (field data: peaks read ~50-65% of position-derived
	// velocity during throws, ratio varies with motion phase = filter lag,
	// not a scale factor). when enabled, linear velocity is recomputed from
	// a ~50ms window of positions and substituted when meaningfully larger
	// than the reported value, so releases carry true peak speed while calm
	// motion keeps the driver's smoother data.
	bool velocityFix = false;
	// diagnostic: throttle-log controller/tracker poses from the PoseUpdated
	// hook (position, velocity, tracking result), with a burst mode that
	// captures high-velocity moments (throws). live-reloaded, so it can be
	// toggled mid-session. groundwork for the throw/velocity fix.
	bool poseLogging = false;
	// GUI-only fence: retired experimental knobs render in the GUI's
	// Graveyard section only when this is set by hand in settings.json.
	// no GUI knob on purpose. values of archived knobs stay ACTIVE
	// regardless — hiding is not disabling.
	bool graveyardEnable = false;
};

// pose adjustments for streamed controllers, applied in the PoseUpdated hook.
// rotation is a local frame euler offset in degrees (x = pitch: positive
// tilts the top of the controller back toward the user), position is a local
// frame offset in cm. lets the grip/aim angle be matched to what games
// expect from other controller types. live reloaded.
// in-headset controller offset aligner. manual mode: sticks adjust the
// selected axis of the selected group (position/rotation) live. automatic
// mode: plant the controller tip on any solid surface, hold the trigger and
// swirl a cone around the planted tip; a least-squares pivot solve recovers
// the position offset (the drawn tip marker freezing is the confirmation).
struct ControllerAlignerConfig{
	bool enable = false;
};

struct ControllersConfig{
	// mixed-space velocity frame fix for playspace-override setups (e.g.
	// lighthouse controllers aligned into the vrlink space): the openvr
	// header leaves DriverPose_t::vecVelocity's frame unspecified while
	// positions are driver-space + WorldFromDriver. with a large alignment
	// yaw the two conventions diverge and thrown objects fly at the right
	// speed in the WRONG direction. 0 = off, 1 = "world" (rotate reported
	// velocity by qWorldFromDriverRotation), 2 = "driver" (inverse).
	// applied only to devices whose WorldFromDriver rotation deviates from
	// identity by more than ~2 degrees, so vanilla devices are untouched.
	// the field test decides which mode matches vrserver's real convention.
	int spaceVelocityFixMode = 0;
	double rotationOffsetDeg[3] = {0, 0, 0};
	double positionOffsetCm[3] = {0, 0, 0};
	ControllerAlignerConfig aligner = {};
};

struct CustomShaderConfig{
	// if shaders should be replaced in the compositor
	bool enable = false;
	bool enableForMeganeX8K = true;
	bool enableForDreamAir = true;
	bool enableForOther = false;
	// contrast with 50 being normal
	double contrast = 50;
	// the point from 0-100% of white that the contrast is centered around
	double contrastMidpoint = 50;
	// if the contrast should be done in linear space instead of gamma
	bool contrastLinear = false;
	// if per eye contrast should be applied
	bool contrastPerEye = false;
	bool contrastPerEyeLinear = false;
	double contrastLeft = 50;
	double contrastMidpointLeft = 50;
	double contrastRight = 50;
	double contrastMidpointRight = 50;
	// increase or decrease the variation of the colors
	double saturation = 50;
	// gamma of the output
	double gamma = 2.2;
	// if the subpixels should be offset
	bool subpixelShift = true;
	// if the mura correction should be skipped
	bool disableMuraCorrection = false;
	// if the black levels should be skipped
	bool disableBlackLevels = false;
	// if the colors should be corrected to display the srgb input as srgb on the display
	bool srgbColorCorrection = false;
	// if the white point correction should be applied to the srgb color correction
	bool srgbWhitePointCorrection = false;
	// a 3x3 matrix to apply to the linear colors
	// if this is an array of 9 flat elements it will override the headset's default matrix
	std::vector<double> srgbColorCorrectionMatrix = {};
	// correct color uniformity issues of the lenses on the MeganeX
	bool lensColorCorrection = true;
	// if a 10 bit input will be dithered down to 8 bit
	bool dither10Bit = false;
	// if the filter should be enabled for overlays (defaults false to avoid performance hit when no overlay is shown)
	bool enableFilterForOverlay = false;
	// if the filter should be enabled when the SteamVR dashboard is open
	bool enableFilterForDashboard = true;
	// filters on the sampling of the texture,  "None", "NearestNeighbor", "FXAA2", "FXAA2CAS", "LumaSharpen", and "CAS"
	std::string samplingFilter = "None";
	// FXAA2 filter parameters
	double samplingFilterFXAA2SharpenStrength = 1.0;
	double samplingFilterFXAA2SharpenClamp = 0.05;
	// FXAA2CAS filter parameters
	double samplingFilterFXAA2CASStrength = 1.0;
	double samplingFilterFXAA2CASContrast = 1.0;
	// luma sharpen filter parameters
	double samplingFilterLumaSharpenStrength = 2.0;
	double samplingFilterLumaSharpenClamp = 0.1;
	int samplingFilterLumaSharpenPattern = 1;
	double samplingFilterLumaSharpenRadius = 1.0;
 	// CAS filter parameters
 	double samplingFilterCASStrength = 1.0;
 	double samplingFilterCASContrast = 1.0;
	// color multiplier for tint adjustments
	ConfigColor colorMultiplier = {1.0, 1.0, 1.0};
};


class Config{
public:
	enum HeadsetType{
		None = 0,
		Other = 1,
		MeganeX8K = 2,
		Vive = 3,
		DreamAir = 4,
	};
	
	class BaseHeadsetConfig{
	public:
		// if the headset should be shimmed by this driver
		bool enable = true;
		// the type of headset this is
		HeadsetType headsetType = HeadsetType::None;
		// ipd in mm
		double ipd = 63.0;
		// ipd offset from the ipd value in mm
		double ipdOffset = 0.0;
		// horizontal offset in mm to shift both eyes to the right
		double horizontalIPDOffset = 0.0;
		// minimum black levels from 0 to 1
		double blackLevel = 0;
		// tint the display this color
		ConfigColor colorMultiplier = {};
		// distortion profile to use
		std::string distortionProfile = "None";
		// amount to zoom in the distortion profile
		double distortionZoom = 1.0;
		// amount to zoom in the FOV, the fov is divided by this value
		double fovZoom = 1.0;
		// amount to zoom in the FOV using tangent-based scaling for flatter perception
		double flatFovZoom = 1.0;
		// multiplier for the subpixel offsets
		double subpixelShift = 1.0;
		// subpixel offsets in pixel units for each color channel [offsetXRed, offsetYRed, offsetXGreen, offsetYGreen, offsetXBlue, offsetYBlue]
		std::vector<double> subpixelOffsets = {0, 0, 0, 0, 0, 0};
		// width of one eye in pixels
		int resolutionX = 3840;
		// height of one eye in pixels
		int resolutionY = 3552;
		// clockwise rotation of the image on the right display, 0:0, 1:90, 2:180, 3:270
		int displayRotation = 0;
		// max horizontal fov
		double maxFovX = 100.0;
		// max vertical fov
		double maxFovY = 96.0;
		// distortion mesh resolution
		int distortionMeshResolution = 127;
		// if the fov should be slightly adjusted each session to prevent sharp burn in along the edges
		bool fovBurnInPrevention = true;
		// if the distortion profile should clamp the image to the bounds of the display or if it will instead render an image at whatever FOV is set
		bool fovClamping = true;
		// device type used to filter distortion profiles in the GUI
		std::string distortionProfileDeviceType = "";
		// multiply 100% render resolution width
		double renderResolutionMultiplierX = 1.0;
		// multiply 100% render resolution height
		double renderResolutionMultiplierY = 1.0;
		// percent of 1:1 resolution to apply the super sampling downscale filter at, this is really high to allow for subpixel sampling
		double superSamplingFilterPercent = 500;
		// seconds of latency to the display
		double secondsFromVsyncToPhotons = 0.007;
		// seconds from the the first to last line of the display
		double secondsFromPhotonsToVblank = 0.0025;
		// angle in degrees for each eye to be rotated outwards
		double eyeRotation = 0.0;
		// disable eyes as much as possible. 0:both enabled 1:left disabled 2:right disabled 3:both disabled
		int disableEye = 0;
		// if the fov should be decreased for the disabled eye, this causes problems in some apps
		bool disableEyeDecreaseFov = false;
		// if a vive link box should be used for bluetooth
		bool useViveBluetooth = false;
		// if the display is in direct mode or false if it is on the desktop
		bool directMode = true;
		// if the icons in the SteamVR status window should be modified
		bool replaceIcons = true;
		// the edid for the headset
		int edidVendorId = 0;
		// the edid for the headset
		int edidProductId = 0;
		// if non zero, override the edid vendor id
		int edidVendorIdOverride = 0;
		// if non zero, override the edid product id
		int edidProductIdOverride = 0;
		// DSC Version
		int dscVersion = -1;
		// DSC Slice count
		int dscSliceCount = -1;
		// DSC bits per pixel
		int dscBPPx16 = -1;
		// if the driver should be enabled for every hmd
		bool forceEnable = false;
		// if parallel projection should be used for rendering
		bool parallelProjection = true;
		// if eye tracking should be enabled
		bool enableEyeTracking = false;
		// Config struct for the hidden area mesh
		HiddenAreaMeshConfig hiddenArea;
		// config for dimming the display when stationary
		StationaryDimmingConfig stationaryDimming = {};
	};
	
	class MeganeX8KConfig : public BaseHeadsetConfig{
	public:
		MeganeX8KConfig(){
			headsetType = HeadsetType::MeganeX8K;
			distortionProfile = "MeganeX8K Default";
			distortionProfileDeviceType = "MeganeX8K";
			edidVendorId = 0xcc4c; // SFL
			displayRotation = 1;
			subpixelOffsets = {-0.33 / 3552.0, 0, 0, 0, 0.33 / 3552.0, 0};
		}
	};
	// config for the MeganeX superlight 8K
	MeganeX8KConfig meganeX8K = {};
	
	class DreamAirConfig : public BaseHeadsetConfig{
		public:
		DreamAirConfig(){
			headsetType = HeadsetType::DreamAir;
			distortionProfile = "Dream Air Default";
			distortionProfileDeviceType = "DreamAir";
			maxFovX = 96;
			maxFovY = 86;
			edidVendorId = 53826; // PVR
			displayRotation = 3;
			subpixelOffsets = {0.33 / 3552.0, 0, 0, 0, -0.33 / 3552.0, 0};
			eyeRotation = 2;
			enableEyeTracking = true;
		}
	};
	// config for the Dream Air
	DreamAirConfig dreamAir = {};
	
	class FakeHeadsetConfig : public BaseHeadsetConfig{
		public:
		FakeHeadsetConfig(){
			enable = false;
			headsetType = HeadsetType::Other;
			distortionProfile = "MeganeX8K Default";
			displayRotation = 0;
			// use a 1080p monitor
			directMode = false;
			resolutionX = 960;
			resolutionY = 1080;
		}
	};
	// config for the fake headset
	FakeHeadsetConfig fakeHeadset = {};
	
	class GeneralHeadsetConfig{
	public:
		// if a vive link box should be used for bluetooth
		bool useViveBluetooth = false;
	};
	GeneralHeadsetConfig generalHeadset = {};
	
	CustomShaderConfig customShader = {};
	
	// processing of direct mode layer textures before a streaming driver (e.g.
	// vrlink / Steam Link) consumes them. this is the always on path for
	// headsets whose driver composites frames itself, where the compositor
	// shader replacement only runs while the dashboard is open.
	StreamFrameConfig streamFrame = {};
	
	// streamed controller pose adjustments
	ControllersConfig controllers = {};
	
	// if devices should always be reported as tracking
	bool forceTracking = false;
	
	// if the screenshot requests should cause full compositor debug screenshots to be taken
	bool takeCompositorScreenshots = false;
	
	// makes the diver only do things related to closed source functionality if it exits
	// this allows for a driver built from source to run along side the driver with proprietary code
	bool onlyHandlePrivateFunctionality = false;
	
	// reload the config every time a file is changed in the distortions directory
	// this is for manual json editing, utilities should touch the main settings file when done modifying distortions instead
	// this is now enabled by default
	// bool watchDistortionProfiles = false;
	
	// if the config has been changes and should be reloaded
	// this will be set the false at the end of RunFrame
	bool hasBeenUpdated = true;
	
};

// config for a single custom distortion profile
class DistortionProfileConfig{
public:
	// name of distortion profile, this will be it's filename
	std::string name = "None";
	// the headset device this profile is for, empty for all devices, or "MeganeX8K" for the MeganeX superlight 8K
	std::string device = "";
	// description to display
	std::string description = "";
	// author of the distortion profile
	std::string author = "";
	// the date when it was created
	double creationDate = 0;
	// last time it was modified, used for reloading if changed
	double modifiedTime = 0;
	// type of distortion, None or RadialBezier
	std::string type = "None";
	// main distortion
	std::vector<double> distortions = {};
	// additional distortion to apply to the red channel
	std::vector<double> distortionsRed = {};
	// additional distortion to apply to the blue channel
	std::vector<double> distortionsBlue = {};
	// offset image outwards on the display using the same 0 to 100 scale 
	float offsetX = 0.0f;
	// offset image upwards on the display using the same 0 to 100 scale
	float offsetY = 0.0f;
	// if legacy smoothing should be used for bezier curves
	bool legacySmoothing = false;
	// amount to smooth the curve from 0 to 1 for legacy smoothing
	double smoothAmount = 0.66;
};

// global config object
extern Config driverConfig;

// config from before the last reload
extern Config driverConfigOld;

// config with default values
extern Config defaultDriverConfig;

// lock for the config to prevent updates while reading
extern std::mutex driverConfigLock;

// version of the application
extern std::string driverVersion;


#if __has_include("../Driver/HidModifierPrivate.cpp")
#define HAS_PRIVATE 1
#endif