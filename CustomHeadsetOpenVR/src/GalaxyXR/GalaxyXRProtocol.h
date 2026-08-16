#pragma once

#include "GalaxyXRTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace galaxyxr {

constexpr std::uint16_t ProtocolMajor = 1;
constexpr std::uint16_t ProtocolMinor = 0;
constexpr std::size_t AuthenticationTagBytes = 16;
constexpr std::size_t EnvelopeBytes = 68;
constexpr std::uint32_t MaximumPayloadBytes = 4096;
constexpr std::uint32_t MaximumTrackingPayloadBytes = 2048;
constexpr std::uint32_t MaximumControlFrameBytes = EnvelopeBytes + MaximumPayloadBytes;

enum class MessageType : std::uint16_t {
	Hello = 1,
	HelloAck = 2,
	Capabilities = 3,
	ClockSyncRequest = 4,
	ClockSyncResponse = 5,
	TrackingSample = 6,
	PresentationSample = 7,
	DiagnosticCounters = 8,
	Disconnect = 9,
};

enum class DecodeError {
	None,
	TooShort,
	BadMagic,
	UnsupportedMajor,
	UnsupportedMinor,
	InvalidHeaderSize,
	PayloadTooLarge,
	LengthMismatch,
	UnknownMessageType,
	AuthenticationUnavailable,
	AuthenticationFailed,
	MalformedPayload,
	InvalidValue,
};

struct Envelope {
	std::uint16_t versionMajor = ProtocolMajor;
	std::uint16_t versionMinor = ProtocolMinor;
	MessageType messageType = MessageType::Hello;
	std::uint16_t headerBytes = static_cast<std::uint16_t>(EnvelopeBytes);
	std::uint32_t payloadBytes = 0;
	std::array<std::uint8_t, SessionIdBytes> sessionId{};
	std::uint64_t sequence = 0;
	std::uint32_t flags = 0;
	std::int64_t clientMonotonicNs = 0;
	std::array<std::uint8_t, AuthenticationTagBytes> authenticationTag{};
};

struct DecodedPacket {
	Envelope envelope;
	std::vector<std::uint8_t> payload;
};

struct HelloAck {
	std::array<std::uint8_t, NonceBytes> hostNonce{};
	std::uint16_t selectedMajor = ProtocolMajor;
	std::uint16_t selectedMinor = ProtocolMinor;
	bool accepted = false;
	std::uint8_t reason = 0;
	std::uint32_t maximumControlPayload = MaximumPayloadBytes;
	std::uint32_t maximumTrackingPayload = MaximumTrackingPayloadBytes;
	std::uint16_t maximumTrackingRateHz = 240;
	std::uint16_t trackingPort = 29982;
	std::string hostVersion;
	std::array<std::uint8_t, Sha256Bytes> hostDllSha256{};
};

struct ClockSyncResponse {
	std::int64_t t0HostSendNs = 0;
	std::int64_t t1ClientReceiveNs = 0;
	std::int64_t t2ClientSendNs = 0;
};

struct DisconnectMessage {
	std::uint16_t reason = 0;
	bool mayResume = false;
	std::uint64_t lastControlSequence = 0;
	std::uint64_t lastTrackingSequence = 0;
};

struct RemoteDiagnosticCounters {
	std::uint32_t capabilityRevision = 0;
	std::uint64_t eyeSamples = 0;
	std::uint64_t faceSamples = 0;
	std::uint64_t sentPackets = 0;
	std::uint64_t droppedBeforeSend = 0;
	std::uint32_t reconnects = 0;
	std::uint32_t socketErrors = 0;
	std::uint32_t extensionFailures = 0;
	std::uint32_t invalidSamples = 0;
	std::uint32_t queueHighWater = 0;
	std::uint64_t lastControlSequence = 0;
	std::uint64_t lastTrackingSequence = 0;
};

bool ParseHexKey32(const std::string& text, std::array<std::uint8_t, Sha256Bytes>& key);
bool ComputeHmacSha256(
	const std::array<std::uint8_t, Sha256Bytes>& key,
	const std::uint8_t* first,
	std::size_t firstBytes,
	const std::uint8_t* second,
	std::size_t secondBytes,
	std::array<std::uint8_t, Sha256Bytes>& digest);
bool DeriveSessionKey(
	const std::array<std::uint8_t, Sha256Bytes>& pairingKey,
	const std::array<std::uint8_t, NonceBytes>& clientNonce,
	const std::array<std::uint8_t, NonceBytes>& hostNonce,
	const std::array<std::uint8_t, SessionIdBytes>& sessionId,
	std::array<std::uint8_t, Sha256Bytes>& sessionKey);

bool EncodePacket(
	const Envelope& envelope,
	const std::vector<std::uint8_t>& payload,
	const std::array<std::uint8_t, Sha256Bytes>& authenticationKey,
	std::vector<std::uint8_t>& packet);
DecodeError DecodePacket(
	const std::uint8_t* packet,
	std::size_t packetBytes,
	const std::array<std::uint8_t, Sha256Bytes>& authenticationKey,
	DecodedPacket& decoded);

bool EncodeHelloPayload(const ClientIdentity& hello, std::vector<std::uint8_t>& payload);
DecodeError DecodeHelloPayload(const std::vector<std::uint8_t>& payload, ClientIdentity& hello);
bool MatchesClientAdmission(
	const ClientIdentity& client,
	std::uint32_t supportedVersionCode,
	const std::array<std::uint8_t, Sha256Bytes>& supportedApkSha256,
	const std::array<std::uint8_t, Sha256Bytes>& supportedBridgeSha256);
bool EncodeHelloAckPayload(const HelloAck& helloAck, std::vector<std::uint8_t>& payload);
DecodeError DecodeHelloAckPayload(const std::vector<std::uint8_t>& payload, HelloAck& helloAck);
bool EncodeCapabilitiesPayload(const CapabilitySnapshot& capabilities, std::vector<std::uint8_t>& payload);
DecodeError DecodeCapabilitiesPayload(const std::vector<std::uint8_t>& payload, CapabilitySnapshot& capabilities);
bool EncodeClockSyncRequestPayload(std::int64_t t0HostSendNs, std::vector<std::uint8_t>& payload);
DecodeError DecodeClockSyncResponsePayload(const std::vector<std::uint8_t>& payload, ClockSyncResponse& response);
bool EncodeTrackingPayload(const TrackingSample& tracking, std::vector<std::uint8_t>& payload);
DecodeError DecodeTrackingPayload(const std::vector<std::uint8_t>& payload, TrackingSample& tracking);
bool EncodePresentationPayload(const PresentationSample& presentation, std::vector<std::uint8_t>& payload);
DecodeError DecodePresentationPayload(const std::vector<std::uint8_t>& payload, PresentationSample& presentation);
bool EncodeDiagnosticCountersPayload(const RemoteDiagnosticCounters& counters, std::vector<std::uint8_t>& payload);
DecodeError DecodeDiagnosticCountersPayload(
	const std::vector<std::uint8_t>& payload,
	RemoteDiagnosticCounters& counters);
bool EncodeDisconnectPayload(const DisconnectMessage& disconnect, std::vector<std::uint8_t>& payload);
DecodeError DecodeDisconnectPayload(const std::vector<std::uint8_t>& payload, DisconnectMessage& disconnect);

const char* DecodeErrorName(DecodeError error);

} // namespace galaxyxr
