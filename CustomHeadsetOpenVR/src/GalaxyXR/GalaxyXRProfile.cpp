#include "GalaxyXRProfile.h"

#include <algorithm>
#include <cctype>

namespace galaxyxr {
namespace {

bool EqualsIgnoreCase(const std::string& left, const std::string& right){
	return left.size() == right.size() &&
		std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b){
			return std::tolower(a) == std::tolower(b);
		});
}

bool IsZeroSession(const std::array<std::uint8_t, SessionIdBytes>& sessionId){
	return std::all_of(sessionId.begin(), sessionId.end(), [](std::uint8_t value){ return value == 0; });
}

} // namespace

bool IsSafePublicSerial(const std::string& serial){
	return !serial.empty() &&
		serial.size() <= 128 &&
		!EqualsIgnoreCase(serial, "unknown") &&
		std::all_of(serial.begin(), serial.end(), [](unsigned char character){
			return std::isalnum(character) != 0 ||
				character == '.' || character == '_' || character == '-';
		});
}

std::string SelectPublicSerial(
	const std::string& clientSerial,
	const std::string& activeSerial,
	const std::string& fallbackSerial){
	if(IsSafePublicSerial(clientSerial)){
		return clientSerial;
	}
	if(IsSafePublicSerial(activeSerial)){
		return activeSerial;
	}
	return fallbackSerial;
}

bool GalaxyXRProfile::BeginSession(
	const std::array<std::uint8_t, SessionIdBytes>& sessionId,
	const ClientIdentity& client,
	std::int64_t establishedHostMonotonicNs){
	if(IsZeroSession(sessionId) ||
		establishedHostMonotonicNs <= 0 ||
		client.packageName != "com.valvesoftware.steamlinkvr" ||
		!EqualsIgnoreCase(client.manufacturer, "Samsung") ||
		!EqualsIgnoreCase(client.model, "Samsung Galaxy XR")){
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if(session.authenticated){
		return false;
	}
	session.authenticated = true;
	session.sessionId = sessionId;
	session.client = client;
	session.establishedHostMonotonicNs = establishedHostMonotonicNs;
	hasCapabilities = false;
	hasTracking = false;
	hasPresentation = false;
	return true;
}

void GalaxyXRProfile::EndSession(){
	std::lock_guard<std::mutex> lock(mutex);
	session = {};
	capabilities = {};
	tracking = {};
	presentation = {};
	hasCapabilities = false;
	hasTracking = false;
	hasPresentation = false;
}

bool GalaxyXRProfile::UpdateCapabilities(const CapabilitySnapshot& value){
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated ||
		value.revision == 0 ||
		value.views.empty() ||
		value.views.size() > 2 ||
		(hasCapabilities && value.revision <= capabilities.revision)){
		return false;
	}
	capabilities = value;
	hasCapabilities = true;
	return true;
}

bool GalaxyXRProfile::UpdateTracking(const TrackingSample& value){
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated ||
		!hasCapabilities ||
		value.capabilityRevision != capabilities.revision ||
		(hasTracking && value.sampleSequence <= tracking.sampleSequence)){
		return false;
	}
	tracking = value;
	hasTracking = true;
	return true;
}

void GalaxyXRProfile::UpdatePresentation(const PresentationSample& value){
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated){
		return;
	}
	presentation = value;
	hasPresentation = true;
}

SessionSnapshot GalaxyXRProfile::GetSession() const{
	std::lock_guard<std::mutex> lock(mutex);
	return session;
}

bool GalaxyXRProfile::GetCapabilities(CapabilitySnapshot& value) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated || !hasCapabilities){
		return false;
	}
	value = capabilities;
	return true;
}

bool GalaxyXRProfile::GetTracking(TrackingSample& value) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated || !hasTracking){
		return false;
	}
	value = tracking;
	return true;
}

bool GalaxyXRProfile::GetPresentation(PresentationSample& value) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated || !hasPresentation){
		return false;
	}
	value = presentation;
	return true;
}

bool GalaxyXRProfile::MatchesActivatedHmd(
	const std::string& configuredSerial,
	const std::string& addedSerial,
	const std::string& activeSerial,
	bool unsafeForceEnable) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!session.authenticated){
		return false;
	}
	if(!EqualsIgnoreCase(session.client.manufacturer, "Samsung") ||
		!EqualsIgnoreCase(session.client.model, "Samsung Galaxy XR")){
		return false;
	}
	if(unsafeForceEnable){
		return true;
	}
	if(configuredSerial.empty()){
		return false;
	}
	return EqualsIgnoreCase(configuredSerial, addedSerial) ||
		EqualsIgnoreCase(configuredSerial, activeSerial) ||
		EqualsIgnoreCase(configuredSerial, session.client.deviceSerial);
}

} // namespace galaxyxr
