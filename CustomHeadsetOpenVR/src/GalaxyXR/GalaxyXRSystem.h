#pragma once

#include "GalaxyXRClockSync.h"
#include "GalaxyXRDeviceRegistry.h"
#include "GalaxyXRDiagnostics.h"
#include "GalaxyXRDisplay.h"
#include "GalaxyXREyePublisher.h"
#include "GalaxyXRFaceOutput.h"
#include "GalaxyXRPoseTiming.h"
#include "GalaxyXRProfile.h"
#include "GalaxyXRStatus.h"
#include "GalaxyXRTransport.h"

#include "../Config/Config.h"

#include "openvr_driver.h"

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

namespace galaxyxr {

class GalaxyXRSystem {
public:
	static GalaxyXRSystem& Instance();

	void RunFrame(const Config::GalaxyXRConfig& configuration);
	void Cleanup();
	void ObserveDeviceAdded(const std::string& serial, vr::ETrackedDeviceClass deviceClass);
	void ActivateDevice(
		std::uint32_t openVrId,
		const std::string& serial,
		vr::ETrackedDeviceClass deviceClass);
	void DeactivateDevice(std::uint32_t openVrId);
	void RegisterHmd(
		std::uint32_t openVrId,
		vr::PropertyContainerHandle_t container,
		const std::string& addedSerial,
		const std::string& activeSerial);
	void UnregisterHmd(std::uint32_t openVrId);
	bool HmdMatchesAuthenticatedSession(
		const std::string& addedSerial,
		const std::string& activeSerial) const;
	bool ApplyPose(std::uint32_t openVrId, vr::DriverPose_t& pose);
	bool ShouldEnableVRLinkCompatibility() const;
	bool ShouldAttemptVRLinkCompatibility() const;
	void SetVRLinkBuildVerified(bool verified);
	bool IsVRLinkBuildVerified() const;

	GalaxyXRDisplay& Display();
	GalaxyXRClockSync& Clock();
	GalaxyXRDiagnostics& Diagnostics();
	const GalaxyXRProfile& Profile() const;
	bool HasAuthenticatedSession() const;

private:
	GalaxyXRSystem();
	static std::int64_t NowNs();
	void StopFeatureState();
	void ConfigureStatus();
	void FlushStatus(
		const char* stage,
		const char* state,
		const char* safeError,
		std::int64_t nowNs,
		bool force = false);
	std::string TransportSignature(const Config::GalaxyXRConfig& configuration) const;

	GalaxyXRProfile profile;
	GalaxyXRClockSync clockSync;
	GalaxyXRDiagnostics diagnostics;
	GalaxyXRDisplay display;
	GalaxyXREyePublisher eyePublisher;
	GalaxyXRFaceOutput faceOutput;
	GalaxyXRDeviceRegistry registry;
	GalaxyXRPoseTiming poseTiming;
	GalaxyXRTransport transport;
	GalaxyXRStatus status;

	Config::GalaxyXRConfig currentConfiguration;
	mutable std::mutex stateMutex;
	bool configured = false;
	bool previousAuthenticated = false;
	std::string activeTransportSignature;
	std::string lastTransportAttemptSignature;
	std::int64_t nextTransportAttemptNs = 0;
	std::int64_t nextStatusFlushNs = 0;
	bool statusConfigured = false;
	std::string activePosePolicySignature;
	std::uint64_t lastPublishedFaceSequence = 0;
	bool faceOutputEnabled = false;
	bool faceOutputValid = false;
	std::uint32_t hmdDeviceId = vr::k_unTrackedDeviceIndexInvalid;
	vr::PropertyContainerHandle_t hmdContainer = vr::k_ulInvalidPropertyContainer;
	std::string hmdAddedSerial;
	std::string hmdActiveSerial;
	std::atomic<bool> hmdBoundToSession{false};
	std::atomic<bool> vrLinkBuildVerified{false};
};

} // namespace galaxyxr
