#include "GalaxyXREyePublisher.h"

#include "../Driver/DriverLog.h"

#include <algorithm>
#include <cmath>

namespace galaxyxr {
namespace {

Vector3 Add(const Vector3& left, const Vector3& right){
	return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vector3 Subtract(const Vector3& left, const Vector3& right){
	return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vector3 Multiply(const Vector3& value, float scalar){
	return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float Dot(const Vector3& left, const Vector3& right){
	return left.x * right.x + left.y * right.y + left.z * right.z;
}

bool Normalize(Vector3& value){
	const float length = std::sqrt(Dot(value, value));
	if(!std::isfinite(length) || length < 1e-5f){
		return false;
	}
	value = Multiply(value, 1.0f / length);
	return true;
}

Vector3 RotateForward(const Quaternion& quaternion){
	// q * (0, 0, -1) * inverse(q)
	return {
		-2.0f * (quaternion.x * quaternion.z + quaternion.w * quaternion.y),
		-2.0f * (quaternion.y * quaternion.z - quaternion.w * quaternion.x),
		-1.0f + 2.0f * (quaternion.x * quaternion.x + quaternion.y * quaternion.y),
	};
}

bool EyeValid(const EyePose& eye){
	return eye.state == EyeState::Gazing && eye.orientationTracked;
}

void SetVector(vr::HmdVector3_t& target, const Vector3& source){
	target.v[0] = source.x;
	target.v[1] = source.y;
	target.v[2] = source.z;
}

} // namespace

bool GalaxyXREyePublisher::Configure(const EyePublisherConfiguration& value){
	std::lock_guard<std::mutex> lock(mutex);
	EyePublisherConfiguration requested = value;
	requested.fallbackDistanceMeters =
		std::max(0.25, std::min(10.0, requested.fallbackDistanceMeters));
	requested.staleAfterMs =
		std::max<std::uint32_t>(10, std::min<std::uint32_t>(1000, requested.staleAfterMs));
	if(requested.source != "off" &&
		requested.source != "synthetic" &&
		requested.source != "vrlink_compat" &&
		requested.source != "android_xr"){
		requested.source = "off";
	}
	const bool ownerChanged = requested.source != owner;
	if(componentCreated && ownerChanged){
		DriverLog(
			"GXR EyePublish: refusing runtime owner switch %s->%s after project component creation; tracked-device restart required to preserve one publisher",
			owner.c_str(),
			requested.source.c_str());
		requested.source = owner;
		configuration = requested;
		return false;
	}
	configuration = requested;
	owner = configuration.source;
	return ownerChanged;
}

bool GalaxyXREyePublisher::Bind(
	vr::PropertyContainerHandle_t value,
	std::uint32_t deviceId){
	std::lock_guard<std::mutex> lock(mutex);
	container = value;
	hmdDeviceId = deviceId;
	invalidPublished = false;
	if(owner == "off" || owner == "vrlink_compat"){
		DriverLog("GXR EyePublish: owner=%s, project component not created", owner.c_str());
		return true;
	}
	if(container == vr::k_ulInvalidPropertyContainer ||
		hmdDeviceId == vr::k_unTrackedDeviceIndexInvalid ||
		vr::VRDriverInput() == nullptr){
		DriverLog("GXR EyePublish: invalid activated HMD container or IVRDriverInput unavailable");
		return false;
	}
	if(componentCreated){
		return true;
	}
	vr::VRProperties()->SetBoolProperty(
		container,
		vr::Prop_SupportsXrEyeGazeInteraction_Bool,
		true);
	const vr::EVRInputError error = vr::VRDriverInput()->CreateEyeTrackingComponent(
		container,
		"/eyetracking",
		&component);
	if(error != vr::VRInputError_None ||
		component == vr::k_ulInvalidInputComponentHandle){
		DriverLog(
			"GXR EyePublish: CreateEyeTrackingComponent failed for HMD %u with error %d",
			hmdDeviceId,
			static_cast<int>(error));
		component = vr::k_ulInvalidInputComponentHandle;
		return false;
	}
	componentCreated = true;
	DriverLog(
		"GXR EyePublish: owner=%s component bound to activated HMD %u container %llu",
		owner.c_str(),
		hmdDeviceId,
		static_cast<unsigned long long>(container));
	return true;
}

void GalaxyXREyePublisher::Unbind(){
	std::lock_guard<std::mutex> lock(mutex);
	if(componentCreated){
		PublishInvalid(0.0);
	}
	container = vr::k_ulInvalidPropertyContainer;
	hmdDeviceId = vr::k_unTrackedDeviceIndexInvalid;
	component = vr::k_ulInvalidInputComponentHandle;
	componentCreated = false;
	invalidPublished = false;
	outputValid = false;
}

void GalaxyXREyePublisher::InvalidateSession(){
	std::lock_guard<std::mutex> lock(mutex);
	if(componentCreated){
		PublishInvalid(0.0);
	}
	invalidPublished = true;
	outputValid = false;
}

void GalaxyXREyePublisher::RunFrame(
	const TrackingSample* sample,
	const GalaxyXRClockSync& clock,
	std::int64_t nowHostMonotonicNs){
	std::lock_guard<std::mutex> lock(mutex);
	if(!componentCreated || vr::VRDriverInput() == nullptr){
		return;
	}
	if(owner == "synthetic"){
		PublishSynthetic(nowHostMonotonicNs);
		return;
	}
	if(owner != "android_xr" || sample == nullptr ||
		sample->baseSpaceId != 1 ||
		sample->coordinateRevision == 0){
		PublishInvalid(0.0);
		return;
	}

	std::int64_t sampleHostNs = 0;
	if(!clock.ClientToHost(sample->captureMonotonicNs, sampleHostNs)){
		PublishInvalid(0.0);
		return;
	}
	const std::int64_t ageNs = nowHostMonotonicNs - sampleHostNs;
	if(ageNs < -10000000LL ||
		ageNs > static_cast<std::int64_t>(configuration.staleAfterMs) * 1000000LL){
		PublishInvalid(0.0);
		return;
	}

	vr::VREyeTrackingData_t output{};
	if(!BuildGaze(*sample, configuration.fallbackDistanceMeters, output)){
		PublishInvalid(static_cast<double>(sampleHostNs - nowHostMonotonicNs) / 1000000000.0);
		return;
	}
	const double timeOffset =
		static_cast<double>(sampleHostNs - nowHostMonotonicNs) / 1000000000.0;
	const vr::EVRInputError error = vr::VRDriverInput()->UpdateEyeTrackingComponent(
		component,
		&output,
		timeOffset);
	if(error != vr::VRInputError_None){
		DriverLog("GXR EyePublish: UpdateEyeTrackingComponent failed with error %d", static_cast<int>(error));
	}
	invalidPublished = false;
	outputValid = error == vr::VRInputError_None;
}

bool GalaxyXREyePublisher::OwnsPublisher() const{
	std::lock_guard<std::mutex> lock(mutex);
	return componentCreated;
}

bool GalaxyXREyePublisher::IsOutputValid() const{
	std::lock_guard<std::mutex> lock(mutex);
	return outputValid;
}

std::string GalaxyXREyePublisher::Owner() const{
	std::lock_guard<std::mutex> lock(mutex);
	return owner;
}

bool GalaxyXREyePublisher::BuildGaze(
	const TrackingSample& sample,
	double fallbackDistanceMeters,
	vr::VREyeTrackingData_t& output){
	const bool leftValid = EyeValid(sample.leftEye);
	const bool rightValid = EyeValid(sample.rightEye);
	if(!leftValid && !rightValid){
		return false;
	}

	Vector3 leftDirection = RotateForward(sample.leftEye.orientation);
	Vector3 rightDirection = RotateForward(sample.rightEye.orientation);
	if((leftValid && !Normalize(leftDirection)) ||
		(rightValid && !Normalize(rightDirection))){
		return false;
	}

	Vector3 origin{};
	Vector3 target{};
	const float fallbackDistance = static_cast<float>(fallbackDistanceMeters);
	if(leftValid && rightValid){
		origin = Multiply(Add(sample.leftEye.origin, sample.rightEye.origin), 0.5f);
		const Vector3 betweenOrigins = Subtract(sample.leftEye.origin, sample.rightEye.origin);
		const float crossDot = Dot(leftDirection, rightDirection);
		const float denominator = 1.0f - crossDot * crossDot;
		bool converged = denominator > 1e-4f;
		float leftDistance = 0.0f;
		float rightDistance = 0.0f;
		if(converged){
			const float leftProjection = Dot(leftDirection, betweenOrigins);
			const float rightProjection = Dot(rightDirection, betweenOrigins);
			leftDistance = (crossDot * rightProjection - leftProjection) / denominator;
			rightDistance = (rightProjection - crossDot * leftProjection) / denominator;
			converged =
				std::isfinite(leftDistance) &&
				std::isfinite(rightDistance) &&
				leftDistance > 0.02f &&
				rightDistance > 0.02f &&
				leftDistance <= 10.0f &&
				rightDistance <= 10.0f;
		}
		if(converged){
			const Vector3 leftPoint = Add(sample.leftEye.origin, Multiply(leftDirection, leftDistance));
			const Vector3 rightPoint = Add(sample.rightEye.origin, Multiply(rightDirection, rightDistance));
			target = Multiply(Add(leftPoint, rightPoint), 0.5f);
		}else{
			Vector3 averageDirection = Add(leftDirection, rightDirection);
			if(!Normalize(averageDirection)){
				return false;
			}
			target = Add(origin, Multiply(averageDirection, fallbackDistance));
		}
	}else{
		const EyePose& eye = leftValid ? sample.leftEye : sample.rightEye;
		const Vector3& direction = leftValid ? leftDirection : rightDirection;
		origin = eye.origin;
		target = Add(origin, Multiply(direction, fallbackDistance));
	}

	output = {};
	output.bActive = true;
	output.bTracked = true;
	output.bValid = true;
	SetVector(output.vGazeOrigin, origin);
	SetVector(output.vGazeTarget, target);
	return true;
}

void GalaxyXREyePublisher::PublishInvalid(double timeOffset){
	if(invalidPublished || !componentCreated || vr::VRDriverInput() == nullptr){
		return;
	}
	vr::VREyeTrackingData_t output{};
	output.bActive = false;
	output.bTracked = false;
	output.bValid = false;
	const vr::EVRInputError error = vr::VRDriverInput()->UpdateEyeTrackingComponent(
		component,
		&output,
		timeOffset);
	if(error != vr::VRInputError_None){
		DriverLog(
			"GXR EyePublish: invalid-state UpdateEyeTrackingComponent failed with error %d",
			static_cast<int>(error));
	}
	invalidPublished = true;
	outputValid = false;
}

void GalaxyXREyePublisher::PublishSynthetic(std::int64_t nowHostMonotonicNs){
	const double phase =
		static_cast<double>(nowHostMonotonicNs % 4000000000LL) /
		4000000000.0 * 6.283185307179586;
	vr::VREyeTrackingData_t output{};
	output.bActive = true;
	output.bTracked = true;
	output.bValid = true;
	output.vGazeOrigin.v[0] = 0.0f;
	output.vGazeOrigin.v[1] = 0.0f;
	output.vGazeOrigin.v[2] = 0.0f;
	output.vGazeTarget.v[0] = static_cast<float>(std::sin(phase) * 0.35);
	output.vGazeTarget.v[1] = static_cast<float>(std::cos(phase) * 0.20);
	output.vGazeTarget.v[2] = -2.0f;
	const vr::EVRInputError error = vr::VRDriverInput()->UpdateEyeTrackingComponent(
		component,
		&output,
		0.0);
	if(error != vr::VRInputError_None){
		DriverLog(
			"GXR EyePublish: synthetic UpdateEyeTrackingComponent failed with error %d",
			static_cast<int>(error));
	}
	invalidPublished = false;
	outputValid = error == vr::VRInputError_None;
}

} // namespace galaxyxr
