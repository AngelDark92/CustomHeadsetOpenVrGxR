#include "GalaxyXRSystem.h"

#include "../Config/ConfigLoader.h"
#include "../Driver/DriverLog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace galaxyxr {
namespace {

std::string LoadPairingToken(const Config::GalaxyXRConfig& configuration){
	if(!configuration.telemetry.pairingTokenFile.empty()){
		std::filesystem::path path(configuration.telemetry.pairingTokenFile);
		if(path.is_relative()){
			path = std::filesystem::path(driverConfigLoader.GetConfigFolder()) / path;
		}
		std::ifstream input(path, std::ios::binary);
		if(input){
			std::string token;
			token.assign(
				std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
			if(token.size() <= 128){
				token.erase(
					std::remove_if(token.begin(), token.end(), [](unsigned char character){
						return std::isspace(character) != 0;
					}),
					token.end());
				return token;
			}
		}
	}
	return configuration.telemetry.pairingTokenHex;
}

bool ComputeCurrentModuleSha256(
	std::array<std::uint8_t, Sha256Bytes>& digest){
#ifdef _WIN32
	static const int ModuleAnchor = 0;
	HMODULE module = nullptr;
	if(!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&ModuleAnchor),
		&module)){
		return false;
	}
	std::array<wchar_t, 32768> modulePath{};
	const DWORD pathCharacters = GetModuleFileNameW(
		module,
		modulePath.data(),
		static_cast<DWORD>(modulePath.size()));
	if(pathCharacters == 0 || pathCharacters >= modulePath.size()){
		return false;
	}
	std::ifstream input(
		std::filesystem::path(modulePath.data()),
		std::ios::binary);
	if(!input){
		return false;
	}

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::vector<std::uint8_t> hashObject;
	bool success = false;
	do{
		if(BCryptOpenAlgorithmProvider(
			&algorithm,
			BCRYPT_SHA256_ALGORITHM,
			nullptr,
			0) < 0){
			break;
		}
		DWORD objectBytes = 0;
		DWORD resultBytes = 0;
		if(BCryptGetProperty(
			algorithm,
			BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectBytes),
			sizeof(objectBytes),
			&resultBytes,
			0) < 0){
			break;
		}
		hashObject.resize(objectBytes);
		if(BCryptCreateHash(
			algorithm,
			&hash,
			hashObject.data(),
			static_cast<ULONG>(hashObject.size()),
			nullptr,
			0,
			0) < 0){
			break;
		}
		std::array<char, 65536> buffer{};
		bool hashDataFailed = false;
		while(input){
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize bytesRead = input.gcount();
			if(bytesRead > 0 &&
				BCryptHashData(
					hash,
					reinterpret_cast<PUCHAR>(buffer.data()),
					static_cast<ULONG>(bytesRead),
					0) < 0){
				hashDataFailed = true;
				break;
			}
		}
		if(hashDataFailed || input.bad()){
			break;
		}
		success = BCryptFinishHash(
			hash,
			digest.data(),
			static_cast<ULONG>(digest.size()),
			0) >= 0;
	}while(false);
	if(hash){ BCryptDestroyHash(hash); }
	if(algorithm){ BCryptCloseAlgorithmProvider(algorithm, 0); }
	if(!success){ digest.fill(0); }
	return success;
#else
	(void)digest;
	return false;
#endif
}

bool HasExtensionToken(const std::string& extensions, const char* expected){
	std::string normalized = extensions;
	std::replace(normalized.begin(), normalized.end(), ',', ' ');
	std::replace(normalized.begin(), normalized.end(), ';', ' ');
	std::istringstream tokens(normalized);
	std::string token;
	while(tokens >> token){
		if(token == expected){
			return true;
		}
	}
	return false;
}

bool CapabilityReady(
	const CapabilitySnapshot& capabilities,
	std::uint64_t feature,
	std::uint32_t permission,
	const char* extension){
	return (capabilities.featureBits & feature) != 0 &&
		(capabilities.permissionFlags & permission) != 0 &&
		HasExtensionToken(capabilities.enabledExtensions, extension);
}

} // namespace

GalaxyXRSystem& GalaxyXRSystem::Instance(){
	static GalaxyXRSystem instance;
	return instance;
}

GalaxyXRSystem::GalaxyXRSystem()
	: transport(profile, clockSync, diagnostics) {}

void GalaxyXRSystem::RunFrame(const Config::GalaxyXRConfig& configuration){
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		currentConfiguration = configuration;
		configured = true;
	}
	diagnostics.SetLogRate(configuration.diagnostics.logRateHz);
	if(!configuration.enable){
		if(transport.IsRunning() ||
			previousAuthenticated ||
			hmdBoundToSession.load(std::memory_order_acquire)){
			StopFeatureState();
		}
		return;
	}
	if(configuration.identityMode != "negotiated"){
		if(activeTransportSignature != "<unsupported-identity-mode>"){
			StopFeatureState();
			activeTransportSignature = "<unsupported-identity-mode>";
			DriverLog(
				"GXR Session: disabled because identityMode=%s is unsupported; only negotiated is fail-closed",
				configuration.identityMode.c_str());
		}
		return;
	}

	DisplayConfiguration displayConfiguration;
	displayConfiguration.geometryMode = configuration.display.geometryMode;
	displayConfiguration.fallbackWidth = configuration.display.fallbackWidth;
	displayConfiguration.fallbackHeight = configuration.display.fallbackHeight;
	displayConfiguration.timingMode = configuration.timing.mode;
	displayConfiguration.staticPhotonLatencySeconds =
		configuration.timing.staticPhotonLatencyMs / 1000.0;
	displayConfiguration.maximumTelemetrySlewSeconds =
		configuration.timing.maximumSlewMs / 1000.0;
	display.Configure(displayConfiguration);

	EyePublisherConfiguration eyeConfiguration;
	eyeConfiguration.source = configuration.eye.source;
	eyeConfiguration.syntheticPattern = configuration.eye.syntheticPattern;
	eyeConfiguration.fallbackDistanceMeters = configuration.eye.fallbackDistanceMeters;
	eyeConfiguration.staleAfterMs = configuration.eye.staleAfterMs;
	if(eyePublisher.Configure(eyeConfiguration)){
		hmdBoundToSession.store(false, std::memory_order_release);
	}

	PoseTimingConfiguration poseConfiguration;
	poseConfiguration.mode = configuration.prediction.mode;
	poseConfiguration.clientPoseAlreadyPredicted =
		configuration.prediction.clientPoseAlreadyPredicted;
	poseConfiguration.suppressVelocityWhenHostPredicted =
		configuration.prediction.suppressVelocityWhenHostPredicted;
	poseConfiguration.fixedPredictionSeconds =
		configuration.prediction.fixedPoseMs / 1000.0;
	poseConfiguration.maximumHmdPredictionSeconds =
		configuration.prediction.maximumHmdMs / 1000.0;
	poseConfiguration.maximumControllerPredictionSeconds =
		configuration.prediction.maximumControllerMs / 1000.0;
	poseTiming.Configure(poseConfiguration);
	std::ostringstream posePolicy;
	posePolicy << configuration.prediction.mode << '|'
		<< configuration.prediction.clientPoseAlreadyPredicted << '|'
		<< configuration.prediction.suppressVelocityWhenHostPredicted << '|'
		<< configuration.prediction.fixedPoseMs << '|'
		<< configuration.prediction.maximumHmdMs << '|'
		<< configuration.prediction.maximumControllerMs;
	if(activePosePolicySignature != posePolicy.str()){
		activePosePolicySignature = posePolicy.str();
		const bool hostPredictionArmed =
			(configuration.prediction.mode == "fixed_pose" ||
			 configuration.prediction.mode == "adaptive_pose") &&
			!configuration.prediction.clientPoseAlreadyPredicted;
		DriverLog(
			"GXR Pose: mode=%s clientPredicted=%d hostPredictionArmed=%d velocitySuppressionWhenApplied=%d fixedMs=%.3f maxHmdMs=%.3f maxControllerMs=%.3f",
			configuration.prediction.mode.c_str(),
			configuration.prediction.clientPoseAlreadyPredicted ? 1 : 0,
			hostPredictionArmed ? 1 : 0,
			configuration.prediction.suppressVelocityWhenHostPredicted ? 1 : 0,
			configuration.prediction.fixedPoseMs,
			configuration.prediction.maximumHmdMs,
			configuration.prediction.maximumControllerMs);
	}

	const std::string requestedSignature = TransportSignature(configuration);
	if(transport.IsRunning() && activeTransportSignature != requestedSignature){
		transport.Stop();
		activeTransportSignature.clear();
	}
	if(configuration.telemetry.enable &&
		(!configuration.telemetry.requirePairing ||
		 configuration.telemetry.controlPort < 1 ||
		 configuration.telemetry.controlPort > std::numeric_limits<std::uint16_t>::max() ||
		 configuration.telemetry.trackingPort < 1 ||
		 configuration.telemetry.trackingPort > std::numeric_limits<std::uint16_t>::max() ||
		 configuration.telemetry.controlPort == configuration.telemetry.trackingPort ||
		 configuration.eye.staleAfterMs < 10 ||
		 configuration.eye.staleAfterMs > 1000)){
		if(activeTransportSignature != "<invalid-transport-config>"){
			StopFeatureState();
			activeTransportSignature = "<invalid-transport-config>";
			DriverLog(
				"GXR Transport: disabled; pairing is mandatory, distinct TCP/UDP ports must be in [1,65535], and staleAfterMs must be in [10,1000]");
		}
		return;
	}
	if(configuration.telemetry.enable && !transport.IsRunning()){
		std::array<std::uint8_t, Sha256Bytes> pairingKey{};
		std::array<std::uint8_t, Sha256Bytes> supportedApkSha256{};
		std::array<std::uint8_t, Sha256Bytes> supportedBridgeSha256{};
		const std::string pairingToken = LoadPairingToken(configuration);
		const bool validAdmission =
			configuration.telemetry.supportedClientVersionCode > 0 &&
			ParseHexKey32(
				configuration.telemetry.supportedApkSha256,
				supportedApkSha256) &&
			ParseHexKey32(
				configuration.telemetry.supportedBridgeSha256,
				supportedBridgeSha256) &&
			!std::all_of(
				supportedApkSha256.begin(),
				supportedApkSha256.end(),
				[](std::uint8_t value){ return value == 0; }) &&
			!std::all_of(
				supportedBridgeSha256.begin(),
				supportedBridgeSha256.end(),
				[](std::uint8_t value){ return value == 0; });
		if(!validAdmission){
			if(activeTransportSignature != "<invalid-client-provenance>"){
				DriverLog(
					"GXR Transport: disabled until supportedClientVersionCode and exact nonzero supportedApkSha256/supportedBridgeSha256 values are configured");
				activeTransportSignature = "<invalid-client-provenance>";
			}
		}else if(!ParseHexKey32(pairingToken, pairingKey)){
			if(activeTransportSignature != "<invalid-pairing>"){
				DriverLog(
					"GXR Transport: disabled until pairingTokenFile or pairingTokenHex provides exactly 64 hexadecimal characters");
				activeTransportSignature = "<invalid-pairing>";
			}
		}else{
			TransportConfiguration transportConfiguration;
			transportConfiguration.listenAddress = configuration.telemetry.listenAddress;
			transportConfiguration.controlPort = configuration.telemetry.controlPort;
			transportConfiguration.trackingPort = configuration.telemetry.trackingPort;
			transportConfiguration.pairingKey = pairingKey;
			transportConfiguration.staleAfterMs = configuration.eye.staleAfterMs;
			transportConfiguration.hostVersion = driverVersion;
			transportConfiguration.supportedClientVersionCode =
				static_cast<std::uint32_t>(
					configuration.telemetry.supportedClientVersionCode);
			transportConfiguration.supportedApkSha256 = supportedApkSha256;
			transportConfiguration.supportedBridgeSha256 = supportedBridgeSha256;
			if(!ComputeCurrentModuleSha256(transportConfiguration.hostDllSha256)){
				if(activeTransportSignature != "<host-hash-unavailable>"){
					DriverLog(
						"GXR Transport: disabled because the loaded host driver SHA-256 could not be established");
					activeTransportSignature = "<host-hash-unavailable>";
				}
			}else if(transport.Start(transportConfiguration)){
				activeTransportSignature = requestedSignature;
			}
		}
	}

	const bool authenticated = transport.HasAuthenticatedSession();
	display.SetAuthenticated(authenticated);
	if(authenticated != previousAuthenticated){
		if(!authenticated){
			eyePublisher.InvalidateSession();
			faceOutput.Invalidate();
			faceOutputEnabled = false;
			faceOutputValid = false;
			display.Reset();
			hmdBoundToSession.store(false, std::memory_order_release);
			lastPublishedFaceSequence = 0;
		}
		previousAuthenticated = authenticated;
	}
	if(!authenticated){
		return;
	}
	if(configuration.face.enableLosslessOutput && !faceOutputEnabled){
		faceOutputEnabled = faceOutput.Start();
	}else if(!configuration.face.enableLosslessOutput && faceOutputEnabled){
		faceOutput.Invalidate();
		faceOutputEnabled = false;
		faceOutputValid = false;
	}

	CapabilitySnapshot capabilities;
	const bool haveCapabilities = profile.GetCapabilities(capabilities);
	if(haveCapabilities){
		display.UpdateCapabilities(capabilities);
	}
	const bool eyeCapabilityReady = haveCapabilities && CapabilityReady(
		capabilities,
		CapabilityFeatureEyeTracking,
		CapabilityPermissionEyeTracking,
		"XR_ANDROID_eye_tracking");
	const bool faceCapabilityReady = haveCapabilities &&
		(capabilities.featureBits & CapabilityFeatureFaceTracking) != 0 &&
		HasExtensionToken(
			capabilities.enabledExtensions,
			"XR_ANDROID_face_tracking");
	const bool faceDataSourceCapabilityReady = haveCapabilities &&
		HasExtensionToken(
			capabilities.enabledExtensions,
			"XR_ANDROID_face_tracking_data_source");
	if(faceOutputEnabled && !faceCapabilityReady && faceOutputValid){
		faceOutput.Invalidate();
		faceOutputValid = false;
	}
	PresentationSample presentation;
	if(profile.GetPresentation(presentation)){
		display.UpdatePresentation(presentation);
	}

	std::uint32_t currentHmdDeviceId = vr::k_unTrackedDeviceIndexInvalid;
	vr::PropertyContainerHandle_t currentHmdContainer =
		vr::k_ulInvalidPropertyContainer;
	std::string currentHmdAddedSerial;
	std::string currentHmdActiveSerial;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		currentHmdDeviceId = hmdDeviceId;
		currentHmdContainer = hmdContainer;
		currentHmdAddedSerial = hmdAddedSerial;
		currentHmdActiveSerial = hmdActiveSerial;
	}
	if(eyeCapabilityReady &&
		!hmdBoundToSession.load(std::memory_order_acquire) &&
		currentHmdContainer != vr::k_ulInvalidPropertyContainer &&
		HmdMatchesAuthenticatedSession(
			currentHmdAddedSerial,
			currentHmdActiveSerial)){
		hmdBoundToSession.store(
			eyePublisher.Bind(currentHmdContainer, currentHmdDeviceId),
			std::memory_order_release);
	}

	TrackingSample tracking;
	const TrackingSample* trackingPointer = nullptr;
	const std::int64_t nowNs = NowNs();
	if(profile.GetTracking(tracking)){
		trackingPointer = eyeCapabilityReady ? &tracking : nullptr;
		const bool visualFacePermission =
			(capabilities.permissionFlags & CapabilityPermissionFaceTracking) != 0;
		const bool audioFacePermission =
			(capabilities.permissionFlags & CapabilityPermissionRecordAudio) != 0;
		const bool faceSourcePermitted =
			(tracking.face.source == FaceSource::Visual && visualFacePermission) ||
			(tracking.face.source == FaceSource::Audio &&
			 faceDataSourceCapabilityReady && audioFacePermission) ||
			(tracking.face.source == FaceSource::Multimodal &&
			 faceDataSourceCapabilityReady && visualFacePermission &&
			 audioFacePermission);
		bool faceFresh =
			faceCapabilityReady &&
			faceSourcePermitted &&
			tracking.face.valid &&
			tracking.face.state == FaceState::Tracking &&
			tracking.face.source != FaceSource::Unspecified &&
			tracking.hostReceiveMonotonicNs > 0 &&
			nowNs - tracking.hostReceiveMonotonicNs >= -10000000LL &&
			nowNs - tracking.hostReceiveMonotonicNs <=
				static_cast<std::int64_t>(configuration.eye.staleAfterMs) * 1000000LL;
		std::int64_t faceHostNs = 0;
		if(faceFresh){
			faceFresh =
				tracking.face.sampleMonotonicNs > 0 &&
				clockSync.ClientToHost(
					tracking.face.sampleMonotonicNs,
					faceHostNs);
			if(faceFresh){
				const std::int64_t faceAgeNs = nowNs - faceHostNs;
				faceFresh = faceAgeNs >= -10000000LL &&
					faceAgeNs <=
						static_cast<std::int64_t>(configuration.eye.staleAfterMs) * 1000000LL;
			}
		}
		if(faceOutputEnabled &&
			faceFresh &&
			tracking.sampleSequence != lastPublishedFaceSequence){
			if(faceFresh){
				faceOutput.Publish(profile.GetSession().sessionId, tracking);
			}else{
				faceOutput.Invalidate();
			}
			lastPublishedFaceSequence = tracking.sampleSequence;
			faceOutputValid = faceFresh;
		}else if(faceOutputEnabled && !faceFresh && faceOutputValid){
			faceOutput.Invalidate();
			faceOutputValid = false;
		}
	}
	eyePublisher.RunFrame(trackingPointer, clockSync, nowNs);
}

void GalaxyXRSystem::Cleanup(){
	transport.Stop();
	eyePublisher.Unbind();
	faceOutput.Stop();
	display.Reset();
	registry.Clear();
	profile.EndSession();
	clockSync.Reset();
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		configured = false;
		hmdDeviceId = vr::k_unTrackedDeviceIndexInvalid;
		hmdContainer = vr::k_ulInvalidPropertyContainer;
		hmdAddedSerial.clear();
		hmdActiveSerial.clear();
	}
	previousAuthenticated = false;
	activeTransportSignature.clear();
	activePosePolicySignature.clear();
	lastPublishedFaceSequence = 0;
	faceOutputEnabled = false;
	faceOutputValid = false;
	hmdBoundToSession.store(false, std::memory_order_release);
}

void GalaxyXRSystem::ObserveDeviceAdded(
	const std::string& serial,
	vr::ETrackedDeviceClass deviceClass){
	registry.ObserveAdded(serial, deviceClass);
}

void GalaxyXRSystem::ActivateDevice(
	std::uint32_t openVrId,
	const std::string& serial,
	vr::ETrackedDeviceClass deviceClass){
	registry.Activate(openVrId, serial, deviceClass);
}

void GalaxyXRSystem::DeactivateDevice(std::uint32_t openVrId){
	registry.Deactivate(openVrId);
	UnregisterHmd(openVrId);
}

void GalaxyXRSystem::RegisterHmd(
	std::uint32_t openVrId,
	vr::PropertyContainerHandle_t container,
	const std::string& addedSerial,
	const std::string& activeSerial){
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		hmdDeviceId = openVrId;
		hmdContainer = container;
		hmdAddedSerial = addedSerial;
		hmdActiveSerial = activeSerial;
	}
	hmdBoundToSession.store(false, std::memory_order_release);
	registry.Activate(openVrId, activeSerial, vr::TrackedDeviceClass_HMD);
}

void GalaxyXRSystem::UnregisterHmd(std::uint32_t openVrId){
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if(openVrId != hmdDeviceId){
			return;
		}
		hmdDeviceId = vr::k_unTrackedDeviceIndexInvalid;
		hmdContainer = vr::k_ulInvalidPropertyContainer;
		hmdAddedSerial.clear();
		hmdActiveSerial.clear();
	}
	eyePublisher.Unbind();
	hmdBoundToSession.store(false, std::memory_order_release);
}

bool GalaxyXRSystem::HmdMatchesAuthenticatedSession(
	const std::string& addedSerial,
	const std::string& activeSerial) const{
	Config::GalaxyXRConfig configuration;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if(!configured){
			return false;
		}
		configuration = currentConfiguration;
	}
	if(!configuration.enable){
		return false;
	}
	if(configuration.vrlinkCompatibilityMode == "vrlink_compat" &&
		!IsVRLinkBuildVerified()){
		return false;
	}
	return profile.MatchesActivatedHmd(
		configuration.serialMatch,
		addedSerial,
		activeSerial,
		configuration.forceEnable);
}

bool GalaxyXRSystem::ApplyPose(
	std::uint32_t openVrId,
	vr::DriverPose_t& pose){
	if(!HasAuthenticatedSession() ||
		!hmdBoundToSession.load(std::memory_order_acquire)){
		return false;
	}
	Config::GalaxyXRConfig configuration;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		configuration = currentConfiguration;
	}
	PresentationSample presentation;
	const PresentationSample* presentationPointer =
		profile.GetPresentation(presentation) ? &presentation : nullptr;
	double horizon = 0.0;
	const bool applied = poseTiming.Apply(
		openVrId,
		pose,
		registry,
		clockSync,
		presentationPointer,
		NowNs(),
		horizon);
	if(applied){
		diagnostics.LogPose(
			openVrId,
			static_cast<unsigned>(registry.RoleFor(openVrId)),
			horizon,
			pose.poseTimeOffset,
			configuration.prediction.suppressVelocityWhenHostPredicted);
	}
	return applied;
}

bool GalaxyXRSystem::ShouldEnableVRLinkCompatibility() const{
	Config::GalaxyXRConfig configuration;
	std::string addedSerial;
	std::string activeSerial;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		configuration = currentConfiguration;
		addedSerial = hmdAddedSerial;
		activeSerial = hmdActiveSerial;
	}
	if(!HasAuthenticatedSession() ||
		eyePublisher.Owner() != "vrlink_compat" ||
		!IsVRLinkBuildVerified()){
		return false;
	}
	return configuration.vrlinkCompatibilityMode == "vrlink_compat" &&
		profile.MatchesActivatedHmd(
			configuration.serialMatch,
			addedSerial,
			activeSerial,
			configuration.forceEnable);
}

bool GalaxyXRSystem::ShouldAttemptVRLinkCompatibility() const{
	Config::GalaxyXRConfig configuration;
	std::string addedSerial;
	std::string activeSerial;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		configuration = currentConfiguration;
		addedSerial = hmdAddedSerial;
		activeSerial = hmdActiveSerial;
	}
	return HasAuthenticatedSession() &&
		configuration.enable &&
		configuration.vrlinkCompatibilityMode == "vrlink_compat" &&
		profile.MatchesActivatedHmd(
			configuration.serialMatch,
			addedSerial,
			activeSerial,
			configuration.forceEnable);
}

void GalaxyXRSystem::SetVRLinkBuildVerified(bool verified){
	vrLinkBuildVerified.store(verified, std::memory_order_release);
}

bool GalaxyXRSystem::IsVRLinkBuildVerified() const{
	return vrLinkBuildVerified.load(std::memory_order_acquire);
}

GalaxyXRDisplay& GalaxyXRSystem::Display(){ return display; }
GalaxyXRClockSync& GalaxyXRSystem::Clock(){ return clockSync; }
GalaxyXRDiagnostics& GalaxyXRSystem::Diagnostics(){ return diagnostics; }
const GalaxyXRProfile& GalaxyXRSystem::Profile() const{ return profile; }

bool GalaxyXRSystem::HasAuthenticatedSession() const{
	return transport.HasAuthenticatedSession();
}

std::int64_t GalaxyXRSystem::NowNs(){
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void GalaxyXRSystem::StopFeatureState(){
	transport.Stop();
	activeTransportSignature.clear();
	activePosePolicySignature.clear();
	eyePublisher.InvalidateSession();
	faceOutput.Stop();
	display.Reset();
	clockSync.Reset();
	profile.EndSession();
	previousAuthenticated = false;
	hmdBoundToSession.store(false, std::memory_order_release);
	lastPublishedFaceSequence = 0;
	faceOutputEnabled = false;
	faceOutputValid = false;
}

std::string GalaxyXRSystem::TransportSignature(
	const Config::GalaxyXRConfig& configuration) const{
	std::ostringstream value;
	value << configuration.telemetry.enable << '|'
		<< configuration.telemetry.listenAddress << '|'
		<< configuration.telemetry.controlPort << '|'
		<< configuration.telemetry.trackingPort << '|'
		<< configuration.telemetry.pairingTokenFile << '|'
		<< configuration.telemetry.pairingTokenHex << '|'
		<< configuration.telemetry.supportedClientVersionCode << '|'
		<< configuration.telemetry.supportedApkSha256 << '|'
		<< configuration.telemetry.supportedBridgeSha256;
	return value.str();
}

} // namespace galaxyxr
