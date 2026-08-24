#include "GalaxyXR.h"

#ifdef VENDOR_GALAXYXR
#include "../Config/ConfigLoader.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <cmath>
#include "nlohmann/json.hpp"

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
	std::string base = "{galaxyxrresources}/icons/galaxyxr/" + prefix;
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

static void ApplyStreamQualitySetting(const std::string& quality){
	const StreamTier* tier = FindTier(quality);
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
static void ApplyNativeResolutionSetting(bool enabled){
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	if(enabled){
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

#endif
void RunGalaxyXRVendorSettings(){
#ifdef VENDOR_GALAXYXR
	static bool initialized = false;
	static bool appliedResolution = false;
	static std::string appliedQuality;
	const bool enabled = driverConfig.galaxyXr.nativeIdentity;
	const bool targetResolution = enabled && driverConfig.galaxyXr.nativeResolution;
	const std::string targetQuality = enabled ? driverConfig.galaxyXr.streamQuality : "default";
	if(!initialized || appliedResolution != targetResolution){
		ApplyNativeResolutionSetting(targetResolution);
		appliedResolution = targetResolution;
	}
	if(!initialized || appliedQuality != targetQuality){
		ApplyStreamQualitySetting(targetQuality);
		appliedQuality = targetQuality;
	}
	initialized = true;
#endif
}

#ifdef VENDOR_GALAXYXR
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
	if(trackingSystem != "SamsungVST" && trackingSystem != "androidxr"){
		DriverLog("GalaxyXRControllerShim: %s tracking system is \"%s\", not SamsungVST/androidxr - staying inert",
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

// generate a uniformly scaled copy of a render model folder: OBJ vertex
// positions and the json's length-valued fields (component origins, motion
// pivot/center, press_translate) are multiplied; direction vectors (axis)
// and angles (rotate_xyz, value_mapping, joystick ranges) are copied as-is;
// all other files (mtl, textures) are copied verbatim.
static bool GenerateScaledRenderModel(const std::string &srcDir, const std::string &dstDir, double scale, const std::string &srcJsonName, const std::string &dstJsonName){
	namespace fs = std::filesystem;
	try{
		if(fs::exists(dstDir)){
			return true;
		}
		std::string tmpDir = dstDir + ".tmp";
		fs::remove_all(tmpDir);
		fs::create_directories(tmpDir);
		for(const auto &entry : fs::directory_iterator(srcDir)){
			if(!entry.is_regular_file()){ continue; }
			std::string name = entry.path().filename().string();
			std::string ext = entry.path().extension().string();
			if(ext == ".obj"){
				std::ifstream in(entry.path());
				std::ofstream out(fs::path(tmpDir) / name);
				std::string line;
				while(std::getline(in, line)){
					double x, y, z;
					if(line.rfind("v ", 0) == 0 && sscanf(line.c_str(), "v %lf %lf %lf", &x, &y, &z) == 3){
						char buf[128];
						snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f", x * scale, y * scale, z * scale);
						out << buf << "\n";
					}else{
						out << line << "\n";
					}
				}
			}else if(name == srcJsonName){
				std::ifstream in(entry.path());
				nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
				std::function<void(nlohmann::json&)> walk = [&](nlohmann::json &node){
					if(node.is_object()){
						for(auto &item : node.items()){
							const std::string &key = item.key();
							nlohmann::json &val = item.value();
							if(val.is_array() && (key == "origin" || key == "pivot" || key == "center" || key == "press_translate")){
								for(auto &n : val){
									if(n.is_number()){ n = n.get<double>() * scale; }
								}
							}else{
								walk(val);
							}
						}
					}else if(node.is_array()){
						for(auto &child : node){ walk(child); }
					}
				};
				walk(j);
				std::ofstream out(fs::path(tmpDir) / dstJsonName);
				out << j.dump(1);
			}else{
				fs::copy_file(entry.path(), fs::path(tmpDir) / name);
			}
		}
		fs::rename(tmpDir, dstDir);
		return true;
	}catch(const std::exception &e){
		DriverLog("GalaxyXR: failed to generate scaled render model %s: %s", dstDir.c_str(), e.what());
		return false;
	}
}

// purge scaled variants other than the one currently wanted (~7MB each
// while the user dials the knob)
static void PurgeStaleScaledModels(const std::string &rendermodelsDir, const std::string &keepPrefix){
	namespace fs = std::filesystem;
	try{
		for(const auto &entry : fs::directory_iterator(rendermodelsDir)){
			std::string name = entry.path().filename().string();
			if(name.rfind("vst_controller_s", 0) == 0 && name.rfind(keepPrefix, 0) != 0){
				fs::remove_all(entry.path());
			}
		}
	}catch(const std::exception &){}
}

std::string GalaxyXRControllerShim::TargetModelName(){
	// replace the dangling {vrlink}/rendermodels/vst_controller_* reference
	// (never resolves: vrlink ships no such model) with our converted asset.
	// a non-empty renderModelVariant redirects to a tuning variant folder;
	// SteamVR reloads the model whenever the name changes, which is what
	// makes live alignment iteration possible.
	// official animated Steam Link models (see resources/PERMISSIONS.md)
	std::string base = "vst_controller";
	bool activeDriverModel = false;
	std::string variant = driverConfig.galaxyXr.renderModelVariant;
	if(!variant.empty()){
		// a stale variant key (e.g. a tuning session that ended without
		// 'done', followed by a rebuild that purged the tune folders) must
		// not leave the controllers without a model: only honor the variant
		// when its folder actually exists, otherwise fall back and log
		std::string variantDir = driverConfigLoader.info.driverResources + "/rendermodels/" + variant + "_" + (isLeft ? "left" : "right");
		if(std::filesystem::exists(variantDir)){
			base = variant;
			activeDriverModel = true;
		}else{
			DriverLog("GalaxyXRControllerShim: renderModelVariant \"%s\" has no folder at %s - using default model",
				variant.c_str(), variantDir.c_str());
		}
	}
	// uniform whole-model-system scale: generate (once per value) a variant
	// with geometry, component origins and motion pivots scaled together,
	// and swap to it via the name-change reload. tuning variants win.
	int scalePct = (int)std::lround(driverConfig.galaxyXr.renderModelScale * 100.0);
	if(variant.empty() && scalePct != 100 && scalePct >= 50 && scalePct <= 200){
		std::string hand = isLeft ? "left" : "right";
		std::string scaledBase = "vst_controller_s" + std::to_string(scalePct);
		std::string rmDir = driverConfigLoader.info.driverResources + "/rendermodels";
		static std::mutex genMutex;
		std::lock_guard<std::mutex> lock(genMutex);
		PurgeStaleScaledModels(rmDir, scaledBase);
		bool ok = GenerateScaledRenderModel(
			rmDir + "/vst_controller_" + hand,
			rmDir + "/" + scaledBase + "_" + hand,
			driverConfig.galaxyXr.renderModelScale,
			"vst_controller_" + hand + ".json",
			scaledBase + "_" + hand + ".json");
		if(ok){
			base = scaledBase;
			activeDriverModel = true;
		}
	}
	const std::string resourceRoot = activeDriverModel
		? "{" + driverConfigLoader.info.driverName + "}"
		: "{galaxyxrresources}";
	return resourceRoot + "/rendermodels/" + base + "_" + (isLeft ? "left" : "right");
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
		std::string profile = "{galaxyxrresources}/input/galaxy_xr_controller_profile.json";
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

#endif
