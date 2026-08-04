#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <tuple>

struct ConfigColor{
	double r = 1.0;
	double g = 1.0;
	double b = 1.0;
};

struct HiddenAreaMeshConfig {
	bool enable = false;
	bool testMode = false;
	int detailLevel = 8;
	double radiusTopOuter = 0.25;
	double radiusTopInner = 0.25;
	double radiusBottomInner = 0.25;
	double radiusBottomOuter = 0.25;

	constexpr bool operator==(const HiddenAreaMeshConfig& other) const {
		return std::tie(this->enable, this->testMode, this->detailLevel, this->radiusTopOuter, this->radiusTopInner, this->radiusBottomInner, this->radiusBottomOuter) ==
		       std::tie(other.enable, other.testMode, other.detailLevel, other.radiusTopOuter, other.radiusTopInner, other.radiusBottomInner, other.radiusBottomOuter);
	}
	constexpr bool operator!=(const HiddenAreaMeshConfig& other) const {
		return !(this->operator==(other));
	}
};

struct StationaryDimmingConfig{
	// if the display should be dimmed when the headset is stationary
	bool enable = true;
	// the angle that the headset has to rotate for it to be considered as moved
	double movementThreshold = 0.4;
	// the time in seconds that the headset has to be stationary for it to be dimmed
	double movementTime = 15.0;
	// the amount to dim the display to when stationary
	double dimBrightnessPercent = 2;
	// the amount per second to dim the display when stationary
	double dimSeconds = 10;
	// the amount per second to brighten the display when moving
	double brightenSeconds = 5;
};


// one control point of the spline distortion curve
struct StreamFrameDistortionPoint{
	// radius, 0 at the optical center, roughly 0.5 at the edge midpoints
	double r = 0;
	// radial scale multiplier at that radius, 1.0 = no change
	double scale = 1;
};

// diagnostic band that limits the distortion correction to a radius range so
// one region of the curve can be tuned against untouched surroundings
struct StreamFrameAnnulusConfig{
	bool enable = false;
	double rMin = 0.0;
	double rMax = 0.75;
	// width of the smooth ramp at both edges of the band
	double feather = 0.05;
};

// one distortion curve: k1/k2 polynomial coefficients and/or spline points,
// which of the two is evaluated follows the global distortion mode
struct StreamFrameCurve{
	double k1 = 0;
	double k2 = 0;
	std::vector<StreamFrameDistortionPoint> points = {};
};

struct StreamFrameDistortionConfig{
	// "k1k2" evaluates 1 + k1 r^2 + k2 r^4, "spline" interpolates the points
	std::string mode = "k1k2";
	// spline control points of the base curve, sorted by r internally. flat
	// outside the range. the base k1/k2 live at the streamFrame top level.
	std::vector<StreamFrameDistortionPoint> points = {};
	// separate curves per eye and/or per axis. per axis blends a horizontal and
	// a vertical curve around the ring, capturing elliptic/astigmatic error.
	bool perEye = false;
	bool perAxis = false;
	// named curves used when the toggles are active. expected keys:
	// perEye: "left", "right". perAxis: "horizontal", "vertical".
	// both: "leftHorizontal", "leftVertical", "rightHorizontal", "rightVertical".
	// a missing key falls back to the base curve.
	std::map<std::string, StreamFrameCurve> curves = {};
	StreamFrameAnnulusConfig annulus = {};
};

struct StreamFrameCASConfig{
	// contrast adaptive sharpening applied before encoding
	bool enable = false;
	// 0 to 1
	double strength = 0.5;
};

// fade the streamed frames to black when the headset has not moved for a
// while, e.g. left on a desk with SteamVR running. uniform full fade, so no
// uneven oled wear. brightness returns quickly once movement is detected.
struct StreamFrameDimmingConfig{
	bool enable = false;
	// the angle in degrees that the headset has to rotate to count as moved
	double movementThreshold = 0.4;
	// seconds of stillness before dimming starts
	double movementTime = 15.0;
	// seconds to fade fully to black
	double dimSeconds = 10.0;
	// seconds to fade back to full brightness on movement
	double brightenSeconds = 1.0;
};

struct StreamFrameConfig{
	// process direct mode layer textures before the streaming driver consumes them
	bool enable = false;
	// saturation with 50 being normal, same semantics as customShader.saturation
	double saturation = 50;
	// contrast with 50 being normal, same semantics as customShader.contrast
	double contrast = 50;
	// the point from 0-100% of white that the contrast is centered around
	double contrastMidpoint = 50;
	// if the contrast should be done in linear space instead of gamma
	bool contrastLinear = false;
	// gamma of the output, 2.2 is neutral
	double gamma = 2.2;
	// per channel tint multiplier
	ConfigColor colorMultiplier = {};
	// 3x3 linear rgb color matrix, row major. active when exactly 9 values.
	std::vector<double> srgbMatrix = {};
	StreamFrameCASConfig cas = {};
	// add low amplitude noise before encoding to reduce banding in dark scenes
	bool dither = false;
	StreamFrameDimmingConfig stationaryDimming = {};
	// radial distortion pre perturbation, applied to the streamed eye images to
	// compensate an imperfect distortion profile on the standalone headset.
	double k1 = 0;
	double k2 = 0;
	StreamFrameDistortionConfig distortion = {};
	// optical center offset from the texture center, in uv units, per eye
	double centerOffsetXLeft = 0;
	double centerOffsetXRight = 0;
	double centerOffsetY = 0;
	// skip the color adjustment while the dashboard is open, in case the
	// compositor shader replacement also applies it to the flattened scene in
	// that state. off by default: the recommended setup is to leave the custom
	// shader disabled or neutral for streamed headsets and let this pass be the
	// single source of truth in every state. does not affect cas/dither.
	bool skipColorWhileDashboardOpen = false;
	// process during SubmitLayer (using the previous frame's sync texture)
	// instead of during Present. try this if Present time processing has no
	// visible effect because the driver already consumes the layer at submit.
	bool processAtSubmitLayer = false;
	// keyed mutex acquire timeout for the frame sync texture, in ms. when it
	// expires the frame passes through unprocessed (a visible "flash" of
	// ungraded color), which happens under heavy load (shader compilation,
	// level streaming). after a skip the timeout escalates (3x, min 15ms) to
	// break flash streaks, and resets on the next acquired frame.
	int syncTimeoutMs = 5;
	// experimental throw/velocity fix. vrlink's reported controller velocity
	// is heavily smoothed (field data: peaks read ~50-65% of position-derived
	// velocity during throws, ratio varies with motion phase = filter lag,
	// not a scale factor). when enabled, linear velocity is recomputed from
	// a ~50ms window of positions and substituted when meaningfully larger
	// than the reported value, so releases carry true peak speed while calm
	// motion keeps the driver's smoother data.
	bool velocityFix = false;
	// diagnostic: throttle-log controller/tracker poses from the PoseUpdated
	// hook (position, velocity, tracking result), with a burst mode that
	// captures high-velocity moments (throws). live-reloaded, so it can be
	// toggled mid-session. groundwork for the throw/velocity fix.
	bool poseLogging = false;
};

struct CustomShaderConfig{
	// if shaders should be replaced in the compositor
	bool enable = false;
	bool enableForMeganeX8K = true;
	bool enableForDreamAir = true;
	bool enableForOther = false;
	// contrast with 50 being normal
	double contrast = 50;
	// the point from 0-100% of white that the contrast is centered around
	double contrastMidpoint = 50;
	// if the contrast should be done in linear space instead of gamma
	bool contrastLinear = false;
	// if per eye contrast should be applied
	bool contrastPerEye = false;
	bool contrastPerEyeLinear = false;
	double contrastLeft = 50;
	double contrastMidpointLeft = 50;
	double contrastRight = 50;
	double contrastMidpointRight = 50;
	// increase or decrease the variation of the colors
	double saturation = 50;
	// gamma of the output
	double gamma = 2.2;
	// if the subpixels should be offset
	bool subpixelShift = true;
	// if the mura correction should be skipped
	bool disableMuraCorrection = false;
	// if the black levels should be skipped
	bool disableBlackLevels = false;
	// if the colors should be corrected to display the srgb input as srgb on the display
	bool srgbColorCorrection = false;
	// if the white point correction should be applied to the srgb color correction
	bool srgbWhitePointCorrection = false;
	// a 3x3 matrix to apply to the linear colors
	// if this is an array of 9 flat elements it will override the headset's default matrix
	std::vector<double> srgbColorCorrectionMatrix = {};
	// correct color uniformity issues of the lenses on the MeganeX
	bool lensColorCorrection = true;
	// if a 10 bit input will be dithered down to 8 bit
	bool dither10Bit = false;
	// if the filter should be enabled for overlays (defaults false to avoid performance hit when no overlay is shown)
	bool enableFilterForOverlay = false;
	// if the filter should be enabled when the SteamVR dashboard is open
	bool enableFilterForDashboard = true;
	// filters on the sampling of the texture,  "None", "NearestNeighbor", "FXAA2", "FXAA2CAS", "LumaSharpen", and "CAS"
	std::string samplingFilter = "None";
	// FXAA2 filter parameters
	double samplingFilterFXAA2SharpenStrength = 1.0;
	double samplingFilterFXAA2SharpenClamp = 0.05;
	// FXAA2CAS filter parameters
	double samplingFilterFXAA2CASStrength = 1.0;
	double samplingFilterFXAA2CASContrast = 1.0;
	// luma sharpen filter parameters
	double samplingFilterLumaSharpenStrength = 2.0;
	double samplingFilterLumaSharpenClamp = 0.1;
	int samplingFilterLumaSharpenPattern = 1;
	double samplingFilterLumaSharpenRadius = 1.0;
 	// CAS filter parameters
 	double samplingFilterCASStrength = 1.0;
 	double samplingFilterCASContrast = 1.0;
	// color multiplier for tint adjustments
	ConfigColor colorMultiplier = {1.0, 1.0, 1.0};
};


class Config{
public:
	enum HeadsetType{
		None = 0,
		Other = 1,
		MeganeX8K = 2,
		Vive = 3,
		DreamAir = 4,
	};
	
	class BaseHeadsetConfig{
	public:
		// if the headset should be shimmed by this driver
		bool enable = true;
		// the type of headset this is
		HeadsetType headsetType = HeadsetType::None;
		// ipd in mm
		double ipd = 63.0;
		// ipd offset from the ipd value in mm
		double ipdOffset = 0.0;
		// horizontal offset in mm to shift both eyes to the right
		double horizontalIPDOffset = 0.0;
		// minimum black levels from 0 to 1
		double blackLevel = 0;
		// tint the display this color
		ConfigColor colorMultiplier = {};
		// distortion profile to use
		std::string distortionProfile = "None";
		// amount to zoom in the distortion profile
		double distortionZoom = 1.0;
		// amount to zoom in the FOV, the fov is divided by this value
		double fovZoom = 1.0;
		// amount to zoom in the FOV using tangent-based scaling for flatter perception
		double flatFovZoom = 1.0;
		// multiplier for the subpixel offsets
		double subpixelShift = 1.0;
		// subpixel offsets in pixel units for each color channel [offsetXRed, offsetYRed, offsetXGreen, offsetYGreen, offsetXBlue, offsetYBlue]
		std::vector<double> subpixelOffsets = {0, 0, 0, 0, 0, 0};
		// width of one eye in pixels
		int resolutionX = 3840;
		// height of one eye in pixels
		int resolutionY = 3552;
		// clockwise rotation of the image on the right display, 0:0, 1:90, 2:180, 3:270
		int displayRotation = 0;
		// max horizontal fov
		double maxFovX = 100.0;
		// max vertical fov
		double maxFovY = 96.0;
		// distortion mesh resolution
		int distortionMeshResolution = 127;
		// if the fov should be slightly adjusted each session to prevent sharp burn in along the edges
		bool fovBurnInPrevention = true;
		// if the distortion profile should clamp the image to the bounds of the display or if it will instead render an image at whatever FOV is set
		bool fovClamping = true;
		// device type used to filter distortion profiles in the GUI
		std::string distortionProfileDeviceType = "";
		// multiply 100% render resolution width
		double renderResolutionMultiplierX = 1.0;
		// multiply 100% render resolution height
		double renderResolutionMultiplierY = 1.0;
		// percent of 1:1 resolution to apply the super sampling downscale filter at, this is really high to allow for subpixel sampling
		double superSamplingFilterPercent = 500;
		// seconds of latency to the display
		double secondsFromVsyncToPhotons = 0.007;
		// seconds from the the first to last line of the display
		double secondsFromPhotonsToVblank = 0.0025;
		// angle in degrees for each eye to be rotated outwards
		double eyeRotation = 0.0;
		// disable eyes as much as possible. 0:both enabled 1:left disabled 2:right disabled 3:both disabled
		int disableEye = 0;
		// if the fov should be decreased for the disabled eye, this causes problems in some apps
		bool disableEyeDecreaseFov = false;
		// if a vive link box should be used for bluetooth
		bool useViveBluetooth = false;
		// if the display is in direct mode or false if it is on the desktop
		bool directMode = true;
		// if the icons in the SteamVR status window should be modified
		bool replaceIcons = true;
		// the edid for the headset
		int edidVendorId = 0;
		// the edid for the headset
		int edidProductId = 0;
		// if non zero, override the edid vendor id
		int edidVendorIdOverride = 0;
		// if non zero, override the edid product id
		int edidProductIdOverride = 0;
		// DSC Version
		int dscVersion = -1;
		// DSC Slice count
		int dscSliceCount = -1;
		// DSC bits per pixel
		int dscBPPx16 = -1;
		// if the driver should be enabled for every hmd
		bool forceEnable = false;
		// if parallel projection should be used for rendering
		bool parallelProjection = true;
		// if eye tracking should be enabled
		bool enableEyeTracking = false;
		// Config struct for the hidden area mesh
		HiddenAreaMeshConfig hiddenArea;
		// config for dimming the display when stationary
		StationaryDimmingConfig stationaryDimming = {};
	};
	
	class MeganeX8KConfig : public BaseHeadsetConfig{
	public:
		MeganeX8KConfig(){
			headsetType = HeadsetType::MeganeX8K;
			distortionProfile = "MeganeX8K Default";
			distortionProfileDeviceType = "MeganeX8K";
			edidVendorId = 0xcc4c; // SFL
			displayRotation = 1;
			subpixelOffsets = {-0.33 / 3552.0, 0, 0, 0, 0.33 / 3552.0, 0};
		}
	};
	// config for the MeganeX superlight 8K
	MeganeX8KConfig meganeX8K = {};
	
	class DreamAirConfig : public BaseHeadsetConfig{
		public:
		DreamAirConfig(){
			headsetType = HeadsetType::DreamAir;
			distortionProfile = "Dream Air Default";
			distortionProfileDeviceType = "DreamAir";
			maxFovX = 96;
			maxFovY = 86;
			edidVendorId = 53826; // PVR
			displayRotation = 3;
			subpixelOffsets = {0.33 / 3552.0, 0, 0, 0, -0.33 / 3552.0, 0};
			eyeRotation = 2;
			enableEyeTracking = true;
		}
	};
	// config for the Dream Air
	DreamAirConfig dreamAir = {};
	
	class FakeHeadsetConfig : public BaseHeadsetConfig{
		public:
		FakeHeadsetConfig(){
			enable = false;
			headsetType = HeadsetType::Other;
			distortionProfile = "MeganeX8K Default";
			displayRotation = 0;
			// use a 1080p monitor
			directMode = false;
			resolutionX = 960;
			resolutionY = 1080;
		}
	};
	// config for the fake headset
	FakeHeadsetConfig fakeHeadset = {};
	
	class GeneralHeadsetConfig{
	public:
		// if a vive link box should be used for bluetooth
		bool useViveBluetooth = false;
	};
	GeneralHeadsetConfig generalHeadset = {};
	
	CustomShaderConfig customShader = {};
	
	// processing of direct mode layer textures before a streaming driver (e.g.
	// vrlink / Steam Link) consumes them. this is the always on path for
	// headsets whose driver composites frames itself, where the compositor
	// shader replacement only runs while the dashboard is open.
	StreamFrameConfig streamFrame = {};
	
	// if devices should always be reported as tracking
	bool forceTracking = false;
	
	// if the screenshot requests should cause full compositor debug screenshots to be taken
	bool takeCompositorScreenshots = false;
	
	// makes the diver only do things related to closed source functionality if it exits
	// this allows for a driver built from source to run along side the driver with proprietary code
	bool onlyHandlePrivateFunctionality = false;
	
	// reload the config every time a file is changed in the distortions directory
	// this is for manual json editing, utilities should touch the main settings file when done modifying distortions instead
	// this is now enabled by default
	// bool watchDistortionProfiles = false;
	
	// if the config has been changes and should be reloaded
	// this will be set the false at the end of RunFrame
	bool hasBeenUpdated = true;
	
};

// config for a single custom distortion profile
class DistortionProfileConfig{
public:
	// name of distortion profile, this will be it's filename
	std::string name = "None";
	// the headset device this profile is for, empty for all devices, or "MeganeX8K" for the MeganeX superlight 8K
	std::string device = "";
	// description to display
	std::string description = "";
	// author of the distortion profile
	std::string author = "";
	// the date when it was created
	double creationDate = 0;
	// last time it was modified, used for reloading if changed
	double modifiedTime = 0;
	// type of distortion, None or RadialBezier
	std::string type = "None";
	// main distortion
	std::vector<double> distortions = {};
	// additional distortion to apply to the red channel
	std::vector<double> distortionsRed = {};
	// additional distortion to apply to the blue channel
	std::vector<double> distortionsBlue = {};
	// offset image outwards on the display using the same 0 to 100 scale 
	float offsetX = 0.0f;
	// offset image upwards on the display using the same 0 to 100 scale
	float offsetY = 0.0f;
	// if legacy smoothing should be used for bezier curves
	bool legacySmoothing = false;
	// amount to smooth the curve from 0 to 1 for legacy smoothing
	double smoothAmount = 0.66;
};

// global config object
extern Config driverConfig;

// config from before the last reload
extern Config driverConfigOld;

// config with default values
extern Config defaultDriverConfig;

// lock for the config to prevent updates while reading
extern std::mutex driverConfigLock;

// version of the application
extern std::string driverVersion;


#if __has_include("../Driver/HidModifierPrivate.cpp")
#define HAS_PRIVATE 1
#endif