#pragma once

#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRFaceOutput.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace galaxyxr::osc {

// Project-defined OSC 1.0 message. This is intentionally not labeled as a
// Valve FB2 or VRCFaceTracking-native schema.
constexpr char FaceFrameAddress[] = "/galaxyxr/face/frame/v1";
constexpr std::size_t MaximumFaceFrameBytes = 512;

bool EncodeFaceFrame(
	const FaceSharedMemoryV1& snapshot,
	std::vector<std::uint8_t>& packet);

} // namespace galaxyxr::osc
