#include "GalaxyXROscCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

namespace galaxyxr::osc {
namespace {

void AppendPaddedString(std::vector<std::uint8_t>& output, const std::string& value){
	output.insert(output.end(), value.begin(), value.end());
	output.push_back(0);
	while(output.size() % 4 != 0){
		output.push_back(0);
	}
}

void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value){
	output.push_back(static_cast<std::uint8_t>(value >> 24));
	output.push_back(static_cast<std::uint8_t>(value >> 16));
	output.push_back(static_cast<std::uint8_t>(value >> 8));
	output.push_back(static_cast<std::uint8_t>(value));
}

void AppendU64(std::vector<std::uint8_t>& output, std::uint64_t value){
	for(int shift = 56; shift >= 0; shift -= 8){
		output.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

void AppendFloat(std::vector<std::uint8_t>& output, float value){
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "OSC float must be IEEE-754 binary32");
	std::memcpy(&bits, &value, sizeof(bits));
	AppendU32(output, bits);
}

bool ValidUnitInterval(float value){
	return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

} // namespace

bool EncodeFaceFrame(
	const FaceSharedMemoryV1& snapshot,
	std::vector<std::uint8_t>& packet){
	if(std::memcmp(snapshot.magic, "GXF1", 4) != 0 ||
		snapshot.version != 1 ||
		snapshot.structureBytes != sizeof(FaceSharedMemoryV1) ||
		snapshot.parameterCount != FaceWeightCount ||
		snapshot.confidenceCount != FaceConfidenceCount ||
		snapshot.state > static_cast<std::uint8_t>(FaceState::Tracking) ||
		snapshot.source > static_cast<std::uint8_t>(FaceSource::Multimodal) ||
		!std::all_of(
			std::begin(snapshot.weights),
			std::end(snapshot.weights),
			ValidUnitInterval) ||
		!std::all_of(
			std::begin(snapshot.confidence),
			std::end(snapshot.confidence),
			ValidUnitInterval)){
		return false;
	}

	std::vector<std::uint8_t> encoded;
	encoded.reserve(MaximumFaceFrameBytes);
	AppendPaddedString(encoded, FaceFrameAddress);
	std::string typeTags = ",iiihhh";
	typeTags.append(FaceWeightCount + FaceConfidenceCount, 'f');
	AppendPaddedString(encoded, typeTags);
	AppendU32(encoded, snapshot.flags & 1U ? 1U : 0U);
	AppendU32(encoded, snapshot.state);
	AppendU32(encoded, snapshot.source);
	AppendU64(encoded, snapshot.sampleSequence);
	AppendU64(encoded, static_cast<std::uint64_t>(snapshot.clientSampleMonotonicNs));
	AppendU64(encoded, static_cast<std::uint64_t>(snapshot.hostReceiveMonotonicNs));
	for(float value : snapshot.weights){
		AppendFloat(encoded, value);
	}
	for(float value : snapshot.confidence){
		AppendFloat(encoded, value);
	}
	if(encoded.size() > MaximumFaceFrameBytes){
		return false;
	}
	packet = std::move(encoded);
	return true;
}

} // namespace galaxyxr::osc
