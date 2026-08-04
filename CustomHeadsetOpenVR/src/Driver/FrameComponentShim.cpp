#include "FrameComponentShim.h"
#include "DriverLog.h"
#include "../Config/ConfigLoader.h"

// ---------------------------------------------------------------------------
// DirectModeComponentShim
// ---------------------------------------------------------------------------

DirectModeComponentShim::DirectModeComponentShim(vr::IVRDriverDirectModeComponent* original){
	this->original = original;
	DriverLog("FrameComponentShim: wrapping IVRDriverDirectModeComponent %p", (void*)original);
}

void DirectModeComponentShim::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet){
	original->CreateSwapTextureSet(unPid, pSwapTextureSetDesc, pOutSwapTextureSet);
	if(pSwapTextureSetDesc && pOutSwapTextureSet){
		// nFormat is a DXGI_FORMAT on D3D11. unTextureFlags tells us about
		// keyed-mutex / shared handle semantics (vr::VRSwapTextureFlag_*).
		DriverLog("FrameComponentShim: CreateSwapTextureSet pid=%u %ux%u format=%u samples=%u -> flags=%u handles=%llx %llx %llx",
			unPid,
			pSwapTextureSetDesc->nWidth, pSwapTextureSetDesc->nHeight,
			pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nSampleCount,
			pOutSwapTextureSet->unTextureFlags,
			(unsigned long long)pOutSwapTextureSet->rSharedTextureHandles[0],
			(unsigned long long)pOutSwapTextureSet->rSharedTextureHandles[1],
			(unsigned long long)pOutSwapTextureSet->rSharedTextureHandles[2]);
	}
}

void DirectModeComponentShim::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle){
	DriverLog("FrameComponentShim: DestroySwapTextureSet %llx", (unsigned long long)sharedTextureHandle);
	processor.EvictTexture(sharedTextureHandle);
	original->DestroySwapTextureSet(sharedTextureHandle);
}

void DirectModeComponentShim::DestroyAllSwapTextureSets(uint32_t unPid){
	DriverLog("FrameComponentShim: DestroyAllSwapTextureSets pid=%u", unPid);
	// handles are not tracked per pid, drop everything and let the caches repopulate
	processor.EvictAll();
	original->DestroyAllSwapTextureSets(unPid);
}

void DirectModeComponentShim::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t (*pIndices)[2]){
	original->GetNextSwapTextureSetIndex(sharedTextureHandles, pIndices);
}

void DirectModeComponentShim::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]){
	layersThisFrame++;
	if(VerboseFrame()){
		DriverLog("FrameComponentShim: SubmitLayer frame=%llu layer=%d tex=(%llx, %llx) depth=(%llx, %llx) boundsL=(%.3f %.3f %.3f %.3f) predict=%.4fs",
			(unsigned long long)frameCount, layersThisFrame,
			(unsigned long long)perEye[0].hTexture, (unsigned long long)perEye[1].hTexture,
			(unsigned long long)perEye[0].hDepthTexture, (unsigned long long)perEye[1].hDepthTexture,
			perEye[0].bounds.uMin, perEye[0].bounds.vMin, perEye[0].bounds.uMax, perEye[0].bounds.vMax,
			perEye[0].flHmdPosePredictionTimeInSecondsFromNow);
	}
	// capture the scene layer (first layer of the frame) for processing in Present.
	// later layers (e.g. the dashboard while it is open) are quads recomposited by
	// the driver at their own pose and are left untouched.
	if(layersThisFrame == 1){
		haveSceneLayer = true;
		sceneLeft = perEye[0].hTexture;
		sceneRight = perEye[1].hTexture;
		sceneLeftBounds = perEye[0].bounds;
		sceneRightBounds = perEye[1].bounds;
		// optional submit time processing, in case the driver already consumes the
		// layer during SubmitLayer. uses the previous frame's sync texture, which
		// stays constant across frames.
		FrameProcessSettings settings;
		bool processAtSubmit = false;
		if(GetActiveSettings(settings, processAtSubmit) && processAtSubmit && lastSyncTexture != 0){
			processor.ProcessSceneLayer(sceneLeft, sceneRight, sceneLeftBounds, sceneRightBounds, lastSyncTexture, settings);
			haveSceneLayer = false;
		}
	}
	original->SubmitLayer(perEye);
}

bool DirectModeComponentShim::GetActiveSettings(FrameProcessSettings &settings, bool &processAtSubmit){
	{
		std::lock_guard<std::mutex> configGuard(driverConfigLock);
		settings.config = driverConfig.streamFrame;
		processAtSubmit = driverConfig.streamFrame.processAtSubmitLayer;
		if(driverConfigLoader.info.isDashboardOpen && driverConfig.streamFrame.skipColorWhileDashboardOpen){
			settings.applyColor = false;
		}
	}
	const StreamFrameConfig &config = settings.config;
	// skip the whole pass when it would be an identity transform
	bool colorActive = settings.applyColor && (
		config.saturation != 50 ||
		config.contrast != 50 ||
		config.gamma != 2.2 ||
		config.colorMultiplier.r != 1.0 || config.colorMultiplier.g != 1.0 || config.colorMultiplier.b != 1.0 ||
		config.srgbMatrix.size() == 9);
	// cas and dither are not affected by the dashboard gating
	colorActive |= config.cas.enable || config.dither;
	bool remapActive = false;
	if(config.distortion.mode == "spline"){
		for(const auto &point : config.distortion.points){
			if(point.scale != 1.0){
				remapActive = true;
				break;
			}
		}
	}else{
		remapActive = config.k1 != 0 || config.k2 != 0;
	}
	return config.enable && (colorActive || remapActive);
}

void DirectModeComponentShim::Present(vr::SharedTextureHandle_t syncTexture){
	if(VerboseFrame()){
		DriverLog("FrameComponentShim: Present frame=%llu sync=%llx layersThisFrame=%d",
			(unsigned long long)frameCount, (unsigned long long)syncTexture, layersThisFrame);
	}else if(frameCount % 1000 == 0){
		// heartbeat so we can confirm the path is still active and see the
		// steady-state layer count (e.g. does the dashboard add a layer?)
		DriverLog("FrameComponentShim: Present heartbeat frame=%llu layersThisFrame=%d",
			(unsigned long long)frameCount, layersThisFrame);
	}
	lastSyncTexture = syncTexture;
	// process the scene layer before the driver consumes it
	if(haveSceneLayer){
		FrameProcessSettings settings;
		bool processAtSubmit = false;
		if(GetActiveSettings(settings, processAtSubmit) && !processAtSubmit){
			processor.ProcessSceneLayer(sceneLeft, sceneRight, sceneLeftBounds, sceneRightBounds, syncTexture, settings);
		}
		haveSceneLayer = false;
	}
	layersThisFrame = 0;
	frameCount++;
	original->Present(syncTexture);
}

void DirectModeComponentShim::PostPresent(const Throttling_t* pThrottling){
	original->PostPresent(pThrottling);
}

void DirectModeComponentShim::GetFrameTiming(vr::DriverDirectMode_FrameTiming* pFrameTiming){
	original->GetFrameTiming(pFrameTiming);
}

// ---------------------------------------------------------------------------
// VirtualDisplayShim
// ---------------------------------------------------------------------------

VirtualDisplayShim::VirtualDisplayShim(vr::IVRVirtualDisplay* original){
	this->original = original;
	DriverLog("FrameComponentShim: wrapping IVRVirtualDisplay %p", (void*)original);
}

void VirtualDisplayShim::Present(const vr::PresentInfo_t* pPresentInfo, uint32_t unPresentInfoSize){
	if(frameCount < 20 || frameCount % 1000 == 0){
		DriverLog("FrameComponentShim: VirtualDisplay Present frame=%llu backbuffer=%llx frameId=%llu",
			(unsigned long long)frameCount,
			pPresentInfo ? (unsigned long long)pPresentInfo->backbufferTextureHandle : 0ull,
			pPresentInfo ? (unsigned long long)pPresentInfo->nFrameId : 0ull);
	}
	frameCount++;
	// PHASE 2 (virtual display variant) GOES HERE: process the backbuffer
	// before the driver encodes it.
	original->Present(pPresentInfo, unPresentInfoSize);
}

void VirtualDisplayShim::WaitForPresent(){
	original->WaitForPresent();
}

bool VirtualDisplayShim::GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter){
	return original->GetTimeSinceLastVsync(pfSecondsSinceLastVsync, pulFrameCounter);
}
