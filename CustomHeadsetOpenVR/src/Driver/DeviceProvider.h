#pragma once

#include <set>
#include <map>
#include <vector>
#include <mutex>

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
	void LogDevicePose(uint32_t openVRID, const vr::DriverPose_t &pose);
	
	// throw/velocity fix state: short ring of recent positions per device,
	// used to recompute linear velocity over a ~50ms window (endpoint
	// difference across the ring rejects sample-to-sample jitter that a
	// plain adjacent diff amplifies). guarded by poseLogLock.
	struct VelFixState {
		static constexpr int ringSize = 8;
		double pos[ringSize][3] = {};
		double time[ringSize] = {};
		int count = 0;   // valid entries
		int head = 0;    // next write slot
		// EMA over the least squares slope, so the substituted velocity is
		// smooth in time (discontinuities here become rendered pose jumps
		// through the runtime's forward prediction)
		bool haveEma = false;
		double emaVel[3] = {};
	};
	std::map<uint32_t, VelFixState> velFixStates = {};
	// returns true and writes the derived velocity when the window is usable
	bool DeriveVelocity(uint32_t openVRID, const vr::DriverPose_t &pose, double derived[3]);
	// cached device classes (Prop_DeviceClass_Int32), resolved on first pose
	std::map<uint32_t, int> deviceClasses = {};
	int GetDeviceClass(uint32_t openVRID);
};