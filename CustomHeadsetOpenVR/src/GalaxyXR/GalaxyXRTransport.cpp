#include "GalaxyXRTransport.h"

#include "../Driver/DriverLog.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace galaxyxr {
namespace {

#ifdef _WIN32
constexpr SOCKET InvalidSocket = INVALID_SOCKET;

SOCKET ToSocket(std::uintptr_t value){
	return static_cast<SOCKET>(value);
}

std::uintptr_t FromSocket(SOCKET value){
	return static_cast<std::uintptr_t>(value);
}

bool SetNonBlocking(SOCKET socket){
	u_long enabled = 1;
	return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

bool WouldBlock(){
	const int error = WSAGetLastError();
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool RandomBytes(std::uint8_t* output, std::size_t count){
	return count <= std::numeric_limits<ULONG>::max() &&
		BCryptGenRandom(
			nullptr,
			output,
			static_cast<ULONG>(count),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}
#endif

bool IsZeroKey(const std::array<std::uint8_t, Sha256Bytes>& key){
	return std::all_of(key.begin(), key.end(), [](std::uint8_t value){ return value == 0; });
}

bool IsZeroSession(const std::array<std::uint8_t, SessionIdBytes>& value){
	return std::all_of(value.begin(), value.end(), [](std::uint8_t byte){ return byte == 0; });
}

std::uint32_t ReadU32(const std::uint8_t* value){
	return static_cast<std::uint32_t>(value[0]) |
		(static_cast<std::uint32_t>(value[1]) << 8) |
		(static_cast<std::uint32_t>(value[2]) << 16) |
		(static_cast<std::uint32_t>(value[3]) << 24);
}

void AppendU32(std::vector<std::uint8_t>& destination, std::uint32_t value){
	destination.push_back(static_cast<std::uint8_t>(value));
	destination.push_back(static_cast<std::uint8_t>(value >> 8));
	destination.push_back(static_cast<std::uint8_t>(value >> 16));
	destination.push_back(static_cast<std::uint8_t>(value >> 24));
}

} // namespace

GalaxyXRTransport::GalaxyXRTransport(
	GalaxyXRProfile& profile,
	GalaxyXRClockSync& clockSync,
	GalaxyXRDiagnostics& diagnostics)
	: profile(profile), clockSync(clockSync), diagnostics(diagnostics) {}

GalaxyXRTransport::~GalaxyXRTransport(){
	Stop();
}

bool GalaxyXRTransport::Start(const TransportConfiguration& value){
	if(running.load(std::memory_order_acquire)){
		return true;
	}
	if(value.controlPort == 0 ||
		value.trackingPort == 0 ||
		value.controlPort == value.trackingPort ||
		IsZeroKey(value.pairingKey) ||
		value.listenAddress.empty()){
		DriverLog(
			"GXR Transport: refused start; explicit listen address, distinct ports, and a nonzero 32-byte pairing key are required");
		return false;
	}
	configuration = value;
	seenClientNonces.clear();
	configuration.staleAfterMs =
		std::max<std::uint32_t>(10, std::min<std::uint32_t>(1000, configuration.staleAfterMs));
	if(!SetupSockets()){
		CloseSockets();
		return false;
	}
	running.store(true, std::memory_order_release);
	worker = std::thread(&GalaxyXRTransport::Worker, this);
	DriverLog(
		"GXR Transport: listening on explicitly configured %s TCP/%u UDP/%u; biometric payload logging is redacted/rate-limited",
		configuration.listenAddress.c_str(),
		configuration.controlPort,
		configuration.trackingPort);
	return true;
}

void GalaxyXRTransport::Stop(){
	if(!running.exchange(false, std::memory_order_acq_rel)){
		CloseSockets();
		return;
	}
#ifdef _WIN32
	if(ToSocket(listenSocket) != InvalidSocket){ shutdown(ToSocket(listenSocket), SD_BOTH); }
	if(ToSocket(controlSocket) != InvalidSocket){ shutdown(ToSocket(controlSocket), SD_BOTH); }
	if(ToSocket(trackingSocket) != InvalidSocket){ shutdown(ToSocket(trackingSocket), SD_BOTH); }
#endif
	if(worker.joinable()){
		worker.join();
	}
	EndSession("transport_stop");
	CloseSockets();
}

bool GalaxyXRTransport::IsRunning() const{
	return running.load(std::memory_order_acquire);
}

bool GalaxyXRTransport::HasAuthenticatedSession() const{
	return sessionAuthenticated.load(std::memory_order_acquire);
}

RemoteDiagnosticCounters GalaxyXRTransport::GetRemoteCounters() const{
	std::lock_guard<std::mutex> lock(remoteCountersMutex);
	return remoteCounters;
}

void GalaxyXRTransport::Worker(){
#ifdef _WIN32
	while(running.load(std::memory_order_acquire)){
		fd_set readSet;
		fd_set writeSet;
		FD_ZERO(&readSet);
		FD_ZERO(&writeSet);
		SOCKET maximum = 0;
		auto addRead = [&](SOCKET socket){
			if(socket != InvalidSocket){
				FD_SET(socket, &readSet);
				maximum = std::max(maximum, socket);
			}
		};
		addRead(ToSocket(listenSocket));
		addRead(ToSocket(trackingSocket));
		addRead(ToSocket(controlSocket));
		if(ToSocket(controlSocket) != InvalidSocket &&
			controlWriteOffset < controlWriteBuffer.size()){
			FD_SET(ToSocket(controlSocket), &writeSet);
			maximum = std::max(maximum, ToSocket(controlSocket));
		}
		timeval timeout{};
		timeout.tv_usec = 20000;
		const int ready = select(
			static_cast<int>(maximum + 1),
			&readSet,
			&writeSet,
			nullptr,
			&timeout);
		if(ready == SOCKET_ERROR){
			diagnostics.CountSocketError();
			continue;
		}
		if(FD_ISSET(ToSocket(listenSocket), &readSet)){ AcceptControl(); }
		if(ToSocket(controlSocket) != InvalidSocket &&
			FD_ISSET(ToSocket(controlSocket), &readSet)){ ReadControl(); }
		if(FD_ISSET(ToSocket(trackingSocket), &readSet)){ ReadTracking(); }
		if(ToSocket(controlSocket) != InvalidSocket &&
			FD_ISSET(ToSocket(controlSocket), &writeSet)){ FlushControlWrites(); }

		const std::int64_t now = NowNs();
		if(handshakePending.load(std::memory_order_acquire) &&
			now - handshakeStartedNs >= 5000000000LL){
			EndSession("handshake_timeout");
			if(ToSocket(controlSocket) != InvalidSocket){
				closesocket(ToSocket(controlSocket));
				controlSocket = FromSocket(InvalidSocket);
			}
			continue;
		}
		if(HasAuthenticatedSession() && now - lastClockRequestNs >= 1000000000LL){
			SendClockRequest(now);
		}
	}
#endif
}

bool GalaxyXRTransport::SetupSockets(){
#ifdef _WIN32
	WSADATA winsock{};
	if(WSAStartup(MAKEWORD(2, 2), &winsock) != 0){
		DriverLog("GXR Transport: WSAStartup failed");
		return false;
	}
	winsockStarted = true;

	IN_ADDR address{};
	if(InetPtonA(AF_INET, configuration.listenAddress.c_str(), &address) != 1 ||
		address.S_un.S_addr == INADDR_ANY ||
		address.S_un.S_addr == INADDR_NONE){
		DriverLog(
			"GXR Transport: listenAddress must be an explicit local IPv4 address (127.0.0.1 is the safe default)");
		return false;
	}

	SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(tcp == InvalidSocket || udp == InvalidSocket){
		if(tcp != InvalidSocket){ closesocket(tcp); }
		if(udp != InvalidSocket){ closesocket(udp); }
		return false;
	}
	BOOL exclusive = TRUE;
	setsockopt(tcp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
	setsockopt(udp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
	sockaddr_in tcpAddress{};
	tcpAddress.sin_family = AF_INET;
	tcpAddress.sin_addr = address;
	tcpAddress.sin_port = htons(configuration.controlPort);
	sockaddr_in udpAddress = tcpAddress;
	udpAddress.sin_port = htons(configuration.trackingPort);
	if(bind(tcp, reinterpret_cast<sockaddr*>(&tcpAddress), sizeof(tcpAddress)) != 0 ||
		bind(udp, reinterpret_cast<sockaddr*>(&udpAddress), sizeof(udpAddress)) != 0 ||
		listen(tcp, 1) != 0 ||
		!SetNonBlocking(tcp) ||
		!SetNonBlocking(udp)){
		DriverLog("GXR Transport: socket bind/listen failed with error %d", WSAGetLastError());
		closesocket(tcp);
		closesocket(udp);
		return false;
	}
	listenSocket = FromSocket(tcp);
	trackingSocket = FromSocket(udp);
	return true;
#else
	DriverLog("GXR Transport: side channel is available only on Windows builds");
	return false;
#endif
}

void GalaxyXRTransport::CloseSockets(){
#ifdef _WIN32
	if(ToSocket(controlSocket) != InvalidSocket){
		closesocket(ToSocket(controlSocket));
		controlSocket = FromSocket(InvalidSocket);
	}
	if(ToSocket(listenSocket) != InvalidSocket){
		closesocket(ToSocket(listenSocket));
		listenSocket = FromSocket(InvalidSocket);
	}
	if(ToSocket(trackingSocket) != InvalidSocket){
		closesocket(ToSocket(trackingSocket));
		trackingSocket = FromSocket(InvalidSocket);
	}
	if(winsockStarted){
		WSACleanup();
		winsockStarted = false;
	}
#endif
	controlReadBuffer.clear();
	controlWriteBuffer.clear();
	controlWriteOffset = 0;
	controlPeerAddress = 0;
}

void GalaxyXRTransport::AcceptControl(){
#ifdef _WIN32
	sockaddr_in peer{};
	int peerBytes = sizeof(peer);
	SOCKET accepted = accept(
		ToSocket(listenSocket),
		reinterpret_cast<sockaddr*>(&peer),
		&peerBytes);
	if(accepted == InvalidSocket){
		if(!WouldBlock()){ diagnostics.CountSocketError(); }
		return;
	}
	if(ToSocket(controlSocket) != InvalidSocket){
		closesocket(accepted);
		return;
	}
	if(!SetNonBlocking(accepted)){
		closesocket(accepted);
		return;
	}
	controlSocket = FromSocket(accepted);
	controlPeerAddress = peer.sin_addr.S_un.S_addr;
	controlReadBuffer.clear();
	controlWriteBuffer.clear();
	controlWriteOffset = 0;
#endif
}

void GalaxyXRTransport::ReadControl(){
#ifdef _WIN32
	std::array<std::uint8_t, 8192> buffer{};
	for(;;){
		const int received = recv(
			ToSocket(controlSocket),
			reinterpret_cast<char*>(buffer.data()),
			static_cast<int>(buffer.size()),
			0);
		if(received == 0){
			EndSession("control_closed");
			closesocket(ToSocket(controlSocket));
			controlSocket = FromSocket(InvalidSocket);
			return;
		}
		if(received < 0){
			if(WouldBlock()){ break; }
			diagnostics.CountSocketError();
			EndSession("control_error");
			closesocket(ToSocket(controlSocket));
			controlSocket = FromSocket(InvalidSocket);
			return;
		}
		controlReadBuffer.insert(controlReadBuffer.end(), buffer.begin(), buffer.begin() + received);
		if(controlReadBuffer.size() > (MaximumControlFrameBytes + 4) * 2){
			diagnostics.CountDecodeFailure(DecodeError::PayloadTooLarge);
			EndSession("control_buffer_limit");
			closesocket(ToSocket(controlSocket));
			controlSocket = FromSocket(InvalidSocket);
			return;
		}
	}

	while(controlReadBuffer.size() >= 4){
		const std::uint32_t frameBytes = ReadU32(controlReadBuffer.data());
		if(frameBytes < EnvelopeBytes || frameBytes > MaximumControlFrameBytes){
			diagnostics.CountDecodeFailure(DecodeError::PayloadTooLarge);
			EndSession("control_frame_limit");
			closesocket(ToSocket(controlSocket));
			controlSocket = FromSocket(InvalidSocket);
			return;
		}
		if(controlReadBuffer.size() < frameBytes + 4){
			break;
		}
		ProcessControlFrame(controlReadBuffer.data() + 4, frameBytes);
		controlReadBuffer.erase(controlReadBuffer.begin(), controlReadBuffer.begin() + 4 + frameBytes);
		if(ToSocket(controlSocket) == InvalidSocket){ return; }
	}
#endif
}

void GalaxyXRTransport::FlushControlWrites(){
#ifdef _WIN32
	while(controlWriteOffset < controlWriteBuffer.size()){
		const std::size_t remaining = controlWriteBuffer.size() - controlWriteOffset;
		const int sent = send(
			ToSocket(controlSocket),
			reinterpret_cast<const char*>(controlWriteBuffer.data() + controlWriteOffset),
			static_cast<int>(std::min<std::size_t>(remaining, std::numeric_limits<int>::max())),
			0);
		if(sent < 0){
			if(WouldBlock()){ return; }
			diagnostics.CountSocketError();
			EndSession("control_send_error");
			closesocket(ToSocket(controlSocket));
			controlSocket = FromSocket(InvalidSocket);
			return;
		}
		controlWriteOffset += static_cast<std::size_t>(sent);
	}
	controlWriteBuffer.clear();
	controlWriteOffset = 0;
#endif
}

void GalaxyXRTransport::ReadTracking(){
#ifdef _WIN32
	std::array<std::uint8_t, EnvelopeBytes + MaximumTrackingPayloadBytes> buffer{};
	for(;;){
		sockaddr_in peer{};
		int peerBytes = sizeof(peer);
		const int received = recvfrom(
			ToSocket(trackingSocket),
			reinterpret_cast<char*>(buffer.data()),
			static_cast<int>(buffer.size()),
			0,
			reinterpret_cast<sockaddr*>(&peer),
			&peerBytes);
		if(received < 0){
			if(!WouldBlock()){ diagnostics.CountSocketError(); }
			return;
		}
		ProcessTrackingDatagram(
			buffer.data(),
			static_cast<std::size_t>(received),
			peer.sin_addr.S_un.S_addr);
	}
#endif
}

void GalaxyXRTransport::ProcessControlFrame(
	const std::uint8_t* packet,
	std::size_t packetBytes){
	DecodedPacket decoded;
	const bool pending = handshakePending.load(std::memory_order_acquire);
	const auto& key =
		HasAuthenticatedSession() || pending
		? sessionKey
		: configuration.pairingKey;
	const DecodeError error = DecodePacket(packet, packetBytes, key, decoded);
	if(error != DecodeError::None){
		diagnostics.CountDecodeFailure(error);
		return;
	}
	if(pending){
		if(decoded.envelope.sessionId != sessionId ||
			decoded.envelope.sequence <= lastControlSequence ||
			decoded.envelope.sequence == std::numeric_limits<std::uint64_t>::max() ||
			decoded.envelope.messageType != MessageType::Capabilities){
			diagnostics.CountDecodeFailure(DecodeError::InvalidValue);
			EndSession("handshake_confirmation_invalid");
			return;
		}
		CapabilitySnapshot capabilities;
		const DecodeError payloadError =
			DecodeCapabilitiesPayload(decoded.payload, capabilities);
		if(payloadError != DecodeError::None ||
			!profile.BeginSession(
				sessionId,
				pendingClient,
				handshakeStartedNs) ||
			!profile.UpdateCapabilities(capabilities)){
			diagnostics.CountDecodeFailure(
				payloadError == DecodeError::None
				? DecodeError::InvalidValue
				: payloadError);
			EndSession("handshake_confirmation_refused");
			return;
		}
		lastControlSequence = decoded.envelope.sequence;
		handshakePending.store(false, std::memory_order_release);
		sessionAuthenticated.store(true, std::memory_order_release);
		diagnostics.CountAcceptedControl();
		diagnostics.LogSession(profile.GetSession());
		diagnostics.LogCapabilities(capabilities);
		return;
	}
	if(!HasAuthenticatedSession()){
		if(decoded.envelope.messageType != MessageType::Hello ||
			!IsZeroSession(decoded.envelope.sessionId) ||
			decoded.envelope.sequence != 1 ||
			!EstablishSession(decoded)){
			diagnostics.CountDecodeFailure(DecodeError::InvalidValue);
		}
		return;
	}
	if(decoded.envelope.sessionId != sessionId){
		diagnostics.CountDecodeFailure(DecodeError::AuthenticationFailed);
		return;
	}
	if(decoded.envelope.sequence <= lastControlSequence){
		if(decoded.envelope.sequence == lastControlSequence){ diagnostics.CountDuplicate(); }
		else{ diagnostics.CountReordered(); }
		return;
	}
	if(decoded.envelope.sequence == std::numeric_limits<std::uint64_t>::max()){
		EndSession("control_sequence_wrap");
		return;
	}
	bool accepted = false;

	switch(decoded.envelope.messageType){
		case MessageType::Capabilities: {
			CapabilitySnapshot capabilities;
			const DecodeError payloadError = DecodeCapabilitiesPayload(decoded.payload, capabilities);
			if(payloadError != DecodeError::None || !profile.UpdateCapabilities(capabilities)){
				diagnostics.CountDecodeFailure(
					payloadError == DecodeError::None ? DecodeError::InvalidValue : payloadError);
				return;
			}
			diagnostics.LogCapabilities(capabilities);
			accepted = true;
			break;
		}
		case MessageType::ClockSyncResponse: {
			ClockSyncResponse response;
			const DecodeError payloadError = DecodeClockSyncResponsePayload(decoded.payload, response);
			if(payloadError != DecodeError::None){
				diagnostics.CountDecodeFailure(payloadError);
				return;
			}
			const bool clockAccepted = clockSync.AddExchange(
				response.t0HostSendNs,
				response.t1ClientReceiveNs,
				response.t2ClientSendNs,
				NowNs());
			diagnostics.LogClock(clockSync.GetEstimate(), clockAccepted);
			accepted = true;
			break;
		}
		case MessageType::DiagnosticCounters: {
			RemoteDiagnosticCounters counters;
			const DecodeError payloadError = DecodeDiagnosticCountersPayload(decoded.payload, counters);
			if(payloadError != DecodeError::None){
				diagnostics.CountDecodeFailure(payloadError);
				return;
			}
			std::lock_guard<std::mutex> lock(remoteCountersMutex);
			remoteCounters = counters;
			accepted = true;
			break;
		}
		case MessageType::Disconnect: {
			DisconnectMessage disconnect;
			const DecodeError payloadError = DecodeDisconnectPayload(decoded.payload, disconnect);
			if(payloadError != DecodeError::None){
				diagnostics.CountDecodeFailure(payloadError);
				return;
			}
			lastControlSequence = decoded.envelope.sequence;
			diagnostics.CountAcceptedControl();
			EndSession("peer_disconnect");
#ifdef _WIN32
			if(ToSocket(controlSocket) != InvalidSocket){
				closesocket(ToSocket(controlSocket));
				controlSocket = FromSocket(InvalidSocket);
			}
#endif
			return;
		}
		default:
			diagnostics.CountDecodeFailure(DecodeError::UnknownMessageType);
			return;
	}
	if(accepted){
		lastControlSequence = decoded.envelope.sequence;
		diagnostics.CountAcceptedControl();
	}
}

void GalaxyXRTransport::ProcessTrackingDatagram(
	const std::uint8_t* packet,
	std::size_t packetBytes,
	std::uint32_t peerAddress){
	if(!HasAuthenticatedSession() ||
		peerAddress != controlPeerAddress ||
		packetBytes > EnvelopeBytes + MaximumTrackingPayloadBytes){
		return;
	}
	DecodedPacket decoded;
	const DecodeError error = DecodePacket(packet, packetBytes, sessionKey, decoded);
	if(error != DecodeError::None){
		diagnostics.CountDecodeFailure(error);
		return;
	}
	if(decoded.envelope.sessionId != sessionId){
		diagnostics.CountDecodeFailure(DecodeError::AuthenticationFailed);
		return;
	}
	if(decoded.envelope.sequence <= lastTrackingSequence){
		if(decoded.envelope.sequence == lastTrackingSequence){ diagnostics.CountDuplicate(); }
		else{ diagnostics.CountReordered(); }
		return;
	}
	if(decoded.envelope.sequence == std::numeric_limits<std::uint64_t>::max()){
		EndSession("tracking_sequence_wrap");
		return;
	}
	const std::uint64_t previousSequence = lastTrackingSequence;
	bool accepted = false;

	switch(decoded.envelope.messageType){
		case MessageType::TrackingSample: {
			TrackingSample tracking;
			const DecodeError payloadError = DecodeTrackingPayload(decoded.payload, tracking);
			if(payloadError != DecodeError::None){
				diagnostics.CountDecodeFailure(payloadError);
				return;
			}
			tracking.captureMonotonicNs = decoded.envelope.clientMonotonicNs;
			tracking.hostReceiveMonotonicNs = NowNs();
			std::int64_t captureHostNs = 0;
			double ageMs = -1.0;
			if(clockSync.ClientToHost(tracking.captureMonotonicNs, captureHostNs)){
				const std::int64_t ageNs = tracking.hostReceiveMonotonicNs - captureHostNs;
				ageMs = static_cast<double>(ageNs) / 1000000.0;
				if(ageNs < -10000000LL ||
					ageNs > static_cast<std::int64_t>(configuration.staleAfterMs) * 1000000LL){
					diagnostics.CountStale();
					return;
				}
			}
			if(!profile.UpdateTracking(tracking)){
				diagnostics.CountDecodeFailure(DecodeError::InvalidValue);
				return;
			}
			diagnostics.LogTracking(tracking, ageMs);
			accepted = true;
			break;
		}
		case MessageType::PresentationSample: {
			PresentationSample presentation;
			const DecodeError payloadError = DecodePresentationPayload(decoded.payload, presentation);
			if(payloadError != DecodeError::None){
				diagnostics.CountDecodeFailure(payloadError);
				return;
			}
			presentation.hostReceiveMonotonicNs = NowNs();
			profile.UpdatePresentation(presentation);
			diagnostics.LogPresentation(presentation);
			accepted = true;
			break;
		}
		case MessageType::DiagnosticCounters: {
			RemoteDiagnosticCounters counters;
			const DecodeError payloadError = DecodeDiagnosticCountersPayload(decoded.payload, counters);
			if(payloadError != DecodeError::None){
				diagnostics.CountDecodeFailure(payloadError);
				return;
			}
			{
				std::lock_guard<std::mutex> lock(remoteCountersMutex);
				remoteCounters = counters;
			}
			accepted = true;
			break;
		}
		default:
			diagnostics.CountDecodeFailure(DecodeError::UnknownMessageType);
			return;
	}
	if(accepted){
		lastTrackingSequence = decoded.envelope.sequence;
		diagnostics.CountAcceptedTracking(previousSequence, decoded.envelope.sequence);
	}
}

bool GalaxyXRTransport::EstablishSession(const DecodedPacket& packet){
	ClientIdentity client;
	if(DecodeHelloPayload(packet.payload, client) != DecodeError::None){
		return false;
	}
	const bool admitted = std::any_of(
		configuration.allowedClients.begin(),
		configuration.allowedClients.end(),
		[&client](const TransportConfiguration::ClientAdmission& allowed){
			return MatchesClientAdmission(
				client,
				allowed.versionCode,
				allowed.apkSha256,
				allowed.bridgeSha256);
		});
	if(!admitted){
		DriverLog(
			"GXR Session: HELLO refused because client version/APK/bridge provenance did not match the configured allowlist");
		return false;
	}
	if(std::all_of(
		client.clientNonce.begin(),
		client.clientNonce.end(),
		[](std::uint8_t value){ return value == 0; }) ||
		std::find(
			seenClientNonces.begin(),
			seenClientNonces.end(),
			client.clientNonce) != seenClientNonces.end()){
		return false;
	}
#ifdef _WIN32
	std::array<std::uint8_t, SessionIdBytes> newSessionId{};
	std::array<std::uint8_t, NonceBytes> hostNonce{};
	if(!RandomBytes(newSessionId.data(), newSessionId.size()) ||
		!RandomBytes(hostNonce.data(), hostNonce.size()) ||
		IsZeroSession(newSessionId)){
		return false;
	}
	std::array<std::uint8_t, Sha256Bytes> newSessionKey{};
	if(!DeriveSessionKey(
		configuration.pairingKey,
		client.clientNonce,
		hostNonce,
		newSessionId,
		newSessionKey)){
		return false;
	}
	const std::int64_t establishedNs = NowNs();
	sessionId = newSessionId;
	sessionKey = newSessionKey;
	pendingClient = client;
	lastControlSequence = packet.envelope.sequence;
	lastTrackingSequence = 0;
	nextHostControlSequence = 1;
	clockSync.Reset();
	{
		std::lock_guard<std::mutex> lock(remoteCountersMutex);
		remoteCounters = {};
	}

	HelloAck ack;
	ack.hostNonce = hostNonce;
	ack.accepted = true;
	ack.trackingPort = configuration.trackingPort;
	ack.hostVersion = configuration.hostVersion;
	ack.hostDllSha256 = configuration.hostDllSha256;
	std::vector<std::uint8_t> payload;
	if(!EncodeHelloAckPayload(ack, payload)){
		EndSession("hello_ack_encode_failed");
		return false;
	}
	Envelope envelope;
	envelope.messageType = MessageType::HelloAck;
	envelope.sessionId = sessionId;
	envelope.sequence = nextHostControlSequence++;
	std::vector<std::uint8_t> encoded;
	// HELLO_ACK is PSK-authenticated; the session key starts after this frame.
	if(!EncodePacket(envelope, payload, configuration.pairingKey, encoded)){
		EndSession("hello_ack_auth_failed");
		return false;
	}
	AppendU32(controlWriteBuffer, static_cast<std::uint32_t>(encoded.size()));
	controlWriteBuffer.insert(controlWriteBuffer.end(), encoded.begin(), encoded.end());
	handshakeStartedNs = establishedNs;
	handshakePending.store(true, std::memory_order_release);
	seenClientNonces.push_back(client.clientNonce);
	if(seenClientNonces.size() > 64){
		seenClientNonces.pop_front();
	}
	DriverLog(
		"GXR Session: HELLO_ACK issued; feature activation pending fresh session-key capability proof");
	return true;
#else
	(void)packet;
	return false;
#endif
}

bool GalaxyXRTransport::SendControl(
	MessageType type,
	const std::vector<std::uint8_t>& payload){
#ifdef _WIN32
	if(!HasAuthenticatedSession() ||
		ToSocket(controlSocket) == InvalidSocket ||
		nextHostControlSequence == std::numeric_limits<std::uint64_t>::max()){
		return false;
	}
	Envelope envelope;
	envelope.messageType = type;
	envelope.sessionId = sessionId;
	envelope.sequence = nextHostControlSequence++;
	std::vector<std::uint8_t> encoded;
	if(!EncodePacket(envelope, payload, sessionKey, encoded)){
		return false;
	}
	if(controlWriteBuffer.size() - controlWriteOffset + encoded.size() + 4 >
		(MaximumControlFrameBytes + 4) * 4){
		diagnostics.CountSocketError();
		return false;
	}
	if(controlWriteOffset != 0 && controlWriteOffset == controlWriteBuffer.size()){
		controlWriteBuffer.clear();
		controlWriteOffset = 0;
	}
	AppendU32(controlWriteBuffer, static_cast<std::uint32_t>(encoded.size()));
	controlWriteBuffer.insert(controlWriteBuffer.end(), encoded.begin(), encoded.end());
	return true;
#else
	(void)type;
	(void)payload;
	return false;
#endif
}

void GalaxyXRTransport::SendClockRequest(std::int64_t nowNs){
	std::vector<std::uint8_t> payload;
	if(EncodeClockSyncRequestPayload(nowNs, payload) &&
		SendControl(MessageType::ClockSyncRequest, payload)){
		lastClockRequestNs = nowNs;
	}
}

void GalaxyXRTransport::EndSession(const char* reason){
	if(!sessionAuthenticated.exchange(false, std::memory_order_acq_rel) &&
		IsZeroSession(sessionId)){
		return;
	}
	DriverLog("GXR Session: disconnected reason=%s; keys and tracking invalidated", reason);
	sessionKey.fill(0);
	sessionId.fill(0);
	pendingClient = {};
	handshakePending.store(false, std::memory_order_release);
	handshakeStartedNs = 0;
	lastControlSequence = 0;
	lastTrackingSequence = 0;
	nextHostControlSequence = 1;
	lastClockRequestNs = 0;
	controlWriteBuffer.clear();
	controlWriteOffset = 0;
	clockSync.Reset();
	profile.EndSession();
}

std::int64_t GalaxyXRTransport::NowNs(){
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace galaxyxr
