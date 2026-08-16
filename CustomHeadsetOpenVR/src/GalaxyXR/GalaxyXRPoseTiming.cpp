#include "GalaxyXRPoseTiming.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace galaxyxr {
namespace {

bool FinitePoseInputs(const vr::DriverPose_t& pose){
	if(!std::isfinite(pose.qRotation.w) ||
		!std::isfinite(pose.qRotation.x) ||
		!std::isfinite(pose.qRotation.y) ||
		!std::isfinite(pose.qRotation.z)){
		return false;
	}
	for(std::size_t axis = 0; axis < 3; ++axis){
		if(!std::isfinite(pose.vecPosition[axis]) ||
			!std::isfinite(pose.vecVelocity[axis]) ||
			!std::isfinite(pose.vecAngularVelocity[axis]) ||
			std::fabs(pose.vecVelocity[axis]) > 20.0 ||
			std::fabs(pose.vecAngularVelocity[axis]) > 50.0){
			return false;
		}
	}
	return true;
}

vr::HmdQuaternion_t Multiply(
	const vr::HmdQuaternion_t& left,
	const vr::HmdQuaternion_t& right){
	return {
		left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
		left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
		left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
		left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
	};
}

bool Normalize(vr::HmdQuaternion_t& value){
	const double norm = std::sqrt(
		value.w * value.w + value.x * value.x +
		value.y * value.y + value.z * value.z);
	if(!std::isfinite(norm) || norm < 0.5 || norm > 1.5){
		return false;
	}
	value.w /= norm;
	value.x /= norm;
	value.y /= norm;
	value.z /= norm;
	return true;
}

bool IsPredictableRole(DeviceRole role){
	return role == DeviceRole::Hmd ||
		role == DeviceRole::LeftController ||
		role == DeviceRole::RightController ||
		role == DeviceRole::LeftHand ||
		role == DeviceRole::RightHand;
}

} // namespace

void GalaxyXRPoseTiming::Configure(const PoseTimingConfiguration& value){
	std::lock_guard<std::mutex> lock(mutex);
	configuration = value;
	configuration.fixedPredictionSeconds =
		std::max(0.0, std::min(0.100, configuration.fixedPredictionSeconds));
	configuration.maximumHmdPredictionSeconds =
		std::max(0.0, std::min(0.100, configuration.maximumHmdPredictionSeconds));
	configuration.maximumControllerPredictionSeconds =
		std::max(0.0, std::min(0.100, configuration.maximumControllerPredictionSeconds));
}

bool GalaxyXRPoseTiming::Apply(
	std::uint32_t openVrId,
	vr::DriverPose_t& pose,
	const GalaxyXRDeviceRegistry& registry,
	const GalaxyXRClockSync& clock,
	const PresentationSample* presentation,
	std::int64_t nowHostMonotonicNs,
	double& appliedHorizonSeconds) const{
	std::lock_guard<std::mutex> lock(mutex);
	appliedHorizonSeconds = 0.0;
	if(configuration.mode == "off" ||
		configuration.mode == "static_display_only" ||
		configuration.clientPoseAlreadyPredicted ||
		!pose.poseIsValid ||
		!FinitePoseInputs(pose)){
		return false;
	}

	const DeviceRole role = registry.RoleFor(openVrId);
	if(!IsPredictableRole(role)){
		return false;
	}
	const double maximumHorizon =
		role == DeviceRole::Hmd
		? configuration.maximumHmdPredictionSeconds
		: configuration.maximumControllerPredictionSeconds;

	double horizon = 0.0;
	if(configuration.mode == "fixed_pose"){
		horizon = configuration.fixedPredictionSeconds;
	}else if(configuration.mode == "adaptive_pose" && presentation){
		std::int64_t predictedHostNs = 0;
		if(!clock.ClientToHost(
			presentation->predictedDisplayMonotonicNs,
			predictedHostNs)){
			return false;
		}
		horizon = static_cast<double>(predictedHostNs - nowHostMonotonicNs) / 1000000000.0;
	}else{
		return false;
	}
	horizon = std::max(0.0, std::min(maximumHorizon, horizon));
	if(horizon <= 0.0){
		return false;
	}

	vr::DriverPose_t predictedPose = pose;
	for(std::size_t axis = 0; axis < 3; ++axis){
		predictedPose.vecPosition[axis] +=
			predictedPose.vecVelocity[axis] * horizon;
	}
	const double angularSpeed = std::sqrt(
		predictedPose.vecAngularVelocity[0] * predictedPose.vecAngularVelocity[0] +
		predictedPose.vecAngularVelocity[1] * predictedPose.vecAngularVelocity[1] +
		predictedPose.vecAngularVelocity[2] * predictedPose.vecAngularVelocity[2]);
	if(angularSpeed > 1e-6){
		const double halfAngle = angularSpeed * horizon * 0.5;
		const double scale = std::sin(halfAngle) / angularSpeed;
		const vr::HmdQuaternion_t delta{
			std::cos(halfAngle),
			predictedPose.vecAngularVelocity[0] * scale,
			predictedPose.vecAngularVelocity[1] * scale,
			predictedPose.vecAngularVelocity[2] * scale,
		};
		predictedPose.qRotation = Multiply(predictedPose.qRotation, delta);
		if(!Normalize(predictedPose.qRotation)){
			return false;
		}
	}
	predictedPose.poseTimeOffset += horizon;
	if(configuration.suppressVelocityWhenHostPredicted){
		std::fill(
			std::begin(predictedPose.vecVelocity),
			std::end(predictedPose.vecVelocity),
			0.0);
		std::fill(
			std::begin(predictedPose.vecAngularVelocity),
			std::end(predictedPose.vecAngularVelocity),
			0.0);
	}
	pose = predictedPose;
	appliedHorizonSeconds = horizon;
	return true;
}

} // namespace galaxyxr
