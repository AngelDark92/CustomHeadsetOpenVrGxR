#include "GalaxyXR.h"
#include "../Config/ConfigLoader.h"
#include <filesystem>

// helper: set a string property only when it differs, returns true if written
static bool SetStringIfDifferent(vr::PropertyContainerHandle_t container, vr::ETrackedDeviceProperty prop, const std::string &value){
	std::string current = vr::VRProperties()->GetStringProperty(container, prop);
	if(current == value){
		return false;
	}
	vr::VRProperties()->SetStringProperty(container, prop, value.c_str());
	return true;
}

// prefix is one of headset_galaxy_xr_status / left_galaxy_xr_status /
// right_galaxy_xr_status (icon set by Vilkka, see icons/galaxy_xr/CREDITS.txt)
static void SetDeviceIcons(vr::PropertyContainerHandle_t container, const std::string &prefix){
	std::string base = "{" + driverConfigLoader.info.driverName + "}/icons/galaxy_xr/" + prefix;
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceOff_String,            base + "_off.png");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceSearching_String,      base + "_searching.gif");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceSearchingAlert_String, base + "_searching_alert.gif");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceReady_String,          base + "_ready.png");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceReadyAlert_String,     base + "_ready_alert.png");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceNotReady_String,       base + "_error.png");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceStandby_String,        base + "_standby.png");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceStandbyAlert_String,   base + "_standby_alert.png");
	SetStringIfDifferent(container, vr::Prop_NamedIconPathDeviceAlertLow_String,       base + "_ready_low.png");
}

// the Galaxy XR native per-eye render geometry; see GalaxyXrConfig::nativeResolution
static const int kGalaxyXrRenderWidth = 3552;
static const int kGalaxyXrRenderHeight = 3840;

struct StreamTier { const char* name; int encodeWidth; int bandwidth; };
// values from the community Apply-Settings tool (streamFormatWidth fixed 1536)
static const StreamTier kStreamTiers[] = {
	{"stable", 2048, 250}, {"quality", 2560, 300}, {"high", 3072, 300},
	{"highest", 3072, 350}, {"ultra", 4032, 350},
};

static const StreamTier* FindTier(const std::string &name){
	for(const auto &t : kStreamTiers){
		if(name == t.name){ return &t; }
	}
	return nullptr;
}

// remove a vrlink int key only when it holds a value we could have written,
// so user- or tool-owned values are never clobbered
static void RemoveIntIfOurs(const char* key, std::initializer_list<int> ourValues){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	int32_t v = vr::VRSettings()->GetInt32("driver_vrlink", key, &err);
	if(err != vr::VRSettingsError_None){ return; }
	for(int ours : ourValues){
		if(v == ours){
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", key);
			return;
		}
	}
}

static void ApplyStreamQualitySetting(){
	const StreamTier* tier = FindTier(driverConfig.galaxyXr.streamQuality);
	if(tier){
		vr::VRSettings()->SetInt32("driver_vrlink", "encodeWidth", tier->encodeWidth);
		vr::VRSettings()->SetInt32("driver_vrlink", "streamFormatWidth", 1536);
		vr::VRSettings()->SetBool("driver_vrlink", "automaticStreamFormatWidth", false);
		vr::VRSettings()->SetBool("driver_vrlink", "automaticBandwidth", false);
		vr::VRSettings()->SetInt32("driver_vrlink", "recommendedBandwidthMbit", tier->bandwidth);
		vr::VRSettings()->SetInt32("driver_vrlink", "targetBandwidth", tier->bandwidth);
		DriverLog("GalaxyXR: stream quality '%s' (encodeWidth %d, streamFormatWidth 1536, %d Mbit/s; effective next start/connect)",
			tier->name, tier->encodeWidth, tier->bandwidth);
	}else{
		// default (or unknown): remove tier keys we own so vrlink built-in
		// defaults apply, matching the community tool's Default mode
		RemoveIntIfOurs("encodeWidth", {2048, 2560, 3072, 4032});
		RemoveIntIfOurs("streamFormatWidth", {1536});
		RemoveIntIfOurs("recommendedBandwidthMbit", {250, 300, 350});
		RemoveIntIfOurs("targetBandwidth", {250, 300, 350});
		vr::EVRSettingsError err = vr::VRSettingsError_None;
		bool a = vr::VRSettings()->GetBool("driver_vrlink", "automaticStreamFormatWidth", &err);
		if(err == vr::VRSettingsError_None && !a){
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "automaticStreamFormatWidth");
		}
		err = vr::VRSettingsError_None;
		a = vr::VRSettings()->GetBool("driver_vrlink", "automaticBandwidth", &err);
		if(err == vr::VRSettingsError_None && !a){
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "automaticBandwidth");
		}
	}
}

// write or remove the global vrlink render override per config. safe to call
// repeatedly; only writes on difference. changes are read by the compositor
// at SteamVR start, so mid-session toggles take effect next launch.
static void ApplyNativeResolutionSetting(){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	if(driverConfig.galaxyXr.nativeResolution){
		int32_t w = vr::VRSettings()->GetInt32("driver_vrlink", "overrideRenderWidth", &err);
		if(err != vr::VRSettingsError_None || w != kGalaxyXrRenderWidth){
			vr::VRSettings()->SetInt32("driver_vrlink", "renderWidth", kGalaxyXrRenderWidth);
			vr::VRSettings()->SetInt32("driver_vrlink", "renderHeight", kGalaxyXrRenderHeight);
			vr::VRSettings()->SetInt32("driver_vrlink", "overrideRenderWidth", kGalaxyXrRenderWidth);
			vr::VRSettings()->SetInt32("driver_vrlink", "overrideRenderHeight", kGalaxyXrRenderHeight);
			vr::VRSettings()->SetInt32("driver_vrlink", "displayFrequency", 90);
			vr::VRSettings()->SetInt32("steamvr", "preferredRefreshRate", 90);
			DriverLog("GalaxyXR: wrote driver_vrlink render %dx%d @90 (native resolution; effective next SteamVR start)",
				kGalaxyXrRenderWidth, kGalaxyXrRenderHeight);
		}
	}else{
		int32_t w = vr::VRSettings()->GetInt32("driver_vrlink", "overrideRenderWidth", &err);
		if(err == vr::VRSettingsError_None && w == kGalaxyXrRenderWidth){
			// only remove values we wrote; a different value means the user or
			// the community Apply-Settings tool owns it - leave it alone
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "renderWidth");
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "renderHeight");
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "overrideRenderWidth");
			vr::VRSettings()->RemoveKeyInSection("driver_vrlink", "overrideRenderHeight");
			RemoveIntIfOurs("displayFrequency", {90});
			vr::EVRSettingsError rerr = vr::VRSettingsError_None;
			int32_t rr = vr::VRSettings()->GetInt32("steamvr", "preferredRefreshRate", &rerr);
			if(rerr == vr::VRSettingsError_None && rr == 90){
				vr::VRSettings()->RemoveKeyInSection("steamvr", "preferredRefreshRate");
			}
			DriverLog("GalaxyXR: removed driver_vrlink render override (nativeResolution off)");
		}
	}
}

// ---------------- HMD ----------------

void GalaxyXRHmdShim::PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue){
	if(returnValue != vr::VRInitError_None){
		return;
	}
	container = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

	// only act on the vrlink-streamed Galaxy XR. on the wire the HMD's
	// tracking system is "oculus" (vrlink's Quest Pro profile asserts it;
	// SamsungVST only survives on the controllers), so gate on the serial
	// with the tracking system as a fallback
	std::string serial = vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String);
	std::string trackingSystem = vr::VRProperties()->GetStringProperty(container, vr::Prop_TrackingSystemName_String);
	if(serial.find("GALAXYXR") == std::string::npos && trackingSystem != "SamsungVST"){
		DriverLog("GalaxyXRHmdShim: serial \"%s\" / tracking system \"%s\" is not a Galaxy XR - staying inert",
			serial.c_str(), trackingSystem.c_str());
		shimActive = false;
		return;
	}

	origModelNumber = vr::VRProperties()->GetStringProperty(container, vr::Prop_ModelNumber_String);
	origManufacturer = vr::VRProperties()->GetStringProperty(container, vr::Prop_ManufacturerName_String);
	origHmdInputProfile = vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
	haveBackup = true;
	active = true;
	DriverLog("GalaxyXRHmdShim: activating identity override (was model=\"%s\" manufacturer=\"%s\")",
		origModelNumber.c_str(), origManufacturer.c_str());
	ApplyIdentity();
	ApplyNativeResolutionSetting();
	appliedNativeResolution = driverConfig.galaxyXr.nativeResolution;
	ApplyStreamQualitySetting();
	appliedStreamQuality = driverConfig.galaxyXr.streamQuality;
}

void GalaxyXRHmdShim::ApplyIdentity(){
	if(!active){
		return;
	}
	bool wrote = false;
	wrote |= SetStringIfDifferent(container, vr::Prop_ModelNumber_String, "Galaxy XR");
	wrote |= SetStringIfDifferent(container, vr::Prop_ManufacturerName_String, "Samsung");
	SetDeviceIcons(container, "headset_galaxy_xr_status");
	if(driverConfig.galaxyXr.nativeInputProfile){
		// repair the HMD's dangling {vrlink}/input/galaxy_xr_hmd_profile.json
		// reference with our shipped official copy
		wrote |= SetStringIfDifferent(container, vr::Prop_InputProfilePath_String,
			"{" + driverConfigLoader.info.driverName + "}/input/galaxy_xr_hmd_profile.json");
	}
	if(wrote){
		DriverLog("GalaxyXRHmdShim: identity applied");
	}
}

bool GalaxyXRHmdShim::PreTrackedDeviceDeactivate(){
	if(active && haveBackup){
		DriverLog("GalaxyXRHmdShim: restoring original identity on deactivate");
		vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, origModelNumber.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, origManufacturer.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, origHmdInputProfile.c_str());
	}
	active = false;
	return true;
}

void GalaxyXRHmdShim::RunFrame(){
	// config hot-reload: apply/remove the resolution override on toggle flips
	if(active && driverConfig.galaxyXr.nativeResolution != appliedNativeResolution){
		appliedNativeResolution = driverConfig.galaxyXr.nativeResolution;
		ApplyNativeResolutionSetting();
	}
	if(active && driverConfig.galaxyXr.streamQuality != appliedStreamQuality){
		appliedStreamQuality = driverConfig.galaxyXr.streamQuality;
		ApplyStreamQualitySetting();
	}
}

void GalaxyXRHmdShim::HandleEvent(const vr::VREvent_t &event){
	// vrlink re-asserts properties after activation; reapply when our
	// container changes. ApplyIdentity only writes on difference, so the
	// PropertyChanged events caused by our own writes converge immediately.
	if(active && event.eventType == vr::VREvent_PropertyChanged && event.data.property.container == container){
		ApplyIdentity();
	}
}

// ---------------- Controllers ----------------

GalaxyXRControllerShim::GalaxyXRControllerShim(const std::string &serial) : serial(serial){
	isLeft = serial.find("Left") != std::string::npos;
}

void GalaxyXRControllerShim::PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue){
	if(returnValue != vr::VRInitError_None){
		return;
	}
	container = vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

	std::string trackingSystem = vr::VRProperties()->GetStringProperty(container, vr::Prop_TrackingSystemName_String);
	if(trackingSystem != "SamsungVST"){
		DriverLog("GalaxyXRControllerShim: %s tracking system is \"%s\", not SamsungVST - staying inert",
			serial.c_str(), trackingSystem.c_str());
		shimActive = false;
		return;
	}

	origRenderModel = vr::VRProperties()->GetStringProperty(container, vr::Prop_RenderModelName_String);
	origInputProfile = vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
	origControllerType = vr::VRProperties()->GetStringProperty(container, vr::Prop_ControllerType_String);
	haveBackup = true;
	active = true;
	DriverLog("GalaxyXRControllerShim: activating for %s (was rendermodel=\"%s\")", serial.c_str(), origRenderModel.c_str());
	ApplyIdentity();
}

std::string GalaxyXRControllerShim::TargetModelName(){
	// replace the dangling {vrlink}/rendermodels/vst_controller_* reference
	// (never resolves: vrlink ships no such model) with our converted asset.
	// a non-empty renderModelVariant redirects to a tuning variant folder;
	// SteamVR reloads the model whenever the name changes, which is what
	// makes live alignment iteration possible.
	// official animated Steam Link models (see resources/PERMISSIONS.md)
	std::string base = "vst_controller";
	// prefer the animated vst overlay when the build shipped it (see
	// build.js --vst-models and PERMISSIONS.md; permission is conditional,
	// so its absence must degrade silently to the MIT models)
	{
		std::string vstDir = driverConfigLoader.info.driverResources + "/rendermodels/galaxy_xr_controller_vst_" + (isLeft ? "left" : "right");
		if(std::filesystem::exists(vstDir)){
			base = "galaxy_xr_controller_vst";
		}
	}
	std::string variant = driverConfig.galaxyXr.renderModelVariant;
	if(!variant.empty()){
		// a stale variant key (e.g. a tuning session that ended without
		// 'done', followed by a rebuild that purged the tune folders) must
		// not leave the controllers without a model: only honor the variant
		// when its folder actually exists, otherwise fall back and log
		std::string variantDir = driverConfigLoader.info.driverResources + "/rendermodels/" + variant + "_" + (isLeft ? "left" : "right");
		if(std::filesystem::exists(variantDir)){
			base = variant;
		}else{
			DriverLog("GalaxyXRControllerShim: renderModelVariant \"%s\" has no folder at %s - using default model",
				variant.c_str(), variantDir.c_str());
		}
	}
	return "{" + driverConfigLoader.info.driverName + "}/rendermodels/" + base + "_" + (isLeft ? "left" : "right");
}

void GalaxyXRControllerShim::ApplyIdentity(){
	if(!active){
		return;
	}
	std::string model = TargetModelName();
	bool wrote = SetStringIfDifferent(container, vr::Prop_RenderModelName_String, model);
	SetDeviceIcons(container, isLeft ? "left_galaxy_xr_status" : "right_galaxy_xr_status");
	if(wrote){
		appliedModel = model;
		DriverLog("GalaxyXRControllerShim: rendermodel %s applied for %s", model.c_str(), serial.c_str());
	}
	if(driverConfig.galaxyXr.nativeInputProfile){
		// the official native input profile: controller type
		// galaxy_xr_controller with Valve's own legacy bindings, remapping
		// and pose components (handgrip at the official z=0.098m/20.6deg,
		// which corrects held-item orientation at the proper layer instead
		// of pose offsets). note: the official remapping has no
		// oculus_touch layout, so user-made custom Touch bindings do not
		// auto-carry; per-game rebinding may be needed.
		std::string profile = "{" + driverConfigLoader.info.driverName + "}/input/galaxy_xr_controller_profile.json";
		bool wroteProfile = SetStringIfDifferent(container, vr::Prop_InputProfilePath_String, profile);
		bool wroteType = SetStringIfDifferent(container, vr::Prop_ControllerType_String, "galaxy_xr_controller");
		if(wroteProfile || wroteType){
			DriverLog("GalaxyXRControllerShim: native input profile applied for %s", serial.c_str());
		}
	}
}

void GalaxyXRControllerShim::RunFrame(){
	// config hot-reload: swap the model live when the variant changes
	if(active && TargetModelName() != appliedModel){
		ApplyIdentity();
	}
}

bool GalaxyXRControllerShim::PreTrackedDeviceDeactivate(){
	if(active && haveBackup){
		vr::VRProperties()->SetStringProperty(container, vr::Prop_RenderModelName_String, origRenderModel.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, origInputProfile.c_str());
		vr::VRProperties()->SetStringProperty(container, vr::Prop_ControllerType_String, origControllerType.c_str());
	}
	active = false;
	return true;
}

void GalaxyXRControllerShim::HandleEvent(const vr::VREvent_t &event){
	if(active && event.eventType == vr::VREvent_PropertyChanged && event.data.property.container == container){
		ApplyIdentity();
	}
}
