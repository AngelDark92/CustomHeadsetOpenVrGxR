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
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace galaxyxr {
namespace {

constexpr std::int64_t TransportRetryIntervalNs = 5'000'000'000LL;
constexpr std::int64_t StatusFlushIntervalNs = 1'000'000'000LL;

enum class ListenAddressStatus {
	Local,
	Invalid,
	NonLocal,
	Unavailable
};

ListenAddressStatus ValidateLocalListenAddress(const std::string& address){
#ifdef _WIN32
	IN_ADDR configuredAddress{};
	if(InetPtonA(AF_INET, address.c_str(), &configuredAddress) != 1 ||
		configuredAddress.S_un.S_addr == INADDR_ANY ||
		configuredAddress.S_un.S_addr == INADDR_NONE){
		return ListenAddressStatus::Invalid;
	}
	if((ntohl(configuredAddress.S_un.S_addr) & 0xff000000U) == 0x7f000000U){
		return ListenAddressStatus::Local;
	}

	ULONG bufferBytes = 15 * 1024;
	std::vector<std::uint8_t> buffer(bufferBytes);
	ULONG result = GetAdaptersAddresses(
		AF_INET,
		GAA_FLAG_SKIP_ANYCAST |
			GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER,
		nullptr,
		reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
		&bufferBytes);
	if(result == ERROR_BUFFER_OVERFLOW && bufferBytes <= 1024 * 1024){
		buffer.resize(bufferBytes);
		result = GetAdaptersAddresses(
			AF_INET,
			GAA_FLAG_SKIP_ANYCAST |
				GAA_FLAG_SKIP_MULTICAST |
				GAA_FLAG_SKIP_DNS_SERVER,
			nullptr,
			reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
			&bufferBytes);
	}
	if(result != NO_ERROR && result != ERROR_NO_DATA){
		return ListenAddressStatus::Unavailable;
	}
	if(result == ERROR_NO_DATA){
		return ListenAddressStatus::NonLocal;
	}

	for(auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
		adapter;
		adapter = adapter->Next){
		for(auto* unicast = adapter->FirstUnicastAddress;
			unicast;
			unicast = unicast->Next){
			if(!unicast->Address.lpSockaddr ||
				unicast->Address.lpSockaddr->sa_family != AF_INET){
				continue;
			}
			const auto* localAddress = reinterpret_cast<const SOCKADDR_IN*>(
				unicast->Address.lpSockaddr);
			if(localAddress->sin_addr.S_un.S_addr == configuredAddress.S_un.S_addr){
				return ListenAddressStatus::Local;
			}
		}
	}
	return ListenAddressStatus::NonLocal;
#else
	(void)address;
	return ListenAddressStatus::Unavailable;
#endif
}

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

const char* TransportErrorCode(const std::string& signature){
	if(signature == "<invalid-client-provenance>"){ return "client_provenance_invalid"; }
	if(signature == "<invalid-pairing>"){ return "pairing_invalid"; }
	if(signature == "<invalid-listen-address>"){ return "listen_address_invalid"; }
	if(signature == "<non-local-listen-address>"){ return "listen_address_not_local"; }
	if(signature == "<host-hash-unavailable>"){ return "host_hash_unavailable"; }
	if(signature == "<invalid-transport-config>"){ return "transport_config_invalid"; }
	return "";
}

} // namespace

GalaxyXRSystem& GalaxyXRSystem::Instance(){
	static GalaxyXRSystem instance;
	return instance;
}

GalaxyXRSystem::GalaxyXRSystem()
	: transport(profile, clockSync, diagnostics) {}

void GalaxyXRSystem::ConfigureStatus(){
	if(statusConfigured){
		return;
	}
	status.ConfigureFile(
		(std::filesystem::path(driverConfigLoader.GetConfigFolder()) /
			"galaxyxr-status.json").string());
	statusConfigured = true;
}

void GalaxyXRSystem::FlushStatus(
	const char* stage,
	const char* state,
	const char* safeError,
	std::int64_t nowNs,
	bool force){
	status.Transition(stage, state, safeError ? safeError : "");
	if(force || nowNs >= nextStatusFlushNs){
		status.Flush();
		nextStatusFlushNs = nowNs + StatusFlushIntervalNs;
	}
}

void GalaxyXRSystem::RunFrame(const Config::GalaxyXRConfig& configuration){
	ConfigureStatus();
	const std::int64_t statusNowNs = NowNs();
	status.SetState("configuration", configuration.enable ? "enabled" : "disabled");
	status.SetState(
		"identity",
		configuration.identityMode == "negotiated" ? "negotiated" : "unsupported");
	status.SetState(
		"vrlink_compatibility",
		configuration.vrlinkCompatibilityMode == "off" ? "off" : "requested");
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
		status.SetState("transport", "stopped");
		status.SetState("session", "inactive");
		status.SetState("capabilities", "unavailable");
		status.SetState("hmd", "unbound");
		status.SetState("eye", "inactive");
		status.SetState("face", "inactive");
		FlushStatus("configuration", "disabled", "", statusNowNs);
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
		status.SetState("transport", "stopped");
		status.SetState("session", "inactive");
		status.SetState("capabilities", "unavailable");
		status.SetState("hmd", "unbound");
		status.SetState("eye", "inactive");
		status.SetState("face", "inactive");
		FlushStatus(
			"configuration",
			"blocked",
			"identity_mode_unsupported",
			statusNowNs);
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
	const std::int64_t transportNowNs = NowNs();
	const bool transportSignatureChanged =
		lastTransportAttemptSignature != requestedSignature;
	if(transportSignatureChanged){
		lastTransportAttemptSignature = requestedSignature;
		nextTransportAttemptNs = 0;
		if(!transport.IsRunning()){
			activeTransportSignature.clear();
		}
	}
	if(transport.IsRunning() && activeTransportSignature != requestedSignature){
		transport.Stop();
		activeTransportSignature.clear();
	}
	if(!configuration.telemetry.enable){
		StopFeatureState();
		activeTransportSignature = "<disabled>";
		nextTransportAttemptNs = 0;
		status.SetState("transport", "disabled");
		status.SetState("session", "inactive");
		status.SetState("capabilities", "unavailable");
		status.SetState("hmd", "unbound");
		status.SetState("eye", "inactive");
		status.SetState("face", "inactive");
		FlushStatus("transport", "disabled", "telemetry_disabled", statusNowNs);
		return;
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
		nextTransportAttemptNs = transportNowNs + TransportRetryIntervalNs;
		status.SetState("transport", "blocked");
		status.SetState("session", "inactive");
		status.SetState("capabilities", "unavailable");
		status.SetState("hmd", "unbound");
		status.SetState("eye", "inactive");
		status.SetState("face", "inactive");
		FlushStatus(
			"transport",
			"blocked",
			"transport_config_invalid",
			statusNowNs);
		return;
	}
	if(configuration.telemetry.enable &&
		!transport.IsRunning() &&
		(transportSignatureChanged || transportNowNs >= nextTransportAttemptNs)){
		// Arm the retry before validation/start so every failure path is bounded.
		nextTransportAttemptNs = transportNowNs + TransportRetryIntervalNs;
		std::array<std::uint8_t, Sha256Bytes> pairingKey{};
		std::vector<TransportConfiguration::ClientAdmission> allowedClients;
		for(const auto& configured : configuration.telemetry.allowedClients){
			TransportConfiguration::ClientAdmission allowed;
			if(configured.versionCode <= 0 ||
				!ParseHexKey32(configured.apkSha256, allowed.apkSha256) ||
				!ParseHexKey32(configured.bridgeSha256, allowed.bridgeSha256) ||
				std::all_of(allowed.apkSha256.begin(), allowed.apkSha256.end(), [](std::uint8_t value){ return value == 0; }) ||
				std::all_of(allowed.bridgeSha256.begin(), allowed.bridgeSha256.end(), [](std::uint8_t value){ return value == 0; })){
				continue;
			}
			allowed.versionCode = static_cast<std::uint32_t>(configured.versionCode);
			allowedClients.push_back(allowed);
		}
		const std::string pairingToken = LoadPairingToken(configuration);
		const bool validAdmission = !allowedClients.empty();
		if(!validAdmission){
			if(activeTransportSignature != "<invalid-client-provenance>"){
				DriverLog(
					"GXR Transport: disabled until allowedClients contains at least one exact versionCode/APK/bridge SHA-256 record");
				activeTransportSignature = "<invalid-client-provenance>";
			}
		}else if(!ParseHexKey32(pairingToken, pairingKey)){
			if(activeTransportSignature != "<invalid-pairing>"){
				DriverLog(
					"GXR Transport: disabled until pairingTokenFile or pairingTokenHex provides exactly 64 hexadecimal characters");
				activeTransportSignature = "<invalid-pairing>";
			}
		}else{
			const ListenAddressStatus listenAddressStatus =
				ValidateLocalListenAddress(configuration.telemetry.listenAddress);
			if(listenAddressStatus == ListenAddressStatus::Invalid){
				if(activeTransportSignature != "<invalid-listen-address>"){
					DriverLog(
						"GXR Transport: configured listenAddress '%s' is not a usable unicast IPv4 address; transport remains disabled",
						configuration.telemetry.listenAddress.c_str());
					activeTransportSignature = "<invalid-listen-address>";
				}
			}else if(listenAddressStatus == ListenAddressStatus::NonLocal){
				if(activeTransportSignature != "<non-local-listen-address>"){
					DriverLog(
						"GXR Transport: configured listenAddress '%s' is not assigned to a local IPv4 interface; transport remains disabled",
						configuration.telemetry.listenAddress.c_str());
					activeTransportSignature = "<non-local-listen-address>";
				}
			}else{
				TransportConfiguration transportConfiguration;
				transportConfiguration.listenAddress = configuration.telemetry.listenAddress;
				transportConfiguration.controlPort = configuration.telemetry.controlPort;
				transportConfiguration.trackingPort = configuration.telemetry.trackingPort;
				transportConfiguration.pairingKey = pairingKey;
				transportConfiguration.staleAfterMs = configuration.eye.staleAfterMs;
				transportConfiguration.hostVersion = driverVersion;
				transportConfiguration.allowedClients = std::move(allowedClients);
				if(!ComputeCurrentModuleSha256(transportConfiguration.hostDllSha256)){
					if(activeTransportSignature != "<host-hash-unavailable>"){
						DriverLog(
							"GXR Transport: disabled because the loaded host driver SHA-256 could not be established");
						activeTransportSignature = "<host-hash-unavailable>";
					}
				}else if(transport.Start(transportConfiguration)){
					activeTransportSignature = requestedSignature;
					nextTransportAttemptNs = 0;
				}
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
		status.SetState("transport", transport.IsRunning() ? "listening" : "stopped");
		status.SetState("session", "waiting");
		status.SetState("capabilities", "unavailable");
		status.SetState("hmd", "unbound");
		status.SetState("eye", "waiting_session");
		status.SetState("face", "waiting_session");
		const char* transportError = TransportErrorCode(activeTransportSignature);
		FlushStatus(
			"session",
			transport.IsRunning() ? "waiting" : "blocked",
			transportError,
			statusNowNs);
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
	const bool haveTracking = profile.GetTracking(tracking);
	if(haveTracking){
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
	const bool hmdBound = hmdBoundToSession.load(std::memory_order_acquire);
	const bool eyeOutputValid = eyePublisher.IsOutputValid();
	status.SetState("transport", "listening");
	status.SetState("session", "authenticated");
	status.SetState("capabilities", haveCapabilities ? "received" : "missing");
	status.SetState("hmd", hmdBound ? "bound" : "waiting");
	status.SetState(
		"eye",
		eyeOutputValid
			? "streaming"
			: (eyeCapabilityReady ? "waiting_sample" : "capability_missing"));
	status.SetState(
		"face",
		faceOutputValid
			? "streaming"
			: (!configuration.face.enableLosslessOutput
				? "disabled"
				: (faceCapabilityReady ? "waiting_sample" : "capability_missing")));
	if(haveTracking){
		status.SetCounter("tracking_sequence", tracking.sampleSequence);
	}
	if(lastPublishedFaceSequence > 0){
		status.SetCounter("face_sequence", lastPublishedFaceSequence);
	}else{
		status.RemoveCounter("face_sequence");
	}
	const bool runtimeReady =
		haveCapabilities && hmdBound && eyeOutputValid &&
		(!configuration.face.enableLosslessOutput || faceOutputValid);
	FlushStatus(
		"runtime",
		runtimeReady ? "ready" : "degraded",
		runtimeReady ? "" : "runtime_signal_incomplete",
		nowNs);
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
	lastTransportAttemptSignature.clear();
	nextTransportAttemptNs = 0;
	activePosePolicySignature.clear();
	lastPublishedFaceSequence = 0;
	faceOutputEnabled = false;
	faceOutputValid = false;
	hmdBoundToSession.store(false, std::memory_order_release);
	if(statusConfigured){
		status.SetState("transport", "stopped");
		status.SetState("session", "inactive");
		status.SetState("capabilities", "unavailable");
		status.SetState("hmd", "unbound");
		status.SetState("eye", "inactive");
		status.SetState("face", "inactive");
		FlushStatus("driver", "stopped", "", NowNs(), true);
	}
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
		<< configuration.telemetry.requirePairing << '|'
		<< configuration.telemetry.pairingTokenFile << '|'
		<< configuration.telemetry.pairingTokenHex << '|'
		<< configuration.eye.staleAfterMs;
	for(const auto& client : configuration.telemetry.allowedClients){
		value << '|' << client.versionCode << ':' << client.apkSha256 << ':' << client.bridgeSha256;
	}
	return value.str();
}

} // namespace galaxyxr
