#include "GalaxyXRFaceOutput.h"

#include "../Driver/DriverLog.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace galaxyxr {
namespace {

constexpr std::uint32_t FaceFlagValid = 1U << 0;

void InitializeSchema(FaceSharedMemoryV1& value){
	std::memset(&value, 0, sizeof(value));
	value.magic[0] = 'G';
	value.magic[1] = 'X';
	value.magic[2] = 'F';
	value.magic[3] = '1';
	value.version = 1;
	value.structureBytes = static_cast<std::uint16_t>(sizeof(value));
	value.parameterCount = static_cast<std::uint8_t>(FaceWeightCount);
	value.confidenceCount = static_cast<std::uint8_t>(FaceConfidenceCount);
}

} // namespace

static_assert(offsetof(FaceSharedMemoryV1, generation) == 8, "shared-memory generation must be aligned");
static_assert(sizeof(FaceSharedMemoryV1) < 512, "face shared-memory schema unexpectedly large");

GalaxyXRFaceOutput::~GalaxyXRFaceOutput(){
	Stop();
}

bool GalaxyXRFaceOutput::Start(){
	std::lock_guard<std::mutex> lock(mutex);
	InitializeSchema(localSnapshot);
#ifdef _WIN32
	if(mapped){
		return true;
	}
	HANDLE mapping = CreateFileMappingW(
		INVALID_HANDLE_VALUE,
		nullptr,
		PAGE_READWRITE,
		0,
		static_cast<DWORD>(sizeof(FaceSharedMemoryV1)),
		FaceSharedMemoryName);
	if(!mapping){
		DriverLog("GXR FaceOutput: CreateFileMappingW failed with error %lu", GetLastError());
		return false;
	}
	void* view = MapViewOfFile(mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(FaceSharedMemoryV1));
	if(!view){
		DriverLog("GXR FaceOutput: MapViewOfFile failed with error %lu", GetLastError());
		CloseHandle(mapping);
		return false;
	}
	mappingHandle = mapping;
	mapped = static_cast<FaceSharedMemoryV1*>(view);
	InitializeSchema(*mapped);
	DriverLog(
		"GXR FaceOutput: lossless Android XR 68-channel shared memory ready at %ls (schema v1, read-only consumers)",
		FaceSharedMemoryName);
#endif
	return true;
}

void GalaxyXRFaceOutput::Stop(){
	std::lock_guard<std::mutex> lock(mutex);
	// Publish a stable, nonzero invalid generation before unmapping. A reader
	// that owns the mapping can outlive this producer view and must not retain
	// the preceding valid biometric frame.
	InvalidateFaceSnapshot(localSnapshot);
#ifdef _WIN32
	if(mapped){
		CommitMappedSnapshot();
		UnmapViewOfFile(mapped);
		mapped = nullptr;
	}
	if(mappingHandle){
		CloseHandle(static_cast<HANDLE>(mappingHandle));
		mappingHandle = nullptr;
	}
#endif
}

void GalaxyXRFaceOutput::Publish(
	const std::array<std::uint8_t, SessionIdBytes>& sessionId,
	const TrackingSample& sample){
	std::lock_guard<std::mutex> lock(mutex);
	FaceSharedMemoryV1 next{};
	InitializeSchema(next);
	next.generation = localSnapshot.generation + 2;
	std::copy(sessionId.begin(), sessionId.end(), next.sessionId);
	next.sampleSequence = sample.sampleSequence;
	next.clientSampleMonotonicNs = sample.face.sampleMonotonicNs;
	next.hostReceiveMonotonicNs = sample.hostReceiveMonotonicNs;
	next.flags = sample.face.valid ? FaceFlagValid : 0;
	next.state = static_cast<std::uint8_t>(sample.face.state);
	next.source = static_cast<std::uint8_t>(sample.face.source);
	std::copy(sample.face.weights.begin(), sample.face.weights.end(), next.weights);
	std::copy(sample.face.confidence.begin(), sample.face.confidence.end(), next.confidence);
	localSnapshot = next;
#ifdef _WIN32
	if(mapped){
		CommitMappedSnapshot();
	}
#endif
}

void GalaxyXRFaceOutput::Invalidate(){
	std::lock_guard<std::mutex> lock(mutex);
	InvalidateFaceSnapshot(localSnapshot);
#ifdef _WIN32
	if(mapped){
		CommitMappedSnapshot();
	}
#endif
}

void GalaxyXRFaceOutput::CommitMappedSnapshot(){
#ifdef _WIN32
	if(!mapped){
		return;
	}
	const LONG64 stableGeneration =
		static_cast<LONG64>(localSnapshot.generation);
	InterlockedExchange64(
		reinterpret_cast<volatile LONG64*>(&mapped->generation),
		stableGeneration - 1);
	std::memcpy(
		reinterpret_cast<std::uint8_t*>(mapped) +
			offsetof(FaceSharedMemoryV1, sessionId),
		reinterpret_cast<const std::uint8_t*>(&localSnapshot) +
			offsetof(FaceSharedMemoryV1, sessionId),
		sizeof(localSnapshot) - offsetof(FaceSharedMemoryV1, sessionId));
	InterlockedExchange64(
		reinterpret_cast<volatile LONG64*>(&mapped->generation),
		stableGeneration);
#endif
}

bool GalaxyXRFaceOutput::Snapshot(FaceSharedMemoryV1& snapshot) const{
	std::lock_guard<std::mutex> lock(mutex);
	snapshot = localSnapshot;
	return snapshot.generation != 0;
}

std::wstring GalaxyXRFaceOutput::Name() const{
	return FaceSharedMemoryName;
}

} // namespace galaxyxr
