#include "GalaxyXRClockSync.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace galaxyxr {
namespace {

constexpr std::size_t MaximumSamples = 32;
constexpr std::size_t MinimumSamples = 4;
constexpr double MaximumRttNs = 250.0 * 1000.0 * 1000.0;
constexpr double MaximumDriftPpm = 200.0;
constexpr double MaximumOffsetResidualNs = 20.0 * 1000.0 * 1000.0;

double Median(std::vector<double> values){
	if(values.empty()){
		return 0.0;
	}
	const std::size_t middle = values.size() / 2;
	std::nth_element(values.begin(), values.begin() + middle, values.end());
	const double upper = values[middle];
	if((values.size() & 1) != 0){
		return upper;
	}
	std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
	return (values[middle - 1] + upper) * 0.5;
}

} // namespace

bool GalaxyXRClockSync::AddExchange(
	std::int64_t t0HostSendNs,
	std::int64_t t1ClientReceiveNs,
	std::int64_t t2ClientSendNs,
	std::int64_t t3HostReceiveNs){
	if(t0HostSendNs <= 0 ||
		t1ClientReceiveNs <= 0 ||
		t2ClientSendNs < t1ClientReceiveNs ||
		t3HostReceiveNs < t0HostSendNs){
		std::lock_guard<std::mutex> lock(mutex);
		++estimate.rejectedSamples;
		return false;
	}

	const double rttNs =
		static_cast<double>(t3HostReceiveNs - t0HostSendNs) -
		static_cast<double>(t2ClientSendNs - t1ClientReceiveNs);
	const double offsetNs =
		(static_cast<double>(t1ClientReceiveNs - t0HostSendNs) +
			static_cast<double>(t2ClientSendNs - t3HostReceiveNs)) * 0.5;
	const double hostMidpointNs =
		(static_cast<double>(t0HostSendNs) + static_cast<double>(t3HostReceiveNs)) * 0.5;
	if(!std::isfinite(rttNs) || !std::isfinite(offsetNs) ||
		rttNs < 0.0 || rttNs > MaximumRttNs){
		std::lock_guard<std::mutex> lock(mutex);
		++estimate.rejectedSamples;
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if(samples.size() >= MinimumSamples){
		std::vector<double> rtts;
		rtts.reserve(samples.size());
		for(const Sample& sample : samples){ rtts.push_back(sample.rttNs); }
		const double medianRtt = Median(rtts);
		std::vector<double> deviations;
		deviations.reserve(rtts.size());
		for(double value : rtts){ deviations.push_back(std::fabs(value - medianRtt)); }
		const double mad = Median(deviations);
		const double rttLimit = medianRtt + std::max(3.0 * mad, 2.0 * 1000.0 * 1000.0);
		if(rttNs > rttLimit){
			++estimate.rejectedSamples;
			return false;
		}
		if(estimate.valid){
			const double elapsed = hostMidpointNs - referenceHostNs;
			const double predictedOffset =
				estimate.offsetNs + elapsed * estimate.driftPpm / 1000000.0;
			if(std::fabs(offsetNs - predictedOffset) > MaximumOffsetResidualNs){
				++estimate.rejectedSamples;
				return false;
			}
		}
	}

	samples.push_back({hostMidpointNs, offsetNs, rttNs});
	if(samples.size() > MaximumSamples){
		samples.pop_front();
	}
	++estimate.acceptedSamples;
	RecalculateLocked();
	return true;
}

bool GalaxyXRClockSync::ClientToHost(
	std::int64_t clientMonotonicNs,
	std::int64_t& hostMonotonicNs) const{
	std::lock_guard<std::mutex> lock(mutex);
	if(!estimate.valid || clientMonotonicNs <= 0){
		return false;
	}

	// offset = client - host. Solve the bounded linear model for host time.
	const double drift = estimate.driftPpm / 1000000.0;
	const double numerator =
		static_cast<double>(clientMonotonicNs) -
		estimate.offsetNs +
		referenceHostNs * drift;
	const double converted = numerator / (1.0 + drift);
	if(!std::isfinite(converted) ||
		converted < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
		converted > static_cast<double>(std::numeric_limits<std::int64_t>::max())){
		return false;
	}
	hostMonotonicNs = static_cast<std::int64_t>(std::llround(converted));
	return true;
}

ClockEstimate GalaxyXRClockSync::GetEstimate() const{
	std::lock_guard<std::mutex> lock(mutex);
	return estimate;
}

void GalaxyXRClockSync::Reset(){
	std::lock_guard<std::mutex> lock(mutex);
	samples.clear();
	estimate = {};
	referenceHostNs = 0.0;
}

void GalaxyXRClockSync::RecalculateLocked(){
	if(samples.empty()){
		estimate.valid = false;
		return;
	}

	auto best = std::min_element(samples.begin(), samples.end(), [](const Sample& left, const Sample& right){
		return left.rttNs < right.rttNs;
	});
	referenceHostNs = best->hostMidpointNs;

	double meanX = 0.0;
	double meanY = 0.0;
	for(const Sample& sample : samples){
		meanX += sample.hostMidpointNs - referenceHostNs;
		meanY += sample.offsetNs;
	}
	meanX /= static_cast<double>(samples.size());
	meanY /= static_cast<double>(samples.size());

	double covariance = 0.0;
	double variance = 0.0;
	for(const Sample& sample : samples){
		const double x = sample.hostMidpointNs - referenceHostNs - meanX;
		const double y = sample.offsetNs - meanY;
		covariance += x * y;
		variance += x * x;
	}
	double slope = variance > 1.0 ? covariance / variance : 0.0;
	slope = std::max(-MaximumDriftPpm / 1000000.0, std::min(MaximumDriftPpm / 1000000.0, slope));
	const double interceptAtReference = meanY - slope * meanX;

	std::vector<double> residuals;
	std::vector<double> rtts;
	residuals.reserve(samples.size());
	rtts.reserve(samples.size());
	for(const Sample& sample : samples){
		const double predicted = interceptAtReference +
			(sample.hostMidpointNs - referenceHostNs) * slope;
		residuals.push_back(std::fabs(sample.offsetNs - predicted));
		rtts.push_back(sample.rttNs);
	}

	estimate.offsetNs = interceptAtReference;
	estimate.driftPpm = slope * 1000000.0;
	estimate.rttNs = Median(rtts);
	estimate.residualNs = Median(residuals);
	estimate.uncertaintyNs = estimate.rttNs * 0.5 + estimate.residualNs;
	estimate.valid = samples.size() >= MinimumSamples;
}

} // namespace galaxyxr
