#include "GalaxyXRDiagnostics.h"

#include "../Driver/DriverLog.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace galaxyxr {

void GalaxyXRDiagnostics::SetLogRate(double rateHz){
	if(!std::isfinite(rateHz) || rateHz <= 0.0){
		logIntervalNs.store(0, std::memory_order_relaxed);
		return;
	}
	rateHz = std::max(0.1, std::min(10.0, rateHz));
	logIntervalNs.store(
		static_cast<std::int64_t>(1000000000.0 / rateHz),
		std::memory_order_relaxed);
}

void GalaxyXRDiagnostics::CountDecodeFailure(DecodeError error){
	if(error == DecodeError::AuthenticationFailed ||
		error == DecodeError::AuthenticationUnavailable){
		authenticationFailures.fetch_add(1, std::memory_order_relaxed);
	}else{
		malformed.fetch_add(1, std::memory_order_relaxed);
	}
}

void GalaxyXRDiagnostics::CountAcceptedControl(){
	acceptedControl.fetch_add(1, std::memory_order_relaxed);
}

void GalaxyXRDiagnostics::CountAcceptedTracking(
	std::uint64_t previousSequence,
	std::uint64_t sequence){
	acceptedTracking.fetch_add(1, std::memory_order_relaxed);
	if(previousSequence != 0 && sequence > previousSequence + 1){
		sequenceGaps.fetch_add(sequence - previousSequence - 1, std::memory_order_relaxed);
	}
}

void GalaxyXRDiagnostics::CountStale(){ stale.fetch_add(1, std::memory_order_relaxed); }
void GalaxyXRDiagnostics::CountDuplicate(){ duplicates.fetch_add(1, std::memory_order_relaxed); }
void GalaxyXRDiagnostics::CountReordered(){ reordered.fetch_add(1, std::memory_order_relaxed); }
void GalaxyXRDiagnostics::CountSocketError(){ socketErrors.fetch_add(1, std::memory_order_relaxed); }
void GalaxyXRDiagnostics::CountInvalidEye(){ invalidEye.fetch_add(1, std::memory_order_relaxed); }
void GalaxyXRDiagnostics::CountInvalidFace(){ invalidFace.fetch_add(1, std::memory_order_relaxed); }

HostDiagnosticCounters GalaxyXRDiagnostics::Snapshot() const{
	HostDiagnosticCounters value;
	value.acceptedControl = acceptedControl.load(std::memory_order_relaxed);
	value.acceptedTracking = acceptedTracking.load(std::memory_order_relaxed);
	value.malformed = malformed.load(std::memory_order_relaxed);
	value.authenticationFailures = authenticationFailures.load(std::memory_order_relaxed);
	value.stale = stale.load(std::memory_order_relaxed);
	value.duplicates = duplicates.load(std::memory_order_relaxed);
	value.reordered = reordered.load(std::memory_order_relaxed);
	value.sequenceGaps = sequenceGaps.load(std::memory_order_relaxed);
	value.socketErrors = socketErrors.load(std::memory_order_relaxed);
	value.invalidEye = invalidEye.load(std::memory_order_relaxed);
	value.invalidFace = invalidFace.load(std::memory_order_relaxed);
	return value;
}

void GalaxyXRDiagnostics::LogSession(const SessionSnapshot& session){
	DriverLog(
		"GXR Session: id=<redacted> package=%s versionCode=%u device=%s/%s serial=%s protocol=1.0",
		session.client.packageName.c_str(),
		session.client.versionCode,
		session.client.manufacturer.c_str(),
		session.client.model.c_str(),
		session.client.deviceSerial.empty() ? "<unset>" : session.client.deviceSerial.c_str());
}

void GalaxyXRDiagnostics::LogCapabilities(const CapabilitySnapshot& capabilities){
	DriverLog(
		"GXR Capability: revision=%u runtimeExtensions=%s views=%u refresh=%.3f featureBits=0x%llX permissions=0x%X",
		capabilities.revision,
		capabilities.enabledExtensions.c_str(),
		static_cast<unsigned>(capabilities.views.size()),
		capabilities.currentRefreshHz,
		static_cast<unsigned long long>(capabilities.featureBits),
		capabilities.permissionFlags);
	for(std::size_t index = 0; index < capabilities.views.size(); ++index){
		const ViewGeometry& view = capabilities.views[index];
		DriverLog(
			"GXR View[%u]: rec=%ux%u max=%ux%u samples=%u fov=[%.5f,%.5f,%.5f,%.5f]",
			static_cast<unsigned>(index),
			view.recommendedWidth,
			view.recommendedHeight,
			view.maximumWidth,
			view.maximumHeight,
			view.recommendedSampleCount,
			view.fovLeft,
			view.fovRight,
			view.fovUp,
			view.fovDown);
	}
}

void GalaxyXRDiagnostics::LogClock(const ClockEstimate& estimate, bool accepted){
	if(!ShouldLog(lastClockLogNs)){
		return;
	}
	DriverLog(
		"GXR Clock: offsetMs=%.4f driftPpm=%.3f rttMs=%.4f residualMs=%.4f uncertaintyMs=%.4f accepted=%d valid=%d",
		estimate.offsetNs / 1000000.0,
		estimate.driftPpm,
		estimate.rttNs / 1000000.0,
		estimate.residualNs / 1000000.0,
		estimate.uncertaintyNs / 1000000.0,
		accepted ? 1 : 0,
		estimate.valid ? 1 : 0);
}

void GalaxyXRDiagnostics::LogTracking(const TrackingSample& tracking, double ageMs){
	if(!ShouldLog(lastTrackingLogNs)){
		return;
	}
	DriverLog(
		"GXR EyeRx: seq=%llu ageMs=%.3f mode=%u left=%u right=%u coordinateRevision=%u",
		static_cast<unsigned long long>(tracking.sampleSequence),
		ageMs,
		static_cast<unsigned>(tracking.eyeMode),
		static_cast<unsigned>(tracking.leftEye.state),
		static_cast<unsigned>(tracking.rightEye.state),
		tracking.coordinateRevision);
	LogFaceTongue(tracking.face);
}

void GalaxyXRDiagnostics::LogPresentation(const PresentationSample& presentation){
	if(!ShouldLog(lastPresentationLogNs)){
		return;
	}
	DriverLog(
		"GXR Display: submitted=%ux%u decode=%ux%u surface=%ux%u clientFrame=%llu correlationFlags=0x%X",
		presentation.submittedWidth,
		presentation.submittedHeight,
		presentation.decoderWidth,
		presentation.decoderHeight,
		presentation.surfaceWidth,
		presentation.surfaceHeight,
		static_cast<unsigned long long>(presentation.clientFrameCounter),
		presentation.flags);
}

void GalaxyXRDiagnostics::LogFaceTongue(const FaceSample& face){
	DriverLog(
		"GXR FaceRx: valid=%d state=%u count=68 conf=[%.3f,%.3f,%.3f]",
		face.valid ? 1 : 0,
		static_cast<unsigned>(face.state),
		face.confidence[0],
		face.confidence[1],
		face.confidence[2]);
	DriverLog(
		"GXR Tongue: out=%.3f left=%.3f right=%.3f up=%.3f down=%.3f",
		face.weights[63],
		face.weights[64],
		face.weights[65],
		face.weights[66],
		face.weights[67]);
}

void GalaxyXRDiagnostics::LogPose(
	std::uint32_t openVrId,
	std::uint32_t role,
	double horizonSeconds,
	double poseTimeOffsetSeconds,
	bool velocitySuppressed){
	if(!ShouldLog(lastPoseLogNs)){
		return;
	}
	DriverLog(
		"GXR Pose: device=%u role=%u horizonMs=%.3f poseOffsetSec=%.6f velocitySuppressed=%d",
		openVrId,
		role,
		horizonSeconds * 1000.0,
		poseTimeOffsetSeconds,
		velocitySuppressed ? 1 : 0);
}

bool GalaxyXRDiagnostics::ShouldLog(std::atomic<std::int64_t>& lastLogNs){
	const std::int64_t interval = logIntervalNs.load(std::memory_order_relaxed);
	if(interval <= 0){
		return false;
	}
	const std::int64_t now = NowNs();
	std::int64_t previous = lastLogNs.load(std::memory_order_relaxed);
	return now - previous >= interval &&
		lastLogNs.compare_exchange_strong(previous, now, std::memory_order_relaxed);
}

std::int64_t GalaxyXRDiagnostics::NowNs(){
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace galaxyxr
