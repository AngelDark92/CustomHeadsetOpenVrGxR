#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace galaxyxr {

// Versioned, read-only runtime snapshot for GUI/collector consumers. Pairing
// material, authentication tags, and biometric arrays must never enter this
// object.
class GalaxyXRStatus {
public:
	GalaxyXRStatus();
	void ConfigureFile(const std::string& path);
	void SetState(const std::string& name, const std::string& value);
	void SetCounter(const std::string& name, std::uint64_t value);
	void SetMetric(const std::string& name, double value);
	void RemoveState(const std::string& name);
	void RemoveCounter(const std::string& name);
	void RemoveMetric(const std::string& name);
	void RemoveByPrefix(const std::string& prefix);
	void Transition(
		const std::string& stage,
		const std::string& state,
		const std::string& safeError = {});
	std::string Serialize() const;
	bool Flush() const;
	static bool IsSnapshotStale(
		std::int64_t updatedUnixMs,
		std::int64_t nowUnixMs,
		std::int64_t staleAfterMs);

private:
	static bool IsSafeName(const std::string& name);
	static bool IsSafeReasonCode(const std::string& value);
	static std::string Escape(const std::string& value);
	static std::int64_t UnixTimeMs();
	static std::int64_t MonotonicTimeNs();

	mutable std::mutex mutex;
	mutable std::mutex ioMutex;
	std::string statusPath;
	std::string transitionStage = "initializing";
	std::string transitionState = "idle";
	std::string lastError;
	const std::int64_t startupUnixMs;
	const std::int64_t startupMonotonicNs;
	mutable std::int64_t updatedUnixMs;
	mutable std::int64_t updatedMonotonicNs;
	mutable std::uint64_t heartbeatSequence = 0;
	std::map<std::string, std::string> states;
	std::map<std::string, std::uint64_t> counters;
	std::map<std::string, double> metrics;
};

} // namespace galaxyxr
