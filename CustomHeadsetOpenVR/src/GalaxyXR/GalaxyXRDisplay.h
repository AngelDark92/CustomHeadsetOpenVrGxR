#pragma once

#include "GalaxyXRClockSync.h"
#include "GalaxyXRTypes.h"

#include "openvr_driver.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace galaxyxr {

struct DisplayConfiguration {
	std::string geometryMode = "negotiated";
	std::uint32_t fallbackWidth = 3552;
	std::uint32_t fallbackHeight = 3840;
	std::string timingMode = "static";
	double staticPhotonLatencySeconds = 0.078;
	double maximumTelemetrySlewSeconds = 0.001;
};

class GalaxyXRDisplay {
public:
	void Configure(const DisplayConfiguration& configuration);
	void SetAuthenticated(bool authenticated);
	void UpdateCapabilities(const CapabilitySnapshot& capabilities);
	void UpdatePresentation(const PresentationSample& presentation);
	void Reset();

	bool GetRecommendedRenderTargetSize(std::uint32_t& width, std::uint32_t& height) const;
	bool GetEyeOutputViewport(
		vr::EVREye eye,
		std::uint32_t& x,
		std::uint32_t& y,
		std::uint32_t& width,
		std::uint32_t& height) const;
	bool GetProjectionRaw(
		vr::EVREye eye,
		float& left,
		float& right,
		float& top,
		float& bottom) const;
	bool GetPhotonLatencySeconds(
		const GalaxyXRClockSync& clock,
		std::int64_t nowHostMonotonicNs,
		double& latencySeconds,
		bool& estimated);

private:
	bool GeometryReadyLocked() const;

	mutable std::mutex mutex;
	DisplayConfiguration configuration;
	bool authenticated = false;
	bool hasCapabilities = false;
	bool hasPresentation = false;
	CapabilitySnapshot capabilities;
	PresentationSample presentation;
	double slewedTelemetryLatencySeconds = 0.078;
};

} // namespace galaxyxr
