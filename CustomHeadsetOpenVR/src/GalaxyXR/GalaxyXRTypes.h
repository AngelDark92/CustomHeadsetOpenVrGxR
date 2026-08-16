#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace galaxyxr {

constexpr std::size_t FaceWeightCount = 68;
constexpr std::size_t FaceConfidenceCount = 3;
constexpr std::size_t SessionIdBytes = 16;
constexpr std::size_t NonceBytes = 32;
constexpr std::size_t Sha256Bytes = 32;
constexpr std::uint64_t CapabilityFeatureEyeTracking = 1ULL << 0;
constexpr std::uint64_t CapabilityFeatureFaceTracking = 1ULL << 1;
constexpr std::uint32_t CapabilityPermissionEyeTracking = 1U << 0;
constexpr std::uint32_t CapabilityPermissionFaceTracking = 1U << 1;
constexpr std::uint32_t CapabilityPermissionRecordAudio = 1U << 2;

enum class EyeMode : std::uint8_t {
	NotTracking = 0,
	Right = 1,
	Left = 2,
	Both = 3,
};

enum class EyeState : std::uint8_t {
	Invalid = 0,
	Gazing = 1,
	Shut = 2,
};

enum class FaceState : std::uint8_t {
	Paused = 0,
	Stopped = 1,
	Tracking = 2,
};

enum class FaceSource : std::uint8_t {
	Unspecified = 0,
	Visual = 1,
	Audio = 2,
	Multimodal = 3,
};

enum class DeviceRole : std::uint8_t {
	Unknown = 0,
	Hmd = 1,
	LeftController = 2,
	RightController = 3,
	LeftHand = 4,
	RightHand = 5,
};

struct Vector3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Quaternion {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 1.0f;
};

struct EyePose {
	Vector3 origin;
	Quaternion orientation;
	EyeState state = EyeState::Invalid;
	bool positionTracked = false;
	bool orientationTracked = false;
};

struct FaceSample {
	std::int64_t sampleXrTime = 0;
	std::int64_t sampleMonotonicNs = 0;
	bool valid = false;
	FaceState state = FaceState::Paused;
	FaceSource source = FaceSource::Unspecified;
	std::array<float, FaceWeightCount> weights{};
	std::array<float, FaceConfidenceCount> confidence{};
};

struct TrackingSample {
	std::uint64_t sampleSequence = 0;
	std::int64_t captureMonotonicNs = 0;
	std::int64_t predictedDisplayMonotonicNs = 0;
	std::int64_t queryXrTime = 0;
	std::int64_t predictedDisplayXrTime = 0;
	std::uint32_t baseSpaceId = 0;
	std::uint32_t coordinateRevision = 0;
	std::uint32_t capabilityRevision = 0;
	std::uint32_t validityFlags = 0;
	EyeMode eyeMode = EyeMode::NotTracking;
	EyePose leftEye;
	EyePose rightEye;
	FaceSample face;
	std::int64_t hostReceiveMonotonicNs = 0;
};

struct ViewGeometry {
	std::uint32_t recommendedWidth = 0;
	std::uint32_t recommendedHeight = 0;
	std::uint32_t maximumWidth = 0;
	std::uint32_t maximumHeight = 0;
	std::uint32_t recommendedSampleCount = 0;
	float fovLeft = 0.0f;
	float fovRight = 0.0f;
	float fovUp = 0.0f;
	float fovDown = 0.0f;
};

struct CapabilitySnapshot {
	std::uint32_t revision = 0;
	std::uint64_t featureBits = 0;
	std::uint32_t permissionFlags = 0;
	std::uint32_t viewConfigurationType = 0;
	std::uint8_t overlayMode = 0;
	std::uint8_t timingFlags = 0;
	float currentRefreshHz = 0.0f;
	std::vector<float> supportedRefreshRates;
	std::vector<ViewGeometry> views;
	std::string activeActivity;
	std::string productId;
	std::string enabledExtensions;
};

struct PresentationSample {
	std::uint64_t clientFrameCounter = 0;
	std::int64_t predictedDisplayMonotonicNs = 0;
	std::int64_t waitFrameMonotonicNs = 0;
	std::int64_t beginFrameMonotonicNs = 0;
	std::int64_t endFrameMonotonicNs = 0;
	std::uint32_t submittedWidth = 0;
	std::uint32_t submittedHeight = 0;
	std::uint32_t decoderWidth = 0;
	std::uint32_t decoderHeight = 0;
	std::uint32_t surfaceWidth = 0;
	std::uint32_t surfaceHeight = 0;
	std::uint64_t decodedFrameId = 0;
	std::uint64_t hostFrameId = 0;
	std::int64_t photonMonotonicNs = 0;
	std::uint32_t flags = 0;
	std::int64_t hostReceiveMonotonicNs = 0;
};

struct ClientIdentity {
	std::array<std::uint8_t, NonceBytes> clientNonce{};
	std::uint64_t featureBits = 0;
	std::uint32_t versionCode = 0;
	bool clockConversionSupported = false;
	std::string packageName;
	std::string versionName;
	std::array<std::uint8_t, Sha256Bytes> apkSha256{};
	std::string bridgeVersion;
	std::array<std::uint8_t, Sha256Bytes> bridgeSha256{};
	std::string manufacturer;
	std::string model;
	std::string deviceSerial;
	std::string buildFingerprint;
	std::string runtimeName;
	std::string runtimeVersion;
};

struct SessionSnapshot {
	bool authenticated = false;
	std::array<std::uint8_t, SessionIdBytes> sessionId{};
	ClientIdentity client;
	std::int64_t establishedHostMonotonicNs = 0;
};

} // namespace galaxyxr
