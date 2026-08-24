#pragma once

#include <string>
#include <vector>

#include "../Driver/DeviceShim.h"
#include "../GalaxyXR/GalaxyXRIdentityState.h"

class CustomHeadsetDeviceProvider;

class GalaxyXRShim : public ShimDefinition{
public:
	explicit GalaxyXRShim(std::string addedSerial);

	CustomHeadsetDeviceProvider* deviceProvider = nullptr;

	virtual void PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue) override;
	virtual bool PreTrackedDeviceDeactivate() override;
	virtual void RunFrame() override;
	void Shutdown();
	virtual bool PreDisplayComponentGetRecommendedRenderTargetSize(
		uint32_t *&pnWidth,
		uint32_t *&pnHeight) override;
	virtual bool PreDisplayComponentGetEyeOutputViewport(
		vr::EVREye &eEye,
		uint32_t *&pnX,
		uint32_t *&pnY,
		uint32_t *&pnWidth,
		uint32_t *&pnHeight) override;
	virtual bool PreDisplayComponentGetProjectionRaw(
		vr::EVREye &eEye,
		float *&pfLeft,
		float *&pfRight,
		float *&pfTop,
		float *&pfBottom) override;

private:
	struct StringPropertyBackup{
		vr::ETrackedDeviceProperty property;
		std::string value;
		bool existed = false;
	};

	void TryApplyIdentity();
	void RestoreIdentity();
	void BackupString(vr::ETrackedDeviceProperty property);
	void SetString(vr::ETrackedDeviceProperty property, const std::string& value);

	std::string addedSerial;
	std::string activeSerial;
	std::vector<StringPropertyBackup> propertyBackups;
	vr::PropertyContainerHandle_t propertyContainer = vr::k_ulInvalidPropertyContainer;
	uint32_t objectId = vr::k_unTrackedDeviceIndexInvalid;
	bool identityApplied = false;
	bool connectedHeadsetBackedUp = false;
	Config::HeadsetType previousConnectedHeadset = Config::HeadsetType::None;
	bool activationSucceeded = false;
};

// Applies vendor-only render-resolution and stream-quality settings without
// participating in HMD identity or device ownership.
void RunGalaxyXRVendorSettings();

#ifdef VENDOR_GALAXYXR
class GalaxyXRControllerShim : public ShimDefinition{
public:
	explicit GalaxyXRControllerShim(const std::string &serial);

	virtual void PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue) override;
	virtual bool PreTrackedDeviceDeactivate() override;
	virtual void HandleEvent(const vr::VREvent_t &event) override;
	// live render-model swap when galaxyXr.renderModelVariant changes
	virtual void RunFrame() override;

private:
	void ApplyIdentity();
	// resolve the current model name from config (variant or default)
	std::string TargetModelName();

	std::string appliedModel;
	std::string serial;
	bool isLeft = false;
	vr::PropertyContainerHandle_t container = vr::k_ulInvalidPropertyContainer;
	bool active = false;
	bool haveBackup = false;
	std::string origRenderModel;
	std::string origInputProfile;
	std::string origControllerType;
};
#endif
