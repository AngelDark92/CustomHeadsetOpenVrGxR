#include "GalaxyXR.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "../Config/ConfigLoader.h"
#include "../Driver/DriverLog.h"
#include "../GalaxyXR/GalaxyXRSystem.h"

namespace {
	bool EqualsIgnoreCase(const std::string& left, const std::string& right){
		return left.size() == right.size() &&
			std::equal(left.begin(), left.end(), right.begin(), [](unsigned char leftCharacter, unsigned char rightCharacter){
				return std::tolower(leftCharacter) == std::tolower(rightCharacter);
			});
	}
}

GalaxyXRShim::GalaxyXRShim(std::string addedSerial) : addedSerial(std::move(addedSerial)){
	// The wrapper remains byte-for-byte pass-through until an authenticated,
	// negotiated Galaxy session supplies validated geometry.
	shimDisplayComponent = true;
}

void GalaxyXRShim::PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue){
	if(returnValue != vr::VRInitError_None){
		return;
	}

	objectId = unObjectId;
	propertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);
	activeSerial = vr::VRProperties()->GetStringProperty(
		propertyContainer,
		vr::Prop_SerialNumber_String);
	activationSucceeded = true;
	galaxyxr::GalaxyXRSystem::Instance().RegisterHmd(
		objectId,
		propertyContainer,
		addedSerial,
		activeSerial);
	TryApplyIdentity();
}

bool GalaxyXRShim::PreTrackedDeviceDeactivate(){
	RestoreIdentity();
	galaxyxr::GalaxyXRSystem::Instance().UnregisterHmd(objectId);
	activationSucceeded = false;
	propertyContainer = vr::k_ulInvalidPropertyContainer;
	objectId = vr::k_unTrackedDeviceIndexInvalid;
	return true;
}

void GalaxyXRShim::RunFrame(){
	if(!activationSucceeded){
		return;
	}
	if(identityApplied &&
		!galaxyxr::GalaxyXRSystem::Instance().HmdMatchesAuthenticatedSession(
			addedSerial,
			activeSerial)){
		RestoreIdentity();
	}
	TryApplyIdentity();
}

void GalaxyXRShim::Shutdown(){
	RestoreIdentity();
	activationSucceeded = false;
}

bool GalaxyXRShim::PreDisplayComponentGetRecommendedRenderTargetSize(
	uint32_t *&pnWidth,
	uint32_t *&pnHeight){
	if(!pnWidth || !pnHeight){
		return true;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	if(!identityApplied ||
		!galaxyxr::GalaxyXRSystem::Instance().Display().GetRecommendedRenderTargetSize(width, height)){
		return true;
	}
	*pnWidth = width;
	*pnHeight = height;
	return false;
}

bool GalaxyXRShim::PreDisplayComponentGetEyeOutputViewport(
	vr::EVREye &eEye,
	uint32_t *&pnX,
	uint32_t *&pnY,
	uint32_t *&pnWidth,
	uint32_t *&pnHeight){
	if(!pnX || !pnY || !pnWidth || !pnHeight){
		return true;
	}
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	if(!identityApplied ||
		!galaxyxr::GalaxyXRSystem::Instance().Display().GetEyeOutputViewport(
			eEye, x, y, width, height)){
		return true;
	}
	*pnX = x;
	*pnY = y;
	*pnWidth = width;
	*pnHeight = height;
	return false;
}

bool GalaxyXRShim::PreDisplayComponentGetProjectionRaw(
	vr::EVREye &eEye,
	float *&pfLeft,
	float *&pfRight,
	float *&pfTop,
	float *&pfBottom){
	if(!pfLeft || !pfRight || !pfTop || !pfBottom){
		return true;
	}
	float left = 0.0f;
	float right = 0.0f;
	float top = 0.0f;
	float bottom = 0.0f;
	if(!identityApplied ||
		!galaxyxr::GalaxyXRSystem::Instance().Display().GetProjectionRaw(
			eEye, left, right, top, bottom)){
		return true;
	}
	*pfLeft = left;
	*pfRight = right;
	*pfTop = top;
	*pfBottom = bottom;
	return false;
}

void GalaxyXRShim::TryApplyIdentity(){
	const Config::GalaxyXRConfig& config = driverConfig.galaxyXR;
	if(identityApplied ||
		!activationSucceeded ||
		!config.enable ||
		!galaxyxr::GalaxyXRSystem::Instance().HmdMatchesAuthenticatedSession(
			addedSerial,
			activeSerial)){
		return;
	}

	DriverLog(
		"GXR Identity: authenticated session matched activated HMD (added=%s active=%s forceDiagnostic=%d)",
		addedSerial.c_str(),
		activeSerial.c_str(),
		config.forceEnable ? 1 : 0);
	previousConnectedHeadset = driverConfigLoader.info.connectedHeadset;
	connectedHeadsetBackedUp = true;
	driverConfigLoader.info.connectedHeadset = Config::HeadsetType::GalaxyXR;
	driverConfigLoader.WriteInfo();

	if(!config.overridePublicIdentity){
		identityApplied = true;
		return;
	}

	const galaxyxr::SessionSnapshot session =
		galaxyxr::GalaxyXRSystem::Instance().Profile().GetSession();
	// SteamVR caches identity during activation. Keep the serial that driver_vrlink used when it
	// added the HMD; the authenticated Android hardware serial is admission/diagnostic data only.
	const std::string stableSerial = addedSerial.empty()
		? "VRLINKHMDGALAXYXR"
		: addedSerial;
	SetString(vr::Prop_TrackingSystemName_String, "androidxr");
	SetString(vr::Prop_SerialNumber_String, stableSerial);
	SetString(vr::Prop_ManufacturerName_String, "Samsung");
	SetString(vr::Prop_ModelNumber_String, "Galaxy XR");
	SetString(vr::Prop_RenderModelName_String, "{galaxyxrresources}/rendermodels/galaxy_xr_hmd");
	SetString(vr::Prop_ResourceRoot_String, "galaxyxrresources");
	SetString(vr::Prop_InputProfilePath_String, "{galaxyxrresources}/input/galaxy_xr_hmd_profile.json");
	SetString(vr::Prop_ControllerType_String, "galaxy_xr_hmd");
	SetString(vr::Prop_RegisteredDeviceType_String, "androidxr/" + stableSerial);
	identityApplied = true;
}

void GalaxyXRShim::RestoreIdentity(){
	if(!identityApplied || propertyContainer == vr::k_ulInvalidPropertyContainer){
		propertyBackups.clear();
		identityApplied = false;
		if(galaxyxr::RestoreConnectedHeadsetBackup(
			driverConfigLoader.info.connectedHeadset,
			connectedHeadsetBackedUp,
			previousConnectedHeadset)){
			driverConfigLoader.WriteInfo();
		}
		return;
	}
	for(const StringPropertyBackup& backup : propertyBackups){
		if(backup.existed){
			vr::VRProperties()->SetStringProperty(propertyContainer, backup.property, backup.value.c_str());
		}else{
			vr::VRProperties()->EraseProperty(propertyContainer, backup.property);
		}
	}
	propertyBackups.clear();
	identityApplied = false;
	if(galaxyxr::RestoreConnectedHeadsetBackup(
		driverConfigLoader.info.connectedHeadset,
		connectedHeadsetBackedUp,
		previousConnectedHeadset)){
		driverConfigLoader.WriteInfo();
	}
	DriverLog("GXR Identity: original HMD properties restored");
}

void GalaxyXRShim::BackupString(vr::ETrackedDeviceProperty property){
	const auto found = std::find_if(
		propertyBackups.begin(),
		propertyBackups.end(),
		[property](const StringPropertyBackup& backup){ return backup.property == property; });
	if(found != propertyBackups.end()){
		return;
	}
	vr::ETrackedPropertyError error = vr::TrackedProp_Success;
	const std::string value =
		vr::VRProperties()->GetStringProperty(propertyContainer, property, &error);
	propertyBackups.push_back({
		property,
		value,
		error == vr::TrackedProp_Success,
	});
}

void GalaxyXRShim::SetString(
	vr::ETrackedDeviceProperty property,
	const std::string& value){
	BackupString(property);
	vr::VRProperties()->SetStringProperty(propertyContainer, property, value.c_str());
}
