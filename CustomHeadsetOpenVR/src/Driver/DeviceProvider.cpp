#include "DeviceProvider.h"
#include "DriverLog.h"
#include "DeviceShim.h"
#include "EyeTrackingTap.h"
#include "CompositorPlugin.h"
#include "HidModifier.h"

#include "Hooking/InterfaceHookInjector.h"

#include "../Headsets/MeganeX8K.h"
#include "../Headsets/DreamAir.h"
#include "../Headsets/GenericHeadset.h"
#include "../Headsets/FakeHeadset.h"
#include "../Helpers/EyeTrackingOutput.h"

#include "../Config/ConfigLoader.h"

#include <chrono>
#include <cmath>
#include <cstring>


// general driver functions
vr::EVRInitError CustomHeadsetDeviceProvider::Init(vr::IVRDriverContext *pDriverContext){
	// initialise this driver
	VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
	char driverPath[2048];
	vr::VRResources()->GetResourceFullPath("", "", driverPath, sizeof(driverPath));
	driverConfigLoader.info.steamvrResources = driverPath;
	vr::VRResources()->GetResourceFullPath("{CustomHeadsetOpenVR}", "", driverPath, sizeof(driverPath));
	driverConfigLoader.info.driverResources = driverPath;
	driverConfigLoader.Start();
	// inject hooks into functions
	InjectHooks(this, pDriverContext);
	hidModifier.InjectHooks();
	
	// arm the host hooks. the TrackedDeviceAdded/PoseUpdated hooks are only installed when a
	// driver requests IVRServerDriverHost through the hooked GetGenericInterface. drivers fetch
	// their host interface eagerly during their own init (VR_INIT_SERVER_DRIVER_CONTEXT ->
	// InitServer), so any driver that loaded before this one (e.g. vrlink) never triggers the
	// detour, and if no driver loads after this one the host hooks are never installed and no
	// devices get wrapped. requesting the interface here goes through the now hooked vtable and
	// installs the host hooks immediately, independent of driver load order.
	vr::EVRInitError hostHookError = vr::VRInitError_None;
	pDriverContext->GetGenericInterface(vr::IVRServerDriverHost_Version, &hostHookError);
	
	// arm the eye tracking tap hooks the same way. drivers that loaded before
	// this one (vrlink) may already hold a cached IVRDriverInput pointer, but
	// the hook patches the interface object's shared vtable, so requesting it
	// once here installs the CreateEyeTrackingComponent /
	// UpdateEyeTrackingComponent detours for every caller regardless of load
	// order.
	pDriverContext->GetGenericInterface(vr::IVRDriverInput_Version, &hostHookError);
	
	// the shim classes can be used to implement entirely new headsets, not just shim existing ones
	if(driverConfig.fakeHeadset.enable){
		FakeHeadset* fakeHeadsetImplementation = new FakeHeadset();
		fakeHeadsetImplementation->deviceProvider = this;
		shims.insert(fakeHeadsetImplementation);
		vr::ITrackedDeviceServerDriver* driver = new ShimTrackedDeviceDriver(fakeHeadsetImplementation, nullptr);
		vr::VRServerDriverHost()->TrackedDeviceAdded("FakeCustomHMD", vr::TrackedDeviceClass_HMD, driver);
	}
	
	return vr::VRInitError_None;
}
const char *const *CustomHeadsetDeviceProvider::GetInterfaceVersions(){
	return vr::k_InterfaceVersions;
}
bool CustomHeadsetDeviceProvider::ShouldBlockStandbyMode(){
	return false;
}
void CustomHeadsetDeviceProvider::Cleanup(){}
void CustomHeadsetDeviceProvider::EnterStandby(){}
void CustomHeadsetDeviceProvider::LeaveStandby(){}

void DebugEventLog(const vr::VREvent_t& vrevent){
	DriverLog("Event type: %d", vrevent.eventType);
	switch(vrevent.eventType){
		case vr::VREvent_PropertyChanged:
			DriverLog("Property changed: %i", vrevent.data.property.prop);
			break;
		case vr::VREvent_Compositor_DisplayReconnected:
			DriverLog("Compositor display reconnected");
			break;
		case vr::VREvent_ProcessConnected:
			DriverLog("Process connected %i", vrevent.data.process.pid);
			break;
	}
}

void CustomHeadsetDeviceProvider::RunFrame(){
	// acquire driverConfig.configLock for the duration of this function
	std::lock_guard<std::mutex> lock(driverConfigLock);
	
	hidModifier.RunFrame();
	
	#ifdef HAS_PRIVATE
	if(driverConfig.onlyHandlePrivateFunctionality){
		driverConfig.hasBeenUpdated = false;
		return;
	}
	#endif
		
	// process events that were submitted for this frame.
	vr::VREvent_t vrevent{};
	while(vr::VRServerDriverHost()->PollNextEvent(&vrevent, sizeof(vr::VREvent_t))){
		// DebugEventLog(vrevent);
		if(vrevent.eventType == VREvent_VendorSpecific_ContextCollection){
			// receive and store data from successful context collection events
			vr::VREvent_Reserved_t data = vrevent.data.reserved;
			if(data.reserved0 == VREvent_VendorSpecific_ContextCollection_MagicDataNumber){
				// add context based on the event data.
				uint32_t id = static_cast<uint32_t>(data.reserved1);
				vr::IVRDriverContext* ctx = (vr::IVRDriverContext*)data.reserved2;
				// logging here seems to deadlock on occasion
				// DriverLog("Received context collection event for device with ID: %d, Context: %p", id, ctx);	
				driverContextsByDeviceId[id] = ctx;
				// send any queued events
				if(queuedEvents.find(id) != queuedEvents.end()){
					for(const auto& event : queuedEvents[id]){
						SendVendorEvent(id, event.eventType, event.eventData, event.eventTimeOffset);
					}
					queuedEvents.erase(id);
				}
			}
		}
		if(vrevent.eventType == vr::VREvent_TrackedDeviceActivated){
			// set nonNativeHeadsetFound if a device with a direct mode component is found
			vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(vrevent.trackedDeviceIndex);
			if(container){
				// DriverLog("Device %d has driver direct mode component: %s", vrevent.trackedDeviceIndex, vr::VRProperties()->GetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool) ? "true" : "false");
				if(vr::VRProperties()->GetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool)){
					driverConfigLoader.info.nonNativeHeadsetFound = true;
					driverConfigLoader.WriteInfo();
				}
			}
		}
		if(vrevent.eventType == vr::VREvent_DashboardActivated){
			if(!driverConfigLoader.info.isDashboardOpen){
				driverConfigLoader.info.isDashboardOpen = true;
				driverConfigLoader.WriteInfo();
			}
		}
		if(vrevent.eventType == vr::VREvent_DashboardDeactivated){
			if(driverConfigLoader.info.isDashboardOpen){
				driverConfigLoader.info.isDashboardOpen = false;
				driverConfigLoader.WriteInfo();
			}
		}
		if(vrevent.eventType == vr::VREvent_ProcessConnected && customShaderEnabled){
			// check new processes and inject if they are the compositor
			InjectCompositorPlugin(vrevent.data.process.pid);
		}
		for(auto shim : shims){
			shim->HandleEvent(vrevent);
		}
	}
	for(auto shim : shims){
		if(shim->shimActive){
			shim->RunFrame();
		}
	}
	if(!customShaderEnabled && IsCustomShaderEnabled()){
		// try to inject when it is first enabled
		InjectCompositorPlugin();
		customShaderEnabled = true;
	}
	eyeTrackingOutput.RunFrame();
	// clear update flag at end of frame
	driverConfig.hasBeenUpdated = false;
}

void CustomHeadsetDeviceProvider::SendContextCollectionEvents(uint32_t id){
	for(auto driverContext : driverContexts){
		vr::EVRInitError eError = vr::VRInitError_None;
		vr::IVRServerDriverHost* VRServerDriverHost =  (vr::IVRServerDriverHost *)driverContext->GetGenericInterface(vr::IVRServerDriverHost_Version, &eError);
		// store data in event
		vr::VREvent_Data_t data = {VREvent_VendorSpecific_ContextCollection_MagicDataNumber, (uint64_t)id, (uint64_t)driverContext};
		// this event will only succeed for the driver that owns the id
		VRServerDriverHost->VendorSpecificEvent(id, VREvent_VendorSpecific_ContextCollection, data, 0);
	}
}

bool CustomHeadsetDeviceProvider::SendVendorEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t & eventData, double eventTimeOffset){
	if(driverContextsByDeviceId.find(unWhichDevice) != driverContextsByDeviceId.end()){
		vr::EVRInitError eError = vr::VRInitError_None;
		vr::IVRServerDriverHost* VRServerDriverHost =  (vr::IVRServerDriverHost *)driverContextsByDeviceId[unWhichDevice]->GetGenericInterface(vr::IVRServerDriverHost_Version, &eError);
		VRServerDriverHost->VendorSpecificEvent(unWhichDevice, eventType, eventData, eventTimeOffset);
		return true;
	}else{
		// try to find context and queue for later
		SendContextCollectionEvents(unWhichDevice);
		if(queuedEvents.find(unWhichDevice) == queuedEvents.end()){
			queuedEvents[unWhichDevice] = {};
		}
		queuedEvents[unWhichDevice].push_back({eventType, eventData, eventTimeOffset});
		return false;
	}
}

static vr::HmdQuaternion_t QuatMultiply(const vr::HmdQuaternion_t &a, const vr::HmdQuaternion_t &b){
	vr::HmdQuaternion_t r;
	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

static vr::HmdQuaternion_t QuatFromEulerDeg(const double deg[3]){
	// intrinsic x (pitch), then y (yaw), then z (roll), in the local frame
	double rx = deg[0] * 3.14159265358979323846 / 180.0 / 2.0;
	double ry = deg[1] * 3.14159265358979323846 / 180.0 / 2.0;
	double rz = deg[2] * 3.14159265358979323846 / 180.0 / 2.0;
	vr::HmdQuaternion_t qx = {cos(rx), sin(rx), 0, 0};
	vr::HmdQuaternion_t qy = {cos(ry), 0, sin(ry), 0};
	vr::HmdQuaternion_t qz = {cos(rz), 0, 0, sin(rz)};
	return QuatMultiply(QuatMultiply(qx, qy), qz);
}

static void QuatRotateVector(const vr::HmdQuaternion_t &q, const double v[3], double out[3]){
	// out = q * v * q^-1
	double tx = 2.0 * (q.y * v[2] - q.z * v[1]);
	double ty = 2.0 * (q.z * v[0] - q.x * v[2]);
	double tz = 2.0 * (q.x * v[1] - q.y * v[0]);
	out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
	out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
	out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

int CustomHeadsetDeviceProvider::GetDeviceClass(uint32_t openVRID){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = deviceClasses.find(openVRID);
		if(found != deviceClasses.end()){
			return found->second;
		}
	}
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(openVRID);
	vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
	int deviceClass = vr::VRProperties()->GetInt32Property(container, vr::Prop_DeviceClass_Int32, &propError);
	if(propError != vr::TrackedProp_Success){
		deviceClass = (int)vr::TrackedDeviceClass_Invalid;
	}
	std::lock_guard<std::mutex> guard(poseLogLock);
	deviceClasses[openVRID] = deviceClass;
	return deviceClass;
}

bool CustomHeadsetDeviceProvider::GetHmdProjectionRaw(int eye, float &left, float &right, float &top, float &bottom){
	if(!hmdProjectionQueried){
		hmdProjectionQueried = true;
		if(hmdDevice){
			void* component = hmdDevice->GetComponent(vr::IVRDisplayComponent_Version);
			if(component){
				vr::IVRDisplayComponent* display = (vr::IVRDisplayComponent*)component;
				for(int e = 0; e < 2; e++){
					display->GetProjectionRaw((vr::EVREye)e,
						&hmdProjection[e][0], &hmdProjection[e][1],
						&hmdProjection[e][2], &hmdProjection[e][3]);
				}
				hmdProjectionValid = true;
				DriverLog("DeviceProvider: hmd projection raw L(l=%.4f r=%.4f t=%.4f b=%.4f) R(l=%.4f r=%.4f t=%.4f b=%.4f)",
					hmdProjection[0][0], hmdProjection[0][1], hmdProjection[0][2], hmdProjection[0][3],
					hmdProjection[1][0], hmdProjection[1][1], hmdProjection[1][2], hmdProjection[1][3]);
			}else{
				DriverLog("DeviceProvider: hmd has no IVRDisplayComponent, gaze mapping falls back to tangent knobs");
			}
		}
	}
	if(!hmdProjectionValid || eye < 0 || eye > 1){
		return false;
	}
	left = hmdProjection[eye][0];
	right = hmdProjection[eye][1];
	top = hmdProjection[eye][2];
	bottom = hmdProjection[eye][3];
	return true;
}

uint32_t CustomHeadsetDeviceProvider::ResolveContainerId(vr::PropertyContainerHandle_t container){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = containerToId.find(container);
		if(found != containerToId.end()){
			return found->second;
		}
	}
	// containers are stable per device; probe the first few ids once
	for(uint32_t id = 0; id < 16; id++){
		if(vr::VRProperties()->TrackedDeviceToPropertyContainer(id) == container){
			std::lock_guard<std::mutex> guard(poseLogLock);
			containerToId[container] = id;
			return id;
		}
	}
	std::lock_guard<std::mutex> guard(poseLogLock);
	containerToId[container] = vr::k_unTrackedDeviceIndexInvalid;
	return vr::k_unTrackedDeviceIndexInvalid;
}

static bool InputPathInteresting(const std::string &lower){
	// anything that plausibly marks holding/releasing an object. session 8
	// taught us not to guess narrowly: 20 throws produced zero release
	// edges because the filter (and boolean-only hooking) missed vrlink's
	// actual grab control.
	if(lower.find("touch") != std::string::npos){
		return false;
	}
	return lower.find("grip") != std::string::npos
		|| lower.find("trigger") != std::string::npos
		|| lower.find("squeeze") != std::string::npos
		|| lower.find("grab") != std::string::npos
		|| lower.find("pinch") != std::string::npos;
}

// classify a component path into a distortion tuner control role. exact
// suffix matches against the confirmed vrlink surface (session log): joystick
// x/y scalars + joystick/a/b/x/y click booleans + grip value scalars.
static int TunerRoleForPath(const std::string &lower, bool isScalar){
	auto endsWith = [&](const char* suffix){
		size_t len = strlen(suffix);
		return lower.size() >= len && lower.compare(lower.size() - len, len, suffix) == 0;
	};
	if(isScalar){
		if(endsWith("/input/joystick/y")){ return 1; }
		if(endsWith("/input/grip/value")){ return 6; }
		if(endsWith("/input/joystick/x")){ return 7; }
		if(endsWith("/input/trigger/value")){ return 8; }
		return 0;
	}
	if(endsWith("/input/a/click")){ return 2; }
	if(endsWith("/input/b/click")){ return 3; }
	if(endsWith("/input/x/click")){ return 4; }
	if(endsWith("/input/y/click")){ return 5; }
	if(endsWith("/input/joystick/click")){ return 9; }
	return 0;
}

void CustomHeadsetDeviceProvider::OnInputComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle){
	if(!name || handle == vr::k_ulInvalidInputComponentHandle){
		return;
	}
	InputComponentInfo info;
	info.container = container;
	info.name = name;
	std::string lower = info.name;
	for(auto &c : lower){ c = (char)tolower(c); }
	info.interesting = InputPathInteresting(lower);
	info.tunerRole = TunerRoleForPath(lower, false);
	// hand classification from the quest layout: x/y buttons exist only on
	// the left controller, a/b only on the right. once known, resolve the
	// openVR id too so pose updates can be routed per hand.
	if(info.tunerRole >= 2 && info.tunerRole <= 5){
		int hand = (info.tunerRole == 2 || info.tunerRole == 3) ? 1 : 0;
		// resolve BEFORE taking poseLogLock: ResolveContainerId takes that
		// lock itself, and std::mutex is non-recursive — nesting it here
		// deadlocked vrserver at the first x/click creation and tripped a
		// SteamVR safe-mode block (session 24 regression)
		uint32_t id = ResolveContainerId(container);
		std::lock_guard<std::mutex> handGuard(poseLogLock);
		containerHand[container] = hand;
		if(id != vr::k_unTrackedDeviceIndexInvalid){
			openVRIDHand[id] = hand;
		}
	}
	// always log creates: component names are the map of vrlink's input
	// surface, and not having them cost a session
	DriverLog("InputTap: boolean component container=%llu path=%s handle=%llu%s",
		(unsigned long long)container, name, (unsigned long long)handle,
		info.interesting ? " [watched]" : "");
	std::lock_guard<std::mutex> guard(poseLogLock);
	inputComponents[handle] = info;
}

void CustomHeadsetDeviceProvider::OnScalarComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle){
	if(!name || handle == vr::k_ulInvalidInputComponentHandle){
		return;
	}
	InputComponentInfo info;
	info.container = container;
	info.name = name;
	info.isScalar = true;
	std::string lower = info.name;
	for(auto &c : lower){ c = (char)tolower(c); }
	info.interesting = InputPathInteresting(lower);
	info.tunerRole = TunerRoleForPath(lower, true);
	DriverLog("InputTap: scalar component container=%llu path=%s handle=%llu%s",
		(unsigned long long)container, name, (unsigned long long)handle,
		info.interesting ? " [watched]" : "");
	std::lock_guard<std::mutex> guard(poseLogLock);
	inputComponents[handle] = info;
}

void CustomHeadsetDeviceProvider::OnScalarComponentUpdated(vr::VRInputComponentHandle_t handle, float value){
	// distortion tuner capture: isolated fields so the tuner never disturbs
	// the release-forensics / velocity-fix state below, and gated by an
	// atomic so the hot path costs one relaxed load when the tuner is off
	if(tunerInputActive.load(std::memory_order_relaxed)){
		std::lock_guard<std::mutex> tunerGuard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found != inputComponents.end() && found->second.tunerRole != 0){
			found->second.tunerScalar = value;
		}
	}
	bool fixOn = driverConfig.streamFrame.velocityFixMode == 2;
	bool logOn = driverConfig.streamFrame.poseLogging;
	if(!fixOn && !logOn){
		return;
	}
	vr::PropertyContainerHandle_t container = 0;
	std::string name;
	bool release = false;
	bool gestureStart = false;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found == inputComponents.end() || !found->second.interesting){
			return;
		}
		InputComponentInfo &info = found->second;
		float previous = info.lastScalar;
		info.lastScalar = value;
		// release GESTURE start: the scalar begins falling from its held
		// plateau — the finger starts opening. this precedes every game's
		// own release threshold, so anchoring the velocity output here
		// means whatever instant the game samples, it reads the throw.
		if(info.scalarPressed && previous > 0.85f && value < previous - 0.03f){
			gestureStart = true;
			container = info.container;
		}
		// hysteresis so analog grabbing (value based grips) produces clean
		// held/released edges: pressed above 0.6, released below 0.25
		if(!info.scalarPressed && value > 0.6f){
			info.scalarPressed = true;
		}else if(info.scalarPressed && value < 0.25f){
			info.scalarPressed = false;
			release = true;
			container = info.container;
			name = info.name;
		}
	}
	if(gestureStart && fixOn){
		uint32_t id = ResolveContainerId(container);
		if(id != vr::k_unTrackedDeviceIndexInvalid){
			AnchorReleaseGesture(id);
		}
	}
	if(release && logOn){
		LogReleaseSnapshot(container, name);
	}
}

void CustomHeadsetDeviceProvider::OnBooleanComponentUpdated(vr::VRInputComponentHandle_t handle, bool value){
	if(tunerInputActive.load(std::memory_order_relaxed)){
		std::lock_guard<std::mutex> tunerGuard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found != inputComponents.end() && found->second.tunerRole != 0){
			found->second.tunerBool = value;
		}
	}
	if(!driverConfig.streamFrame.poseLogging){
		return;
	}
	vr::PropertyContainerHandle_t container = 0;
	std::string name;
	bool release = false;
	bool edge = false;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = inputComponents.find(handle);
		if(found == inputComponents.end()){
			return;
		}
		InputComponentInfo &info = found->second;
		bool changed = !info.haveValue || info.lastValue != value;
		bool wasHeld = info.haveValue && info.lastValue;
		info.haveValue = true;
		info.lastValue = value;
		if(!changed){
			return;
		}
		container = info.container;
		name = info.name;
		edge = true;
		release = info.interesting && wasHeld && !value;
	}
	if(release){
		LogReleaseSnapshot(container, name);
	}else if(edge){
		// low rate visibility of ALL boolean edges so the actual grab
		// control names itself in the log even if the watch filter misses
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		bool doLog = false;
		{
			std::lock_guard<std::mutex> guard(poseLogLock);
			if(now - lastEdgeLogTime >= 0.2){
				lastEdgeLogTime = now;
				doLog = true;
			}
		}
		if(doLog){
			DriverLog("InputTap: edge %s -> %d (id=%u)", name.c_str(), (int)value, ResolveContainerId(container));
		}
	}
}

void CustomHeadsetDeviceProvider::LogReleaseSnapshot(vr::PropertyContainerHandle_t container, const std::string &name){
	{
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		std::lock_guard<std::mutex> guard(poseLogLock);
		if(now - lastReleaseLogTime < 0.05){
			return; // 20Hz cap
		}
		lastReleaseLogTime = now;
	}
	uint32_t id = ResolveContainerId(container);
	// release latch trigger: arm the peak replay for this device the moment
	// the input tap reports the release. identity resolved ABOVE, outside
	// any lock; deriveFilterLock taken alone here (leaf, never nested)
	// EXPERIMENT B trigger: on release, arm the kalman rewind window
	if(driverConfig.streamFrame.velocityFixMode == 4
			&& driverConfig.streamFrame.kalmanReleaseRewindMs > 0.5
			&& IsStreamedController(id)){
		double nowRw = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		double holdS = driverConfig.streamFrame.kalmanRewindHoldMs / 1000.0;
		if(holdS < 0.02){ holdS = 0.02; }
		{
			std::lock_guard<std::mutex> rwGuard(deriveFilterLock);
			KalState &ksr = kalStates[id];
			ksr.rewindUntil = nowRw + holdS;
			ksr.rewindTarget = nowRw - driverConfig.streamFrame.kalmanReleaseRewindMs / 1000.0;
		}
		// outside the lock; bounded by the caller's release throttle
		DriverLog("VelocityFix: kalman rewind armed id=%u rewind=%.0fms hold=%.0fms",
			id, driverConfig.streamFrame.kalmanReleaseRewindMs, driverConfig.streamFrame.kalmanRewindHoldMs);
	}
	if(driverConfig.streamFrame.velocityFixMode == 3
			&& driverConfig.streamFrame.deriveReleaseLatch
			&& IsStreamedController(id)){
		double nowLatch = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		double holdS = driverConfig.streamFrame.deriveLatchHoldMs / 1000.0;
		if(holdS < 0.02){ holdS = 0.02; }
		{
			std::lock_guard<std::mutex> latchGuard(deriveFilterLock);
			deriveFilterStates[id].latchUntil = nowLatch + holdS;
		}
		// engagement confirmation, rate-limited by the caller's 20Hz release
		// throttle above; outside all locks
		DriverLog("VelocityFix: latch armed id=%u hold=%.0fms", id, holdS * 1000.0);
	}
	MotionSnapshot snap;
	bool haveSnap = false;
	double snapAge = -1;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = motionSnapshots.find(id);
		if(found != motionSnapshots.end()){
			snap = found->second;
			haveSnap = true;
			double now = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
			snapAge = (now - snap.time) * 1000.0;
		}
	}
	if(haveSnap){
		DriverLog("ReleaseSnap: id=%u %s released: out=(%.3f, %.3f, %.3f) |out|=%.3f ang=(%.2f, %.2f, %.2f) trackingOk=%d result=%d snapAge=%.1fms",
			id, name.c_str(),
			snap.outVel[0], snap.outVel[1], snap.outVel[2], snap.outSpeed,
			snap.outAng[0], snap.outAng[1], snap.outAng[2],
			(int)snap.trackingOk, snap.result, snapAge);
	}else{
		DriverLog("ReleaseSnap: id=%u %s released: no motion snapshot yet", id, name.c_str());
	}
}

void CustomHeadsetDeviceProvider::OnPoseComponentCreated(vr::PropertyContainerHandle_t container, const char* name, vr::VRInputComponentHandle_t handle){
	if(!name || handle == vr::k_ulInvalidInputComponentHandle){
		return;
	}
	// always log: gaze published as a pose component would be exactly the
	// "openvr paths" channel DFR tools bind (AngelDark report)
	DriverLog("InputTap: pose component container=%llu path=%s handle=%llu",
		(unsigned long long)container, name, (unsigned long long)handle);
	PoseComponentInfo info;
	info.container = container;
	info.name = name;
	// tip components: resolve which HAND this container's tip belongs to
	// from the container's own controller-role property (vrlink puts tip
	// poses on the paired hand devices, not the button controllers, so
	// button-derived hand maps can't associate them). resolved OUTSIDE
	// poseLogLock: property queries must never run under our lock.
	std::string nameStr = name;
	if(nameStr.size() >= 9 && nameStr.compare(nameStr.size() - 9, 9, "/pose/tip") == 0){
		vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
		int32_t role = vr::VRProperties()->GetInt32Property(container,
			vr::Prop_ControllerRoleHint_Int32, &propError);
		int hand = -1;
		if(propError == vr::TrackedProp_Success){
			if(role == vr::TrackedControllerRole_LeftHand){ hand = 0; }
			if(role == vr::TrackedControllerRole_RightHand){ hand = 1; }
		}
		DriverLog("InputTap: /pose/tip container=%llu role=%d -> hand=%s",
			(unsigned long long)container, (int)role,
			hand == 0 ? "LEFT" : (hand == 1 ? "RIGHT" : "UNKNOWN (aligner tip marker unavailable for it)"));
		if(hand >= 0){
			std::lock_guard<std::mutex> tipGuard(poseLogLock);
			containerTipHand[container] = hand;
		}
	}
	std::lock_guard<std::mutex> guard(poseLogLock);
	poseComponents[handle] = info;
}

void CustomHeadsetDeviceProvider::OnPoseComponentUpdated(vr::VRInputComponentHandle_t handle, const vr::HmdMatrix34_t* offset, double timeOffset){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	std::string name;
	uint64_t updates = 0;
	bool doLog = false;
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = poseComponents.find(handle);
		if(found == poseComponents.end()){
			return;
		}
		// tip offset capture for the controller aligner: /pose/tip is the
		// controller-local tip transform vrlink itself publishes
		if(offset && found->second.name.size() >= 9
				&& found->second.name.compare(found->second.name.size() - 9, 9, "/pose/tip") == 0){
			auto handFound = containerTipHand.find(found->second.container);
			if(handFound != containerTipHand.end()){
				AlignControllerState &state = alignControllers[handFound->second];
				state.tipValid = true;
				state.tipLocal[0] = offset->m[0][3];
				state.tipLocal[1] = offset->m[1][3];
				state.tipLocal[2] = offset->m[2][3];
			}
		}
		found->second.updates++;
		// first update always, then 1 per 5s per component
		if(found->second.updates == 1 || now - found->second.lastLogTime >= 5.0){
			found->second.lastLogTime = now;
			name = found->second.name;
			updates = found->second.updates;
			doLog = true;
		}
	}
	if(doLog && offset){
		DriverLog("InputTap: pose component %s update %llu offset=(%.4f, %.4f, %.4f) fwd=(%.4f, %.4f, %.4f) timeOffset=%.4f",
			name.c_str(), (unsigned long long)updates,
			offset->m[0][3], offset->m[1][3], offset->m[2][3],
			-offset->m[0][2], -offset->m[1][2], -offset->m[2][2],
			timeOffset);
	}
}

void CustomHeadsetDeviceProvider::AnchorReleaseGesture(uint32_t openVRID){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	std::lock_guard<std::mutex> guard(poseLogLock);
	VelFixState &state = velFixStates[openVRID];
	state.anchorTime = now;
	state.anchorHasValue = false; // next pose update seeds it
}

bool CustomHeadsetDeviceProvider::HandleDevicePoseUpdated(uint32_t openVRID, vr::DriverPose_t &pose){
	if(driverConfig.forceTracking){
		pose.poseIsValid = true;
		if(pose.result != vr::TrackingResult_Fallback_RotationOnly){
			pose.result = vr::TrackingResult_Running_OK;
		}
	}
	// controller pose offsets: local frame rotation and translation, applied
	// before velocity derivation so the ring tracks the adjusted origin.
	// (a config change mid-session moves the origin once; the teleport guard
	// resets the ring and the moment passes.)
	const ControllersConfig &controllersConfig = driverConfig.controllers;
	double rotationOffsetDeg[3];
	double positionOffsetCm[3];
	if(alignerOverrideActive.load(std::memory_order_relaxed)){
		// aligner working offsets replace the configured ones, so stick
		// edits and pivot solves are visible in the very next pose
		std::lock_guard<std::mutex> alignGuard(poseLogLock);
		for(int i = 0; i < 3; i++){
			rotationOffsetDeg[i] = alignerRotDeg[i];
			positionOffsetCm[i] = alignerPosCm[i];
		}
	}else{
		for(int i = 0; i < 3; i++){
			rotationOffsetDeg[i] = controllersConfig.rotationOffsetDeg[i];
			positionOffsetCm[i] = controllersConfig.positionOffsetCm[i];
		}
	}
	bool hasRotationOffset = rotationOffsetDeg[0] != 0
		|| rotationOffsetDeg[1] != 0 || rotationOffsetDeg[2] != 0;
	bool hasPositionOffset = positionOffsetCm[0] != 0
		|| positionOffsetCm[1] != 0 || positionOffsetCm[2] != 0;
	if((hasRotationOffset || hasPositionOffset) && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& GetDeviceClass(openVRID) == (int)vr::TrackedDeviceClass_Controller){
		if(hasPositionOffset){
			double local[3] = {
				positionOffsetCm[0] / 100.0,
				positionOffsetCm[1] / 100.0,
				positionOffsetCm[2] / 100.0,
			};
			double world[3];
			QuatRotateVector(pose.qRotation, local, world);
			pose.vecPosition[0] += world[0];
			pose.vecPosition[1] += world[1];
			pose.vecPosition[2] += world[2];
		}
		if(hasRotationOffset){
			pose.qRotation = QuatMultiply(pose.qRotation, QuatFromEulerDeg(rotationOffsetDeg));
		}
		if(alignerOverrideActive.load(std::memory_order_relaxed)){
			std::lock_guard<std::mutex> logGuard(poseLogLock);
			if(!alignerAppliedLogged){
				alignerAppliedLogged = true;
				DriverLog("Aligner: working offsets APPLYING to device id=%u (rot %.1f,%.1f,%.1f deg pos %.2f,%.2f,%.2f cm)",
					openVRID, rotationOffsetDeg[0], rotationOffsetDeg[1], rotationOffsetDeg[2],
					positionOffsetCm[0], positionOffsetCm[1], positionOffsetCm[2]);
			}
		}
	}
	// capture the post-offset pose per hand for the controller aligner (the
	// drawn tip marker must reflect the live working offsets)
	if(openVRID == vr::k_unTrackedDeviceIndex_Hmd && pose.poseIsValid){
		// cache head orientation for the gaze aim assist (leaf lock)
		std::lock_guard<std::mutex> hmdGuard(deriveFilterLock);
		hmdQuatForGaze = pose.qRotation;
		haveHmdQuat = true;
	}
	if(openVRID != vr::k_unTrackedDeviceIndex_Hmd && pose.poseIsValid){
		std::lock_guard<std::mutex> alignGuard(poseLogLock);
		auto handFound = openVRIDHand.find(openVRID);
		if(handFound != openVRIDHand.end()){
			AlignControllerState &state = alignControllers[handFound->second];
			state.poseValid = true;
			state.pos[0] = pose.vecPosition[0];
			state.pos[1] = pose.vecPosition[1];
			state.pos[2] = pose.vecPosition[2];
			state.rot = pose.qRotation;
			state.poseTime = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		}
	}
	// throw/velocity fix: substitute position-derived velocity when it is
	// meaningfully larger than the driver's smoothed report, so throw
	// releases carry true peak speed. cheap unsynchronized bool reads keep
	// the hot path free when both features are disabled.
	int velocityFixMode = driverConfig.streamFrame.velocityFixMode;
	if(velocityFixMode > 0 && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK
			&& IsStreamedController(openVRID)){
		bool classicMode = velocityFixMode == 1;
		bool deriveMode = velocityFixMode == 3;
		double derivedVel[3], derivedAng[3];
		double secantVel[3] = {0, 0, 0}, secantAng[3] = {0, 0, 0};
		// runtime's own report, captured before any substitution: its
		// magnitude is heavily smoothed but its DIRECTION comes from
		// device-side sensor fusion and is a candidate direction source
		double runtimeVel[3] = { pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2] };
		double runtimeAng[3] = { pose.vecAngularVelocity[0], pose.vecAngularVelocity[1], pose.vecAngularVelocity[2] };
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		// ==== KALMAN mode: one coherent estimated state, reported whole.
		// replicates the native lighthouse ARCHITECTURE: pose, velocity and
		// angular velocity all come from a single causal estimator, so the
		// runtime's forward prediction and game-side pose-history throw
		// estimators agree by construction. per-axis constant-velocity
		// Kalman for p/v; orientation integrated by the filtered w and
		// corrected by the measurement residual (MEKF-lite). ====
		if(velocityFixMode == 4){
			// gaze fetched BEFORE the filter lock (never call out under a
			// lock); freshness guarded 100ms like the frame consumer
			bool gazeFresh = false;
			double gazeHead[3] = {0, 0, -1};
			if(driverConfig.streamFrame.kalmanGazeAssist > 0.001){
				EyeTrackingTap::Sample gs;
				if(eyeTrackingTap.GetLatestSample(gs, 0.1) && gs.valid){
					double gx = gs.targetX - gs.originX;
					double gy = gs.targetY - gs.originY;
					double gz = gs.targetZ - gs.originZ;
					double gn = sqrt(gx * gx + gy * gy + gz * gz);
					if(gn > 1e-6){
						gazeHead[0] = gx / gn; gazeHead[1] = gy / gn; gazeHead[2] = gz / gn;
						gazeFresh = true;
					}
				}
			}
			double qa = driverConfig.streamFrame.kalmanProcessAccel;
			if(qa < 1.0){ qa = 1.0; }
			if(qa > 2000.0){ qa = 2000.0; }
			double rp = driverConfig.streamFrame.kalmanPosNoiseMm / 1000.0;
			if(rp < 0.0002){ rp = 0.0002; }
			double R = rp * rp;
			double qaA = driverConfig.streamFrame.kalmanProcessAngAccel;
			if(qaA < 10.0){ qaA = 10.0; }
			if(qaA > 20000.0){ qaA = 20000.0; }
			double ro = driverConfig.streamFrame.kalmanOriNoiseDeg * 3.14159265358979323846 / 180.0;
			if(ro < 0.0005){ ro = 0.0005; }
			double Ra = ro * ro;
			double lead = driverConfig.streamFrame.kalmanLeadMs / 1000.0;
			if(lead < 0){ lead = 0; }
			if(lead > 0.05){ lead = 0.05; }
			bool announceKalman = false;
			bool announceGaze = false;
			bool logKalDiag = false;
			double diagNis = 0;
			double diagStepMax = 0;
			int diagFrozen = 0;
			int diagDup = 0;
			int diagBends = 0;
			double diagBendMean = 0;
			double diagBendMax = 0;
			int diagDtBack = 0;
			double diagDtMean = 0;
			double diagDtMax = 0;
			double diagCoastMax = 0;
			double diagANis = 0;
			double diagFdtMean = 0;
			double diagFdtMax = 0;
			// device-time measurement stamp: the device says WHEN this
			// pose was true (poseTimeOffset); the filter previously
			// treated every sample as "now" — timing is the proven
			// pathology of this platform. sanity: an offset beyond
			// 100ms is not believed (falls back to receipt time).
			bool devTime = driverConfig.streamFrame.kalmanDeviceTime;
			double tOff = pose.poseTimeOffset;
			if(tOff < -0.1 || tOff > 0.1){ tOff = 0; }
			double tMeas = devTime ? now + tOff : now;
			{
			std::lock_guard<std::mutex> kalGuard(deriveFilterLock);
			KalState &ks = kalStates[openVRID];
			double dt = devTime ? tMeas - ks.tMeas : now - ks.time;
			bool dropSample = false;
			if(devTime && ks.have && dt <= 0 && dt > -0.2){
				// out-of-order on the device clock: this sample is OLDER
				// than the state. it carries no new information — drop
				// it. never reinit here: zeroing velocity mid-throw on a
				// late packet is exactly the failure the old dt<=0
				// reinit would produce once device time is in play.
				dropSample = true;
				ks.dtBack++;
			}
			// duplicate detection, BEFORE any clock or state commit: in
			// drop mode a detected repeat is treated as never having
			// arrived, so ks.tMeas must stay at the last ACCEPTED
			// measurement — the next real sample then predicts across
			// the full accumulated device-time gap in one honest step.
			// (committing the clock here would under-advance that
			// prediction and re-introduce the stale-stillness drag.)
			int dupMode = driverConfig.streamFrame.kalmanDupMode;
			bool dupHit = false;
			if(!dropSample && ks.have && dt > 0 && dt <= 0.2 && dupMode != 0 && ks.haveMeas){
				double ddx = pose.vecPosition[0] - ks.lastMeas[0];
				double ddy = pose.vecPosition[1] - ks.lastMeas[1];
				double ddz = pose.vecPosition[2] - ks.lastMeas[2];
				double stepD = sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
				double stSpd = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
				if(stepD < 0.0003 && stSpd > 0.5){
					// run cap (bug fix, caught live: NIS 570 for 8+s).
					// skipping repeats blocks the very measurements that
					// update the speed this gate tests, so an abrupt
					// stop could skip forever on stale velocity. a
					// repeat sustained past the cap IS stillness:
					// process it normally. applies to coast AND drop.
					double coastMax = driverConfig.streamFrame.kalmanDupCoastMaxMs / 1000.0;
					if(coastMax < 0.01){ coastMax = 0.01; }
					if(coastMax > 0.5){ coastMax = 0.5; }
					if(ks.coastStart < 0){ ks.coastStart = now; }
					double coastLen = now - ks.coastStart + dt;
					if(coastLen <= coastMax){
						dupHit = true;
						ks.dupSkipped++;
						double cMs = coastLen * 1000.0;
						if(cMs > ks.coastMaxMs){ ks.coastMaxMs = cMs; }
					}
				}
			}
			bool dupDrop = dupHit && dupMode == 2;
			if(dupHit && dupMode == 3){
				// SOFT: this repeat WILL be processed as a measurement,
				// but with honest noise for a sample of unknown age —
				// inflate R (and the angular Ra: the payload freezes as
				// a whole) for this callback only. gain on the repeat
				// shrinks ~k^2; covariance keeps accumulating through
				// the run, so the fresh sample's catch-up gain
				// self-schedules. k=1 is bit-identical to off.
				double sK = driverConfig.streamFrame.kalmanDupRScale;
				if(sK < 1.0){ sK = 1.0; }
				if(sK > 100.0){ sK = 100.0; }
				R *= sK * sK;
				Ra *= sK * sK;
			}
			if(dropSample || dupDrop){
				// state, clocks, and dt statistics untouched. the
				// reported pose repeats the last filtered state; the
				// runtime's forward prediction keeps the rendered hand
				// animating from the still-live velocity.
			}else if(!ks.have || dt <= 0 || dt > 0.2){
				ks.have = true;
				ks.time = now;
				ks.tMeas = tMeas;
				ks.coastStart = -1.0;
				for(int a2 = 0; a2 < 3; a2++){
					ks.p[a2] = pose.vecPosition[a2];
					ks.v[a2] = 0;
					ks.P[a2][0] = 0.01; ks.P[a2][1] = 0; ks.P[a2][2] = 1.0;
					ks.w[a2] = 0;
					ks.Pa[a2][0] = 0.05; ks.Pa[a2][1] = 0; ks.Pa[a2][2] = 10.0;
				}
				ks.q = pose.qRotation;
			}else{
				ks.time = now;
				ks.tMeas = tMeas;
				ks.dtSumMs += dt * 1000.0;
				ks.dtN++;
				if(dt * 1000.0 > ks.dtMaxMs){ ks.dtMaxMs = dt * 1000.0; }
				double dt2 = dt * dt;
				// dup decision was made above (coast and soft reach
				// here; drop never does — it exits via the drop path)
				bool dupCoast = dupHit && dupMode == 1;
				if(!dupHit){
					// any non-repeat sample ends the dup run. keyed on
					// dupHit, not dupCoast: soft-mode repeats are
					// processed but must still accumulate toward the
					// cap, or sustained stillness would stay distrusted
					ks.coastStart = -1.0;
				}
				if(dupCoast){
					// coast: advance both estimators along their velocity,
					// inflate covariance, NO measurement update (a repeat
					// is missing data, not evidence of stillness)
					for(int a2 = 0; a2 < 3; a2++){
						ks.p[a2] += ks.v[a2] * dt;
						ks.P[a2][0] += 2.0 * ks.P[a2][1] * dt + ks.P[a2][2] * dt2 + qa * qa * dt2 * dt2 / 4.0;
						ks.P[a2][1] += ks.P[a2][2] * dt + qa * qa * dt2 * dt / 2.0;
						ks.P[a2][2] += qa * qa * dt2;
						if(ks.haveFast){
							ks.pF[a2] += ks.vF[a2] * dt;
							double qaF = driverConfig.streamFrame.kalmanMagAccel;
							if(qaF < 1.0){ qaF = 1.0; }
							if(qaF > 2000.0){ qaF = 2000.0; }
							ks.PF[a2][0] += 2.0 * ks.PF[a2][1] * dt + ks.PF[a2][2] * dt2 + qaF * qaF * dt2 * dt2 / 4.0;
							ks.PF[a2][1] += ks.PF[a2][2] * dt + qaF * qaF * dt2 * dt / 2.0;
							ks.PF[a2][2] += qaF * qaF * dt2;
						}
					}
					// orientation coasts by the current angular velocity
					double halfDtC = 0.5 * dt;
					vr::HmdQuaternion_t dqc = {1.0, ks.w[0] * halfDtC, ks.w[1] * halfDtC, ks.w[2] * halfDtC};
					ks.q = QuatMultiply(dqc, ks.q);
					double qnc = sqrt(ks.q.w * ks.q.w + ks.q.x * ks.q.x + ks.q.y * ks.q.y + ks.q.z * ks.q.z);
					if(qnc > 1e-9){ ks.q.w /= qnc; ks.q.x /= qnc; ks.q.y /= qnc; ks.q.z /= qnc; }
				}else{
				// linear channel: per-axis constant-velocity Kalman
				double nisAccum = 0;
				for(int a2 = 0; a2 < 3; a2++){
					ks.p[a2] += ks.v[a2] * dt;
					double Ppp = ks.P[a2][0] + 2.0 * ks.P[a2][1] * dt + ks.P[a2][2] * dt2 + qa * qa * dt2 * dt2 / 4.0;
					double Ppv = ks.P[a2][1] + ks.P[a2][2] * dt + qa * qa * dt2 * dt / 2.0;
					double Pvv = ks.P[a2][2] + qa * qa * dt2;
					double y = pose.vecPosition[a2] - ks.p[a2];
					double S = Ppp + R;
					nisAccum += y * y / S;
					double Kp = Ppp / S;
					double Kv = Ppv / S;
					ks.p[a2] += Kp * y;
					ks.v[a2] += Kv * y;
					ks.P[a2][0] = (1.0 - Kp) * Ppp;
					ks.P[a2][1] = (1.0 - Kp) * Ppv;
					ks.P[a2][2] = Pvv - Kv * Ppv;
				}
				ks.nisEma += 0.1 * (nisAccum / 3.0 - ks.nisEma);
				// raw-step telemetry (EMA-free, so single-frame freezes or
				// teleports cannot hide): settles the FOV question
				{
					if(ks.haveMeas){
						double dx = pose.vecPosition[0] - ks.lastMeas[0];
						double dy = pose.vecPosition[1] - ks.lastMeas[1];
						double dz = pose.vecPosition[2] - ks.lastMeas[2];
						double step = sqrt(dx * dx + dy * dy + dz * dz);
						if(step > ks.stepMax){ ks.stepMax = step; }
						double stSpeed = sqrt(ks.v[0] * ks.v[0] + ks.v[1] * ks.v[1] + ks.v[2] * ks.v[2]);
						if(step < 0.0003 && stSpeed > 0.7){ ks.stepFrozen++; }
						// fresh-to-fresh clock: time between DISTINCT
						// raw samples on the measurement clock — the
						// tracker's true cadence (~8.3ms expected),
						// as opposed to dtMean's callback cadence.
						// mode-independent by design; also the
						// instrument for any transport-side fix.
						if(step >= 0.0003){
							if(ks.tFresh > 0){
								double fdt = (tMeas - ks.tFresh) * 1000.0;
								if(fdt > 0){
									ks.fdtSumMs += fdt;
									ks.fdtN++;
									if(fdt > ks.fdtMaxMs){ ks.fdtMaxMs = fdt; }
								}
							}
							ks.tFresh = tMeas;
						}
					}
					ks.haveMeas = true;
					ks.lastMeas[0] = pose.vecPosition[0];
					ks.lastMeas[1] = pose.vecPosition[1];
					ks.lastMeas[2] = pose.vecPosition[2];
				}
				// parallel FAST velocity estimator for the magnitude channel
				{
					double qaF = driverConfig.streamFrame.kalmanMagAccel;
					if(qaF < 1.0){ qaF = 1.0; }
					if(qaF > 2000.0){ qaF = 2000.0; }
					if(!ks.haveFast){
						ks.haveFast = true;
						for(int a2 = 0; a2 < 3; a2++){
							ks.pF[a2] = pose.vecPosition[a2];
							ks.vF[a2] = 0;
							ks.PF[a2][0] = 0.01; ks.PF[a2][1] = 0; ks.PF[a2][2] = 1.0;
						}
					}else{
						for(int a2 = 0; a2 < 3; a2++){
							ks.pF[a2] += ks.vF[a2] * dt;
							double Ppp = ks.PF[a2][0] + 2.0 * ks.PF[a2][1] * dt + ks.PF[a2][2] * dt2 + qaF * qaF * dt2 * dt2 / 4.0;
							double Ppv = ks.PF[a2][1] + ks.PF[a2][2] * dt + qaF * qaF * dt2 * dt / 2.0;
							double Pvv = ks.PF[a2][2] + qaF * qaF * dt2;
							double y = pose.vecPosition[a2] - ks.pF[a2];
							double S = Ppp + R;
							double Kp = Ppp / S;
							double Kv = Ppv / S;
							ks.pF[a2] += Kp * y;
							ks.vF[a2] += Kv * y;
							ks.PF[a2][0] = (1.0 - Kp) * Ppp;
							ks.PF[a2][1] = (1.0 - Kp) * Ppv;
							ks.PF[a2][2] = Pvv - Kv * Ppv;
						}
					}
				}
				// angular channel: predict q by w, correct by residual
				double halfDt = 0.5 * dt;
				vr::HmdQuaternion_t dq = {1.0, ks.w[0] * halfDt, ks.w[1] * halfDt, ks.w[2] * halfDt};
				vr::HmdQuaternion_t qPred = QuatMultiply(dq, ks.q);
				double qn = sqrt(qPred.w * qPred.w + qPred.x * qPred.x + qPred.y * qPred.y + qPred.z * qPred.z);
				if(qn > 1e-9){ qPred.w /= qn; qPred.x /= qn; qPred.y /= qn; qPred.z /= qn; }
				vr::HmdQuaternion_t qc = {qPred.w, -qPred.x, -qPred.y, -qPred.z};
				vr::HmdQuaternion_t qe = QuatMultiply(pose.qRotation, qc);
				double sgn = qe.w < 0 ? -1.0 : 1.0;
				double res[3] = { 2.0 * sgn * qe.x, 2.0 * sgn * qe.y, 2.0 * sgn * qe.z };
				double corr[3];
				double aNisAccum = 0;
				for(int a2 = 0; a2 < 3; a2++){
					double Ppp = ks.Pa[a2][0] + 2.0 * ks.Pa[a2][1] * dt + ks.Pa[a2][2] * dt2 + qaA * qaA * dt2 * dt2 / 4.0;
					double Ppv = ks.Pa[a2][1] + ks.Pa[a2][2] * dt + qaA * qaA * dt2 * dt / 2.0;
					double Pvv = ks.Pa[a2][2] + qaA * qaA * dt2;
					double S = Ppp + Ra;
					aNisAccum += res[a2] * res[a2] / S;
					double Kp = Ppp / S;
					double Kv = Ppv / S;
					corr[a2] = Kp * res[a2];
					ks.w[a2] += Kv * res[a2];
					ks.Pa[a2][0] = (1.0 - Kp) * Ppp;
					ks.Pa[a2][1] = (1.0 - Kp) * Ppv;
					ks.Pa[a2][2] = Pvv - Kv * Ppv;
				}
				ks.nisAEma += 0.1 * (aNisAccum / 3.0 - ks.nisAEma);
				vr::HmdQuaternion_t qCorr = {1.0, corr[0] * 0.5, corr[1] * 0.5, corr[2] * 0.5};
				ks.q = QuatMultiply(qCorr, qPred);
				double qn2 = sqrt(ks.q.w * ks.q.w + ks.q.x * ks.q.x + ks.q.y * ks.q.y + ks.q.z * ks.q.z);
				if(qn2 > 1e-9){ ks.q.w /= qn2; ks.q.x /= qn2; ks.q.y /= qn2; ks.q.z /= qn2; }
				}
			}
			// report THE STATE, whole and self consistent (optional fixed
			// forward lead, as native drivers use against transport lag)
			// history push (cheap, always on: keeps the rewind warm so
			// enabling the experiment mid-session works immediately)
			ks.histT[ks.histHead] = now;
			for(int a2 = 0; a2 < 3; a2++){
				ks.histV[ks.histHead][a2] = ks.v[a2];
				ks.histW[ks.histHead][a2] = ks.w[a2];
				ks.histP[ks.histHead][a2] = ks.p[a2];
			}
			ks.histQ[ks.histHead] = ks.q;
			ks.histHead = (ks.histHead + 1) % KalState::histSize;
			if(ks.histCount < KalState::histSize){ ks.histCount++; }
			double vRep[3] = { ks.v[0], ks.v[1], ks.v[2] };
			double wRep[3] = { ks.w[0], ks.w[1], ks.w[2] };
			double ksOutP[3] = {0, 0, 0};
			vr::HmdQuaternion_t ksOutQ = {1, 0, 0, 0};
			bool useSmoothOut = false;
			if(driverConfig.streamFrame.kalmanReleaseRewindMs > 0.5 && now < ks.rewindUntil && ks.histCount > 2){
				// find the history sample closest to the rewind target and
				// report it, blending back to live over the hold window
				int best = -1;
				double bestD = 1e9;
				for(int i = 0; i < ks.histCount; i++){
					double d = fabs(ks.histT[i] - ks.rewindTarget);
					if(d < bestD){ bestD = d; best = i; }
				}
				if(best >= 0 && bestD < 0.1){
					double holdS = driverConfig.streamFrame.kalmanRewindHoldMs / 1000.0;
					if(holdS < 0.02){ holdS = 0.02; }
					double frac = (ks.rewindUntil - now) / holdS; // 1 -> 0
					if(frac > 1){ frac = 1; }
					if(frac < 0){ frac = 0; }
					for(int a2 = 0; a2 < 3; a2++){
						vRep[a2] = ks.histV[best][a2] * frac + vRep[a2] * (1.0 - frac);
						wRep[a2] = ks.histW[best][a2] * frac + wRep[a2] * (1.0 - frac);
					}
				}
			}
			// direction/magnitude split: direction from the slow EMA,
			// magnitude live. slow copies always maintained (cheap) so the
			// knob engages instantly; gated at low speed where direction
			// is meaningless.
			{
				double dMs = driverConfig.streamFrame.kalmanDirSmoothMs;
				double aMs = driverConfig.streamFrame.kalmanAngDirSmoothMs;
				double sdt = dt;
				if(sdt <= 0 || sdt > 0.05){ sdt = 0.011; }
				if(!ks.haveSlow){
					ks.haveSlow = true;
					for(int a2 = 0; a2 < 3; a2++){ ks.vSlow[a2] = vRep[a2]; ks.wSlow[a2] = wRep[a2]; }
				}else{
					double aV = dMs > 0.5 ? 1.0 - exp(-sdt / (dMs / 1000.0)) : 1.0;
					double aW = aMs > 0.5 ? 1.0 - exp(-sdt / (aMs / 1000.0)) : 1.0;
					for(int a2 = 0; a2 < 3; a2++){
						ks.vSlow[a2] += aV * (vRep[a2] - ks.vSlow[a2]);
						ks.wSlow[a2] += aW * (wRep[a2] - ks.wSlow[a2]);
					}
				}
				if(dMs > 0.5){
					double mLive = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					double mSlow = sqrt(ks.vSlow[0] * ks.vSlow[0] + ks.vSlow[1] * ks.vSlow[1] + ks.vSlow[2] * ks.vSlow[2]);
					if(mLive > 0.3 && mSlow > 0.15){
						for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = ks.vSlow[a2] / mSlow * mLive; }
					}
				}
				if(aMs > 0.5){
					double mLiveW = sqrt(wRep[0] * wRep[0] + wRep[1] * wRep[1] + wRep[2] * wRep[2]);
					double mSlowW = sqrt(ks.wSlow[0] * ks.wSlow[0] + ks.wSlow[1] * ks.wSlow[1] + ks.wSlow[2] * ks.wSlow[2]);
					if(mLiveW > 1.0 && mSlowW > 0.5){
						for(int a2 = 0; a2 < 3; a2++){ wRep[a2] = ks.wSlow[a2] / mSlowW * mLiveW; }
					}
				}
			}
			// magnitude channel: |v| from the fast estimator on the calm
			// state's direction, then the always-on trim multipliers
			{
				if(driverConfig.streamFrame.kalmanMagSource == 1 && ks.haveFast){
					// STRICT channel separation (field 2026-08-10: every 180
					// flip traced to the fast estimator's DIRECTION leaking
					// into the output — via the raw-vector fallback in the
					// first version, via disagreement handoffs in the blend
					// version. the fast channel's direction is noise; it is
					// NEVER reported. direction comes from the calm state
					// only; fast contributes MAGNITUDE, and only once the
					// calm direction is established. otherwise the output
					// is pure calm: occasionally weak, never flipped.)
					double mDir = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					double mFast = sqrt(ks.vF[0] * ks.vF[0] + ks.vF[1] * ks.vF[1] + ks.vF[2] * ks.vF[2]);
					if(mDir > 0.3 && mFast > mDir){
						for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = vRep[a2] / mDir * mFast; }
					}
					// otherwise: pure calm output stands
				}
				double mS = driverConfig.streamFrame.kalmanMagScale;
				if(mS < 0.25){ mS = 0.25; }
				if(mS > 4.0){ mS = 4.0; }
				double mSA = driverConfig.streamFrame.kalmanAngMagScale;
				if(mSA < 0.25){ mSA = 0.25; }
				if(mSA > 4.0){ mSA = 4.0; }
				for(int a2 = 0; a2 < 3; a2++){
					vRep[a2] *= mS;
					wRep[a2] *= mSA;
				}
			}
			// gaze aim assist: bend the reported direction toward where
			// the eyes already are. direction only; magnitude preserved.
			{
				double assist = driverConfig.streamFrame.kalmanGazeAssist;
				if(assist > 0.001 && gazeFresh && haveHmdQuat){
					// values > 1 are the TEST regime: they shrink the
					// disagreement angle needed for full gaze takeover
					// (full lock at >= 45/G degrees). G=10 with maxDeg=180
					// locks any throw more than ~4.5 deg off gaze straight
					// onto it — for verifying the pipeline end to end.
					if(assist > 20.0){ assist = 20.0; }
					double mV = sqrt(vRep[0] * vRep[0] + vRep[1] * vRep[1] + vRep[2] * vRep[2]);
					if(mV > driverConfig.streamFrame.kalmanGazeMinSpeed){
						// rotate head-space gaze into driver space: g' = q g q*
						vr::HmdQuaternion_t q = hmdQuatForGaze;
						vr::HmdQuaternion_t gq = {0, gazeHead[0], gazeHead[1], gazeHead[2]};
						vr::HmdQuaternion_t qc = {q.w, -q.x, -q.y, -q.z};
						vr::HmdQuaternion_t t1 = QuatMultiply(q, gq);
						vr::HmdQuaternion_t gw = QuatMultiply(t1, qc);
						double gW[3] = { gw.x, gw.y, gw.z };
						double dirV[3] = { vRep[0] / mV, vRep[1] / mV, vRep[2] / mV };
						double d = dirV[0] * gW[0] + dirV[1] * gW[1] + dirV[2] * gW[2];
						if(d > -0.999 && d < 0.999){
							double angBetween = acos(d);
							// v2 weighting (field 2026-08-10: a fixed cap
							// neuters the assist on reversals — a 170-deg
							// wrong throw bent 30 deg is still wrong. small
							// disagreement = the hand is basically right,
							// refine gently; large disagreement at throw
							// speed = the hand data is invalid and gaze,
							// which fixated the target early, takes over.
							// weight ramps with disagreement: t = assist *
							// angle/45deg, clamped to 1 — continuous, no
							// thresholds. maxDeg remains as a pure safety
							// clamp on the final bend.
							double t = assist * (angBetween / (45.0 * 3.14159265358979323846 / 180.0));
							if(t > 1.0){ t = 1.0; }
							double bend = t * angBetween;
							double maxR = driverConfig.streamFrame.kalmanGazeMaxDeg * 3.14159265358979323846 / 180.0;
							if(bend > maxR){ bend = maxR; }
							if(bend > 1e-4){
								t = bend / angBetween;
								double nd[3];
								double nn = 0;
								for(int a2 = 0; a2 < 3; a2++){
									nd[a2] = dirV[a2] * (1.0 - t) + gW[a2] * t;
									nn += nd[a2] * nd[a2];
								}
								nn = sqrt(nn);
								if(nn > 1e-6){
									for(int a2 = 0; a2 < 3; a2++){ vRep[a2] = nd[a2] / nn * mV; }
								}
								ks.gazeBends++;
								double bendDeg = bend * 180.0 / 3.14159265358979323846;
								ks.gazeBendSum += bendDeg;
								if(bendDeg > ks.gazeBendMax){ ks.gazeBendMax = bendDeg; }
								if(!gazeAssistAnnounced){
									gazeAssistAnnounced = true;
									announceGaze = true;
								}
							}
						}
					}
				}
			}
			// fixed-lag smoothed reporting: fuse the stored forward state
			// at t-L with the current state backcast to t-L, and report the
			// WHOLE state (pose + velocities) from that instant, coherent.
			double smoothLag = driverConfig.streamFrame.kalmanSmoothLagMs / 1000.0;
			if(smoothLag > 0.001 && ks.histCount > 3){
				if(smoothLag > 0.2){ smoothLag = 0.2; }
				double tTarget = now - smoothLag;
				int best = -1;
				double bestD = 1e9;
				for(int i = 0; i < ks.histCount; i++){
					double dTi = fabs(ks.histT[i] - tTarget);
					if(dTi < bestD){ bestD = dTi; best = i; }
				}
				if(best >= 0 && bestD < 0.08){
					for(int a2 = 0; a2 < 3; a2++){
						double pBack = ks.p[a2] - ks.v[a2] * smoothLag;
						double pFwd = ks.histP[best][a2];
						double vFwd = ks.histV[best][a2];
						double wFwd = ks.histW[best][a2];
						ksOutP[a2] = 0.5 * (pFwd + pBack);
						vRep[a2] = 0.5 * (vFwd + vRep[a2]);
						wRep[a2] = 0.5 * (wFwd + wRep[a2]);
					}
					// orientation: backcast current q by -w*L, nlerp with
					// the stored q (hemisphere safe)
					double hb = -0.5 * smoothLag;
					vr::HmdQuaternion_t dqb = {1.0, ks.w[0] * hb, ks.w[1] * hb, ks.w[2] * hb};
					vr::HmdQuaternion_t qBack = QuatMultiply(dqb, ks.q);
					vr::HmdQuaternion_t qF = ks.histQ[best];
					double dotq = qF.w * qBack.w + qF.x * qBack.x + qF.y * qBack.y + qF.z * qBack.z;
					double sg = dotq < 0 ? -1.0 : 1.0;
					vr::HmdQuaternion_t qs = {
						0.5 * (sg * qF.w + qBack.w), 0.5 * (sg * qF.x + qBack.x),
						0.5 * (sg * qF.y + qBack.y), 0.5 * (sg * qF.z + qBack.z)};
					double qsn = sqrt(qs.w * qs.w + qs.x * qs.x + qs.y * qs.y + qs.z * qs.z);
					if(qsn > 1e-9){ qs.w /= qsn; qs.x /= qsn; qs.y /= qsn; qs.z /= qsn; }
					ksOutQ = qs;
					useSmoothOut = true;
				}
			}
			for(int a2 = 0; a2 < 3; a2++){
				pose.vecPosition[a2] = (useSmoothOut ? ksOutP[a2] : ks.p[a2]) + ks.v[a2] * lead;
				pose.vecVelocity[a2] = vRep[a2];
				pose.vecAngularVelocity[a2] = wRep[a2];
			}
			if(useSmoothOut){
				pose.qRotation = ksOutQ;
			}
			if(!useSmoothOut){
				if(lead > 0){
					double hl = 0.5 * lead;
					vr::HmdQuaternion_t dql = {1.0, ks.w[0] * hl, ks.w[1] * hl, ks.w[2] * hl};
					vr::HmdQuaternion_t ql = QuatMultiply(dql, ks.q);
					double n3 = sqrt(ql.w * ql.w + ql.x * ql.x + ql.y * ql.y + ql.z * ql.z);
					if(n3 > 1e-9){ ql.w /= n3; ql.x /= n3; ql.y /= n3; ql.z /= n3; }
					pose.qRotation = ql;
				}else{
					pose.qRotation = ks.q;
				}
			}
			{
				// announce on ANY kalman knob change (2026-08-11: the
				// P-sweep sessions were invisible in the log because
				// only qa re-announced — never again)
				double sig = driverConfig.streamFrame.kalmanProcessAccel
					+ driverConfig.streamFrame.kalmanPosNoiseMm * 1e3
					+ driverConfig.streamFrame.kalmanProcessAngAccel * 1e5
					+ driverConfig.streamFrame.kalmanOriNoiseDeg * 1e8
					+ driverConfig.streamFrame.kalmanLeadMs * 1e10
					+ driverConfig.streamFrame.kalmanDupMode * 1e12
					+ driverConfig.streamFrame.kalmanDupRScale * 1e13
					+ (driverConfig.streamFrame.kalmanDeviceTime ? 1e15 : 0)
					+ driverConfig.streamFrame.kalmanDupCoastMaxMs * 1e16;
				if(!ks.announced || ks.lastQa != sig){
					ks.announced = true;
					ks.lastQa = sig;
					announceKalman = true;
				}
			}
			if(driverConfig.streamFrame.poseLogging && now - ks.lastDiagLog >= 2.0){
				ks.lastDiagLog = now;
				diagNis = ks.nisEma;
				diagStepMax = ks.stepMax;
				diagFrozen = ks.stepFrozen;
				diagDup = ks.dupSkipped;
				diagBends = ks.gazeBends;
				diagBendMean = ks.gazeBends > 0 ? ks.gazeBendSum / ks.gazeBends : 0.0;
				diagBendMax = ks.gazeBendMax;
				diagDtBack = ks.dtBack;
				diagDtMean = ks.dtN > 0 ? ks.dtSumMs / ks.dtN : 0.0;
				diagDtMax = ks.dtMaxMs;
				diagCoastMax = ks.coastMaxMs;
				diagANis = ks.nisAEma;
				diagFdtMean = ks.fdtN > 0 ? ks.fdtSumMs / ks.fdtN : 0.0;
				diagFdtMax = ks.fdtMaxMs;
				ks.stepMax = 0;
				ks.stepFrozen = 0;
				ks.dupSkipped = 0;
				ks.gazeBends = 0;
				ks.gazeBendSum = 0;
				ks.gazeBendMax = 0;
				ks.dtBack = 0;
				ks.dtSumMs = 0;
				ks.dtN = 0;
				ks.dtMaxMs = 0;
				ks.coastMaxMs = 0;
				ks.fdtSumMs = 0;
				ks.fdtN = 0;
				ks.fdtMaxMs = 0;
				logKalDiag = true;
			}
			}
			if(announceKalman){
			// outside the lock — lock discipline
			DriverLog("VelocityFix: kalman mode active id=%u qa=%.0f rp=%.1fmm qaA=%.0f ro=%.2fdeg lead=%.0fms dup=%d dupR=%.0f devT=%d cap=%.0f log=%d",
				openVRID, driverConfig.streamFrame.kalmanProcessAccel,
				driverConfig.streamFrame.kalmanPosNoiseMm,
				driverConfig.streamFrame.kalmanProcessAngAccel,
				driverConfig.streamFrame.kalmanOriNoiseDeg,
				driverConfig.streamFrame.kalmanLeadMs,
				driverConfig.streamFrame.kalmanDupMode,
				driverConfig.streamFrame.kalmanDupRScale,
				driverConfig.streamFrame.kalmanDeviceTime ? 1 : 0,
				driverConfig.streamFrame.kalmanDupCoastMaxMs,
				driverConfig.streamFrame.poseLogging ? 1 : 0);
			}
			if(announceGaze){
				DriverLog("VelocityFix: gaze aim assist ENGAGED id=%u strength=%.2f maxDeg=%.0f", openVRID,
					driverConfig.streamFrame.kalmanGazeAssist, driverConfig.streamFrame.kalmanGazeMaxDeg);
			}
			if(logKalDiag){
				// tuning guide: NIS ~ 1 means the noise models match
				// reality; sustained > 3 = too stiff; < 0.3 = too loose
				DriverLog("PoseLog: KALDIAG id=%u nis=%.2f aNis=%.2f stepMax=%.1fmm frozenSteps=%d dupSkipped=%d dtBack=%d dtMean=%.2fms dtMax=%.1fms fdtMean=%.2fms fdtMax=%.1fms coastMax=%.0fms gazeBends=%d bendMean=%.1fdeg bendMax=%.1fdeg", openVRID, diagNis, diagANis, diagStepMax * 1000.0, diagFrozen, diagDup, diagDtBack, diagDtMean, diagDtMax, diagFdtMean, diagFdtMax, diagCoastMax, diagBends, diagBendMean, diagBendMax);
			}
		}
		// ==== end kalman mode ====
		if(DeriveMotion(openVRID, pose, derivedVel, derivedAng, secantVel, secantAng)){
			double derivedSpeed = sqrt(derivedVel[0] * derivedVel[0] + derivedVel[1] * derivedVel[1] + derivedVel[2] * derivedVel[2]);
			double derivedAngSpeed = sqrt(derivedAng[0] * derivedAng[0] + derivedAng[1] * derivedAng[1] + derivedAng[2] * derivedAng[2]);
			double reportedSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
				+ pose.vecVelocity[1] * pose.vecVelocity[1]
				+ pose.vecVelocity[2] * pose.vecVelocity[2]);
			double reportedAngSpeed = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
				+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
				+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
			// one weight for both channels so v and w stay phase consistent
			// (games compute released object velocity as v + w x gripOffset).
			// the 0.15 factor converts rad/s to an effective m/s so wrist
			// flick throws (w dominant, little linear motion) also engage.
			double linS = derivedSpeed > reportedSpeed ? derivedSpeed : reportedSpeed;
			double angS = derivedAngSpeed > reportedAngSpeed ? derivedAngSpeed : reportedAngSpeed;
			double sEff = linS > 0.15 * angS ? linS : 0.15 * angS;
			if(classicMode){
				sEff = linS; // classic: linear speeds only, as in v3
			}
			if(deriveMode){
				// derive: the runtime's velocity is discarded outright and
				// the pose-derived estimate is reported at all speeds — no
				// engage gate, no blend. caps fall back to the report (the
				// estimator flags a teleport, not a real motion, there).
				if(derivedSpeed < 20.0 && derivedAngSpeed < 60.0){
					// speed-adaptive smoothing: the endpoint-derivative
					// estimator is low lag but noisy, and with no engage
					// gate that noise trembles held objects. one alpha for
					// both channels keeps v and w phase consistent.
					double tauSlow = driverConfig.streamFrame.deriveSmoothTauSlowMs / 1000.0;
					double tauFast = driverConfig.streamFrame.deriveSmoothTauFastMs / 1000.0;
					double spLow = driverConfig.streamFrame.deriveSmoothSpeedLow;
					double spHigh = driverConfig.streamFrame.deriveSmoothSpeedHigh;
					if(tauSlow < 0.001){ tauSlow = 0.001; }
					if(tauFast < 0.001){ tauFast = 0.001; }
					if(spHigh <= spLow + 0.01){ spHigh = spLow + 0.01; }
					double sAdapt = derivedSpeed + 0.15 * derivedAngSpeed;
					double m = (sAdapt - spLow) / (spHigh - spLow);
					if(m < 0){ m = 0; }
					if(m > 1){ m = 1; }
					m = m * m * (3.0 - 2.0 * m);
					double tau = tauSlow + (tauFast - tauSlow) * m;
					// split-direction knobs read outside the lock (relaxed
					// consistency is fine: they only shape this frame's math)
					bool splitLin = driverConfig.streamFrame.deriveSplitDirLinear;
					bool splitAng = driverConfig.streamFrame.deriveSplitDirAngular;
					double dirWindow = driverConfig.streamFrame.deriveDirWindowMs / 1000.0;
					if(dirWindow < 0.005){ dirWindow = 0.005; }
					if(dirWindow > 0.2){ dirWindow = 0.2; }
					double dirPow = driverConfig.streamFrame.deriveDirWeightPow;
					if(dirPow < 0.0){ dirPow = 0.0; }
					if(dirPow > 6.0){ dirPow = 6.0; }
					bool logSplit = false;
					int dirSource = driverConfig.streamFrame.deriveDirSource;
					if(dirSource < 0 || dirSource > 2){ dirSource = 1; }
					bool logDirDiag = false;
					double diagOut[3] = {0, 0, 0};
					double diagRaw[3] = {0, 0, 0};
					double diagAngOut[3] = {0, 0, 0};
					{
						std::lock_guard<std::mutex> filterGuard(deriveFilterLock);
						DeriveFilterState &fs = deriveFilterStates[openVRID];
						double fdt = now - fs.time;
						if(!fs.have || fdt <= 0 || fdt > 0.1){
							for(int a = 0; a < 3; a++){
								fs.vel[a] = derivedVel[a];
								fs.ang[a] = derivedAng[a];
							}
							fs.magEma = derivedSpeed;
							fs.angMagEma = derivedAngSpeed;
							// a filter reset means a time gap or teleport:
							// the direction ring's history is equally stale
							fs.dirCount = 0;
							fs.dirHead = 0;
						}else{
							double alpha = 1.0 - exp(-fdt / tau);
							// angular channel: shared alpha (original, phase
							// locked) or its own adaptive alpha from the
							// angular knobs when separate smoothing is on
							double alphaAng = alpha;
							if(driverConfig.streamFrame.deriveSmoothAngSeparate){
								double aTauSlow = driverConfig.streamFrame.deriveSmoothAngTauSlowMs / 1000.0;
								double aTauFast = driverConfig.streamFrame.deriveSmoothAngTauFastMs / 1000.0;
								double aLow = driverConfig.streamFrame.deriveSmoothAngSpeedLow;
								double aHigh = driverConfig.streamFrame.deriveSmoothAngSpeedHigh;
								if(aTauSlow < 0.001){ aTauSlow = 0.001; }
								if(aTauFast < 0.001){ aTauFast = 0.001; }
								if(aHigh <= aLow + 0.01){ aHigh = aLow + 0.01; }
								double mA = (derivedAngSpeed - aLow) / (aHigh - aLow);
								if(mA < 0){ mA = 0; }
								if(mA > 1){ mA = 1; }
								mA = mA * mA * (3.0 - 2.0 * mA);
								double tauAng = aTauSlow + (aTauFast - aTauSlow) * mA;
								alphaAng = 1.0 - exp(-fdt / tauAng);
							}
							for(int a = 0; a < 3; a++){
								fs.vel[a] += alpha * (derivedVel[a] - fs.vel[a]);
								fs.ang[a] += alphaAng * (derivedAng[a] - fs.ang[a]);
							}
							// scalar magnitude channels, matching alphas
							fs.magEma += alpha * (derivedSpeed - fs.magEma);
							fs.angMagEma += alphaAng * (derivedAngSpeed - fs.angMagEma);
						}
						fs.time = now;
						fs.have = true;
						// always push the RAW estimate into the direction
						// ring (cheap), so toggling split live mid-session
						// starts with a warm window
						fs.dirTime[fs.dirHead] = now;
						for(int a = 0; a < 3; a++){
							fs.dirVel[fs.dirHead][a] = derivedVel[a];
							fs.dirAng[fs.dirHead][a] = derivedAng[a];
						}
						fs.dirHead = (fs.dirHead + 1) % DeriveFilterState::dirRingSize;
						if(fs.dirCount < DeriveFilterState::dirRingSize){ fs.dirCount++; }
						double outVel[3] = { fs.vel[0], fs.vel[1], fs.vel[2] };
						double outAng[3] = { fs.ang[0], fs.ang[1], fs.ang[2] };
						if(splitLin || splitAng){
							// direction basis per source. window (0): the
							// speed^pow weighted sum of ring samples —
							// kept for A/B, but the raw estimates' noise
							// is correlated across the window so it helps
							// little. secant (1): raw displacement across
							// the derive ring. runtime (2): vrlink's own
							// reported vector.
							double basisVel[3] = { 0, 0, 0 };
							double basisAng[3] = { 0, 0, 0 };
							if(dirSource == 1){
								for(int a = 0; a < 3; a++){
									basisVel[a] = secantVel[a];
									basisAng[a] = secantAng[a];
								}
							}else if(dirSource == 2){
								for(int a = 0; a < 3; a++){
									basisVel[a] = runtimeVel[a];
									basisAng[a] = runtimeAng[a];
								}
							}else{
								for(int i = 0; i < fs.dirCount; i++){
									int idx = (fs.dirHead + DeriveFilterState::dirRingSize - 1 - i) % DeriveFilterState::dirRingSize;
									if(now - fs.dirTime[idx] > dirWindow){
										break;
									}
									double sv = sqrt(fs.dirVel[idx][0] * fs.dirVel[idx][0]
										+ fs.dirVel[idx][1] * fs.dirVel[idx][1]
										+ fs.dirVel[idx][2] * fs.dirVel[idx][2]);
									double sa = sqrt(fs.dirAng[idx][0] * fs.dirAng[idx][0]
										+ fs.dirAng[idx][1] * fs.dirAng[idx][1]
										+ fs.dirAng[idx][2] * fs.dirAng[idx][2]);
									double wv = pow(sv, dirPow);
									double wa = pow(sa, dirPow);
									for(int a = 0; a < 3; a++){
										basisVel[a] += fs.dirVel[idx][a] * wv;
										basisAng[a] += fs.dirAng[idx][a] * wa;
									}
								}
							}
							if(splitLin){
								double mag = driverConfig.streamFrame.deriveMagSource == 1
									? fs.magEma
									: sqrt(fs.vel[0] * fs.vel[0] + fs.vel[1] * fs.vel[1] + fs.vel[2] * fs.vel[2]);
								double dn = sqrt(basisVel[0] * basisVel[0] + basisVel[1] * basisVel[1] + basisVel[2] * basisVel[2]);
								if(dn > 1e-9 && mag > 1e-9){
									for(int a = 0; a < 3; a++){
										outVel[a] = basisVel[a] / dn * mag;
									}
								}
							}
							if(splitAng){
								double magA = driverConfig.streamFrame.deriveMagSource == 1
									? fs.angMagEma
									: sqrt(fs.ang[0] * fs.ang[0] + fs.ang[1] * fs.ang[1] + fs.ang[2] * fs.ang[2]);
								double dnA = sqrt(basisAng[0] * basisAng[0] + basisAng[1] * basisAng[1] + basisAng[2] * basisAng[2]);
								if(dnA > 1e-9 && magA > 1e-9){
									for(int a = 0; a < 3; a++){
										outAng[a] = basisAng[a] / dnA * magA;
									}
								}
							}
							if(!fs.splitLogged || fs.lastSource != dirSource){
								fs.splitLogged = true;
								fs.lastSource = dirSource;
								logSplit = true;
							}
						}else{
							// re-log if it gets re-enabled after being off
							fs.splitLogged = false;
						}
						// release latch: keep the rolling window peak of the
						// OUTPUT, and if an input release armed the latch,
						// replay the peak (full strength for the first half
						// of the hold, linear decay after)
						{
							// per-channel peaks: each channel keyed on ITS
							// OWN speed, so arm-throw windup (angular spike,
							// backward v) can never poison the linear replay
							// peaks keyed on the SECANT magnitudes, not on
							// |out|: the scalar-EMA magnitude crests AFTER
							// physical release (lag), by which time the
							// direction has reversed — |out|-keyed peaks
							// captured the snap-back (field 2026-08-10:
							// release dir 121 deg median off the true peak,
							// magnitude 1.4-3x). the secant COLLAPSES at
							// reversal (forward and back cancel), so a
							// secant-keyed peak structurally cannot land in
							// the snap-back.
							double secSp = sqrt(secantVel[0] * secantVel[0] + secantVel[1] * secantVel[1] + secantVel[2] * secantVel[2]);
							double secAngSp = sqrt(secantAng[0] * secantAng[0] + secantAng[1] * secantAng[1] + secantAng[2] * secantAng[2]);
							double latchWindow = driverConfig.streamFrame.deriveLatchWindowMs / 1000.0;
							if(latchWindow < 0.02){ latchWindow = 0.02; }
							if(secSp >= fs.linPeakMag || now - fs.linPeakTime > latchWindow){
								fs.linPeakMag = secSp;
								fs.linPeakTime = now;
								for(int a = 0; a < 3; a++){
									fs.linPeakVel[a] = outVel[a];
								}
							}
							if(secAngSp >= fs.angPeakMag || now - fs.angPeakTime > latchWindow){
								fs.angPeakMag = secAngSp;
								fs.angPeakTime = now;
								for(int a = 0; a < 3; a++){
									fs.angPeakVel[a] = outAng[a];
								}
							}
							if(driverConfig.streamFrame.deriveReleaseLatch && now < fs.latchUntil){
								double holdS = driverConfig.streamFrame.deriveLatchHoldMs / 1000.0;
								if(holdS < 0.02){ holdS = 0.02; }
								double frac = (fs.latchUntil - now) / holdS; // 1 -> 0
								double w = frac * 2.0;
								if(w > 1.0){ w = 1.0; }
								if(w < 0.0){ w = 0.0; }
								// each channel replays only if ITS peak passes
								// ITS gate — casual regrabs no longer twitch
								if(fs.linPeakMag > driverConfig.streamFrame.deriveLatchMinSpeed){
									for(int a = 0; a < 3; a++){
										outVel[a] = fs.linPeakVel[a] * w + outVel[a] * (1.0 - w);
									}
								}
								if(fs.angPeakMag > driverConfig.streamFrame.deriveLatchAngMinSpeed){
									for(int a = 0; a < 3; a++){
										outAng[a] = fs.angPeakVel[a] * w + outAng[a] * (1.0 - w);
									}
								}
								// pose-assist: continue the throw arc in the
								// REPORTED position for engines that derive
								// throws from pose deltas rather than
								// vecVelocity. offset = latched v * elapsed
								// hold time, faded with the same decay.
								if(driverConfig.streamFrame.deriveLatchPoseAssist
										&& fs.linPeakMag > driverConfig.streamFrame.deriveLatchMinSpeed){
									double holdSFull = driverConfig.streamFrame.deriveLatchHoldMs / 1000.0;
									if(holdSFull < 0.02){ holdSFull = 0.02; }
									double elapsed = holdSFull - (fs.latchUntil - now);
									if(elapsed < 0){ elapsed = 0; }
									for(int a = 0; a < 3; a++){
										pose.vecPosition[a] += fs.linPeakVel[a] * elapsed * w;
									}
								}
							}
							// consumer discriminator: zero the reported
							// velocity so a 2-minute field test proves
							// whether this game reads vecVelocity at all
							if(driverConfig.streamFrame.deriveDiagVelocity == 1){
								for(int a = 0; a < 3; a++){
									outVel[a] = 0;
									outAng[a] = 0;
								}
							}
						}
						pose.vecVelocity[0] = outVel[0];
						pose.vecVelocity[1] = outVel[1];
						pose.vecVelocity[2] = outVel[2];
						pose.vecAngularVelocity[0] = outAng[0];
						pose.vecAngularVelocity[1] = outAng[1];
						pose.vecAngularVelocity[2] = outAng[2];
						// direction-source diagnostic: while poseLogging is
						// on and the hand moves at throw speed, capture the
						// output plus every candidate direction vector so a
						// field log can rank the sources against reality
						// (100Hz throttle per device; values copied out and
						// the line written outside the lock)
						if(driverConfig.streamFrame.poseLogging){
							double outSp = sqrt(outVel[0] * outVel[0] + outVel[1] * outVel[1] + outVel[2] * outVel[2]);
							double outAngSp = sqrt(outAng[0] * outAng[0] + outAng[1] * outAng[1] + outAng[2] * outAng[2]);
							// effective speed so wrist flicks are captured
							if(outSp + 0.15 * outAngSp > 2.0 && now - fs.lastDirLogTime >= 0.01){
								fs.lastDirLogTime = now;
								logDirDiag = true;
								for(int a = 0; a < 3; a++){
									diagOut[a] = outVel[a];
									diagRaw[a] = derivedVel[a];
									diagAngOut[a] = outAng[a];
								}
							}
						}
					}
					if(logSplit){
						// outside the lock — lock discipline
						DriverLog("VelocityFix: split-dir active id=%u linear=%d angular=%d source=%s window=%.0fms pow=%.1f",
							openVRID, splitLin ? 1 : 0, splitAng ? 1 : 0,
							dirSource == 1 ? "secant" : (dirSource == 2 ? "runtime" : "window"),
							dirWindow * 1000.0, dirPow);
					}
					if(logDirDiag){
						// secantVel/runtimeVel are locals of this frame —
						// no lock needed; raw and out copied under the lock
						DriverLog("PoseLog: BURSTDIR id=%u out=(%.3f, %.3f, %.3f) raw=(%.3f, %.3f, %.3f) sec=(%.3f, %.3f, %.3f) run=(%.3f, %.3f, %.3f)",
							openVRID,
							diagOut[0], diagOut[1], diagOut[2],
							diagRaw[0], diagRaw[1], diagRaw[2],
							secantVel[0], secantVel[1], secantVel[2],
							runtimeVel[0], runtimeVel[1], runtimeVel[2]);
						DriverLog("PoseLog: BURSTANG id=%u out=(%.3f, %.3f, %.3f) raw=(%.3f, %.3f, %.3f) sec=(%.3f, %.3f, %.3f) run=(%.3f, %.3f, %.3f)",
							openVRID,
							diagAngOut[0], diagAngOut[1], diagAngOut[2],
							derivedAng[0], derivedAng[1], derivedAng[2],
							secantAng[0], secantAng[1], secantAng[2],
							runtimeAng[0], runtimeAng[1], runtimeAng[2]);
					}
				}
			}else if(derivedSpeed < 20.0 && derivedAngSpeed < 60.0 && sEff > 1.0){
				double w = (sEff - 1.0) / 1.5;
				if(w > 1.0){ w = 1.0; }
				w = w * w * (3.0 - 2.0 * w); // smoothstep
				pose.vecVelocity[0] = pose.vecVelocity[0] * (1.0 - w) + derivedVel[0] * w;
				pose.vecVelocity[1] = pose.vecVelocity[1] * (1.0 - w) + derivedVel[1] * w;
				pose.vecVelocity[2] = pose.vecVelocity[2] * (1.0 - w) + derivedVel[2] * w;
				if(!classicMode){
					pose.vecAngularVelocity[0] = pose.vecAngularVelocity[0] * (1.0 - w) + derivedAng[0] * w;
					pose.vecAngularVelocity[1] = pose.vecAngularVelocity[1] * (1.0 - w) + derivedAng[1] * w;
					pose.vecAngularVelocity[2] = pose.vecAngularVelocity[2] * (1.0 - w) + derivedAng[2] * w;
				}
			}
			// record the motion snapshot for the release tap regardless of
			// what the peak hold decides below
			// (filled in after the hold logic so it reflects the final output)
			// full mode only from here: classic (v3) is estimator + blend
			// and nothing else; derive is pure replacement (no peak hold —
			// the estimate IS the signal, latching would re-introduce a
			// hybrid)
			if(!classicMode && !deriveMode)
			// joint peak hold: right after release the hand snaps back and
			// a low lag estimate faithfully reports that reversal, so games
			// sampling a frame or two late read a backward/down vector.
			// hold the (v, w) pair from the most recent linear speed peak,
			// decaying over 90ms, so late sampling still reads the throw.
			{
				const double holdSeconds = 0.07;
				std::lock_guard<std::mutex> guard(poseLogLock);
				VelFixState &state = velFixStates[openVRID];
				double outSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
					+ pose.vecVelocity[1] * pose.vecVelocity[1]
					+ pose.vecVelocity[2] * pose.vecVelocity[2]);
				double elapsed = now - state.peakTime;
				double decay = 1.0 - elapsed / holdSeconds;
				if(decay < 0){ decay = 0; }
				// plausibility: a step to more than 1.8x + 1 of the previous
				// output is a suspected spike; let it pass through this
				// frame but never latch it as a peak
				bool plausible = outSpeed <= state.lastOutSpeed * 1.8 + 1.0;
				// hold only engages on VIOLENT deceleration (>60 m/s^2 drop
				// from the previous output). post release snap back is
				// 100+ m/s^2, deliberate stops are 10-30, so normal play no
				// longer floats on held velocity (session 8 feedback).
				double sampleDt = now - state.lastOutTime;
				bool violentDecel = state.lastOutTime > 0 && sampleDt > 0.001 && sampleDt < 0.1
					&& (state.lastOutSpeed - outSpeed) / sampleDt > 60.0;
				// direction aware latch: a fresh peak may only be replaced
				// by motion in the same hemisphere. session 11 forensics: a
				// gentle lob's hand RETRACTION (faster than the lob itself)
				// latched as "the peak" and the direction hold then
				// enforced the downward retraction. opposite-direction
				// motion must wait out the freshness window (a new gesture)
				// before it can own the peak.
				bool sameHemisphere = true;
				if(now - state.peakTime < 0.25 && state.peakSpeed > 0.3 && outSpeed > 0.3){
					double dot = pose.vecVelocity[0] * state.peakVel[0]
						+ pose.vecVelocity[1] * state.peakVel[1]
						+ pose.vecVelocity[2] * state.peakVel[2];
					sameHemisphere = dot > 0;
				}
				if(plausible && sameHemisphere && outSpeed >= state.peakSpeed * decay){
					state.holdActive = false;
					state.peakSpeed = outSpeed;
					state.peakTime = now;
					state.peakVel[0] = pose.vecVelocity[0];
					state.peakVel[1] = pose.vecVelocity[1];
					state.peakVel[2] = pose.vecVelocity[2];
					state.peakAng[0] = pose.vecAngularVelocity[0];
					state.peakAng[1] = pose.vecAngularVelocity[1];
					state.peakAng[2] = pose.vecAngularVelocity[2];
				}else if(state.peakSpeed > 2.0 && decay > 0 && state.peakSpeed * decay > outSpeed
						&& (violentDecel || state.holdActive)){
					state.holdActive = true;
					pose.vecVelocity[0] = state.peakVel[0] * decay;
					pose.vecVelocity[1] = state.peakVel[1] * decay;
					pose.vecVelocity[2] = state.peakVel[2] * decay;
					pose.vecAngularVelocity[0] = state.peakAng[0] * decay;
					pose.vecAngularVelocity[1] = state.peakAng[1] * decay;
					pose.vecAngularVelocity[2] = state.peakAng[2] * decay;
				}
				// direction hold: within 120ms of a real peak, keep the
				// OUTPUT DIRECTION aligned with the peak's direction while
				// magnitude stays honest. session 10 forensics: throw
				// releases scatter up to ~60ms after the velocity peak, and
				// by then the upward component has flipped sign — the item
				// dives ("textbook failure") even though speed is fine.
				// direction hold fixes that without the magnitude float
				// that the violent-decel gate was added to prevent, because
				// prediction still moves hands at the true (decaying) speed.
				double elapsedDir = now - state.peakTime;
				if(state.peakSpeed > 1.5 && elapsedDir < 0.12
						&& outSpeed > 0.25 * state.peakSpeed && outSpeed > 0.3){
					double hw = 1.0 - elapsedDir / 0.12;
					double curSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
						+ pose.vecVelocity[1] * pose.vecVelocity[1]
						+ pose.vecVelocity[2] * pose.vecVelocity[2]);
					double peakMag = state.peakSpeed;
					if(curSpeed > 0.001 && peakMag > 0.001){
						// nlerp between near-opposite unit vectors passes
						// through ~zero and normalizes into noise (= the
						// "random direction" throws). opposite-direction
						// current output within the window IS the snap back
						// we are protecting against: use the peak direction
						// outright.
						double dirDot = (pose.vecVelocity[0] * state.peakVel[0]
							+ pose.vecVelocity[1] * state.peakVel[1]
							+ pose.vecVelocity[2] * state.peakVel[2]) / (curSpeed * peakMag);
						double useHw = dirDot < 0.1 ? 1.0 : hw;
						double dir[3];
						double blended[3] = {
							pose.vecVelocity[0] / curSpeed * (1.0 - useHw) + state.peakVel[0] / peakMag * useHw,
							pose.vecVelocity[1] / curSpeed * (1.0 - useHw) + state.peakVel[1] / peakMag * useHw,
							pose.vecVelocity[2] / curSpeed * (1.0 - useHw) + state.peakVel[2] / peakMag * useHw,
						};
						double bn = sqrt(blended[0] * blended[0] + blended[1] * blended[1] + blended[2] * blended[2]);
						if(bn > 0.001){
							dir[0] = blended[0] / bn; dir[1] = blended[1] / bn; dir[2] = blended[2] / bn;
							pose.vecVelocity[0] = dir[0] * curSpeed;
							pose.vecVelocity[1] = dir[1] * curSpeed;
							pose.vecVelocity[2] = dir[2] * curSpeed;
						}
					}
					// same treatment for angular velocity so w x r keeps
					// steering with the throw
					double curAng = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
						+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
						+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
					double peakAngMag = sqrt(state.peakAng[0] * state.peakAng[0]
						+ state.peakAng[1] * state.peakAng[1] + state.peakAng[2] * state.peakAng[2]);
					if(curAng > 0.05 && peakAngMag > 0.05){
						double angDot = (pose.vecAngularVelocity[0] * state.peakAng[0]
							+ pose.vecAngularVelocity[1] * state.peakAng[1]
							+ pose.vecAngularVelocity[2] * state.peakAng[2]) / (curAng * peakAngMag);
						double useHwAng = angDot < 0.1 ? 1.0 : hw;
						double blended[3] = {
							pose.vecAngularVelocity[0] / curAng * (1.0 - useHwAng) + state.peakAng[0] / peakAngMag * useHwAng,
							pose.vecAngularVelocity[1] / curAng * (1.0 - useHwAng) + state.peakAng[1] / peakAngMag * useHwAng,
							pose.vecAngularVelocity[2] / curAng * (1.0 - useHwAng) + state.peakAng[2] / peakAngMag * useHwAng,
						};
						double bn = sqrt(blended[0] * blended[0] + blended[1] * blended[1] + blended[2] * blended[2]);
						if(bn > 0.001){
							pose.vecAngularVelocity[0] = blended[0] / bn * curAng;
							pose.vecAngularVelocity[1] = blended[1] / bn * curAng;
							pose.vecAngularVelocity[2] = blended[2] / bn * curAng;
						}
					}
				}
				// release gesture anchor takes precedence over the
				// heuristic holds above: from the moment the finger starts
				// opening (scalar falling from plateau) until 150ms later,
				// the output RATCHETS — rising motion updates it, falling
				// motion cannot degrade it. games sample their release
				// anywhere in this window; all of them read the peak.
				if(now - state.anchorTime < 0.15 && state.anchorTime > 0){
					double curOutSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
						+ pose.vecVelocity[1] * pose.vecVelocity[1]
						+ pose.vecVelocity[2] * pose.vecVelocity[2]);
					if(!state.anchorHasValue || curOutSpeed > state.anchorSpeed){
						state.anchorHasValue = true;
						state.anchorSpeed = curOutSpeed;
						state.anchorVel[0] = pose.vecVelocity[0];
						state.anchorVel[1] = pose.vecVelocity[1];
						state.anchorVel[2] = pose.vecVelocity[2];
						state.anchorAng[0] = pose.vecAngularVelocity[0];
						state.anchorAng[1] = pose.vecAngularVelocity[1];
						state.anchorAng[2] = pose.vecAngularVelocity[2];
					}else{
						pose.vecVelocity[0] = state.anchorVel[0];
						pose.vecVelocity[1] = state.anchorVel[1];
						pose.vecVelocity[2] = state.anchorVel[2];
						pose.vecAngularVelocity[0] = state.anchorAng[0];
						pose.vecAngularVelocity[1] = state.anchorAng[1];
						pose.vecAngularVelocity[2] = state.anchorAng[2];
					}
				}
				state.lastOutSpeed = outSpeed;
				state.lastOutTime = now;
				MotionSnapshot &snap = motionSnapshots[openVRID];
				snap.time = now;
				snap.outVel[0] = pose.vecVelocity[0];
				snap.outVel[1] = pose.vecVelocity[1];
				snap.outVel[2] = pose.vecVelocity[2];
				snap.outAng[0] = pose.vecAngularVelocity[0];
				snap.outAng[1] = pose.vecAngularVelocity[1];
				snap.outAng[2] = pose.vecAngularVelocity[2];
				snap.outSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
					+ pose.vecVelocity[1] * pose.vecVelocity[1]
					+ pose.vecVelocity[2] * pose.vecVelocity[2]);
				snap.trackingOk = true;
				snap.result = (int)pose.result;
			}
		}
	}else if(velocityFixMode == 2 && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& IsStreamedController(openVRID)){
		// tracking dropped mid motion (camera based tracking loses the
		// controller exactly at throw windup). bridge VELOCITY only: if a
		// peak is fresh, keep replaying its decay so a release read during
		// a short dropout still carries the throw instead of zero.
		// positions are never synthesized.
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		std::lock_guard<std::mutex> guard(poseLogLock);
		auto found = velFixStates.find(openVRID);
		if(found != velFixStates.end()){
			VelFixState &state = found->second;
			double elapsed = now - state.peakTime;
			double decay = 1.0 - elapsed / 0.07;
			if(state.peakSpeed > 2.0 && decay > 0){
				pose.vecVelocity[0] = state.peakVel[0] * decay;
				pose.vecVelocity[1] = state.peakVel[1] * decay;
				pose.vecVelocity[2] = state.peakVel[2] * decay;
				pose.vecAngularVelocity[0] = state.peakAng[0] * decay;
				pose.vecAngularVelocity[1] = state.peakAng[1] * decay;
				pose.vecAngularVelocity[2] = state.peakAng[2] * decay;
			}
			MotionSnapshot &snap = motionSnapshots[openVRID];
			snap.time = now;
			snap.outVel[0] = pose.vecVelocity[0];
			snap.outVel[1] = pose.vecVelocity[1];
			snap.outVel[2] = pose.vecVelocity[2];
			snap.outSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
				+ pose.vecVelocity[1] * pose.vecVelocity[1]
				+ pose.vecVelocity[2] * pose.vecVelocity[2]);
			snap.trackingOk = false;
			snap.result = (int)pose.result;
		}
	}
	// mixed-space velocity frame fix (playspace-override setups): the
	// openvr header leaves vecVelocity's frame unspecified while positions
	// are driver-space + WorldFromDriver. an overrider aligning lighthouse
	// space into the vrlink space carries a large WorldFromDriver yaw, and
	// with mismatched conventions thrown objects fly at the right speed in
	// the wrong direction. "world" (1) rotates the reported velocity by
	// qWorldFromDriverRotation, "driver" (2) applies the inverse; the
	// field test decides which matches vrserver's real convention. only
	// devices whose WorldFromDriver rotation deviates >2 deg from identity
	// are touched (and logged once either way, so a log alone shows the
	// alignment angle and whether this fix is even relevant).
	if(openVRID != vr::k_unTrackedDeviceIndex_Hmd && openVRID < 64 && pose.poseIsValid){
		const vr::HmdQuaternion_t &qwd = pose.qWorldFromDriverRotation;
		double wClamped = qwd.w > 1.0 ? 1.0 : (qwd.w < -1.0 ? -1.0 : qwd.w);
		double angleDeg = 2.0 * acos(fabs(wClamped)) * 180.0 / 3.14159265358979323846;
		if(angleDeg > 2.0){
			int spaceFixMode = driverConfig.controllers.spaceVelocityFixMode;
			uint64_t bit = 1ull << openVRID;
			if(!(spaceFixLoggedMask.load(std::memory_order_relaxed) & bit)){
				spaceFixLoggedMask.fetch_or(bit, std::memory_order_relaxed);
				DriverLog("SpaceVelFix: id=%u WorldFromDriver angle=%.1f deg, mode=%s",
					openVRID, angleDeg,
					spaceFixMode == 1 ? "world" : (spaceFixMode == 2 ? "driver" : "off (candidate)"));
			}
			if(spaceFixMode > 0){
				vr::HmdQuaternion_t q = qwd;
				if(spaceFixMode == 2){
					q.x = -q.x; q.y = -q.y; q.z = -q.z;
				}
				double vIn[3] = { pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2] };
				double wIn[3] = { pose.vecAngularVelocity[0], pose.vecAngularVelocity[1], pose.vecAngularVelocity[2] };
				double vOut[3], wOut[3];
				QuatRotateVector(q, vIn, vOut);
				QuatRotateVector(q, wIn, wOut);
				pose.vecVelocity[0] = vOut[0]; pose.vecVelocity[1] = vOut[1]; pose.vecVelocity[2] = vOut[2];
				pose.vecAngularVelocity[0] = wOut[0]; pose.vecAngularVelocity[1] = wOut[1]; pose.vecAngularVelocity[2] = wOut[2];
			}
		}
	}

	if(driverConfig.streamFrame.poseLogging && openVRID != vr::k_unTrackedDeviceIndex_Hmd){
		LogDevicePose(openVRID, pose);
	}
	return true;
}

bool CustomHeadsetDeviceProvider::IsStreamedController(uint32_t openVRID){
	{
		std::lock_guard<std::mutex> guard(streamedIdentityLock);
		auto found = streamedControllerCache.find(openVRID);
		if(found != streamedControllerCache.end()){
			return found->second != 0;
		}
	}
	// property query with NO lock held (concurrency law: never call out
	// while holding a lock — ResolveContainerId taught us that one)
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(openVRID);
	vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
	char serial[128] = {};
	vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String, serial, sizeof(serial), &propError);
	bool streamed = false;
	if(propError == vr::TrackedProp_Success){
		streamed = strncmp(serial, "VRLINK", 6) == 0 || strncmp(serial, "SamsungVST", 10) == 0;
	}else{
		// property not readable yet: do not cache, do not touch
		return false;
	}
	{
		std::lock_guard<std::mutex> guard(streamedIdentityLock);
		streamedControllerCache[openVRID] = streamed ? 1 : 0;
	}
	DriverLog("VelocityFix: id=%u serial=%s streamed=%d%s", openVRID, serial, streamed ? 1 : 0,
		streamed ? "" : " (native velocity, never touched)");
	return streamed;
}

// direction secant over the full derive ring: raw displacement newest-oldest
// over the ring span. at throw speeds the displacement (5-20cm) dwarfs the
// ~1-4mm per-sample position noise, so this direction is clean to a few
// degrees where the endpoint-fit direction is noise dominated (the fit's
// consecutive estimates share 7/8 of their inputs — their noise is common
// mode and does not average away). pure math; called under poseLogLock.
void CustomHeadsetDeviceProvider::ComputeRingSecant(const VelFixState &state, bool useSmoothed, double secantVel[3], double secantAng[3]){
	int newest = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
	int oldest = state.head; // ring is full at every call site
	double span = state.time[newest] - state.time[oldest];
	if(span <= 1e-6){
		for(int a = 0; a < 3; a++){ secantVel[a] = 0; secantAng[a] = 0; }
		return;
	}
	const double (*P)[3] = useSmoothed ? state.smPos : state.pos;
	for(int a = 0; a < 3; a++){
		secantVel[a] = (P[newest][a] - P[oldest][a]) / span;
	}
	// angular secant: world-frame relative rotation oldest -> newest as a
	// rotation vector over the span (same small angle mapping as the fit)
	const vr::HmdQuaternion_t* Q = useSmoothed ? state.smQuat : state.quat;
	vr::HmdQuaternion_t qOldConj = {Q[oldest].w, -Q[oldest].x, -Q[oldest].y, -Q[oldest].z};
	vr::HmdQuaternion_t dq = QuatMultiply(Q[newest], qOldConj);
	double sign = dq.w < 0 ? -1.0 : 1.0;
	double vn = sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
	double angle = 2.0 * atan2(vn, fabs(dq.w));
	double scale = vn > 1e-9 ? sign * angle / (vn * span) : 0.0;
	secantAng[0] = dq.x * scale;
	secantAng[1] = dq.y * scale;
	secantAng[2] = dq.z * scale;
}

bool CustomHeadsetDeviceProvider::DeriveMotion(uint32_t openVRID, const vr::DriverPose_t &pose, double derivedVel[3], double derivedAng[3], double secantVel[3], double secantAng[3]){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	std::lock_guard<std::mutex> guard(poseLogLock);
	VelFixState &state = velFixStates[openVRID];
	
	// teleport / recenter rejection: a step implying > 30 m/s from the
	// previous sample invalidates the window
	if(state.count > 0){
		int prev = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
		double dt = now - state.time[prev];
		if(dt <= 0 || dt > 0.1){
			state.count = 0;
			state.haveEma = false;
			state.haveEmaAng = false;
		}else{
			double dx = pose.vecPosition[0] - state.pos[prev][0];
			double dy = pose.vecPosition[1] - state.pos[prev][1];
			double dz = pose.vecPosition[2] - state.pos[prev][2];
			if(sqrt(dx * dx + dy * dy + dz * dz) / dt > 30.0){
				state.count = 0;
				state.haveEma = false;
				state.haveEmaAng = false;
			}
		}
	}
	
	// skip duplicated / oversampled updates so the ring spans real time
	if(state.count > 0){
		int prev = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
		if(now - state.time[prev] < 0.003){
			// still allow output from the existing window
			if(state.count < VelFixState::ringSize || !state.haveEma || !state.haveEmaAng){
				return false;
			}
			derivedVel[0] = state.emaVel[0];
			derivedVel[1] = state.emaVel[1];
			derivedVel[2] = state.emaVel[2];
			derivedAng[0] = state.emaAng[0];
			derivedAng[1] = state.emaAng[1];
			derivedAng[2] = state.emaAng[2];
			ComputeRingSecant(state, state.haveSm, secantVel, secantAng);
			return true;
		}
	}
	// input prefilter (opt-in): per-axis median of the last 3 raw
	// positions. one-sample lag; single-sample spikes (network jitter
	// tails) can no longer reach the fit or the secant. state is under
	// poseLogLock like the rest of VelFixState; pure math only.
	double pushPos[3] = { pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2] };
	if(driverConfig.streamFrame.derivePreFilter == 1){
		state.rawPos[2][0] = state.rawPos[1][0]; state.rawPos[2][1] = state.rawPos[1][1]; state.rawPos[2][2] = state.rawPos[1][2];
		state.rawPos[1][0] = state.rawPos[0][0]; state.rawPos[1][1] = state.rawPos[0][1]; state.rawPos[1][2] = state.rawPos[0][2];
		state.rawPos[0][0] = pushPos[0]; state.rawPos[0][1] = pushPos[1]; state.rawPos[0][2] = pushPos[2];
		if(state.rawCount < 3){ state.rawCount++; }
		if(state.rawCount == 3){
			for(int a = 0; a < 3; a++){
				double x = state.rawPos[0][a], y = state.rawPos[1][a], z = state.rawPos[2][a];
				double lo = x < y ? (x < z ? x : z) : (y < z ? y : z);
				double hi = x > y ? (x > z ? x : z) : (y > z ? y : z);
				pushPos[a] = x + y + z - lo - hi;
			}
		}
	}else{
		state.rawCount = 0;
	}
	// pre-smoothing EMA (adjustable strength): maintained whenever the
	// knob is nonzero so the smoothed ring is warm; consumed by the
	// secant (always, when on) and by the fit (scope=both)
	double smoothMs = driverConfig.streamFrame.derivePreSmoothMs;
	if(smoothMs > 0.001){
		if(smoothMs > 100.0){ smoothMs = 100.0; }
		double sdt = state.count > 0 ? now - state.time[(state.head + VelFixState::ringSize - 1) % VelFixState::ringSize] : 0.0;
		if(!state.haveSm || sdt <= 0 || sdt > 0.1){
			state.smPosEma[0] = pushPos[0];
			state.smPosEma[1] = pushPos[1];
			state.smPosEma[2] = pushPos[2];
			state.smQuatEma = pose.qRotation;
			state.haveSm = true;
		}else{
			double sAlpha = 1.0 - exp(-sdt / (smoothMs / 1000.0));
			for(int a = 0; a < 3; a++){
				state.smPosEma[a] += sAlpha * (pushPos[a] - state.smPosEma[a]);
			}
			// nlerp EMA toward the incoming orientation (hemisphere safe)
			vr::HmdQuaternion_t q = pose.qRotation;
			double dot = q.w * state.smQuatEma.w + q.x * state.smQuatEma.x + q.y * state.smQuatEma.y + q.z * state.smQuatEma.z;
			double sgn = dot < 0 ? -1.0 : 1.0;
			state.smQuatEma.w += sAlpha * (sgn * q.w - state.smQuatEma.w);
			state.smQuatEma.x += sAlpha * (sgn * q.x - state.smQuatEma.x);
			state.smQuatEma.y += sAlpha * (sgn * q.y - state.smQuatEma.y);
			state.smQuatEma.z += sAlpha * (sgn * q.z - state.smQuatEma.z);
			double qn = sqrt(state.smQuatEma.w * state.smQuatEma.w + state.smQuatEma.x * state.smQuatEma.x
				+ state.smQuatEma.y * state.smQuatEma.y + state.smQuatEma.z * state.smQuatEma.z);
			if(qn > 1e-9){
				state.smQuatEma.w /= qn; state.smQuatEma.x /= qn; state.smQuatEma.y /= qn; state.smQuatEma.z /= qn;
			}
		}
	}else{
		state.haveSm = false;
	}
	state.smPos[state.head][0] = state.haveSm ? state.smPosEma[0] : pushPos[0];
	state.smPos[state.head][1] = state.haveSm ? state.smPosEma[1] : pushPos[1];
	state.smPos[state.head][2] = state.haveSm ? state.smPosEma[2] : pushPos[2];
	state.smQuat[state.head] = state.haveSm ? state.smQuatEma : pose.qRotation;
	bool feedFitSmoothed = state.haveSm && driverConfig.streamFrame.derivePreSmoothScope == 1;
	state.pos[state.head][0] = feedFitSmoothed ? state.smPos[state.head][0] : pushPos[0];
	state.pos[state.head][1] = feedFitSmoothed ? state.smPos[state.head][1] : pushPos[1];
	state.pos[state.head][2] = feedFitSmoothed ? state.smPos[state.head][2] : pushPos[2];
	state.quat[state.head] = pose.qRotation;
	state.time[state.head] = now;
	state.head = (state.head + 1) % VelFixState::ringSize;
	if(state.count < VelFixState::ringSize){
		state.count++;
		state.haveEma = false;
		state.haveEmaAng = false;
	}
	if(state.count < VelFixState::ringSize){
		return false;
	}
	
	// quadratic least squares over the whole ring, derivative evaluated at
	// the NEWEST sample (Savitzky-Golay style endpoint derivative). a linear
	// fit's slope is the velocity at the window CENTROID (~35ms ago), and
	// during a wrist snap that lag is ~20 degrees of arc = throws flying in
	// wrong directions. the quadratic term captures the arc's curvature so
	// the endpoint evaluation has near zero lag while every sample still
	// contributes to noise averaging.
	double tMean = 0;
	for(int i = 0; i < VelFixState::ringSize; i++){
		tMean += state.time[i];
	}
	tMean /= VelFixState::ringSize;
	double s2 = 0, s3 = 0, s4 = 0;
	double sp[3] = {0, 0, 0}, spt[3] = {0, 0, 0}, spt2[3] = {0, 0, 0};
	for(int i = 0; i < VelFixState::ringSize; i++){
		double dt = state.time[i] - tMean;
		double dt2 = dt * dt;
		s2 += dt2; s3 += dt2 * dt; s4 += dt2 * dt2;
		for(int a = 0; a < 3; a++){
			double p = state.pos[i][a];
			sp[a] += p; spt[a] += p * dt; spt2[a] += p * dt2;
		}
	}
	// normal equations for [a, b, c] over basis [1, t, t^2] with centered t
	// (sum of t is 0): | n 0 s2 ; 0 s2 s3 ; s2 s3 s4 |. closed form cramer
	// solutions for b and c (verified against brute force fits):
	//   det = n(s2 s4 - s3^2) - s2^3
	//   b   = (n s4 Spt - n s3 Spt2 + s2 s3 Sp - s2^2 Spt) / det
	//   c   = (n s2 Spt2 - n s3 Spt - s2^2 Sp) / det
	const double n = (double)VelFixState::ringSize;
	double det = n * (s2 * s4 - s3 * s3) - s2 * s2 * s2;
	if(fabs(det) <= 1e-18 || s2 <= 1e-9){
		return false;
	}
	int newestIdx = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
	double tN = state.time[newestIdx] - tMean;
	double slope[3];
	for(int a = 0; a < 3; a++){
		double b = (n * s4 * spt[a] - n * s3 * spt2[a] + s2 * s3 * sp[a] - s2 * s2 * spt[a]) / det;
		double c = (n * s2 * spt2[a] - n * s3 * spt[a] - s2 * s2 * sp[a]) / det;
		// v(t) = b + 2 c t, evaluated at the newest sample
		slope[a] = b + 2.0 * c * tN;
	}
	// angular velocity through the same machinery: express each ring
	// orientation as a rotation vector relative to the middle sample
	// (halves the max angle, keeping the small angle linearization honest:
	// < ~0.4 rad within the window even at 10 rad/s), fit the same
	// endpoint evaluated quadratic to the rotation vector series, then map
	// the derivative back to world axes through the reference orientation.
	int refIdx = (state.head + VelFixState::ringSize / 2) % VelFixState::ringSize;
	vr::HmdQuaternion_t qRef = state.quat[refIdx];
	vr::HmdQuaternion_t qRefConj = {qRef.w, -qRef.x, -qRef.y, -qRef.z};
	double sr[3] = {0, 0, 0}, srt[3] = {0, 0, 0}, srt2[3] = {0, 0, 0};
	for(int i = 0; i < VelFixState::ringSize; i++){
		vr::HmdQuaternion_t dq = QuatMultiply(qRefConj, state.quat[i]);
		double sign = dq.w < 0 ? -1.0 : 1.0;
		double vn = sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
		double angle = 2.0 * atan2(vn, fabs(dq.w));
		double scale = vn > 1e-9 ? sign * angle / vn : sign * 2.0;
		double r[3] = {dq.x * scale, dq.y * scale, dq.z * scale};
		double dt = state.time[i] - tMean;
		for(int a = 0; a < 3; a++){
			sr[a] += r[a]; srt[a] += r[a] * dt; srt2[a] += r[a] * dt * dt;
		}
	}
	double angSlopeRef[3];
	for(int a = 0; a < 3; a++){
		double b = (n * s4 * srt[a] - n * s3 * srt2[a] + s2 * s3 * sr[a] - s2 * s2 * srt[a]) / det;
		double c = (n * s2 * srt2[a] - n * s3 * srt[a] - s2 * s2 * sr[a]) / det;
		angSlopeRef[a] = b + 2.0 * c * tN;
	}
	double angSlope[3];
	QuatRotateVector(qRef, angSlopeRef, angSlope);
	
	// light EMA for smoothness in time (kept small: it adds lag back)
	if(!state.haveEma){
		state.haveEma = true;
		state.emaVel[0] = slope[0];
		state.emaVel[1] = slope[1];
		state.emaVel[2] = slope[2];
	}else{
		state.emaVel[0] = state.emaVel[0] * 0.65 + slope[0] * 0.35;
		state.emaVel[1] = state.emaVel[1] * 0.65 + slope[1] * 0.35;
		state.emaVel[2] = state.emaVel[2] * 0.65 + slope[2] * 0.35;
	}
	if(!state.haveEmaAng){
		state.haveEmaAng = true;
		state.emaAng[0] = angSlope[0];
		state.emaAng[1] = angSlope[1];
		state.emaAng[2] = angSlope[2];
	}else{
		state.emaAng[0] = state.emaAng[0] * 0.65 + angSlope[0] * 0.35;
		state.emaAng[1] = state.emaAng[1] * 0.65 + angSlope[1] * 0.35;
		state.emaAng[2] = state.emaAng[2] * 0.65 + angSlope[2] * 0.35;
	}
	derivedVel[0] = state.emaVel[0];
	derivedVel[1] = state.emaVel[1];
	derivedVel[2] = state.emaVel[2];
	derivedAng[0] = state.emaAng[0];
	derivedAng[1] = state.emaAng[1];
	derivedAng[2] = state.emaAng[2];
	ComputeRingSecant(state, state.haveSm, secantVel, secantAng);
	return true;
}

void CustomHeadsetDeviceProvider::LogDevicePose(uint32_t openVRID, const vr::DriverPose_t &pose){
	double now = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
	double speed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
		+ pose.vecVelocity[1] * pose.vecVelocity[1]
		+ pose.vecVelocity[2] * pose.vecVelocity[2]);
	double angularSpeed = sqrt(pose.vecAngularVelocity[0] * pose.vecAngularVelocity[0]
		+ pose.vecAngularVelocity[1] * pose.vecAngularVelocity[1]
		+ pose.vecAngularVelocity[2] * pose.vecAngularVelocity[2]);
	
	// steady line every 2s per device; burst lines (max 100Hz per device)
	// while linear speed exceeds 2 m/s, which is what captures throw arcs
	// and the velocity reported at the moment of release.
	bool steady = false;
	bool burst = false;
	bool announce = false;
	bool trackChange = false;
	double peakForLog = 0;
	double fdSpeed = 0;
	double fdAngSpeed = 0;
	{
		// resolve streamed identity BEFORE taking poseLogLock: on a cache miss
	// IsStreamedController queries VRProperties, which must never happen
	// under this lock (the ResolveContainerId self-deadlock class)
	bool streamedForSnapshot = driverConfig.streamFrame.velocityFixMode == 2
		? IsStreamedController(openVRID) : false;
	std::lock_guard<std::mutex> guard(poseLogLock);
		PoseLogState &state = poseLogStates[openVRID];
		if(!state.announced){
			state.announced = true;
			announce = true;
		}
		// velocity derived from position deltas, lightly smoothed. if the
		// reported |v| saturates near 2 m/s while this keeps climbing during
		// a throw, the clamp lives in the driver's reported velocity and can
		// be replaced from poses.
		// min dt guard: vrlink resubmits re-predicted poses fractions of a
		// millisecond apart; dividing mm differences by sub-ms dt produced
		// absurd fd spikes (field data: 90 m/s at rest) and false bursts.
		// teleport-scale instants are dropped instead of averaged in.
		if(state.havePos && now - state.lastSampleTime >= 0.003 && now - state.lastSampleTime < 0.1){
			double dt = now - state.lastSampleTime;
			double dx = pose.vecPosition[0] - state.lastPos[0];
			double dy = pose.vecPosition[1] - state.lastPos[1];
			double dz = pose.vecPosition[2] - state.lastPos[2];
			double instant = sqrt(dx * dx + dy * dy + dz * dz) / dt;
			if(instant < 30.0){
				state.fdSpeedEma = state.fdSpeedEma * 0.7 + instant * 0.3;
			}
			// quaternion derived angular speed for the same comparison on
			// the rotational side: 2 acos(|<q1,q2>|) / dt
			if(state.haveQuat){
				double dot = state.lastQuat.w * pose.qRotation.w + state.lastQuat.x * pose.qRotation.x
					+ state.lastQuat.y * pose.qRotation.y + state.lastQuat.z * pose.qRotation.z;
				if(dot < 0){ dot = -dot; }
				if(dot > 1.0){ dot = 1.0; }
				double angInstant = 2.0 * acos(dot) / dt;
				if(angInstant < 100.0){
					state.fdAngSpeedEma = state.fdAngSpeedEma * 0.7 + angInstant * 0.3;
				}
			}
			state.lastQuat = pose.qRotation;
			state.haveQuat = true;
		}else if(!state.havePos){
			state.lastQuat = pose.qRotation;
			state.haveQuat = true;
		}
		if(now - state.lastSampleTime >= 0.003 || !state.havePos){
			state.lastPos[0] = pose.vecPosition[0];
			state.lastPos[1] = pose.vecPosition[1];
			state.lastPos[2] = pose.vecPosition[2];
			state.lastSampleTime = now;
			state.havePos = true;
		}
		fdSpeed = state.fdSpeedEma;
		fdAngSpeed = state.fdAngSpeedEma;
		// with velocityFix off the fix path never runs, so record the raw
		// pose as the release snapshot here — A/B sessions then get
		// ReleaseSnap lines in both arms
		// full mode records snapshots in the fix path; off and classic
		// record here (classic's post-blend velocities are what this pose
		// carries by the time logging runs)
		if(driverConfig.streamFrame.velocityFixMode != 2 || !streamedForSnapshot){
			MotionSnapshot &snap = motionSnapshots[openVRID];
			snap.time = now;
			snap.outVel[0] = pose.vecVelocity[0];
			snap.outVel[1] = pose.vecVelocity[1];
			snap.outVel[2] = pose.vecVelocity[2];
			snap.outAng[0] = pose.vecAngularVelocity[0];
			snap.outAng[1] = pose.vecAngularVelocity[1];
			snap.outAng[2] = pose.vecAngularVelocity[2];
			snap.outSpeed = speed;
			snap.trackingOk = pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK;
			snap.result = (int)pose.result;
		}
		if(speed > state.peakSpeed){
			state.peakSpeed = speed;
		}
		// tracking state transitions are logged immediately (dropouts during
		// fast motion zero the speed, so speed-triggered bursts miss them —
		// exactly the "item falls straight down" moments)
		bool nowValid = pose.poseIsValid;
		int nowResult = (int)pose.result;
		if(!state.haveTrackState){
			state.haveTrackState = true;
			state.lastLoggedValid = nowValid;
			state.lastLoggedResult = nowResult;
		}else if((nowValid != state.lastLoggedValid || nowResult != state.lastLoggedResult)
				&& now - state.lastBurstLog >= 0.005){
			state.lastLoggedValid = nowValid;
			state.lastLoggedResult = nowResult;
			state.lastBurstLog = now;
			trackChange = true;
		}
		// keep burst logging alive for 300ms after fast motion so the
		// post release phase (including any dropout / zeroing) is captured.
		// EFFECTIVE speed (|v| + 0.15|w|): pure wrist flicks are w-dominant
		// with little linear motion, and a linear-only trigger made them
		// systematically invisible to the diagnostics (field 2026-08-10:
		// 3 flick samples out of 581)
		double effSpeed = speed + 0.15 * angularSpeed;
		double fdEffSpeed = fdSpeed + 0.15 * fdAngSpeed;
		if(effSpeed > 2.0 || fdEffSpeed > 2.0){
			state.recentFastTime = now;
		}
		bool inPostFastWindow = now - state.recentFastTime < 0.3;
		if(now - state.lastSteadyLog >= 2.0){
			state.lastSteadyLog = now;
			steady = true;
			peakForLog = state.peakSpeed;
			state.peakSpeed = 0;
		}else if((effSpeed > 2.0 || fdEffSpeed > 2.0 || inPostFastWindow) && now - state.lastBurstLog >= 0.01){
			state.lastBurstLog = now;
			burst = true;
		}
	}
	if(trackChange){
		DriverLog("PoseLog: TRACKING id=%u valid=%d result=%d |v|=%.3f fd|v|=%.3f pos=(%.3f, %.3f, %.3f)",
			openVRID, (int)pose.poseIsValid, (int)pose.result, speed, fdSpeed,
			pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2]);
	}
	if(announce){
		// resolve which physical device this id is, once, so pose lines are
		// attributable without guessing at activation order
		char serial[128] = {};
		vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(openVRID);
		vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
		vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String, serial, sizeof(serial), &propError);
		DriverLog("PoseLog: id=%u serial=%s", openVRID,
			propError == vr::TrackedProp_Success ? serial : "(unknown)");
	}
	if(steady){
		DriverLog("PoseLog: id=%u pos=(%.3f, %.3f, %.3f) |v|=%.3f fd|v|=%.3f |w|=%.2f fd|w|=%.2f peak|v|=%.3f valid=%d connected=%d result=%d timeOffset=%.4f",
			openVRID, pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2],
			speed, fdSpeed, angularSpeed, fdAngSpeed, peakForLog,
			(int)pose.poseIsValid, (int)pose.deviceIsConnected, (int)pose.result,
			pose.poseTimeOffset);
	}else if(burst){
		DriverLog("PoseLog: BURST id=%u v=(%.3f, %.3f, %.3f) |v|=%.3f fd|v|=%.3f |w|=%.2f fd|w|=%.2f valid=%d result=%d timeOffset=%.4f",
			openVRID, pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2],
			speed, fdSpeed, angularSpeed, fdAngSpeed, (int)pose.poseIsValid, (int)pose.result,
			pose.poseTimeOffset);
	}
}

bool CustomHeadsetDeviceProvider::HandleDeviceAdded(const char *&pchDeviceSerialNumber, vr::ETrackedDeviceClass &eDeviceClass, vr::ITrackedDeviceServerDriver *&pDriver){
	#ifdef HAS_PRIVATE
	if(driverConfig.onlyHandlePrivateFunctionality){
		return true;
	}
	#endif
	DriverLog("HandleDeviceAdded %s\n", pchDeviceSerialNumber);
	if(eDeviceClass == vr::TrackedDeviceClass_HMD){
		// keep the (possibly later wrapped) source device for projection
		// queries; GetComponent forwards through shims either way
		hmdDevice = pDriver;
		
		// add more shims here, they can stack and none of the functions are particularly hot
		// later shims can override earlier shims
		// the PosTrackedDeviceActivate function will likely have enough information that you can decide if it is the device you want and can then set shimActive to false to deactivate the shim
		
		// TODO: validate the interface versions of drivers and make the shims conform to versions to prevent potential crashes
		
		if(driverConfig.dreamAir.enable){
			DreamAirShim* dreamAirShim = new DreamAirShim();
			dreamAirShim->deviceProvider = this;
			shims.insert(dreamAirShim);
			pDriver = new ShimTrackedDeviceDriver(dreamAirShim, pDriver);
		}
		
		if(driverConfig.meganeX8K.enable){
			MeganeX8KShim* meganeX8KShim = new MeganeX8KShim();
			meganeX8KShim->deviceProvider = this;
			shims.insert(meganeX8KShim);
			pDriver = new ShimTrackedDeviceDriver(meganeX8KShim, pDriver);
		}
		
		GenericHeadsetShim* genericHeadsetShim = new GenericHeadsetShim();
		genericHeadsetShim->deviceProvider = this;
		shims.insert(genericHeadsetShim);
		pDriver = new ShimTrackedDeviceDriver(genericHeadsetShim, pDriver);
	}
	// you can change eDeviceClass to change what an existing device shows up as
	
	// if false is returned the device will not be added
	return true;
}

void CustomHeadsetDeviceProvider::GetTunerInput(TunerInputState &out){
	out = TunerInputState();
	std::lock_guard<std::mutex> guard(poseLogLock);
	for(const auto &pair : inputComponents){
		const InputComponentInfo &info = pair.second;
		switch(info.tunerRole){
			case 1:
				// largest-magnitude joystick y across hands, so either stick
				// nudges and an idle stick cannot cancel a deflected one
				if(fabsf(info.tunerScalar) > fabsf(out.stickY)){ out.stickY = info.tunerScalar; }
				break;
			case 2: out.bandOut = out.bandOut || info.tunerBool; break;
			case 3: out.bandIn = out.bandIn || info.tunerBool; break;
			case 4: out.eyeToggle = out.eyeToggle || info.tunerBool; break;
			case 5: out.resetBand = out.resetBand || info.tunerBool; break;
			case 6: if(info.tunerScalar > out.grip){ out.grip = info.tunerScalar; } break;
			case 7:
				if(fabsf(info.tunerScalar) > fabsf(out.stickX)){ out.stickX = info.tunerScalar; }
				break;
			case 8: if(info.tunerScalar > out.trigger){ out.trigger = info.tunerScalar; } break;
			case 9: out.segToggle = out.segToggle || info.tunerBool; break;
		}
	}
}

void CustomHeadsetDeviceProvider::GetAlignController(int hand, AlignControllerState &out){
	std::lock_guard<std::mutex> guard(poseLogLock);
	if(hand == 0 || hand == 1){
		out = alignControllers[hand];
	}else{
		out = AlignControllerState();
	}
}

void CustomHeadsetDeviceProvider::SetAlignerOffsets(bool active, const double rotDeg[3], const double posCm[3]){
	{
		std::lock_guard<std::mutex> guard(poseLogLock);
		for(int i = 0; i < 3; i++){
			alignerRotDeg[i] = rotDeg[i];
			alignerPosCm[i] = posCm[i];
		}
		if(!active){
			alignerAppliedLogged = false;
		}
	}
	alignerOverrideActive.store(active, std::memory_order_relaxed);
}
