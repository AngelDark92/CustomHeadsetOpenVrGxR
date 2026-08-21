#pragma once

#include "GalaxyXRClockSync.h"
#include "GalaxyXRDiagnostics.h"
#include "GalaxyXRProfile.h"
#include "GalaxyXRProtocol.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace galaxyxr {

struct TransportConfiguration {
	struct ClientAdmission {
		std::uint32_t versionCode = 0;
		std::array<std::uint8_t, Sha256Bytes> apkSha256{};
		std::array<std::uint8_t, Sha256Bytes> bridgeSha256{};
	};
	std::string listenAddress = "127.0.0.1";
	std::uint16_t controlPort = 29981;
	std::uint16_t trackingPort = 29982;
	std::array<std::uint8_t, Sha256Bytes> pairingKey{};
	std::uint32_t staleAfterMs = 100;
	std::string hostVersion;
	std::array<std::uint8_t, Sha256Bytes> hostDllSha256{};
	std::vector<ClientAdmission> allowedClients;
};

class GalaxyXRTransport {
public:
	GalaxyXRTransport(
		GalaxyXRProfile& profile,
		GalaxyXRClockSync& clockSync,
		GalaxyXRDiagnostics& diagnostics);
	~GalaxyXRTransport();

	bool Start(const TransportConfiguration& configuration);
	void Stop();
	bool IsRunning() const;
	bool HasAuthenticatedSession() const;
	RemoteDiagnosticCounters GetRemoteCounters() const;

private:
	void Worker();
	bool SetupSockets();
	void CloseSockets();
	void AcceptControl();
	void ReadControl();
	void FlushControlWrites();
	void ReadTracking();
	void ProcessControlFrame(const std::uint8_t* packet, std::size_t packetBytes);
	void ProcessTrackingDatagram(
		const std::uint8_t* packet,
		std::size_t packetBytes,
		std::uint32_t peerAddress);
	bool EstablishSession(const DecodedPacket& packet);
	bool SendControl(MessageType type, const std::vector<std::uint8_t>& payload);
	void SendClockRequest(std::int64_t nowNs);
	void EndSession(const char* reason);
	static std::int64_t NowNs();

	GalaxyXRProfile& profile;
	GalaxyXRClockSync& clockSync;
	GalaxyXRDiagnostics& diagnostics;
	TransportConfiguration configuration;
	std::atomic<bool> running{false};
	std::thread worker;

	std::uintptr_t listenSocket = static_cast<std::uintptr_t>(~0ULL);
	std::uintptr_t controlSocket = static_cast<std::uintptr_t>(~0ULL);
	std::uintptr_t trackingSocket = static_cast<std::uintptr_t>(~0ULL);
	bool winsockStarted = false;
	std::uint32_t controlPeerAddress = 0;
	std::vector<std::uint8_t> controlReadBuffer;
	std::vector<std::uint8_t> controlWriteBuffer;
	std::size_t controlWriteOffset = 0;

	std::array<std::uint8_t, SessionIdBytes> sessionId{};
	std::array<std::uint8_t, Sha256Bytes> sessionKey{};
	std::atomic<bool> sessionAuthenticated{false};
	std::atomic<bool> handshakePending{false};
	ClientIdentity pendingClient;
	std::int64_t handshakeStartedNs = 0;
	std::deque<std::array<std::uint8_t, NonceBytes>> seenClientNonces;
	std::uint64_t lastControlSequence = 0;
	std::uint64_t lastTrackingSequence = 0;
	std::uint64_t nextHostControlSequence = 1;
	std::int64_t lastClockRequestNs = 0;
	mutable std::mutex remoteCountersMutex;
	RemoteDiagnosticCounters remoteCounters;
};

} // namespace galaxyxr
