#pragma once
#include "../Driver/DeviceShim.h"
#include "../Driver/DeviceProvider.h"
#include "../Driver/DriverLog.h"



class GenericHeadsetShim : public ShimDefinition{
public:
	GenericHeadsetShim(){
		// wrap the frame delivery component of foreign HMDs (e.g. vrlink / Steam Link)
		// so frames can be observed and later processed before the driver consumes them
		shimFrameComponent = true;
	}
	
	CustomHeadsetDeviceProvider* deviceProvider = nullptr;
	
	virtual void PosTrackedDeviceActivate(uint32_t &unObjectId, vr::EVRInitError &returnValue) override;
	
	virtual void HandleEvent(const vr::VREvent_t &event) override;
};