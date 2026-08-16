#pragma once

#include "GalaxyXRClockSync.h"
#include "GalaxyXRTypes.h"

#include "openvr_driver.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace galaxyxr {

struct EyePublisherConfiguration {
	std::string source = "off";
	std::string syntheticPattern = "off";
	double fallbackDistanceMeters = 2.0;
	std::uint32_t staleAfterMs = 100;
};

class GalaxyXREyePublisher {
public:
	// Returns true when the selected owner changed and the HMD should be
	// rebound. Once a project component exists, owner changes require a
	// tracked-device restart because OpenVR exposes no component removal API.
	bool Configure(const EyePublisherConfiguration& configuration);
	bool Bind(vr::PropertyContainerHandle_t container, std::uint32_t hmdDeviceId);
	void InvalidateSession();
	void Unbind();
	void RunFrame(
		const TrackingSample* sample,
		const GalaxyXRClockSync& clock,
	std::int64_t nowHostMonotonicNs);
	bool OwnsPublisher() const;
	std::string Owner() const;

	static bool BuildGaze(
		const TrackingSample& sample,
		double fallbackDistanceMeters,
		vr::VREyeTrackingData_t& output);

private:
	void PublishInvalid(double timeOffset);
	void PublishSynthetic(std::int64_t nowHostMonotonicNs);

	EyePublisherConfiguration configuration;
	mutable std::mutex mutex;
	vr::PropertyContainerHandle_t container = vr::k_ulInvalidPropertyContainer;
	std::uint32_t hmdDeviceId = vr::k_unTrackedDeviceIndexInvalid;
	vr::VRInputComponentHandle_t component = vr::k_ulInvalidInputComponentHandle;
	bool componentCreated = false;
	bool invalidPublished = false;
	std::string owner = "off";
};

} // namespace galaxyxr
