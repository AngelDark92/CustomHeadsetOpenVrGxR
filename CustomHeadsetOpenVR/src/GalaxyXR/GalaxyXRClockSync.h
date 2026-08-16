#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace galaxyxr {

struct ClockEstimate {
	bool valid = false;
	double offsetNs = 0.0;
	double driftPpm = 0.0;
	double rttNs = 0.0;
	double residualNs = 0.0;
	double uncertaintyNs = 0.0;
	std::uint32_t acceptedSamples = 0;
	std::uint32_t rejectedSamples = 0;
};

class GalaxyXRClockSync {
public:
	bool AddExchange(
		std::int64_t t0HostSendNs,
		std::int64_t t1ClientReceiveNs,
		std::int64_t t2ClientSendNs,
		std::int64_t t3HostReceiveNs);
	bool ClientToHost(std::int64_t clientMonotonicNs, std::int64_t& hostMonotonicNs) const;
	ClockEstimate GetEstimate() const;
	void Reset();

private:
	struct Sample {
		double hostMidpointNs = 0.0;
		double offsetNs = 0.0;
		double rttNs = 0.0;
	};

	void RecalculateLocked();

	mutable std::mutex mutex;
	std::deque<Sample> samples;
	ClockEstimate estimate;
	double referenceHostNs = 0.0;
};

} // namespace galaxyxr
