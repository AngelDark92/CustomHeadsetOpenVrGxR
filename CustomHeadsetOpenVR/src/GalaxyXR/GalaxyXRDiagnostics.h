#pragma once

#include "GalaxyXRClockSync.h"
#include "GalaxyXRProtocol.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace galaxyxr {

struct HostDiagnosticCounters {
	std::uint64_t acceptedControl = 0;
	std::uint64_t acceptedTracking = 0;
	std::uint64_t malformed = 0;
	std::uint64_t authenticationFailures = 0;
	std::uint64_t stale = 0;
	std::uint64_t duplicates = 0;
	std::uint64_t reordered = 0;
	std::uint64_t sequenceGaps = 0;
	std::uint64_t socketErrors = 0;
	std::uint64_t invalidEye = 0;
	std::uint64_t invalidFace = 0;
};

class GalaxyXRDiagnostics {
public:
	void SetLogRate(double rateHz);
	void CountDecodeFailure(DecodeError error);
	void CountAcceptedControl();
	void CountAcceptedTracking(std::uint64_t previousSequence, std::uint64_t sequence);
	void CountStale();
	void CountDuplicate();
	void CountReordered();
	void CountSocketError();
	void CountInvalidEye();
	void CountInvalidFace();
	HostDiagnosticCounters Snapshot() const;

	void LogSession(const SessionSnapshot& session);
	void LogCapabilities(const CapabilitySnapshot& capabilities);
	void LogClock(const ClockEstimate& estimate, bool accepted);
	void LogTracking(const TrackingSample& tracking, double ageMs);
	void LogPresentation(const PresentationSample& presentation);
	void LogFaceTongue(const FaceSample& face);
	void LogPose(
		std::uint32_t openVrId,
		std::uint32_t role,
		double horizonSeconds,
		double poseTimeOffsetSeconds,
		bool velocitySuppressed);

private:
	bool ShouldLog(std::atomic<std::int64_t>& lastLogNs);
	static std::int64_t NowNs();

	std::atomic<std::int64_t> logIntervalNs{500000000};
	std::atomic<std::int64_t> lastTrackingLogNs{0};
	std::atomic<std::int64_t> lastClockLogNs{0};
	std::atomic<std::int64_t> lastPresentationLogNs{0};
	std::atomic<std::int64_t> lastPoseLogNs{0};
	std::atomic<std::uint64_t> acceptedControl{0};
	std::atomic<std::uint64_t> acceptedTracking{0};
	std::atomic<std::uint64_t> malformed{0};
	std::atomic<std::uint64_t> authenticationFailures{0};
	std::atomic<std::uint64_t> stale{0};
	std::atomic<std::uint64_t> duplicates{0};
	std::atomic<std::uint64_t> reordered{0};
	std::atomic<std::uint64_t> sequenceGaps{0};
	std::atomic<std::uint64_t> socketErrors{0};
	std::atomic<std::uint64_t> invalidEye{0};
	std::atomic<std::uint64_t> invalidFace{0};
};

} // namespace galaxyxr
