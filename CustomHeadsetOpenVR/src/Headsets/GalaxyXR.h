#pragma once

#include <string>
#include "../Driver/DeviceShim.h"

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
