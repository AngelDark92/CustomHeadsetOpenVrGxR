#pragma once

#include "GalaxyXRTypes.h"

#include <mutex>
#include <string>

namespace galaxyxr {

bool IsSafePublicSerial(const std::string& serial);
std::string SelectPublicSerial(
	const std::string& clientSerial,
	const std::string& activeSerial,
	const std::string& fallbackSerial);

class GalaxyXRProfile {
public:
	bool BeginSession(
		const std::array<std::uint8_t, SessionIdBytes>& sessionId,
		const ClientIdentity& client,
		std::int64_t establishedHostMonotonicNs);
	void EndSession();
	bool UpdateCapabilities(const CapabilitySnapshot& capabilities);
	bool UpdateTracking(const TrackingSample& tracking);
	void UpdatePresentation(const PresentationSample& presentation);

	SessionSnapshot GetSession() const;
	bool GetCapabilities(CapabilitySnapshot& capabilities) const;
	bool GetTracking(TrackingSample& tracking) const;
	bool GetPresentation(PresentationSample& presentation) const;
	bool MatchesActivatedHmd(
		const std::string& configuredSerial,
		const std::string& addedSerial,
		const std::string& activeSerial,
		bool unsafeForceEnable) const;

private:
	mutable std::mutex mutex;
	SessionSnapshot session;
	CapabilitySnapshot capabilities;
	TrackingSample tracking;
	PresentationSample presentation;
	bool hasCapabilities = false;
	bool hasTracking = false;
	bool hasPresentation = false;
};

} // namespace galaxyxr
