#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRClockSync.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRDisplay.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRFaceOutput.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRIdentityState.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRPoseTiming.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRProfile.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRProtocol.h"
#include "../CustomHeadsetOpenVR/src/GalaxyXR/GalaxyXRStatus.h"
#include "../tools/GalaxyXROscCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message){
	if(!condition){
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

std::vector<std::uint8_t> Hex(const std::string& text){
	std::vector<std::uint8_t> bytes;
	auto nibble = [](char value) -> int {
		if(value >= '0' && value <= '9'){ return value - '0'; }
		if(value >= 'a' && value <= 'f'){ return value - 'a' + 10; }
		if(value >= 'A' && value <= 'F'){ return value - 'A' + 10; }
		return -1;
	};
	for(std::size_t index = 0; index + 1 < text.size(); index += 2){
		bytes.push_back(static_cast<std::uint8_t>(
			(nibble(text[index]) << 4) | nibble(text[index + 1])));
	}
	return bytes;
}

std::uint32_t ReadNetworkU32(
	const std::vector<std::uint8_t>& packet,
	std::size_t offset){
	return (static_cast<std::uint32_t>(packet[offset]) << 24) |
		(static_cast<std::uint32_t>(packet[offset + 1]) << 16) |
		(static_cast<std::uint32_t>(packet[offset + 2]) << 8) |
		static_cast<std::uint32_t>(packet[offset + 3]);
}

float ReadNetworkFloat(
	const std::vector<std::uint8_t>& packet,
	std::size_t offset){
	const std::uint32_t bits = ReadNetworkU32(packet, offset);
	float value = 0.0f;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

std::string ReadOscString(
	const std::vector<std::uint8_t>& packet,
	std::size_t& offset){
	const std::size_t start = offset;
	while(offset < packet.size() && packet[offset] != 0){
		++offset;
	}
	const std::string value(
		reinterpret_cast<const char*>(packet.data() + start),
		offset - start);
	++offset;
	while(offset % 4 != 0){
		++offset;
	}
	return value;
}

template<std::size_t Count>
std::array<std::uint8_t, Count> Sequence(std::uint8_t first){
	std::array<std::uint8_t, Count> value{};
	for(std::size_t index = 0; index < Count; ++index){
		value[index] = static_cast<std::uint8_t>(first + index);
	}
	return value;
}

void TestGoldenEnvelope(){
	const auto expected = Hex(
		"47585250010000000600440004000000101112131415161718191a1b1c1d1e1f"
		"0807060504030201a5a5000015cd5b070000000084bd76966791fc6495bd3513"
		"101afdeb01020304");
	const auto key = Sequence<galaxyxr::Sha256Bytes>(0);
	galaxyxr::Envelope envelope;
	envelope.messageType = galaxyxr::MessageType::TrackingSample;
	envelope.sessionId = Sequence<galaxyxr::SessionIdBytes>(0x10);
	envelope.sequence = 0x0102030405060708ULL;
	envelope.flags = 0x0000A5A5;
	envelope.clientMonotonicNs = 123456789;
	std::vector<std::uint8_t> packet;
	Check(
		galaxyxr::EncodePacket(envelope, {1, 2, 3, 4}, key, packet),
		"golden packet encodes");
	Check(packet == expected, "golden packet bytes match Android/Windows fixture");

	galaxyxr::DecodedPacket decoded;
	Check(
		galaxyxr::DecodePacket(expected.data(), expected.size(), key, decoded) ==
			galaxyxr::DecodeError::None,
		"golden packet authenticates");
	Check(decoded.envelope.sequence == envelope.sequence, "golden sequence round-trips");
	Check(decoded.payload == std::vector<std::uint8_t>({1, 2, 3, 4}), "golden payload round-trips");
}

void TestGoldenKdf(){
	const auto pairing = Sequence<galaxyxr::Sha256Bytes>(0);
	const auto client = Sequence<galaxyxr::NonceBytes>(0x20);
	const auto host = Sequence<galaxyxr::NonceBytes>(0x40);
	const auto session = Sequence<galaxyxr::SessionIdBytes>(0x10);
	const auto expectedBytes = Hex(
		"2a59a90558674b5320408230903c123fc7fb0abc3e8f00e7e489b379199364f3");
	std::array<std::uint8_t, galaxyxr::Sha256Bytes> expected{};
	std::copy(expectedBytes.begin(), expectedBytes.end(), expected.begin());
	std::array<std::uint8_t, galaxyxr::Sha256Bytes> derived{};
	Check(
		galaxyxr::DeriveSessionKey(pairing, client, host, session, derived),
		"session key derives");
	Check(derived == expected, "session KDF matches Android/Windows fixture");
}

void TestClientAdmission(){
	galaxyxr::ClientIdentity client;
	client.versionCode = 5002276;
	client.apkSha256 = Sequence<galaxyxr::Sha256Bytes>(0x10);
	client.bridgeSha256 = Sequence<galaxyxr::Sha256Bytes>(0x40);
	Check(
		galaxyxr::MatchesClientAdmission(
			client,
			5002276,
			client.apkSha256,
			client.bridgeSha256),
		"exact version/APK/bridge provenance is admitted");
	auto wrongApk = client.apkSha256;
	wrongApk[0] ^= 0xFF;
	Check(
		!galaxyxr::MatchesClientAdmission(
			client,
			5002276,
			wrongApk,
			client.bridgeSha256),
		"APK provenance mismatch is refused");
	auto wrongBridge = client.bridgeSha256;
	wrongBridge[31] ^= 0xFF;
	Check(
		!galaxyxr::MatchesClientAdmission(
			client,
			5002276,
			client.apkSha256,
			wrongBridge),
		"bridge provenance mismatch is refused");
	Check(
		!galaxyxr::MatchesClientAdmission(
			client,
			5002275,
			client.apkSha256,
			client.bridgeSha256),
		"client version mismatch is refused");
}

void TestInvalidFaceLifecycle(){
	galaxyxr::FaceSharedMemoryV1 snapshot{};
	snapshot.generation = 2;
	snapshot.flags = 1;
	snapshot.sessionId[0] = 7;
	snapshot.sampleSequence = 42;
	snapshot.clientSampleMonotonicNs = 100;
	snapshot.hostReceiveMonotonicNs = 200;
	snapshot.weights[0] = 0.75f;
	snapshot.confidence[0] = 0.5f;
	galaxyxr::InvalidateFaceSnapshot(snapshot);
	Check(
		snapshot.generation == 4 &&
		snapshot.flags == 0 &&
		snapshot.sessionId[0] == 0 &&
		snapshot.sampleSequence == 0 &&
		snapshot.clientSampleMonotonicNs == 0 &&
		snapshot.hostReceiveMonotonicNs == 0 &&
		snapshot.weights[0] == 0.0f &&
		snapshot.confidence[0] == 0.0f,
		"shutdown commits a stable nonzero invalid face generation");
}

void TestPoseFailureAtomicity(){
	galaxyxr::GalaxyXRPoseTiming timing;
	galaxyxr::PoseTimingConfiguration configuration;
	configuration.mode = "fixed_pose";
	configuration.clientPoseAlreadyPredicted = false;
	configuration.fixedPredictionSeconds = 0.010;
	timing.Configure(configuration);
	galaxyxr::GalaxyXRDeviceRegistry registry;
	registry.Activate(0, "HMD", vr::TrackedDeviceClass_HMD);
	galaxyxr::GalaxyXRClockSync clock;
	vr::DriverPose_t pose{};
	pose.poseIsValid = true;
	pose.qRotation.w = 0.1;
	pose.vecPosition[0] = 2.0;
	pose.vecVelocity[0] = 1.0;
	pose.vecAngularVelocity[1] = 1.0;
	const vr::DriverPose_t before = pose;
	double horizon = -1.0;
	Check(
		!timing.Apply(
			0,
			pose,
			registry,
			clock,
			nullptr,
			100,
			horizon),
		"out-of-norm source quaternion refuses pose prediction");
	Check(
		pose.vecPosition[0] == before.vecPosition[0] &&
		pose.qRotation.w == before.qRotation.w &&
		pose.poseTimeOffset == before.poseTimeOffset &&
		pose.vecVelocity[0] == before.vecVelocity[0] &&
		horizon == 0.0,
		"refused pose prediction leaves the caller pose unchanged");
}

void TestIdentityRollbackState(){
	Config::HeadsetType connectedHeadset = Config::HeadsetType::GalaxyXR;
	bool backupValid = true;
	Check(
		galaxyxr::RestoreConnectedHeadsetBackup(
			connectedHeadset,
			backupValid,
			Config::HeadsetType::Other) &&
		connectedHeadset == Config::HeadsetType::Other &&
		!backupValid,
		"identity rollback restores the preceding toolkit headset status");
	Check(
		!galaxyxr::RestoreConnectedHeadsetBackup(
			connectedHeadset,
			backupValid,
			Config::HeadsetType::None) &&
		connectedHeadset == Config::HeadsetType::Other,
		"identity rollback consumes its backup exactly once");
}

galaxyxr::TrackingSample ValidTracking(){
	galaxyxr::TrackingSample tracking;
	tracking.sampleSequence = 42;
	tracking.predictedDisplayMonotonicNs = 2000000000;
	tracking.queryXrTime = 300;
	tracking.predictedDisplayXrTime = 400;
	tracking.baseSpaceId = 1;
	tracking.coordinateRevision = 1;
	tracking.capabilityRevision = 7;
	tracking.eyeMode = galaxyxr::EyeMode::Both;
	tracking.leftEye.origin = {-0.032f, 0.0f, 0.0f};
	tracking.rightEye.origin = {0.032f, 0.0f, 0.0f};
	tracking.leftEye.orientation = {};
	tracking.rightEye.orientation = {};
	tracking.leftEye.state = galaxyxr::EyeState::Gazing;
	tracking.rightEye.state = galaxyxr::EyeState::Gazing;
	tracking.leftEye.positionTracked = true;
	tracking.leftEye.orientationTracked = true;
	tracking.rightEye.positionTracked = true;
	tracking.rightEye.orientationTracked = true;
	tracking.face.sampleXrTime = 500;
	tracking.face.sampleMonotonicNs = 1900000000;
	tracking.face.valid = true;
	tracking.face.state = galaxyxr::FaceState::Tracking;
	tracking.face.source = galaxyxr::FaceSource::Visual;
	tracking.face.confidence = {0.0f, 0.5f, 1.0f};
	return tracking;
}

void TestTrackingAndFace(){
	for(std::size_t oneHot = 0; oneHot < galaxyxr::FaceWeightCount; ++oneHot){
		auto source = ValidTracking();
		source.face.weights[oneHot] = 1.0f;
		std::vector<std::uint8_t> payload;
		Check(galaxyxr::EncodeTrackingPayload(source, payload), "one-hot tracking encodes");
		galaxyxr::TrackingSample decoded;
		Check(
			galaxyxr::DecodeTrackingPayload(payload, decoded) == galaxyxr::DecodeError::None,
			"one-hot tracking decodes");
		for(std::size_t index = 0; index < galaxyxr::FaceWeightCount; ++index){
			Check(
				decoded.face.weights[index] == (index == oneHot ? 1.0f : 0.0f),
				"all 68 canonical face indices survive");
		}
	}
	auto tongue = ValidTracking();
	tongue.face.weights[63] = 0.1f;
	tongue.face.weights[64] = 0.2f;
	tongue.face.weights[65] = 0.3f;
	tongue.face.weights[66] = 0.4f;
	tongue.face.weights[67] = 0.5f;
	std::vector<std::uint8_t> payload;
	Check(galaxyxr::EncodeTrackingPayload(tongue, payload), "five tongue values encode");
	galaxyxr::TrackingSample decoded;
	Check(
		galaxyxr::DecodeTrackingPayload(payload, decoded) == galaxyxr::DecodeError::None,
		"five tongue values decode");
	for(std::size_t index = 63; index <= 67; ++index){
		Check(decoded.face.weights[index] == tongue.face.weights[index], "tongue channels remain distinct");
	}

	auto invalid = ValidTracking();
	invalid.face.weights[12] = std::numeric_limits<float>::quiet_NaN();
	Check(!galaxyxr::EncodeTrackingPayload(invalid, payload), "NaN face weight is rejected");
	invalid = ValidTracking();
	invalid.leftEye.orientation.w = 0.1f;
	Check(!galaxyxr::EncodeTrackingPayload(invalid, payload), "non-unit quaternion is rejected");
}

void TestAndroidWireEnumParity(){
	Check(
		static_cast<std::uint8_t>(galaxyxr::EyeMode::NotTracking) == 0 &&
		static_cast<std::uint8_t>(galaxyxr::EyeMode::Right) == 1 &&
		static_cast<std::uint8_t>(galaxyxr::EyeMode::Left) == 2 &&
		static_cast<std::uint8_t>(galaxyxr::EyeMode::Both) == 3,
		"eye mode raw values match XrEyeTrackingModeANDROID");
	Check(
		static_cast<std::uint8_t>(galaxyxr::FaceState::Paused) == 0 &&
		static_cast<std::uint8_t>(galaxyxr::FaceState::Stopped) == 1 &&
		static_cast<std::uint8_t>(galaxyxr::FaceState::Tracking) == 2,
		"face state raw values match XrFaceTrackingStateANDROID");
}

void TestProjectionConvention(){
	galaxyxr::GalaxyXRDisplay display;
	galaxyxr::DisplayConfiguration configuration;
	configuration.geometryMode = "negotiated";
	display.Configure(configuration);
	display.SetAuthenticated(true);
	galaxyxr::CapabilitySnapshot capabilities;
	capabilities.revision = 1;
	capabilities.currentRefreshHz = 90.0f;
	capabilities.views.resize(2);
	for(auto& view : capabilities.views){
		view.recommendedWidth = 3552;
		view.recommendedHeight = 3840;
		view.maximumWidth = 4096;
		view.maximumHeight = 4096;
		view.recommendedSampleCount = 1;
		view.fovLeft = -0.7f;
		view.fovRight = 0.8f;
		view.fovUp = 0.9f;
		view.fovDown = -0.6f;
	}
	display.UpdateCapabilities(capabilities);
	float left = 0.0f;
	float right = 0.0f;
	float openVrTop = 0.0f;
	float openVrBottom = 0.0f;
	Check(
		display.GetProjectionRaw(
			vr::Eye_Left,
			left,
			right,
			openVrTop,
			openVrBottom),
		"negotiated projection is available after authentication");
	Check(left < 0.0f && right > 0.0f, "projection horizontal tangents retain signs");
	Check(
		openVrTop < 0.0f && openVrBottom > 0.0f,
		"OpenVR top/bottom names receive down/up tangents respectively");

	configuration.timingMode = "telemetry";
	configuration.staticPhotonLatencySeconds = 0.078;
	display.Configure(configuration);
	galaxyxr::PresentationSample uncorrelated;
	uncorrelated.predictedDisplayMonotonicNs = 123456789;
	uncorrelated.flags = 3;
	display.UpdatePresentation(uncorrelated);
	double latencySeconds = 0.0;
	bool estimated = false;
	galaxyxr::GalaxyXRClockSync unsynchronizedClock;
	Check(
		display.GetPhotonLatencySeconds(
			unsynchronizedClock,
			100000000,
			latencySeconds,
			estimated),
		"telemetry mode provides a bounded fallback without exact correlation");
	Check(
		std::fabs(latencySeconds - 0.078) < 1e-9 && estimated,
		"uncorrelated Android v1 presentation retains the 78 ms estimated fallback");

	std::vector<std::uint8_t> payload;
	capabilities.views[0].fovUp = -0.1f;
	Check(
		!galaxyxr::EncodeCapabilitiesPayload(capabilities, payload),
		"sign-crossing OpenXR FOV is rejected");
	capabilities.views[0].fovUp = 1.56f;
	Check(
		!galaxyxr::EncodeCapabilitiesPayload(capabilities, payload),
		"near-singular OpenXR FOV is rejected");
}

void TestAuthenticationAndFuzz(){
	const auto key = Sequence<galaxyxr::Sha256Bytes>(0);
	auto packet = Hex(
		"47585250010000000600440004000000101112131415161718191a1b1c1d1e1f"
		"0807060504030201a5a5000015cd5b070000000084bd76966791fc6495bd3513"
		"101afdeb01020304");
	auto bad = packet;
	bad.back() ^= 0x80;
	galaxyxr::DecodedPacket decoded;
	Check(
		galaxyxr::DecodePacket(bad.data(), bad.size(), key, decoded) ==
			galaxyxr::DecodeError::AuthenticationFailed,
		"payload mutation fails authentication");

	galaxyxr::Envelope futureMinor;
	futureMinor.versionMinor = 1;
	futureMinor.messageType = galaxyxr::MessageType::Hello;
	std::vector<std::uint8_t> futurePacket;
	Check(
		!galaxyxr::EncodePacket(futureMinor, {}, key, futurePacket),
		"undefined minor revision is not emitted as v1.0");
	auto authenticatedFutureMinor = packet;
	authenticatedFutureMinor[6] = 1;
	authenticatedFutureMinor[7] = 0;
	std::fill(
		authenticatedFutureMinor.begin() + 52,
		authenticatedFutureMinor.begin() + 68,
		std::uint8_t{0});
	std::array<std::uint8_t, galaxyxr::Sha256Bytes> futureDigest{};
	Check(
		galaxyxr::ComputeHmacSha256(
			key,
			authenticatedFutureMinor.data(),
			68,
			authenticatedFutureMinor.data() + 68,
			authenticatedFutureMinor.size() - 68,
			futureDigest),
		"future-minor fixture authenticates");
	std::copy_n(
		futureDigest.begin(),
		galaxyxr::AuthenticationTagBytes,
		authenticatedFutureMinor.begin() + 52);
	Check(
		galaxyxr::DecodePacket(
			authenticatedFutureMinor.data(),
			authenticatedFutureMinor.size(),
			key,
			decoded) == galaxyxr::DecodeError::UnsupportedMinor,
		"undefined minor revision fails closed before payload interpretation");

	std::uint32_t state = 0xC001D00D;
	for(int iteration = 0; iteration < 10000; ++iteration){
		state = state * 1664525U + 1013904223U;
		std::vector<std::uint8_t> candidate = packet;
		const std::size_t mutations = 1 + (state % 4);
		for(std::size_t mutation = 0; mutation < mutations; ++mutation){
			state = state * 1664525U + 1013904223U;
			candidate[state % candidate.size()] ^= static_cast<std::uint8_t>(state >> 24);
		}
		(void)galaxyxr::DecodePacket(candidate.data(), candidate.size(), key, decoded);
	}
	Check(true, "deterministic 10k mutation decode completed without crash/hang");
}

void TestClockAndProfile(){
	galaxyxr::GalaxyXRClockSync clock;
	for(std::int64_t index = 0; index < 8; ++index){
		const std::int64_t t0 = 1000000000LL + index * 1000000000LL;
		const std::int64_t t1 = t0 + 6000000LL;
		const std::int64_t t2 = t1 + 100000LL;
		const std::int64_t t3 = t0 + 2100000LL;
		Check(clock.AddExchange(t0, t1, t2, t3), "clock sample accepted");
	}
	const auto estimate = clock.GetEstimate();
	Check(estimate.valid, "clock estimate becomes valid");
	Check(std::fabs(estimate.offsetNs - 5000000.0) < 1.0, "clock offset is correct");
	std::int64_t host = 0;
	Check(clock.ClientToHost(9005000000LL, host), "client time converts");
	Check(std::llabs(host - 9000000000LL) <= 1, "client-to-host conversion is correct");

	galaxyxr::GalaxyXRProfile profile;
	galaxyxr::ClientIdentity client;
	client.clientNonce = Sequence<galaxyxr::NonceBytes>(0x20);
	client.packageName = "com.valvesoftware.steamlinkvr";
	client.versionCode = 5002276;
	client.manufacturer = "Samsung";
	client.model = "Samsung Galaxy XR";
	client.deviceSerial = "VRLINKHMDGALAXYXR";
	const auto session = Sequence<galaxyxr::SessionIdBytes>(0x10);
	Check(profile.BeginSession(session, client, 100), "authenticated Galaxy profile begins");
	Check(
		galaxyxr::SelectPublicSerial(
			"unknown",
			"VRLINKHMDGALAXYXR",
			"fallback") == "VRLINKHMDGALAXYXR",
		"Android Build.SERIAL unknown falls back to the pre-override active serial");
	Check(
		galaxyxr::SelectPublicSerial(
			"bad/serial",
			"",
			"VRLINKHMDGALAXYXR") == "VRLINKHMDGALAXYXR",
		"unsafe public serial characters fall back to the stable legacy serial");
	Check(
		galaxyxr::SelectPublicSerial(
			"GalaxyXR_123-ABC",
			"VRLINKHMDGALAXYXR",
			"fallback") == "GalaxyXR_123-ABC",
		"bounded safe negotiated serial is retained");
	galaxyxr::CapabilitySnapshot capabilities;
	capabilities.revision = 7;
	capabilities.currentRefreshHz = 90.0f;
	capabilities.views.resize(2);
	for(auto& view : capabilities.views){
		view.recommendedWidth = 3552;
		view.recommendedHeight = 3840;
		view.maximumWidth = 4096;
		view.maximumHeight = 4096;
		view.recommendedSampleCount = 1;
		view.fovLeft = -0.8f;
		view.fovRight = 0.8f;
		view.fovUp = 0.8f;
		view.fovDown = -0.8f;
	}
	Check(profile.UpdateCapabilities(capabilities), "capability revision accepted");
	auto firstTracking = ValidTracking();
	firstTracking.sampleSequence = 10;
	Check(profile.UpdateTracking(firstTracking), "first payload-local tracking sequence accepted");
	galaxyxr::PresentationSample interleavedPresentation;
	interleavedPresentation.clientFrameCounter = 99;
	profile.UpdatePresentation(interleavedPresentation);
	auto skippedTracking = ValidTracking();
	skippedTracking.sampleSequence = 12;
	Check(
		profile.UpdateTracking(skippedTracking),
		"tracking payload sequence is monotonic independently of interleaved UDP envelope sequence");
	Check(
		profile.MatchesActivatedHmd(
			"VRLINKHMDGALAXYXR",
			"VRLINKHMDGALAXYXR",
			"",
			false),
		"authenticated session and exact serial match");
	profile.EndSession();
	Check(
		!profile.MatchesActivatedHmd(
			"VRLINKHMDGALAXYXR",
			"VRLINKHMDGALAXYXR",
			"",
			false),
		"no identity match after disconnect");
}

void TestOscFaceFrame(){
	galaxyxr::FaceSharedMemoryV1 snapshot{};
	std::memcpy(snapshot.magic, "GXF1", 4);
	snapshot.version = 1;
	snapshot.structureBytes = sizeof(snapshot);
	snapshot.generation = 2;
	snapshot.sampleSequence = 77;
	snapshot.clientSampleMonotonicNs = 100;
	snapshot.hostReceiveMonotonicNs = 200;
	snapshot.flags = 1;
	snapshot.state = static_cast<std::uint8_t>(galaxyxr::FaceState::Tracking);
	snapshot.source = static_cast<std::uint8_t>(galaxyxr::FaceSource::Visual);
	snapshot.parameterCount = galaxyxr::FaceWeightCount;
	snapshot.confidenceCount = galaxyxr::FaceConfidenceCount;
	snapshot.weights[63] = 0.1f;
	snapshot.weights[64] = 0.2f;
	snapshot.weights[65] = 0.3f;
	snapshot.weights[66] = 0.4f;
	snapshot.weights[67] = 0.5f;
	snapshot.confidence[0] = 0.6f;
	snapshot.confidence[1] = 0.7f;
	snapshot.confidence[2] = 0.8f;
	std::vector<std::uint8_t> packet;
	Check(galaxyxr::osc::EncodeFaceFrame(snapshot, packet), "lossless OSC frame encodes");
	Check(packet.size() <= galaxyxr::osc::MaximumFaceFrameBytes, "OSC frame stays under bounded datagram size");
	std::size_t offset = 0;
	Check(
		ReadOscString(packet, offset) == galaxyxr::osc::FaceFrameAddress,
		"OSC project address is explicit");
	const std::string typeTags = ReadOscString(packet, offset);
	Check(typeTags == std::string(",iiihhh") +
		std::string(galaxyxr::FaceWeightCount + galaxyxr::FaceConfidenceCount, 'f'),
		"OSC type tags retain every canonical face and confidence float");
	offset += 3 * sizeof(std::uint32_t) + 3 * sizeof(std::uint64_t);
	for(std::size_t index = 63; index <= 67; ++index){
		Check(
			ReadNetworkFloat(packet, offset + index * sizeof(float)) ==
				snapshot.weights[index],
			"OSC preserves each distinct tongue channel");
	}
	auto invalid = snapshot;
	invalid.weights[0] = std::numeric_limits<float>::quiet_NaN();
	Check(!galaxyxr::osc::EncodeFaceFrame(invalid, packet), "OSC rejects non-finite biometric values");
}

void TestRedactedRuntimeStatus(){
	galaxyxr::GalaxyXRStatus status;
	status.SetState("session", "authenticated");
	status.SetState("pairing_token", "must-not-serialize");
	status.SetCounter("tracking_sequence", 42);
	status.Transition("runtime", "ready");
	const std::string serialized = status.Serialize();
	Check(
		serialized.find("\"session\": \"authenticated\"") != std::string::npos,
		"runtime status exposes safe readiness state");
	Check(
		serialized.find("must-not-serialize") == std::string::npos,
		"runtime status rejects secret-bearing names");
	Check(
		galaxyxr::GalaxyXRStatus::IsSnapshotStale(1000, 7001, 5000),
		"runtime status detects stale heartbeat");
	Check(
		!galaxyxr::GalaxyXRStatus::IsSnapshotStale(1000, 6000, 5000),
		"runtime status accepts heartbeat at freshness boundary");
}

} // namespace

int main(){
	TestGoldenEnvelope();
	TestGoldenKdf();
	TestClientAdmission();
	TestInvalidFaceLifecycle();
	TestPoseFailureAtomicity();
	TestIdentityRollbackState();
	TestTrackingAndFace();
	TestAndroidWireEnumParity();
	TestProjectionConvention();
	TestAuthenticationAndFuzz();
	TestClockAndProfile();
	TestOscFaceFrame();
	TestRedactedRuntimeStatus();
	if(failures != 0){
		std::cerr << failures << " Galaxy XR test(s) failed\n";
		return 1;
	}
	std::cout << "Galaxy XR protocol/clock/profile tests passed\n";
	return 0;
}
