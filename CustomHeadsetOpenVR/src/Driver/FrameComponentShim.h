#pragma once
#include "openvr_driver.h"
#include "FrameProcessor.h"

// Phase 1: pass-through wrappers around a foreign HMD driver's frame-delivery
// components (used for the vrlink / Steam Link HMD, e.g. Galaxy XR).
//
// Purpose right now is purely diagnostic: confirm which component vrcompositor
// actually fetches (IVRDriverDirectModeComponent vs IVRVirtualDisplay) and log
// the facts the phase-2 GPU processing pass must be designed around:
// swap texture formats and flags (keyed mutex or not), layers per frame,
// texture bounds, and pose prediction times.
//
// Phase 2 will insert a D3D11 pass (saturation + radial distortion
// pre-perturbation) on the scene layer textures inside SubmitLayer/Present
// before forwarding to the original component.

// Wraps IVRDriverDirectModeComponent (compiled against IVRDriverDirectModeComponent_009).
class DirectModeComponentShim : public vr::IVRDriverDirectModeComponent{
public:
	explicit DirectModeComponentShim(vr::IVRDriverDirectModeComponent* original);
	vr::IVRDriverDirectModeComponent* original;

	void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet) override;
	void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
	void DestroyAllSwapTextureSets(uint32_t unPid) override;
	void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]) override;
	void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override;
	void Present(vr::SharedTextureHandle_t syncTexture) override;
	void PostPresent(const Throttling_t* pThrottling) override;
	void GetFrameTiming(vr::DriverDirectMode_FrameTiming* pFrameTiming) override;

private:
	// layers submitted since the last Present, to learn the per-frame layer structure
	int layersThisFrame = 0;
	uint64_t frameCount = 0;
	// log verbosely only for the first frames to avoid spamming vrserver.txt
	bool VerboseFrame() const { return frameCount < 20; }

	// scene layer captured in SubmitLayer (the first layer of the frame) and
	// processed in Present under the sync texture keyed mutex
	bool haveSceneLayer = false;
	vr::SharedTextureHandle_t sceneLeft = 0;
	vr::SharedTextureHandle_t sceneRight = 0;
	vr::VRTextureBounds_t sceneLeftBounds = {};
	vr::VRTextureBounds_t sceneRightBounds = {};

	// sync texture from the previous Present, used when processing at submit time
	vr::SharedTextureHandle_t lastSyncTexture = 0;

	// snapshot current settings; returns false if processing is disabled or identity
	bool GetActiveSettings(FrameProcessSettings &settings, bool &processAtSubmit);
	// gaze prediction state (recent gaze angular motion, EMA smoothed)
	bool gazePrevValid = false;
	double gazePrevDir[3] = {0, 0, -1};
	double gazePrevTime = 0;
	double gazeVelEma[3] = {0, 0, 0};
	bool gazeSmoothValid = false;
	double gazeSmoothEma[3] = {0, 0, -1};

	// stationary dimming state: last hmd orientation basis, time of last
	// detected movement, and the current dim factor (0 bright .. 1 black)
	bool havePose = false;
	float lastPoseX[3] = {1, 0, 0};
	float lastPoseZ[3] = {0, 0, 1};
	double lastMovementTime = 0;
	double lastDimUpdateTime = 0;
	double dimFactor = 0;
	// track stillness from a submitted pose and advance the dim factor
	void UpdateStationaryDimming(const vr::HmdMatrix34_t &pose);

	FrameProcessor processor;
};

// Wraps IVRVirtualDisplay (compiled against IVRVirtualDisplay_002).
class VirtualDisplayShim : public vr::IVRVirtualDisplay{
public:
	explicit VirtualDisplayShim(vr::IVRVirtualDisplay* original);
	vr::IVRVirtualDisplay* original;

	void Present(const vr::PresentInfo_t* pPresentInfo, uint32_t unPresentInfoSize) override;
	void WaitForPresent() override;
	bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) override;

private:
	uint64_t frameCount = 0;
};
