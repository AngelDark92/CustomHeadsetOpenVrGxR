#include "GalaxyXRDeviceRegistry.h"

#include <algorithm>
#include <cctype>

namespace galaxyxr {
namespace {

std::string Lowercase(std::string value){
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character){
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

} // namespace

void GalaxyXRDeviceRegistry::ObserveAdded(
	const std::string& serial,
	vr::ETrackedDeviceClass deviceClass){
	std::lock_guard<std::mutex> lock(mutex);
	observedSerials[serial] = deviceClass;
}

void GalaxyXRDeviceRegistry::Activate(
	std::uint32_t openVrId,
	const std::string& serial,
	vr::ETrackedDeviceClass deviceClass){
	if(openVrId == vr::k_unTrackedDeviceIndexInvalid){
		return;
	}
	RegisteredDevice device;
	device.openVrId = openVrId;
	device.deviceClass = deviceClass;
	device.role = InferRole(serial, deviceClass);
	device.serial = serial;
	device.active = true;
	std::lock_guard<std::mutex> lock(mutex);
	activeDevices[openVrId] = std::move(device);
}

void GalaxyXRDeviceRegistry::Deactivate(std::uint32_t openVrId){
	std::lock_guard<std::mutex> lock(mutex);
	activeDevices.erase(openVrId);
}

void GalaxyXRDeviceRegistry::Clear(){
	std::lock_guard<std::mutex> lock(mutex);
	activeDevices.clear();
	observedSerials.clear();
}

bool GalaxyXRDeviceRegistry::Get(
	std::uint32_t openVrId,
	RegisteredDevice& device) const{
	std::lock_guard<std::mutex> lock(mutex);
	const auto found = activeDevices.find(openVrId);
	if(found == activeDevices.end()){
		return false;
	}
	device = found->second;
	return true;
}

DeviceRole GalaxyXRDeviceRegistry::RoleFor(std::uint32_t openVrId) const{
	RegisteredDevice device;
	return Get(openVrId, device) ? device.role : DeviceRole::Unknown;
}

DeviceRole GalaxyXRDeviceRegistry::InferRole(
	const std::string& serial,
	vr::ETrackedDeviceClass deviceClass){
	if(deviceClass == vr::TrackedDeviceClass_HMD){
		return DeviceRole::Hmd;
	}
	if(deviceClass != vr::TrackedDeviceClass_Controller &&
		deviceClass != vr::TrackedDeviceClass_GenericTracker){
		return DeviceRole::Unknown;
	}
	const std::string lower = Lowercase(serial);
	const bool left = lower.find("left") != std::string::npos ||
		lower.find("_l") != std::string::npos;
	const bool right = lower.find("right") != std::string::npos ||
		lower.find("_r") != std::string::npos;
	const bool hand = lower.find("hand") != std::string::npos;
	if(left){ return hand ? DeviceRole::LeftHand : DeviceRole::LeftController; }
	if(right){ return hand ? DeviceRole::RightHand : DeviceRole::RightController; }
	return DeviceRole::Unknown;
}

} // namespace galaxyxr
