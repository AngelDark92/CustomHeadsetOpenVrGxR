#pragma once

#include "GalaxyXRClockSync.h"
#include "GalaxyXRDeviceRegistry.h"
#include "GalaxyXRTypes.h"

#include "openvr_driver.h"

#include <mutex>
#include <string>

namespace galaxyxr {

struct PoseTimingConfiguration {
	std::string mode = "off";
	bool clientPoseAlreadyPredicted = true;
	bool suppressVelocityWhenHostPredicted = true;
	double fixedPredictionSeconds = 0.0;
	double maximumHmdPredictionSeconds = 0.050;
	double maximumControllerPredictionSeconds = 0.030;
};

class GalaxyXRPoseTiming {
public:
	void Configure(const PoseTimingConfiguration& configuration);
	bool Apply(
		std::uint32_t openVrId,
		vr::DriverPose_t& pose,
		const GalaxyXRDeviceRegistry& registry,
		const GalaxyXRClockSync& clock,
		const PresentationSample* presentation,
		std::int64_t nowHostMonotonicNs,
		double& appliedHorizonSeconds) const;

private:
	mutable std::mutex mutex;
	PoseTimingConfiguration configuration;
};

} // namespace galaxyxr
