#include "DeviceProvider.h"
#include "DriverLog.h"
#include "DeviceShim.h"
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
			auto handFound = containerHand.find(found->second.container);
			if(handFound != containerHand.end()){
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
	}
	// capture the post-offset pose per hand for the controller aligner (the
	// drawn tip marker must reflect the live working offsets)
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
			&& pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK){
		bool classicMode = velocityFixMode == 1;
		double derivedVel[3], derivedAng[3];
		double now = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
		if(DeriveMotion(openVRID, pose, derivedVel, derivedAng)){
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
			if(derivedSpeed < 20.0 && derivedAngSpeed < 60.0 && sEff > 1.0){
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
			// and nothing else, matching the best-rated field iteration
			if(!classicMode)
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
	}else if(velocityFixMode == 2 && openVRID != vr::k_unTrackedDeviceIndex_Hmd){
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
	if(driverConfig.streamFrame.poseLogging && openVRID != vr::k_unTrackedDeviceIndex_Hmd){
		LogDevicePose(openVRID, pose);
	}
	return true;
}

bool CustomHeadsetDeviceProvider::DeriveMotion(uint32_t openVRID, const vr::DriverPose_t &pose, double derivedVel[3], double derivedAng[3]){
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
			return true;
		}
	}
	state.pos[state.head][0] = pose.vecPosition[0];
	state.pos[state.head][1] = pose.vecPosition[1];
	state.pos[state.head][2] = pose.vecPosition[2];
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
		if(driverConfig.streamFrame.velocityFixMode != 2){
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
		// post release phase (including any dropout / zeroing) is captured
		if(speed > 2.0 || fdSpeed > 2.0){
			state.recentFastTime = now;
		}
		bool inPostFastWindow = now - state.recentFastTime < 0.3;
		if(now - state.lastSteadyLog >= 2.0){
			state.lastSteadyLog = now;
			steady = true;
			peakForLog = state.peakSpeed;
			state.peakSpeed = 0;
		}else if((speed > 2.0 || fdSpeed > 2.0 || inPostFastWindow) && now - state.lastBurstLog >= 0.01){
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
	}
	alignerOverrideActive.store(active, std::memory_order_relaxed);
}
