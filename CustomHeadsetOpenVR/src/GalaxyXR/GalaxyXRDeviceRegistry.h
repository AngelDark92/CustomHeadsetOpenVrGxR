#pragma once

#include "GalaxyXRTypes.h"

#include "openvr_driver.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace galaxyxr {

struct RegisteredDevice {
	std::uint32_t openVrId = vr::k_unTrackedDeviceIndexInvalid;
	vr::ETrackedDeviceClass deviceClass = vr::TrackedDeviceClass_Invalid;
	DeviceRole role = DeviceRole::Unknown;
	std::string serial;
	bool active = false;
};

class GalaxyXRDeviceRegistry {
public:
	void ObserveAdded(const std::string& serial, vr::ETrackedDeviceClass deviceClass);
	void Activate(
		std::uint32_t openVrId,
		const std::string& serial,
		vr::ETrackedDeviceClass deviceClass);
	void Deactivate(std::uint32_t openVrId);
	void Clear();
	bool Get(std::uint32_t openVrId, RegisteredDevice& device) const;
	DeviceRole RoleFor(std::uint32_t openVrId) const;

private:
	static DeviceRole InferRole(const std::string& serial, vr::ETrackedDeviceClass deviceClass);

	mutable std::mutex mutex;
	std::map<std::uint32_t, RegisteredDevice> activeDevices;
	std::map<std::string, vr::ETrackedDeviceClass> observedSerials;
};

} // namespace galaxyxr
