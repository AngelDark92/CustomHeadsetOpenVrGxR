#pragma once

#include <set>
#include <map>
#include <vector>
#include <mutex>
#include <string>
#include <atomic>

#include "openvr_driver.h"

class ShimDefinition;

#define VREvent_VendorSpecific_ContextCollection (vr::EVREventType)(vr::VREvent_VendorSpecific_Reserved_Start + 5872)
#define VREvent_VendorSpecific_ContextCollection_MagicDataNumber 32643216579172981


class CustomHeadsetDeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init(vr::IVRDriverContext *pDriverContext) override;
	const char *const *GetInterfaceVersions() override;
	
	// called by the main loop of the server
	void RunFrame() override;
	// deprecated function, but still must be defined
	bool ShouldBlockStandbyMode() override;
	// SteamVR is entering/leaving standby mode
	void EnterStandby() override;
	void LeaveStandby() override;
	// cleanup on exit
	void Cleanup() override;
	
	// handle hook of TrackedDevicePoseUpdated
	bool HandleDevicePoseUpdated(uint32_t openVRID, vr::DriverPose_t &pose);
	// handle hook of TrackedDeviceAdded
	bool HandleDeviceAdded(const char* &pchDeviceSerialNumber, vr::ETrackedDeviceClass &eDeviceClass, vr::ITrackedDeviceServerDriver* &pDriver);
	// set of driver conexts collected by the hooking process
	std::set<vr::IVRDriverContext*> driverContexts = {};
	// map of driver contexts by device id
	// this is populated by VREvent_VendorSpecific_ContextCollection events
	std::map<uint32_t, vr::IVRDriverContext*> driverContextsByDeviceId = {};
	// sends out VREvent_VendorSpecific_ContextCollection events for a given device id
	// after some time, the driverContextsByDeviceId map should be contain the context for this device
	void SendContextCollectionEvents(uint32_t id);
	// attempt to send the event if the context is available, returns true if successful.
	// if false was returned the message has queued to be sent if the driver context can be found
	// events must be sent from the context that owns the device, so this is necessary
	bool SendVendorEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t & eventData, double eventTimeOffset);
	// a set of all shim objects to manage
	// this allows them to have RunThread called
	std::set<ShimDefinition*> shims;
private:
	struct QueuedEvent {
		vr::EVREventType eventType;
		vr::VREvent_Data_t eventData;
		double eventTimeOffset;
	};
	// events that are waiting for a context to be found
	std::map<uint32_t, std::vector<QueuedEvent>> queuedEvents = {};
	bool customShaderEnabled = false;
	
	// pose logging diagnostic state (streamFrame.poseLogging), per device.
	// pose updates arrive on the source drivers' own threads, hence the lock.
	struct PoseLogState {
		double lastSteadyLog = 0;
		double lastBurstLog = 0;
		// peak linear speed observed since the last steady log line
		double peakSpeed = 0;
		// serial announced once on first sight (maps openVRID -> device)
		bool announced = false;
		// tracking state transition + post throw window logging
		bool haveTrackState = false;
		bool lastLoggedValid = false;
		int lastLoggedResult = 0;
		double recentFastTime = 0;
		// finite difference velocity from positions, to compare against the
		// velocity the driver reports (suspected ~2 m/s clamp in vrlink)
		bool havePos = false;
		double lastPos[3] = {0, 0, 0};
		double lastSampleTime = 0;
		double fdSpeedEma = 0;
		// quaternion-derived angular speed, same idea as fdSpeed: compare
		// against the driver's reported |w| to see if angular velocity is
		// smoothed the same way linear velocity is
		bool haveQuat = false;
		vr::HmdQuaternion_t lastQuat = {1, 0, 0, 0};
		double fdAngSpeedEma = 0;
	};
	std::map<uint32_t, PoseLogState> poseLogStates = {};
	std::mutex poseLogLock = {};
	// one-shot per-device announcement of a non-identity WorldFromDriver
	// (mixed-space setups); lock free for the pose hot path
	std::atomic<uint64_t> spaceFixLoggedMask{0};
	void LogDevicePose(uint32_t openVRID, const vr::DriverPose_t &pose);
	
	// throw/velocity fix state: short ring of recent positions per device,
	// used to recompute linear velocity over a ~50ms window (endpoint
	// difference across the ring rejects sample-to-sample jitter that a
	// plain adjacent diff amplifies). guarded by poseLogLock.
	struct VelFixState {
		static constexpr int ringSize = 8;
		double pos[ringSize][3] = {};
		// input prefilter history: last raw positions for the per-axis
		// median-of-3 that feeds the ring when derivePreFilter is on
		// (kills single-sample network spikes before the fit AND secant)
		double rawPos[3][3] = {};
		int rawCount = 0;
		// pre-smoothed parallel streams (EMA, derivePreSmoothMs): the
		// secant reads these when smoothing is on; the fit also reads the
		// smoothed positions when scope=both
		double smPos[ringSize][3] = {};
		vr::HmdQuaternion_t smQuat[ringSize] = {};
		double smPosEma[3] = {};
		vr::HmdQuaternion_t smQuatEma = {1, 0, 0, 0};
		bool haveSm = false;
		double time[ringSize] = {};
		int count = 0;   // valid entries
		int head = 0;    // next write slot
		// orientation alongside position, for angular velocity derivation
		vr::HmdQuaternion_t quat[ringSize] = {};
		// EMA over the least squares slope, so the substituted velocity is
		// smooth in time (discontinuities here become rendered pose jumps
		// through the runtime's forward prediction)
		bool haveEma = false;
		double emaVel[3] = {};
		bool haveEmaAng = false;
		double emaAng[3] = {};
		// joint peak hold: v and w captured at the most recent linear speed
		// peak, replayed with decay for ~90ms so a game sampling just after
		// release reads the intended throw instead of the hand's snap back
		double peakVel[3] = {};
		double peakAng[3] = {};
		double peakSpeed = 0;
		double peakTime = 0;
		// previous output speed, for peak plausibility (a single spiked
		// sample must not become a held peak)
		double lastOutSpeed = 0;
		double lastOutTime = 0;
		// hold is latched by violent deceleration and stays engaged until
		// the decay window ends or a new peak latches
		bool holdActive = false;
		// release gesture anchor (v5): set when the grip/trigger scalar
		// starts FALLING from its held plateau — the biomechanical moment
		// the hand begins letting go, which precedes every game's release
		// threshold. during the anchor window the output ratchets up with
		// rising motion and freezes against falling motion, so whatever
		// instant the game samples, it reads the throw's peak.
		double anchorTime = 0;
		bool anchorHasValue = false;
		double anchorVel[3] = {};
		double anchorAng[3] = {};
		double anchorSpeed = 0;
	};
	// pose component tracking (ET hunt: gaze may be published as a pose
	// component; log creates and throttle updates from the HMD container)
	struct PoseComponentInfo {
		vr::PropertyContainerHandle_t container = 0;
		std::string name;
		uint64_t updates = 0;
		double lastLogTime = 0;
	};
	std::map<vr::VRInputComponentHandle_t, PoseComponentInfo> poseComponents = {};
public:
	void AnchorReleaseGesture(uint32_t openVRID);
private:
	std::map<uint32_t, VelFixState> velFixStates = {};
	// returns true and writes the derived velocity when the window is usable
	bool DeriveMotion(uint32_t openVRID, const vr::DriverPose_t &pose, double derivedVel[3], double derivedAng[3], double secantVel[3], double secantAng[3]);
	static void ComputeRingSecant(const VelFixState &state, bool useSmoothed, double secantVel[3], double secantAng[3]);
	// cached device classes (Prop_DeviceClass_Int32), resolved on first pose
	std::map<uint32_t, int> deviceClasses = {};
	// streamed-controller identity cache (serial prefix VRLINK*/SamsungVST*
	// = vrlink device). the velocity fix must never touch lighthouse
	// devices: their native velocity is correct and mixed sessions
	// (knuckles + playspace override) are a supported setup. queried once
	// per id OUTSIDE any lock, then cached.
	std::map<uint32_t, int> streamedControllerCache = {};
	std::mutex streamedIdentityLock;
	bool IsStreamedController(uint32_t openVRID);
	// derive-mode adaptive smoothing state (pure math under its own lock;
	// never calls out — lock discipline)
	// one coherent estimated kinematic state per controller (kalman mode):
	// per-axis constant-velocity Kalman for position/velocity, quaternion
	// state integrated by the filtered angular velocity and corrected by
	// measurements (MEKF-lite: residual rotation vector drives per-axis
	// CV Kalman filters for the angular channel). guarded by
	// deriveFilterLock; pure math only under the lock.
	struct KalState {
		bool have = false;
		double time = 0;
		double p[3] = {};
		double v[3] = {};
		// per-axis covariance [Ppp, Ppv, Pvv]
		double P[3][3] = {};
		vr::HmdQuaternion_t q = {1, 0, 0, 0};
		double w[3] = {};
		double Pa[3][3] = {}; // angular per-axis covariance
		bool announced = false;
		// telemetry only: EMA of normalized innovation squared (NIS ~ 1
		// when the noise models match reality) + diag log throttle
		double nisEma = 1.0;
		double lastDiagLog = 0;
		// raw input-step telemetry (FOV / tracking artifact watch): max
		// single-step measurement distance and count of near-zero steps
		// while the state was moving, since the last KALDIAG line
		double lastMeas[3] = {};
		bool haveMeas = false;
		double stepMax = 0;
		int stepFrozen = 0;
		int dupSkipped = 0;
		// device-time measurement stamping (correctness pass): timestamp
		// of the last ACCEPTED measurement on the device clock
		// (receipt + poseTimeOffset); receipt-time ks.time stays as the
		// legacy clock so the off-toggle reproduces old behavior exactly
		double tMeas = 0;
		// continuous dup-coast tracking for the runaway cap: receipt time
		// the current coast run began, or -1 when not coasting
		double coastStart = -1.0;
		// per-window diag: dropped out-of-order samples, accepted-dt
		// stats (ms), longest continuous coast (ms)
		int dtBack = 0;
		double dtSumMs = 0;
		int dtN = 0;
		double dtMaxMs = 0;
		double coastMaxMs = 0;
		int gazeBends = 0;
		double gazeBendSum = 0;
		double gazeBendMax = 0;
		// velocity history ring for the release-rewind experiment: ~260ms
		// of (t, v, w) at stream cadence, plus the rewind window armed by
		// the input tap when the experiment is enabled
		static constexpr int histSize = 24;
		double histT[histSize] = {};
		double histV[histSize][3] = {};
		double histW[histSize][3] = {};
		double histP[histSize][3] = {};
		vr::HmdQuaternion_t histQ[histSize] = {};
		int histHead = 0;
		int histCount = 0;
		double rewindUntil = 0;
		double rewindTarget = 0;
		// slow-direction EMA copies for split reporting
		double vSlow[3] = {};
		double wSlow[3] = {};
		bool haveSlow = false;
		// knob echo, so live tuning re-announces in the log
		double lastQa = -1;
		// parallel fast velocity estimator (magnitude channel): per-axis
		// CV kalman over the same measurements with its own accel
		double pF[3] = {};
		double vF[3] = {};
		double PF[3][3] = {};
		bool haveFast = false;
	};
	std::map<uint32_t, KalState> kalStates;
	// latest HMD orientation, for rotating the head-space gaze ray into
	// driver space (guarded by deriveFilterLock)
	vr::HmdQuaternion_t hmdQuatForGaze = {1, 0, 0, 0};
	bool haveHmdQuat = false;
	bool gazeAssistAnnounced = false;
	struct DeriveFilterState {
		double vel[3] = {};
		double ang[3] = {};
		double time = 0;
		bool have = false;
		// split-direction support: short ring of RAW estimator outputs.
		// the axis-wise EMA above smooths magnitude well but its
		// direction is noise dominated except at the highest speeds
		// (field data 2026-08-09: 73-83 deg/sample median direction
		// swings at ~0.5 m/s). direction is instead taken from a
		// speed^pow weighted sum of these raw samples inside a short
		// window, so high-SNR samples pin it. 16 slots at 3-10ms
		// spacing covers the whole allowed window range (5-200ms is
		// clamped in the consumer; older entries simply age out).
		static const int dirRingSize = 16;
		double dirTime[dirRingSize] = {};
		double dirVel[dirRingSize][3] = {};
		double dirAng[dirRingSize][3] = {};
		int dirHead = 0;
		int dirCount = 0;
		bool splitLogged = false;
		// scalar magnitude channel: EMA of |raw| directly. the vector EMA's
		// magnitude CANCELS during direction changes (opposing components
		// average toward zero), which both jitters and under-reads; the
		// scalar EMA smooths the speed itself (field data 2026-08-10: the
		// vector-EMA output still jittered 8-15%/sample at 10ms cadence)
		double magEma = 0;
		double angMagEma = 0;
		// release latch: per-channel rolling peaks of the OUTPUT over the
		// latch window. v replays from the linear-peak moment, w from the
		// angular-peak moment (a single combined key poisoned arm throws
		// with the windup vector — field 2026-08-10)
		double linPeakMag = 0;
		double linPeakVel[3] = {};
		double linPeakTime = 0;
		double angPeakMag = 0;
		double angPeakVel[3] = {};
		double angPeakTime = 0;
		double latchUntil = 0;
		// last direction source, so a mid-session source switch re-logs
		int lastSource = -1;
		// throttle for the BURSTDIR diagnostic line (direction-source
		// comparison data; written from the fix block outside all locks)
		double lastDirLogTime = 0;
	};
	std::map<uint32_t, DeriveFilterState> deriveFilterStates = {};
	std::mutex deriveFilterLock;
	int GetDeviceClass(uint32_t openVRID);
	
	// ---- release ground truth tap ----
	// vrlink publishes grip/trigger through IVRDriverInput booleans; the
	// injector forwards creates and updates here. on grip/trigger
	// transitions we log a snapshot of the motion state so every release in
	// a session shows exactly what velocity a game could have read and what
	// the tracking state was. this replaces theorizing about WHY a given
	// throw died (snap back? dropout? zero?) with direct evidence.
	struct InputComponentInfo {
		vr::PropertyContainerHandle_t container = 0;
		std::string name;
		bool lastValue = false;
		bool haveValue = false;
		bool interesting = false; // grip / trigger / squeeze / grab / pinch
		bool isScalar = false;
		float lastScalar = 0;
		bool scalarPressed = false;
		// distortion tuner control role, classified from the path at create:
		// 0 none, 1 joystick y (nudge), 2 a (band out), 3 b (band in),
		// 4 x (eye cycle), 5 y (reset band), 6 grip value (hold to save).
		// tuner values live in their own fields so the tuner never disturbs
		// lastValue/lastScalar, which the release forensics and velocity fix
		// use for edge and gesture detection.
		int tunerRole = 0;
		float tunerScalar = 0;
		bool tunerBool = false;
	};
	std::map<vr::VRInputComponentHandle_t, InputComponentInfo> inputComponents = {};
	// gate for tuner input capture on the hot component-update path
	std::atomic<bool> tunerInputActive {false};

	std::map<vr::PropertyContainerHandle_t, uint32_t> containerToId = {};
	struct MotionSnapshot {
		double time = 0;
		double outVel[3] = {};
		double outAng[3] = {};
		double outSpeed = 0;
		bool trackingOk = false;
		int result = 0;
	};
	std::map<uint32_t, MotionSnapshot> motionSnapshots = {};
	double lastReleaseLogTime = 0;
	double lastEdgeLogTime = 0;
	void LogReleaseSnapshot(vr::PropertyContainerHandle_t container, const std::string &name);
	uint32_t ResolveContainerId(vr::PropertyContainerHandle_t container);
	// the vrlink HMD device, stored at TrackedDeviceAdded so the real
	// per-eye projection frusta can be queried from its display component
	// (used for the gaze -> viewport mapping, same math the runtime uses
	// for GetEyeTrackedFoveationCenter)
	vr::ITrackedDeviceServerDriver* hmdDevice = nullptr;
	bool hmdProjectionQueried = false;
	bool hmdProjectionValid = false;
	float hmdProjection[2][4] = {}; // [eye][left,right,top,bottom]
public:
	// returns false until the display component has been queried successfully
	bool GetHmdProjectionRaw(int eye, float &left, float &right, float &top, float &bottom);
private:
public:
	// ---- distortion tuner input surface ----
	// aggregated latest controller state for the interactive distortion
	// tuner: largest-magnitude joystick y across hands, band/eye/reset
	// click states, and the max grip value. capture only happens while the
	// tuner is armed (cheap atomic gate on the hot update path).
	struct TunerInputState {
		float stickY = 0;
		float stickX = 0;
		bool bandOut = false;   // a click
		bool bandIn = false;    // b click
		bool eyeToggle = false; // x click
		bool resetBand = false; // y click
		bool segToggle = false; // joystick click (either stick). band tuner with
		                        // segments > 1 uses it for the EYE cycle (X walks
		                        // segments there); unused in classic sessions
		float grip = 0;
		float trigger = 0;
	};
	void SetTunerInputActive(bool active){ tunerInputActive.store(active, std::memory_order_relaxed); }
	void GetTunerInput(TunerInputState &out);
	// ---- controller aligner surface ----
	// latest post-offset controller pose + vrlink tip offset per hand
	// (0 = left, 1 = right), for the aligner's tip marker and pivot solve
	struct AlignControllerState {
		bool poseValid = false;
		double pos[3] = {0, 0, 0};
		vr::HmdQuaternion_t rot = {1, 0, 0, 0};
		double poseTime = 0;      // NowSeconds of last update
		bool tipValid = false;
		double tipLocal[3] = {0, 0, 0};
	};
	void GetAlignController(int hand, AlignControllerState &out);
	// while the aligner is active its WORKING offsets replace the configured
	// controller offsets in the pose path, so edits are live
	void SetAlignerOffsets(bool active, const double rotDeg[3], const double posCm[3]);
private:
	// controller aligner state (guarded by poseLogLock): per-hand pose/tip
	// capture, container->hand classification, live offset override
	AlignControllerState alignControllers[2] = {};
	std::map<vr::PropertyContainerHandle_t, int> containerHand;
	// containers whose /pose/tip belongs to left(0)/right(1), resolved from
	// the container's own controller-role property: vrlink publishes tip
	// poses on the paired hand devices, NOT the button controllers, so the
	// button-derived containerHand map cannot associate them (session 25)
	std::map<vr::PropertyContainerHandle_t, int> containerTipHand;
	bool alignerAppliedLogged = false;
	std::map<uint32_t, int> openVRIDHand;
	std::atomic<bool> alignerOverrideActive {false};
	double alignerRotDeg[3] = {0, 0, 0};
	double alignerPosCm[3] = {0, 0, 0};
public:
	void OnInputComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle);
	void OnBooleanComponentUpdated(vr::VRInputComponentHandle_t handle, bool value);
	void OnScalarComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle);
	void OnScalarComponentUpdated(vr::VRInputComponentHandle_t handle, float value);
	void OnPoseComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle);
	void OnPoseComponentUpdated(vr::VRInputComponentHandle_t handle, const vr::HmdMatrix34_t* offset, double timeOffset);
private:
};

// defined in HmdDriverFactory.cpp
extern CustomHeadsetDeviceProvider deviceProvider;
