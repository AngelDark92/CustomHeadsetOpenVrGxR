#include "GalaxyXRProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace galaxyxr {
namespace {

constexpr std::array<std::uint8_t, 4> Magic = {'G', 'X', 'R', 'P'};
constexpr char SessionKeyLabel[] = "GXRP-SESSION-V1";
constexpr std::size_t MaximumViews = 2;
constexpr std::size_t MaximumRefreshRates = 16;

class Writer {
public:
	void U8(std::uint8_t value) { bytes.push_back(value); }
	void U16(std::uint16_t value) {
		bytes.push_back(static_cast<std::uint8_t>(value));
		bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	}
	void U32(std::uint32_t value) {
		for(std::size_t index = 0; index < 4; ++index){
			bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
		}
	}
	void U64(std::uint64_t value) {
		for(std::size_t index = 0; index < 8; ++index){
			bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
		}
	}
	void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }
	void F32(float value) {
		std::uint32_t bits = 0;
		static_assert(sizeof(bits) == sizeof(value), "binary32 size mismatch");
		std::memcpy(&bits, &value, sizeof(bits));
		U32(bits);
	}
	void Raw(const std::uint8_t* data, std::size_t count) {
		bytes.insert(bytes.end(), data, data + count);
	}
	template<std::size_t Count>
	void Raw(const std::array<std::uint8_t, Count>& value) {
		Raw(value.data(), value.size());
	}
	bool String(const std::string& value, std::size_t maximum) {
		if(value.size() > maximum || value.size() > std::numeric_limits<std::uint16_t>::max()){
			return false;
		}
		U16(static_cast<std::uint16_t>(value.size()));
		Raw(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
		return true;
	}

	std::vector<std::uint8_t> bytes;
};

class Reader {
public:
	Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}

	bool U8(std::uint8_t& value) {
		if(!Available(1)){ return false; }
		value = data[position++];
		return true;
	}
	bool U16(std::uint16_t& value) {
		if(!Available(2)){ return false; }
		value = static_cast<std::uint16_t>(data[position]) |
			(static_cast<std::uint16_t>(data[position + 1]) << 8);
		position += 2;
		return true;
	}
	bool U32(std::uint32_t& value) {
		if(!Available(4)){ return false; }
		value = 0;
		for(std::size_t index = 0; index < 4; ++index){
			value |= static_cast<std::uint32_t>(data[position + index]) << (index * 8);
		}
		position += 4;
		return true;
	}
	bool U64(std::uint64_t& value) {
		if(!Available(8)){ return false; }
		value = 0;
		for(std::size_t index = 0; index < 8; ++index){
			value |= static_cast<std::uint64_t>(data[position + index]) << (index * 8);
		}
		position += 8;
		return true;
	}
	bool I64(std::int64_t& value) {
		std::uint64_t bits = 0;
		if(!U64(bits)){ return false; }
		value = static_cast<std::int64_t>(bits);
		return true;
	}
	bool F32(float& value) {
		std::uint32_t bits = 0;
		if(!U32(bits)){ return false; }
		std::memcpy(&value, &bits, sizeof(value));
		return true;
	}
	bool Raw(std::uint8_t* destination, std::size_t count) {
		if(!Available(count)){ return false; }
		std::memcpy(destination, data + position, count);
		position += count;
		return true;
	}
	template<std::size_t Count>
	bool Raw(std::array<std::uint8_t, Count>& value) {
		return Raw(value.data(), value.size());
	}
	bool String(std::string& value, std::size_t maximum) {
		std::uint16_t count = 0;
		if(!U16(count) || count > maximum || !Available(count)){ return false; }
		value.assign(reinterpret_cast<const char*>(data + position), count);
		position += count;
		return value.find('\0') == std::string::npos;
	}
	bool Done() const { return position == size; }

private:
	bool Available(std::size_t count) const {
		return count <= size && position <= size - count;
	}

	const std::uint8_t* data = nullptr;
	std::size_t size = 0;
	std::size_t position = 0;
};

bool IsKnownMessageType(std::uint16_t value){
	return value >= static_cast<std::uint16_t>(MessageType::Hello) &&
		value <= static_cast<std::uint16_t>(MessageType::Disconnect);
}

bool IsZeroSession(const std::array<std::uint8_t, SessionIdBytes>& sessionId){
	return std::all_of(sessionId.begin(), sessionId.end(), [](std::uint8_t value){ return value == 0; });
}

bool ConstantTimeEqual(const std::uint8_t* left, const std::uint8_t* right, std::size_t count){
	std::uint8_t difference = 0;
	for(std::size_t index = 0; index < count; ++index){
		difference |= left[index] ^ right[index];
	}
	return difference == 0;
}

bool Finite(float value){
	return std::isfinite(value);
}

bool ValidViewFov(const ViewGeometry& view){
	constexpr float HalfPiLimit = 1.55334306f; // 89 degrees; tan remains bounded.
	return Finite(view.fovLeft) && Finite(view.fovRight) &&
		Finite(view.fovUp) && Finite(view.fovDown) &&
		view.fovLeft < 0.0f && view.fovLeft > -HalfPiLimit &&
		view.fovRight > 0.0f && view.fovRight < HalfPiLimit &&
		view.fovUp > 0.0f && view.fovUp < HalfPiLimit &&
		view.fovDown < 0.0f && view.fovDown > -HalfPiLimit;
}

bool ValidUnitQuaternion(const Quaternion& value){
	if(!Finite(value.x) || !Finite(value.y) || !Finite(value.z) || !Finite(value.w)){
		return false;
	}
	const float normSquared =
		value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
	return normSquared >= 0.9025f && normSquared <= 1.1025f;
}

bool ValidVector(const Vector3& value, float maximumMagnitude){
	return Finite(value.x) && Finite(value.y) && Finite(value.z) &&
		std::fabs(value.x) <= maximumMagnitude &&
		std::fabs(value.y) <= maximumMagnitude &&
		std::fabs(value.z) <= maximumMagnitude;
}

bool ValidEyeMode(std::uint8_t value){
	return value <= static_cast<std::uint8_t>(EyeMode::Both);
}

bool ValidEyeState(std::uint8_t value){
	return value <= static_cast<std::uint8_t>(EyeState::Shut);
}

bool ValidFaceState(std::uint8_t value){
	return value <= static_cast<std::uint8_t>(FaceState::Tracking);
}

bool ValidFaceSource(std::uint8_t value){
	return value <= static_cast<std::uint8_t>(FaceSource::Multimodal);
}

void WriteEnvelopeWithoutTag(const Envelope& envelope, Writer& writer){
	writer.Raw(Magic);
	writer.U16(envelope.versionMajor);
	writer.U16(envelope.versionMinor);
	writer.U16(static_cast<std::uint16_t>(envelope.messageType));
	writer.U16(envelope.headerBytes);
	writer.U32(envelope.payloadBytes);
	writer.Raw(envelope.sessionId);
	writer.U64(envelope.sequence);
	writer.U32(envelope.flags);
	writer.I64(envelope.clientMonotonicNs);
	std::array<std::uint8_t, AuthenticationTagBytes> zeroTag{};
	writer.Raw(zeroTag);
}

bool EncodeEye(const EyePose& eye, Writer& writer){
	if(!ValidVector(eye.origin, 2.0f) || !ValidUnitQuaternion(eye.orientation)){
		return false;
	}
	writer.F32(eye.origin.x);
	writer.F32(eye.origin.y);
	writer.F32(eye.origin.z);
	writer.F32(eye.orientation.x);
	writer.F32(eye.orientation.y);
	writer.F32(eye.orientation.z);
	writer.F32(eye.orientation.w);
	return true;
}

bool DecodeEye(Reader& reader, EyePose& eye){
	return reader.F32(eye.origin.x) &&
		reader.F32(eye.origin.y) &&
		reader.F32(eye.origin.z) &&
		reader.F32(eye.orientation.x) &&
		reader.F32(eye.orientation.y) &&
		reader.F32(eye.orientation.z) &&
		reader.F32(eye.orientation.w) &&
		ValidVector(eye.origin, 2.0f) &&
		ValidUnitQuaternion(eye.orientation);
}

} // namespace

bool ParseHexKey32(const std::string& text, std::array<std::uint8_t, Sha256Bytes>& key){
	if(text.size() != key.size() * 2){
		return false;
	}
	auto nibble = [](char character) -> int {
		if(character >= '0' && character <= '9'){ return character - '0'; }
		if(character >= 'a' && character <= 'f'){ return character - 'a' + 10; }
		if(character >= 'A' && character <= 'F'){ return character - 'A' + 10; }
		return -1;
	};
	for(std::size_t index = 0; index < key.size(); ++index){
		const int high = nibble(text[index * 2]);
		const int low = nibble(text[index * 2 + 1]);
		if(high < 0 || low < 0){
			key.fill(0);
			return false;
		}
		key[index] = static_cast<std::uint8_t>((high << 4) | low);
	}
	return true;
}

bool ComputeHmacSha256(
	const std::array<std::uint8_t, Sha256Bytes>& key,
	const std::uint8_t* first,
	std::size_t firstBytes,
	const std::uint8_t* second,
	std::size_t secondBytes,
	std::array<std::uint8_t, Sha256Bytes>& digest){
#ifdef _WIN32
	if((firstBytes != 0 && first == nullptr) || (secondBytes != 0 && second == nullptr) ||
		firstBytes > std::numeric_limits<ULONG>::max() ||
		secondBytes > std::numeric_limits<ULONG>::max()){
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
			BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0){
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
			const_cast<PUCHAR>(key.data()),
			static_cast<ULONG>(key.size()),
			0) < 0){
			break;
		}
		if((firstBytes != 0 && BCryptHashData(
				hash,
				const_cast<PUCHAR>(first),
				static_cast<ULONG>(firstBytes),
				0) < 0) ||
			(secondBytes != 0 && BCryptHashData(
				hash,
				const_cast<PUCHAR>(second),
				static_cast<ULONG>(secondBytes),
				0) < 0)){
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
	(void)key;
	(void)first;
	(void)firstBytes;
	(void)second;
	(void)secondBytes;
	digest.fill(0);
	return false;
#endif
}

bool DeriveSessionKey(
	const std::array<std::uint8_t, Sha256Bytes>& pairingKey,
	const std::array<std::uint8_t, NonceBytes>& clientNonce,
	const std::array<std::uint8_t, NonceBytes>& hostNonce,
	const std::array<std::uint8_t, SessionIdBytes>& sessionId,
	std::array<std::uint8_t, Sha256Bytes>& sessionKey){
	if(IsZeroSession(sessionId)){
		return false;
	}
	Writer material;
	material.Raw(reinterpret_cast<const std::uint8_t*>(SessionKeyLabel), sizeof(SessionKeyLabel) - 1);
	material.Raw(clientNonce);
	material.Raw(hostNonce);
	material.Raw(sessionId);
	return ComputeHmacSha256(
		pairingKey,
		material.bytes.data(),
		material.bytes.size(),
		nullptr,
		0,
		sessionKey);
}

bool EncodePacket(
	const Envelope& envelope,
	const std::vector<std::uint8_t>& payload,
	const std::array<std::uint8_t, Sha256Bytes>& authenticationKey,
	std::vector<std::uint8_t>& packet){
	if(payload.size() > MaximumPayloadBytes ||
		envelope.versionMajor != ProtocolMajor ||
		envelope.versionMinor > ProtocolMinor ||
		!IsKnownMessageType(static_cast<std::uint16_t>(envelope.messageType)) ||
		envelope.headerBytes != EnvelopeBytes){
		return false;
	}

	Envelope normalized = envelope;
	normalized.payloadBytes = static_cast<std::uint32_t>(payload.size());
	normalized.authenticationTag.fill(0);
	Writer writer;
	WriteEnvelopeWithoutTag(normalized, writer);
	if(writer.bytes.size() != EnvelopeBytes){
		return false;
	}

	std::array<std::uint8_t, Sha256Bytes> digest{};
	if(!ComputeHmacSha256(
		authenticationKey,
		writer.bytes.data(),
		writer.bytes.size(),
		payload.data(),
		payload.size(),
		digest)){
		return false;
	}
	std::copy_n(digest.begin(), AuthenticationTagBytes, writer.bytes.begin() + 52);
	writer.bytes.insert(writer.bytes.end(), payload.begin(), payload.end());
	packet = std::move(writer.bytes);
	return true;
}

DecodeError DecodePacket(
	const std::uint8_t* packet,
	std::size_t packetBytes,
	const std::array<std::uint8_t, Sha256Bytes>& authenticationKey,
	DecodedPacket& decoded){
	if(packet == nullptr || packetBytes < EnvelopeBytes){
		return DecodeError::TooShort;
	}
	if(!std::equal(Magic.begin(), Magic.end(), packet)){
		return DecodeError::BadMagic;
	}

	Reader reader(packet + 4, EnvelopeBytes - 4);
	std::uint16_t type = 0;
	if(!reader.U16(decoded.envelope.versionMajor) ||
		!reader.U16(decoded.envelope.versionMinor) ||
		!reader.U16(type) ||
		!reader.U16(decoded.envelope.headerBytes) ||
		!reader.U32(decoded.envelope.payloadBytes) ||
		!reader.Raw(decoded.envelope.sessionId) ||
		!reader.U64(decoded.envelope.sequence) ||
		!reader.U32(decoded.envelope.flags) ||
		!reader.I64(decoded.envelope.clientMonotonicNs) ||
		!reader.Raw(decoded.envelope.authenticationTag) ||
		!reader.Done()){
		return DecodeError::TooShort;
	}
	if(decoded.envelope.versionMajor != ProtocolMajor){
		return DecodeError::UnsupportedMajor;
	}
	// Version 1.0 has no extension/TLV area. A later minor revision must not be
	// interpreted as 1.0 until its optional-field skipping rules are defined.
	if(decoded.envelope.versionMinor > ProtocolMinor){
		return DecodeError::UnsupportedMinor;
	}
	if(decoded.envelope.headerBytes != EnvelopeBytes){
		return DecodeError::InvalidHeaderSize;
	}
	if(decoded.envelope.payloadBytes > MaximumPayloadBytes){
		return DecodeError::PayloadTooLarge;
	}
	if(packetBytes != EnvelopeBytes + decoded.envelope.payloadBytes){
		return DecodeError::LengthMismatch;
	}
	if(!IsKnownMessageType(type)){
		return DecodeError::UnknownMessageType;
	}
	decoded.envelope.messageType = static_cast<MessageType>(type);

	std::vector<std::uint8_t> authenticatedHeader(packet, packet + EnvelopeBytes);
	std::fill(
		authenticatedHeader.begin() + 52,
		authenticatedHeader.begin() + 52 + AuthenticationTagBytes,
		std::uint8_t{0});
	std::array<std::uint8_t, Sha256Bytes> digest{};
	if(!ComputeHmacSha256(
		authenticationKey,
		authenticatedHeader.data(),
		authenticatedHeader.size(),
		packet + EnvelopeBytes,
		decoded.envelope.payloadBytes,
		digest)){
		return DecodeError::AuthenticationUnavailable;
	}
	if(!ConstantTimeEqual(
		decoded.envelope.authenticationTag.data(),
		digest.data(),
		AuthenticationTagBytes)){
		return DecodeError::AuthenticationFailed;
	}
	decoded.payload.assign(packet + EnvelopeBytes, packet + packetBytes);
	return DecodeError::None;
}

bool EncodeHelloPayload(const ClientIdentity& hello, std::vector<std::uint8_t>& payload){
	Writer writer;
	writer.Raw(hello.clientNonce);
	writer.U64(hello.featureBits);
	writer.U32(hello.versionCode);
	writer.U8(hello.clockConversionSupported ? 1 : 0);
	writer.U8(0);
	writer.U8(0);
	writer.U8(0);
	if(!writer.String(hello.packageName, 128) ||
		!writer.String(hello.versionName, 64)){
		return false;
	}
	writer.Raw(hello.apkSha256);
	if(!writer.String(hello.bridgeVersion, 64)){
		return false;
	}
	writer.Raw(hello.bridgeSha256);
	if(!writer.String(hello.manufacturer, 64) ||
		!writer.String(hello.model, 64) ||
		!writer.String(hello.deviceSerial, 128) ||
		!writer.String(hello.buildFingerprint, 256) ||
		!writer.String(hello.runtimeName, 128) ||
		!writer.String(hello.runtimeVersion, 64)){
		return false;
	}
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeHelloPayload(const std::vector<std::uint8_t>& payload, ClientIdentity& hello){
	Reader reader(payload.data(), payload.size());
	std::uint8_t clockConversion = 0;
	std::uint8_t reserved0 = 0;
	std::uint8_t reserved1 = 0;
	std::uint8_t reserved2 = 0;
	if(!reader.Raw(hello.clientNonce) ||
		!reader.U64(hello.featureBits) ||
		!reader.U32(hello.versionCode) ||
		!reader.U8(clockConversion) ||
		!reader.U8(reserved0) ||
		!reader.U8(reserved1) ||
		!reader.U8(reserved2) ||
		clockConversion > 1 ||
		reserved0 != 0 || reserved1 != 0 || reserved2 != 0 ||
		!reader.String(hello.packageName, 128) ||
		!reader.String(hello.versionName, 64) ||
		!reader.Raw(hello.apkSha256) ||
		!reader.String(hello.bridgeVersion, 64) ||
		!reader.Raw(hello.bridgeSha256) ||
		!reader.String(hello.manufacturer, 64) ||
		!reader.String(hello.model, 64) ||
		!reader.String(hello.deviceSerial, 128) ||
		!reader.String(hello.buildFingerprint, 256) ||
		!reader.String(hello.runtimeName, 128) ||
		!reader.String(hello.runtimeVersion, 64) ||
		!reader.Done()){
		return DecodeError::MalformedPayload;
	}
	if(hello.packageName != "com.valvesoftware.steamlinkvr" ||
		hello.versionCode == 0 ||
		std::all_of(hello.clientNonce.begin(), hello.clientNonce.end(), [](std::uint8_t value){ return value == 0; })){
		return DecodeError::InvalidValue;
	}
	hello.clockConversionSupported = clockConversion != 0;
	return DecodeError::None;
}

bool MatchesClientAdmission(
	const ClientIdentity& client,
	std::uint32_t supportedVersionCode,
	const std::array<std::uint8_t, Sha256Bytes>& supportedApkSha256,
	const std::array<std::uint8_t, Sha256Bytes>& supportedBridgeSha256){
	return supportedVersionCode != 0 &&
		client.versionCode == supportedVersionCode &&
		client.apkSha256 == supportedApkSha256 &&
		client.bridgeSha256 == supportedBridgeSha256;
}

bool EncodeHelloAckPayload(const HelloAck& helloAck, std::vector<std::uint8_t>& payload){
	Writer writer;
	writer.Raw(helloAck.hostNonce);
	writer.U16(helloAck.selectedMajor);
	writer.U16(helloAck.selectedMinor);
	writer.U8(helloAck.accepted ? 1 : 0);
	writer.U8(helloAck.reason);
	writer.U16(0);
	writer.U32(helloAck.maximumControlPayload);
	writer.U32(helloAck.maximumTrackingPayload);
	writer.U16(helloAck.maximumTrackingRateHz);
	writer.U16(helloAck.trackingPort);
	if(!writer.String(helloAck.hostVersion, 64)){
		return false;
	}
	writer.Raw(helloAck.hostDllSha256);
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeHelloAckPayload(const std::vector<std::uint8_t>& payload, HelloAck& helloAck){
	Reader reader(payload.data(), payload.size());
	std::uint8_t accepted = 0;
	std::uint16_t reserved = 0;
	if(!reader.Raw(helloAck.hostNonce) ||
		!reader.U16(helloAck.selectedMajor) ||
		!reader.U16(helloAck.selectedMinor) ||
		!reader.U8(accepted) ||
		!reader.U8(helloAck.reason) ||
		!reader.U16(reserved) ||
		!reader.U32(helloAck.maximumControlPayload) ||
		!reader.U32(helloAck.maximumTrackingPayload) ||
		!reader.U16(helloAck.maximumTrackingRateHz) ||
		!reader.U16(helloAck.trackingPort) ||
		!reader.String(helloAck.hostVersion, 64) ||
		!reader.Raw(helloAck.hostDllSha256) ||
		!reader.Done() ||
		accepted > 1 || reserved != 0){
		return DecodeError::MalformedPayload;
	}
	helloAck.accepted = accepted != 0;
	return DecodeError::None;
}

bool EncodeCapabilitiesPayload(const CapabilitySnapshot& capabilities, std::vector<std::uint8_t>& payload){
	if(capabilities.views.empty() || capabilities.views.size() > MaximumViews ||
		capabilities.supportedRefreshRates.size() > MaximumRefreshRates ||
		!Finite(capabilities.currentRefreshHz)){
		return false;
	}
	Writer writer;
	writer.U32(capabilities.revision);
	writer.U64(capabilities.featureBits);
	writer.U32(capabilities.permissionFlags);
	writer.U32(capabilities.viewConfigurationType);
	writer.U8(static_cast<std::uint8_t>(capabilities.views.size()));
	writer.U8(capabilities.overlayMode);
	writer.U8(capabilities.timingFlags);
	writer.U8(0);
	writer.F32(capabilities.currentRefreshHz);
	writer.U8(static_cast<std::uint8_t>(capabilities.supportedRefreshRates.size()));
	for(float refresh : capabilities.supportedRefreshRates){
		if(!Finite(refresh) || refresh < 30.0f || refresh > 240.0f){ return false; }
		writer.F32(refresh);
	}
	if(!writer.String(capabilities.activeActivity, 256) ||
		!writer.String(capabilities.productId, 128) ||
		!writer.String(capabilities.enabledExtensions, 1024)){
		return false;
	}
	for(const ViewGeometry& view : capabilities.views){
		if(view.recommendedWidth < 256 || view.recommendedHeight < 256 ||
			view.maximumWidth < view.recommendedWidth ||
			view.maximumHeight < view.recommendedHeight ||
			view.maximumWidth > 8192 || view.maximumHeight > 8192 ||
			view.recommendedSampleCount == 0 || view.recommendedSampleCount > 8 ||
			!ValidViewFov(view)){
			return false;
		}
		writer.U32(view.recommendedWidth);
		writer.U32(view.recommendedHeight);
		writer.U32(view.maximumWidth);
		writer.U32(view.maximumHeight);
		writer.U32(view.recommendedSampleCount);
		writer.F32(view.fovLeft);
		writer.F32(view.fovRight);
		writer.F32(view.fovUp);
		writer.F32(view.fovDown);
	}
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeCapabilitiesPayload(const std::vector<std::uint8_t>& payload, CapabilitySnapshot& capabilities){
	Reader reader(payload.data(), payload.size());
	std::uint8_t viewCount = 0;
	std::uint8_t refreshCount = 0;
	std::uint8_t reserved = 0;
	if(!reader.U32(capabilities.revision) ||
		!reader.U64(capabilities.featureBits) ||
		!reader.U32(capabilities.permissionFlags) ||
		!reader.U32(capabilities.viewConfigurationType) ||
		!reader.U8(viewCount) ||
		!reader.U8(capabilities.overlayMode) ||
		!reader.U8(capabilities.timingFlags) ||
		!reader.U8(reserved) ||
		!reader.F32(capabilities.currentRefreshHz) ||
		!reader.U8(refreshCount) ||
		viewCount == 0 || viewCount > MaximumViews ||
		refreshCount > MaximumRefreshRates ||
		reserved != 0 ||
		!Finite(capabilities.currentRefreshHz) ||
		capabilities.currentRefreshHz < 30.0f || capabilities.currentRefreshHz > 240.0f){
		return DecodeError::MalformedPayload;
	}
	capabilities.supportedRefreshRates.resize(refreshCount);
	for(float& refresh : capabilities.supportedRefreshRates){
		if(!reader.F32(refresh) || !Finite(refresh) || refresh < 30.0f || refresh > 240.0f){
			return DecodeError::InvalidValue;
		}
	}
	if(!reader.String(capabilities.activeActivity, 256) ||
		!reader.String(capabilities.productId, 128) ||
		!reader.String(capabilities.enabledExtensions, 1024)){
		return DecodeError::MalformedPayload;
	}
	capabilities.views.resize(viewCount);
	for(ViewGeometry& view : capabilities.views){
		if(!reader.U32(view.recommendedWidth) ||
			!reader.U32(view.recommendedHeight) ||
			!reader.U32(view.maximumWidth) ||
			!reader.U32(view.maximumHeight) ||
			!reader.U32(view.recommendedSampleCount) ||
			!reader.F32(view.fovLeft) ||
			!reader.F32(view.fovRight) ||
			!reader.F32(view.fovUp) ||
			!reader.F32(view.fovDown)){
			return DecodeError::MalformedPayload;
		}
		if(view.recommendedWidth < 256 || view.recommendedHeight < 256 ||
			view.maximumWidth < view.recommendedWidth ||
			view.maximumHeight < view.recommendedHeight ||
			view.maximumWidth > 8192 || view.maximumHeight > 8192 ||
			view.recommendedSampleCount == 0 || view.recommendedSampleCount > 8 ||
			!ValidViewFov(view)){
			return DecodeError::InvalidValue;
		}
	}
	return reader.Done() ? DecodeError::None : DecodeError::MalformedPayload;
}

bool EncodeClockSyncRequestPayload(std::int64_t t0HostSendNs, std::vector<std::uint8_t>& payload){
	Writer writer;
	writer.I64(t0HostSendNs);
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeClockSyncResponsePayload(const std::vector<std::uint8_t>& payload, ClockSyncResponse& response){
	Reader reader(payload.data(), payload.size());
	if(!reader.I64(response.t0HostSendNs) ||
		!reader.I64(response.t1ClientReceiveNs) ||
		!reader.I64(response.t2ClientSendNs) ||
		!reader.Done() ||
		response.t0HostSendNs <= 0 ||
		response.t1ClientReceiveNs <= 0 ||
		response.t2ClientSendNs < response.t1ClientReceiveNs){
		return DecodeError::MalformedPayload;
	}
	return DecodeError::None;
}

bool EncodeTrackingPayload(const TrackingSample& tracking, std::vector<std::uint8_t>& payload){
	if(!ValidEyeMode(static_cast<std::uint8_t>(tracking.eyeMode)) ||
		!ValidEyeState(static_cast<std::uint8_t>(tracking.leftEye.state)) ||
		!ValidEyeState(static_cast<std::uint8_t>(tracking.rightEye.state)) ||
		!ValidFaceState(static_cast<std::uint8_t>(tracking.face.state)) ||
		!ValidFaceSource(static_cast<std::uint8_t>(tracking.face.source))){
		return false;
	}
	Writer writer;
	writer.U64(tracking.sampleSequence);
	writer.I64(tracking.predictedDisplayMonotonicNs);
	writer.I64(tracking.queryXrTime);
	writer.I64(tracking.predictedDisplayXrTime);
	writer.U32(tracking.baseSpaceId);
	writer.U32(tracking.coordinateRevision);
	writer.U32(tracking.capabilityRevision);
	writer.U32(tracking.validityFlags);
	writer.U8(static_cast<std::uint8_t>(tracking.eyeMode));
	writer.U8(static_cast<std::uint8_t>(tracking.leftEye.state));
	writer.U8(static_cast<std::uint8_t>(tracking.rightEye.state));
	std::uint8_t eyeFlags = 0;
	eyeFlags |= tracking.leftEye.positionTracked ? 1 : 0;
	eyeFlags |= tracking.leftEye.orientationTracked ? 2 : 0;
	eyeFlags |= tracking.rightEye.positionTracked ? 4 : 0;
	eyeFlags |= tracking.rightEye.orientationTracked ? 8 : 0;
	writer.U8(eyeFlags);
	if(!EncodeEye(tracking.leftEye, writer) || !EncodeEye(tracking.rightEye, writer)){
		return false;
	}
	writer.I64(tracking.face.sampleXrTime);
	writer.I64(tracking.face.sampleMonotonicNs);
	writer.U8(tracking.face.valid ? 1 : 0);
	writer.U8(static_cast<std::uint8_t>(tracking.face.state));
	writer.U8(static_cast<std::uint8_t>(tracking.face.source));
	writer.U8(static_cast<std::uint8_t>(FaceWeightCount));
	for(float weight : tracking.face.weights){
		if(!Finite(weight) || weight < 0.0f || weight > 1.0f){ return false; }
		writer.F32(weight);
	}
	writer.U8(static_cast<std::uint8_t>(FaceConfidenceCount));
	for(float confidence : tracking.face.confidence){
		if(!Finite(confidence) || confidence < 0.0f || confidence > 1.0f){ return false; }
		writer.F32(confidence);
	}
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeTrackingPayload(const std::vector<std::uint8_t>& payload, TrackingSample& tracking){
	Reader reader(payload.data(), payload.size());
	std::uint8_t eyeMode = 0;
	std::uint8_t leftState = 0;
	std::uint8_t rightState = 0;
	std::uint8_t eyeFlags = 0;
	std::uint8_t faceValid = 0;
	std::uint8_t faceState = 0;
	std::uint8_t faceSource = 0;
	std::uint8_t parameterCount = 0;
	std::uint8_t confidenceCount = 0;
	if(!reader.U64(tracking.sampleSequence) ||
		!reader.I64(tracking.predictedDisplayMonotonicNs) ||
		!reader.I64(tracking.queryXrTime) ||
		!reader.I64(tracking.predictedDisplayXrTime) ||
		!reader.U32(tracking.baseSpaceId) ||
		!reader.U32(tracking.coordinateRevision) ||
		!reader.U32(tracking.capabilityRevision) ||
		!reader.U32(tracking.validityFlags) ||
		!reader.U8(eyeMode) ||
		!reader.U8(leftState) ||
		!reader.U8(rightState) ||
		!reader.U8(eyeFlags) ||
		!ValidEyeMode(eyeMode) || !ValidEyeState(leftState) || !ValidEyeState(rightState) ||
		(eyeFlags & 0xF0) != 0){
		return DecodeError::MalformedPayload;
	}
	tracking.eyeMode = static_cast<EyeMode>(eyeMode);
	tracking.leftEye.state = static_cast<EyeState>(leftState);
	tracking.rightEye.state = static_cast<EyeState>(rightState);
	tracking.leftEye.positionTracked = (eyeFlags & 1) != 0;
	tracking.leftEye.orientationTracked = (eyeFlags & 2) != 0;
	tracking.rightEye.positionTracked = (eyeFlags & 4) != 0;
	tracking.rightEye.orientationTracked = (eyeFlags & 8) != 0;
	if(!DecodeEye(reader, tracking.leftEye) ||
		!DecodeEye(reader, tracking.rightEye) ||
		!reader.I64(tracking.face.sampleXrTime) ||
		!reader.I64(tracking.face.sampleMonotonicNs) ||
		!reader.U8(faceValid) ||
		!reader.U8(faceState) ||
		!reader.U8(faceSource) ||
		!reader.U8(parameterCount) ||
		faceValid > 1 ||
		!ValidFaceState(faceState) ||
		!ValidFaceSource(faceSource) ||
		parameterCount != FaceWeightCount){
		return DecodeError::MalformedPayload;
	}
	tracking.face.valid = faceValid != 0;
	tracking.face.state = static_cast<FaceState>(faceState);
	tracking.face.source = static_cast<FaceSource>(faceSource);
	for(float& weight : tracking.face.weights){
		if(!reader.F32(weight) || !Finite(weight) || weight < 0.0f || weight > 1.0f){
			return DecodeError::InvalidValue;
		}
	}
	if(!reader.U8(confidenceCount) || confidenceCount != FaceConfidenceCount){
		return DecodeError::MalformedPayload;
	}
	for(float& confidence : tracking.face.confidence){
		if(!reader.F32(confidence) || !Finite(confidence) || confidence < 0.0f || confidence > 1.0f){
			return DecodeError::InvalidValue;
		}
	}
	return reader.Done() ? DecodeError::None : DecodeError::MalformedPayload;
}

bool EncodePresentationPayload(const PresentationSample& presentation, std::vector<std::uint8_t>& payload){
	Writer writer;
	writer.U64(presentation.clientFrameCounter);
	writer.I64(presentation.predictedDisplayMonotonicNs);
	writer.I64(presentation.waitFrameMonotonicNs);
	writer.I64(presentation.beginFrameMonotonicNs);
	writer.I64(presentation.endFrameMonotonicNs);
	writer.U32(presentation.submittedWidth);
	writer.U32(presentation.submittedHeight);
	writer.U32(presentation.decoderWidth);
	writer.U32(presentation.decoderHeight);
	writer.U32(presentation.surfaceWidth);
	writer.U32(presentation.surfaceHeight);
	writer.U64(presentation.decodedFrameId);
	writer.U64(presentation.hostFrameId);
	writer.I64(presentation.photonMonotonicNs);
	writer.U32(presentation.flags);
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodePresentationPayload(const std::vector<std::uint8_t>& payload, PresentationSample& presentation){
	Reader reader(payload.data(), payload.size());
	if(!reader.U64(presentation.clientFrameCounter) ||
		!reader.I64(presentation.predictedDisplayMonotonicNs) ||
		!reader.I64(presentation.waitFrameMonotonicNs) ||
		!reader.I64(presentation.beginFrameMonotonicNs) ||
		!reader.I64(presentation.endFrameMonotonicNs) ||
		!reader.U32(presentation.submittedWidth) ||
		!reader.U32(presentation.submittedHeight) ||
		!reader.U32(presentation.decoderWidth) ||
		!reader.U32(presentation.decoderHeight) ||
		!reader.U32(presentation.surfaceWidth) ||
		!reader.U32(presentation.surfaceHeight) ||
		!reader.U64(presentation.decodedFrameId) ||
		!reader.U64(presentation.hostFrameId) ||
		!reader.I64(presentation.photonMonotonicNs) ||
		!reader.U32(presentation.flags) ||
		!reader.Done()){
		return DecodeError::MalformedPayload;
	}
	auto validDimension = [](std::uint32_t value){ return value == 0 || (value >= 256 && value <= 8192); };
	if(!validDimension(presentation.submittedWidth) ||
		!validDimension(presentation.submittedHeight) ||
		!validDimension(presentation.decoderWidth) ||
		!validDimension(presentation.decoderHeight) ||
		!validDimension(presentation.surfaceWidth) ||
		!validDimension(presentation.surfaceHeight)){
		return DecodeError::InvalidValue;
	}
	return DecodeError::None;
}

bool EncodeDiagnosticCountersPayload(
	const RemoteDiagnosticCounters& counters,
	std::vector<std::uint8_t>& payload){
	Writer writer;
	writer.U32(counters.capabilityRevision);
	writer.U64(counters.eyeSamples);
	writer.U64(counters.faceSamples);
	writer.U64(counters.sentPackets);
	writer.U64(counters.droppedBeforeSend);
	writer.U32(counters.reconnects);
	writer.U32(counters.socketErrors);
	writer.U32(counters.extensionFailures);
	writer.U32(counters.invalidSamples);
	writer.U32(counters.queueHighWater);
	writer.U64(counters.lastControlSequence);
	writer.U64(counters.lastTrackingSequence);
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeDiagnosticCountersPayload(
	const std::vector<std::uint8_t>& payload,
	RemoteDiagnosticCounters& counters){
	Reader reader(payload.data(), payload.size());
	if(!reader.U32(counters.capabilityRevision) ||
		!reader.U64(counters.eyeSamples) ||
		!reader.U64(counters.faceSamples) ||
		!reader.U64(counters.sentPackets) ||
		!reader.U64(counters.droppedBeforeSend) ||
		!reader.U32(counters.reconnects) ||
		!reader.U32(counters.socketErrors) ||
		!reader.U32(counters.extensionFailures) ||
		!reader.U32(counters.invalidSamples) ||
		!reader.U32(counters.queueHighWater) ||
		!reader.U64(counters.lastControlSequence) ||
		!reader.U64(counters.lastTrackingSequence) ||
		!reader.Done()){
		return DecodeError::MalformedPayload;
	}
	return DecodeError::None;
}

bool EncodeDisconnectPayload(const DisconnectMessage& disconnect, std::vector<std::uint8_t>& payload){
	Writer writer;
	writer.U16(disconnect.reason);
	writer.U8(disconnect.mayResume ? 1 : 0);
	writer.U8(0);
	writer.U64(disconnect.lastControlSequence);
	writer.U64(disconnect.lastTrackingSequence);
	payload = std::move(writer.bytes);
	return true;
}

DecodeError DecodeDisconnectPayload(const std::vector<std::uint8_t>& payload, DisconnectMessage& disconnect){
	Reader reader(payload.data(), payload.size());
	std::uint8_t mayResume = 0;
	std::uint8_t reserved = 0;
	if(!reader.U16(disconnect.reason) ||
		!reader.U8(mayResume) ||
		!reader.U8(reserved) ||
		!reader.U64(disconnect.lastControlSequence) ||
		!reader.U64(disconnect.lastTrackingSequence) ||
		!reader.Done() ||
		mayResume > 1 || reserved != 0){
		return DecodeError::MalformedPayload;
	}
	disconnect.mayResume = mayResume != 0;
	return DecodeError::None;
}

const char* DecodeErrorName(DecodeError error){
	switch(error){
		case DecodeError::None: return "none";
		case DecodeError::TooShort: return "too_short";
		case DecodeError::BadMagic: return "bad_magic";
		case DecodeError::UnsupportedMajor: return "unsupported_major";
		case DecodeError::InvalidHeaderSize: return "invalid_header_size";
		case DecodeError::PayloadTooLarge: return "payload_too_large";
		case DecodeError::LengthMismatch: return "length_mismatch";
		case DecodeError::UnknownMessageType: return "unknown_message_type";
		case DecodeError::AuthenticationUnavailable: return "authentication_unavailable";
		case DecodeError::AuthenticationFailed: return "authentication_failed";
		case DecodeError::MalformedPayload: return "malformed_payload";
		case DecodeError::InvalidValue: return "invalid_value";
	}
	return "unknown";
}

} // namespace galaxyxr
