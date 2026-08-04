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

bool CustomHeadsetDeviceProvider::HandleDevicePoseUpdated(uint32_t openVRID, vr::DriverPose_t &pose){
	if(driverConfig.forceTracking){
		pose.poseIsValid = true;
		if(pose.result != vr::TrackingResult_Fallback_RotationOnly){
			pose.result = vr::TrackingResult_Running_OK;
		}
	}
	// throw/velocity fix: substitute position-derived velocity when it is
	// meaningfully larger than the driver's smoothed report, so throw
	// releases carry true peak speed. cheap unsynchronized bool reads keep
	// the hot path free when both features are disabled.
	if(driverConfig.streamFrame.velocityFix && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK){
		double derived[3];
		if(DeriveVelocity(openVRID, pose, derived)){
			double derivedSpeed = sqrt(derived[0] * derived[0] + derived[1] * derived[1] + derived[2] * derived[2]);
			double reportedSpeed = sqrt(pose.vecVelocity[0] * pose.vecVelocity[0]
				+ pose.vecVelocity[1] * pose.vecVelocity[1]
				+ pose.vecVelocity[2] * pose.vecVelocity[2]);
			// continuous blend, never a switch: the runtime extrapolates the
			// rendered pose with this velocity, so any discontinuity in the
			// output becomes a visible hand jump. weight ramps smoothly from
			// 0 (calm: keep the driver's smooth data) to 1 (fast: use the
			// derived velocity whose peaks are not smoothed away) between
			// 1.0 and 2.5 m/s.
			double s = derivedSpeed > reportedSpeed ? derivedSpeed : reportedSpeed;
			if(derivedSpeed < 20.0 && s > 1.0){
				double w = (s - 1.0) / 1.5;
				if(w > 1.0){ w = 1.0; }
				w = w * w * (3.0 - 2.0 * w); // smoothstep
				pose.vecVelocity[0] = pose.vecVelocity[0] * (1.0 - w) + derived[0] * w;
				pose.vecVelocity[1] = pose.vecVelocity[1] * (1.0 - w) + derived[1] * w;
				pose.vecVelocity[2] = pose.vecVelocity[2] * (1.0 - w) + derived[2] * w;
			}
		}
	}
	if(driverConfig.streamFrame.poseLogging && openVRID != vr::k_unTrackedDeviceIndex_Hmd){
		LogDevicePose(openVRID, pose);
	}
	return true;
}

bool CustomHeadsetDeviceProvider::DeriveVelocity(uint32_t openVRID, const vr::DriverPose_t &pose, double derived[3]){
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
		}else{
			double dx = pose.vecPosition[0] - state.pos[prev][0];
			double dy = pose.vecPosition[1] - state.pos[prev][1];
			double dz = pose.vecPosition[2] - state.pos[prev][2];
			if(sqrt(dx * dx + dy * dy + dz * dz) / dt > 30.0){
				state.count = 0;
				state.haveEma = false;
			}
		}
	}
	
	// skip duplicated / oversampled updates so the ring spans real time
	if(state.count > 0){
		int prev = (state.head + VelFixState::ringSize - 1) % VelFixState::ringSize;
		if(now - state.time[prev] < 0.003){
			// still allow output from the existing window
			if(state.count < VelFixState::ringSize || !state.haveEma){
				return false;
			}
			derived[0] = state.emaVel[0];
			derived[1] = state.emaVel[1];
			derived[2] = state.emaVel[2];
			return true;
		}
	}
	state.pos[state.head][0] = pose.vecPosition[0];
	state.pos[state.head][1] = pose.vecPosition[1];
	state.pos[state.head][2] = pose.vecPosition[2];
	state.time[state.head] = now;
	state.head = (state.head + 1) % VelFixState::ringSize;
	if(state.count < VelFixState::ringSize){
		state.count++;
		state.haveEma = false;
	}
	if(state.count < VelFixState::ringSize){
		return false;
	}
	
	// least squares slope of position vs time over the whole ring (~70ms at
	// 100Hz): every sample contributes, so single-sample jitter cannot swing
	// the estimate the way an endpoint difference can
	double tMean = 0;
	for(int i = 0; i < VelFixState::ringSize; i++){
		tMean += state.time[i];
	}
	tMean /= VelFixState::ringSize;
	double num[3] = {0, 0, 0};
	double den = 0;
	for(int i = 0; i < VelFixState::ringSize; i++){
		double dt = state.time[i] - tMean;
		den += dt * dt;
		num[0] += dt * state.pos[i][0];
		num[1] += dt * state.pos[i][1];
		num[2] += dt * state.pos[i][2];
	}
	if(den <= 1e-9){
		return false;
	}
	double slope[3] = { num[0] / den, num[1] / den, num[2] / den };
	// light EMA for smoothness in time
	if(!state.haveEma){
		state.haveEma = true;
		state.emaVel[0] = slope[0];
		state.emaVel[1] = slope[1];
		state.emaVel[2] = slope[2];
	}else{
		state.emaVel[0] = state.emaVel[0] * 0.5 + slope[0] * 0.5;
		state.emaVel[1] = state.emaVel[1] * 0.5 + slope[1] * 0.5;
		state.emaVel[2] = state.emaVel[2] * 0.5 + slope[2] * 0.5;
	}
	derived[0] = state.emaVel[0];
	derived[1] = state.emaVel[1];
	derived[2] = state.emaVel[2];
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
	double peakForLog = 0;
	double fdSpeed = 0;
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
		if(state.havePos && now > state.lastSampleTime && now - state.lastSampleTime < 0.1){
			double dt = now - state.lastSampleTime;
			double dx = pose.vecPosition[0] - state.lastPos[0];
			double dy = pose.vecPosition[1] - state.lastPos[1];
			double dz = pose.vecPosition[2] - state.lastPos[2];
			double instant = sqrt(dx * dx + dy * dy + dz * dz) / dt;
			state.fdSpeedEma = state.fdSpeedEma * 0.7 + instant * 0.3;
		}
		state.lastPos[0] = pose.vecPosition[0];
		state.lastPos[1] = pose.vecPosition[1];
		state.lastPos[2] = pose.vecPosition[2];
		state.lastSampleTime = now;
		state.havePos = true;
		fdSpeed = state.fdSpeedEma;
		if(speed > state.peakSpeed){
			state.peakSpeed = speed;
		}
		if(now - state.lastSteadyLog >= 2.0){
			state.lastSteadyLog = now;
			steady = true;
			peakForLog = state.peakSpeed;
			state.peakSpeed = 0;
		}else if((speed > 2.0 || fdSpeed > 2.0) && now - state.lastBurstLog >= 0.01){
			state.lastBurstLog = now;
			burst = true;
		}
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
		DriverLog("PoseLog: id=%u pos=(%.3f, %.3f, %.3f) |v|=%.3f fd|v|=%.3f |w|=%.2f peak|v|=%.3f valid=%d connected=%d result=%d timeOffset=%.4f",
			openVRID, pose.vecPosition[0], pose.vecPosition[1], pose.vecPosition[2],
			speed, fdSpeed, angularSpeed, peakForLog,
			(int)pose.poseIsValid, (int)pose.deviceIsConnected, (int)pose.result,
			pose.poseTimeOffset);
	}else if(burst){
		DriverLog("PoseLog: BURST id=%u v=(%.3f, %.3f, %.3f) |v|=%.3f fd|v|=%.3f |w|=%.2f valid=%d result=%d timeOffset=%.4f",
			openVRID, pose.vecVelocity[0], pose.vecVelocity[1], pose.vecVelocity[2],
			speed, fdSpeed, angularSpeed, (int)pose.poseIsValid, (int)pose.result,
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
