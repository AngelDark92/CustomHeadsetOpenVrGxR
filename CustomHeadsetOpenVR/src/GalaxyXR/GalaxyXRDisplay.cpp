#include "GalaxyXRDisplay.h"

#include <algorithm>
#include <cmath>

namespace galaxyxr {

void GalaxyXRDisplay::Configure(const DisplayConfiguration& value){
	std::lock_guard<std::mutex> lock(mutex);
	configuration = value;
	configuration.fallbackWidth =
		std::max<std::uint32_t>(256, std::min<std::uint32_t>(8192, configuration.fallbackWidth));
	configuration.fallbackHeight =
		std::max<std::uint32_t>(256, std::min<std::uint32_t>(8192, configuration.fallbackHeight));
	configuration.staticPhotonLatencySeconds =
		std::max(0.0, std::min(0.5, configuration.staticPhotonLatencySeconds));
	configuration.maximumTelemetrySlewSeconds =
		std::max(0.0001, std::min(0.010, configuration.maximumTelemetrySlewSeconds));
	if(!std::isfinite(slewedTelemetryLatencySeconds)){
		slewedTelemetryLatencySeconds = configuration.staticPhotonLatencySeconds;
	}
}

void GalaxyXRDisplay::SetAuthenticated(bool value){
	std::lock_guard<std::mutex> lock(mutex);
	authenticated = value;
	if(!authenticated){
		hasCapabilities = false;
		hasPresentation = false;
		slewedTelemetryLatencySeconds = configuration.staticPhotonLatencySeconds;
	}
}

void GalaxyXRDisplay::UpdateCapabilities(const CapabilitySnapshot& value){
	std::lock_guard<std::mutex> lock(mutex);
	if(!authenticated){
		return;
	}
	capabilities = value;
	hasCapabilities = !capabilities.views.empty() && capabilities.views.size() <= 2;
}

void GalaxyXRDisplay::UpdatePresentation(const PresentationSample& value){
	std::lock_guard<std::mutex> lock(mutex);
	if(authenticated){
		presentation = value;
		hasPresentation = true;
	}
}

void GalaxyXRDisplay::Reset(){
	std::lock_guard<std::mutex> lock(mutex);
	authenticated = false;
	hasCapabilities = false;
	hasPresentation = false;
	capabilities = {};
	presentation = {};
	slewedTelemetryLatencySeconds = configuration.staticPhotonLatencySeconds;
}

bool GalaxyXRDisplay::GetRecommendedRenderTargetSize(
	std::uint32_t& width,
	std::uint32_t& height) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!GeometryReadyLocked()){
		return false;
	}
	width = 0;
	height = 0;
	for(const ViewGeometry& view : capabilities.views){
		width = std::max(width, view.recommendedWidth);
		height = std::max(height, view.recommendedHeight);
	}
	return width != 0 && height != 0;
}

bool GalaxyXRDisplay::GetEyeOutputViewport(
	vr::EVREye eye,
	std::uint32_t& x,
	std::uint32_t& y,
	std::uint32_t& width,
	std::uint32_t& height) const{
	(void)eye;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	// The v1.0 capability payload describes each view's dimensions, but not
	// the opaque VRLink compositor texture layout. Preserve the wrapped
	// driver's viewport until that ABI is negotiated explicitly.
	return false;
}

bool GalaxyXRDisplay::GetProjectionRaw(
	vr::EVREye eye,
	float& left,
	float& right,
	float& top,
	float& bottom) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!GeometryReadyLocked() || capabilities.views.empty()){
		return false;
	}
	const std::size_t index =
		eye == vr::Eye_Left || capabilities.views.size() == 1 ? 0 : 1;
	const ViewGeometry& view = capabilities.views[index];
	left = std::tan(view.fovLeft);
	right = std::tan(view.fovRight);
	// OpenVR's historic parameter names are vertically reversed: pfTop is
	// the bottom (-Y) tangent and pfBottom is the top (+Y) tangent.
	top = std::tan(view.fovDown);
	bottom = std::tan(view.fovUp);
	return std::isfinite(left) && std::isfinite(right) &&
		std::isfinite(top) && std::isfinite(bottom);
}

bool GalaxyXRDisplay::GetPhotonLatencySeconds(
	const GalaxyXRClockSync& clock,
	std::int64_t nowHostMonotonicNs,
	double& latencySeconds,
	bool& estimated){
	std::lock_guard<std::mutex> lock(mutex);
	if(!authenticated || configuration.timingMode == "passthrough"){
		return false;
	}
	if(configuration.timingMode == "static"){
		latencySeconds = configuration.staticPhotonLatencySeconds;
		estimated = false;
		return true;
	}
	if(configuration.timingMode != "telemetry"){
		return false;
	}
	// v1 Android flags 0/1 describe predicted-display and wait-frame timing,
	// not a decoded host-frame-to-photon correlation. Bit 2 is reserved by
	// this project for exact frame correlation and is accepted only with all
	// required nonzero IDs/timestamp.
	const bool exactCorrelation =
		hasPresentation &&
		(presentation.flags & (1U << 2U)) != 0 &&
		presentation.hostFrameId != 0 &&
		presentation.decodedFrameId != 0 &&
		presentation.photonMonotonicNs > 0;
	if(!exactCorrelation){
		latencySeconds = configuration.staticPhotonLatencySeconds;
		estimated = true;
		return true;
	}

	std::int64_t targetHostNs = 0;
	if(!clock.ClientToHost(presentation.photonMonotonicNs, targetHostNs)){
		latencySeconds = configuration.staticPhotonLatencySeconds;
		estimated = true;
		return true;
	}
	double targetLatency =
		static_cast<double>(targetHostNs - nowHostMonotonicNs) / 1000000000.0;
	if(!std::isfinite(targetLatency) || targetLatency < 0.0 || targetLatency > 0.5){
		latencySeconds = configuration.staticPhotonLatencySeconds;
		estimated = true;
		return true;
	}
	const double delta = std::max(
		-configuration.maximumTelemetrySlewSeconds,
		std::min(
			configuration.maximumTelemetrySlewSeconds,
			targetLatency - slewedTelemetryLatencySeconds));
	slewedTelemetryLatencySeconds += delta;
	latencySeconds = slewedTelemetryLatencySeconds;
	estimated = true;
	return true;
}

bool GalaxyXRDisplay::GeometryReadyLocked() const{
	return authenticated &&
		hasCapabilities &&
		configuration.geometryMode == "negotiated";
}

} // namespace galaxyxr
