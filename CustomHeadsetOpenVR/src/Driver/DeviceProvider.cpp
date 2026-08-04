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
	bool hasRotationOffset = controllersConfig.rotationOffsetDeg[0] != 0
		|| controllersConfig.rotationOffsetDeg[1] != 0 || controllersConfig.rotationOffsetDeg[2] != 0;
	bool hasPositionOffset = controllersConfig.positionOffsetCm[0] != 0
		|| controllersConfig.positionOffsetCm[1] != 0 || controllersConfig.positionOffsetCm[2] != 0;
	if((hasRotationOffset || hasPositionOffset) && openVRID != vr::k_unTrackedDeviceIndex_Hmd
			&& GetDeviceClass(openVRID) == (int)vr::TrackedDeviceClass_Controller){
		if(hasPositionOffset){
			double local[3] = {
				controllersConfig.positionOffsetCm[0] / 100.0,
				controllersConfig.positionOffsetCm[1] / 100.0,
				controllersConfig.positionOffsetCm[2] / 100.0,
			};
			double world[3];
			QuatRotateVector(pose.qRotation, local, world);
			pose.vecPosition[0] += world[0];
			pose.vecPosition[1] += world[1];
			pose.vecPosition[2] += world[2];
		}
		if(hasRotationOffset){
			pose.qRotation = QuatMultiply(pose.qRotation, QuatFromEulerDeg(controllersConfig.rotationOffsetDeg));
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
