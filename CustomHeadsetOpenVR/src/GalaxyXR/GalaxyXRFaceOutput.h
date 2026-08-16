#pragma once

#include "GalaxyXRTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace galaxyxr {

constexpr wchar_t FaceSharedMemoryName[] = L"Local\\CustomHeadsetOpenVR.GalaxyXR.Face.v1";

#pragma pack(push, 1)
struct FaceSharedMemoryV1 {
	char magic[4];
	std::uint16_t version;
	std::uint16_t structureBytes;
	std::int64_t generation;
	std::uint8_t sessionId[SessionIdBytes];
	std::uint64_t sampleSequence;
	std::int64_t clientSampleMonotonicNs;
	std::int64_t hostReceiveMonotonicNs;
	std::uint32_t flags;
	std::uint8_t state;
	std::uint8_t source;
	std::uint8_t parameterCount;
	std::uint8_t confidenceCount;
	float weights[FaceWeightCount];
	float confidence[FaceConfidenceCount];
};
#pragma pack(pop)

inline void InvalidateFaceSnapshot(FaceSharedMemoryV1& snapshot){
	snapshot.magic[0] = 'G';
	snapshot.magic[1] = 'X';
	snapshot.magic[2] = 'F';
	snapshot.magic[3] = '1';
	snapshot.version = 1;
	snapshot.structureBytes =
		static_cast<std::uint16_t>(sizeof(FaceSharedMemoryV1));
	const std::int64_t stableGeneration =
		snapshot.generation > 0
		? (snapshot.generation & ~std::int64_t{1}) + 2
		: 2;
	snapshot.generation = stableGeneration;
	std::fill_n(snapshot.sessionId, SessionIdBytes, std::uint8_t{0});
	snapshot.sampleSequence = 0;
	snapshot.clientSampleMonotonicNs = 0;
	snapshot.hostReceiveMonotonicNs = 0;
	snapshot.flags = 0;
	snapshot.state = static_cast<std::uint8_t>(FaceState::Stopped);
	snapshot.source = 0;
	snapshot.parameterCount = static_cast<std::uint8_t>(FaceWeightCount);
	snapshot.confidenceCount =
		static_cast<std::uint8_t>(FaceConfidenceCount);
	std::fill_n(snapshot.weights, FaceWeightCount, 0.0f);
	std::fill_n(snapshot.confidence, FaceConfidenceCount, 0.0f);
}

class GalaxyXRFaceOutput {
public:
	~GalaxyXRFaceOutput();

	bool Start();
	void Stop();
	void Publish(
		const std::array<std::uint8_t, SessionIdBytes>& sessionId,
		const TrackingSample& sample);
	void Invalidate();
	bool Snapshot(FaceSharedMemoryV1& snapshot) const;
	std::wstring Name() const;

private:
	void CommitMappedSnapshot();
	mutable std::mutex mutex;
	FaceSharedMemoryV1 localSnapshot{};
#ifdef _WIN32
	void* mappingHandle = nullptr;
	FaceSharedMemoryV1* mapped = nullptr;
#endif
};

} // namespace galaxyxr
