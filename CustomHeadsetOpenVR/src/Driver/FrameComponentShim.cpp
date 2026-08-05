#include "FrameComponentShim.h"
#include "DriverLog.h"
#include "EyeTrackingTap.h"
#include "DeviceProvider.h"
#include "../Config/ConfigLoader.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ctime>
#include "nlohmann/json.hpp"

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
		UpdateStationaryDimming(perEye[0].mHmdPose);
		UpdateSwimProbePose(perEye[0].mHmdPose);
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
			MaybeLogSwimProbe(settings);
			processor.ProcessSceneLayer(sceneLeft, sceneRight, sceneLeftBounds, sceneRightBounds, lastSyncTexture, settings);
			haveSceneLayer = false;
		}
	}
	original->SubmitLayer(perEye);
}

static double NowSeconds(){
	return std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DirectModeComponentShim::UpdateStationaryDimming(const vr::HmdMatrix34_t &pose){
	StreamFrameDimmingConfig dimming;
	{
		std::lock_guard<std::mutex> configGuard(driverConfigLock);
		dimming = driverConfig.streamFrame.stationaryDimming;
	}
	double now = NowSeconds();
	if(!dimming.enable){
		dimFactor = 0;
		lastMovementTime = now;
		lastDimUpdateTime = now;
		return;
	}
	// orientation basis vectors of the pose
	float x[3] = { pose.m[0][0], pose.m[1][0], pose.m[2][0] };
	float z[3] = { pose.m[0][2], pose.m[1][2], pose.m[2][2] };
	if(havePose){
		float dotX = x[0] * lastPoseX[0] + x[1] * lastPoseX[1] + x[2] * lastPoseX[2];
		float dotZ = z[0] * lastPoseZ[0] + z[1] * lastPoseZ[1] + z[2] * lastPoseZ[2];
		float minDot = dotX < dotZ ? dotX : dotZ;
		if(minDot > 1.0f){ minDot = 1.0f; }
		double angleDegrees = std::acos((double)minDot) * 180.0 / 3.14159265358979;
		if(angleDegrees > dimming.movementThreshold){
			lastMovementTime = now;
			lastPoseX[0] = x[0]; lastPoseX[1] = x[1]; lastPoseX[2] = x[2];
			lastPoseZ[0] = z[0]; lastPoseZ[1] = z[1]; lastPoseZ[2] = z[2];
		}
	}else{
		havePose = true;
		lastMovementTime = now;
		lastPoseX[0] = x[0]; lastPoseX[1] = x[1]; lastPoseX[2] = x[2];
		lastPoseZ[0] = z[0]; lastPoseZ[1] = z[1]; lastPoseZ[2] = z[2];
	}
	double delta = now - lastDimUpdateTime;
	if(delta < 0 || delta > 1){ delta = 0; }
	lastDimUpdateTime = now;
	bool still = now - lastMovementTime > dimming.movementTime;
	if(still){
		double rate = dimming.dimSeconds > 0.01 ? 1.0 / dimming.dimSeconds : 100.0;
		dimFactor += delta * rate;
	}else{
		double rate = dimming.brightenSeconds > 0.01 ? 1.0 / dimming.brightenSeconds : 100.0;
		dimFactor -= delta * rate;
	}
	if(dimFactor < 0){ dimFactor = 0; }
	if(dimFactor > 1){ dimFactor = 1; }
}

void DirectModeComponentShim::UpdateSwimProbePose(const vr::HmdMatrix34_t &pose){
	bool calibDot;
	{
		std::lock_guard<std::mutex> configGuard(driverConfigLock);
		// probeCapture is the one-switch scoring mode: it implies the dot
		calibDot = driverConfig.streamFrame.eyeGaze.calibDot
			|| driverConfig.streamFrame.eyeGaze.probeCapture;
	}
	double now = NowSeconds();
	// orientation basis columns of the render pose (head basis in world)
	float x[3] = { pose.m[0][0], pose.m[1][0], pose.m[2][0] };
	float y[3] = { pose.m[0][1], pose.m[1][1], pose.m[2][1] };
	float z[3] = { pose.m[0][2], pose.m[1][2], pose.m[2][2] };
	// keep the full basis for the world-locked calibration grid
	for(int i = 0; i < 3; i++){
		headBasisW[0][i] = x[i];
		headBasisW[1][i] = y[i];
		headBasisW[2][i] = z[i];
	}
	headBasisValid = true;
	// head angular velocity between successive submitted render poses, so
	// the probe can reject or regress high-velocity samples. the larger of
	// the x/z basis rotations bounds the true rotation well enough here.
	if(probePoseValid){
		double dt = now - probePrevTime;
		if(dt > 0.0001 && dt < 0.5){
			double dotX = x[0] * probePrevX[0] + x[1] * probePrevX[1] + x[2] * probePrevX[2];
			double dotZ = z[0] * probePrevZ[0] + z[1] * probePrevZ[1] + z[2] * probePrevZ[2];
			double minDot = dotX < dotZ ? dotX : dotZ;
			if(minDot > 1.0){ minDot = 1.0; }
			if(minDot < -1.0){ minDot = -1.0; }
			headVelDegS = std::acos(minDot) * 180.0 / 3.14159265358979 / dt;
		}
	}
	probePoseValid = true;
	probePrevTime = now;
	probePrevX[0] = x[0]; probePrevX[1] = x[1]; probePrevX[2] = x[2];
	probePrevZ[0] = z[0]; probePrevZ[1] = z[1]; probePrevZ[2] = z[2];
	if(!calibDot){
		// toggling the dot off clears the latch, so the next enable
		// re-centers it on the current view direction
		dotLatched = false;
		dotHeadValid = false;
		return;
	}
	if(!dotLatched){
		dotLatched = true;
		// head forward (0, 0, -1) in world space is the negated z basis.
		// direction only (dot at infinity): rotate the head in place while
		// probing; translation would add parallax the dot cannot show.
		dotWorldDir[0] = -z[0];
		dotWorldDir[1] = -z[1];
		dotWorldDir[2] = -z[2];
		DriverLog("SwimProbe: fixation dot latched, world dir=(%.4f, %.4f, %.4f)",
			dotWorldDir[0], dotWorldDir[1], dotWorldDir[2]);
	}
	// world -> head is the transpose of the rotation (columns are the head
	// basis): each head component is the dot with a basis column
	dotHeadDir[0] = x[0] * dotWorldDir[0] + x[1] * dotWorldDir[1] + x[2] * dotWorldDir[2];
	dotHeadDir[1] = y[0] * dotWorldDir[0] + y[1] * dotWorldDir[1] + y[2] * dotWorldDir[2];
	dotHeadDir[2] = z[0] * dotWorldDir[0] + z[1] * dotWorldDir[1] + z[2] * dotWorldDir[2];
	dotHeadValid = true;
}

void DirectModeComponentShim::MaybeLogSwimProbe(const FrameProcessSettings &settings){
	bool capture = settings.config.eyeGaze.probeCapture;
	if(!(settings.config.eyeGaze.swimProbe || capture) || !(settings.config.eyeGaze.calibDot || capture)){
		return;
	}
	if(!settings.dotValid || !settings.gazeValid){
		return;
	}
	double now = NowSeconds();
	if(now - lastSwimProbeLogTime < 0.05){
		return;
	}
	lastSwimProbeLogTime = now;
	// angular residuals between the gaze and the dot, both in head space.
	// resid uses the smoothed/predicted gaze the correction consumes;
	// residRaw uses the gaze as published, which is what fitting wants
	// (the speed-adaptive smoothing lags during VOR).
	// NORMALIZE both vectors first: the published gaze target is only
	// approximately unit length (magnitude wobbles ~0.5%), and acos of a
	// non-unit dot product turns that into degrees of phantom residual
	// (0.4% magnitude error at perfect alignment reads as ~5 degrees, and
	// magnitudes above 1 clamp to an impossible exact 0).
	auto angleDeg = [](double ax, double ay, double az, double bx, double by, double bz){
		double na = std::sqrt(ax * ax + ay * ay + az * az);
		double nb = std::sqrt(bx * bx + by * by + bz * bz);
		if(na < 1e-6 || nb < 1e-6){
			return 0.0;
		}
		double d = (ax * bx + ay * by + az * bz) / (na * nb);
		if(d > 1.0){ d = 1.0; }
		if(d < -1.0){ d = -1.0; }
		return std::acos(d) * 180.0 / 3.14159265358979;
	};
	double resid = angleDeg(settings.gazeDirX, settings.gazeDirY, settings.gazeDirZ,
		settings.dotDirX, settings.dotDirY, settings.dotDirZ);
	double residRaw = angleDeg(settings.gazeRawDirX, settings.gazeRawDirY, settings.gazeRawDirZ,
		settings.dotDirX, settings.dotDirY, settings.dotDirZ);
	// published-target magnitude, to keep the vrlink normalization wobble
	// visible in the data (it is NOT exactly unit length)
	double gazeRawLen = std::sqrt(settings.gazeRawDirX * settings.gazeRawDirX
		+ settings.gazeRawDirY * settings.gazeRawDirY
		+ settings.gazeRawDirZ * settings.gazeRawDirZ);
	// per-eye lens uvs of dot and raw gaze through the shared mapping, so
	// residuals can be binned by where the rays cross the lens
	double dotUv[2][2] = {{-1, -1}, {-1, -1}};
	double gazeUv[2][2] = {{-1, -1}, {-1, -1}};
	for(int eye = 0; eye < 2; eye++){
		double u, v;
		if(MapHeadDirToEyeUv(settings, eye, settings.dotDirX, settings.dotDirY, settings.dotDirZ, u, v)){
			dotUv[eye][0] = u;
			dotUv[eye][1] = v;
		}
		if(MapHeadDirToEyeUv(settings, eye, settings.gazeRawDirX, settings.gazeRawDirY, settings.gazeRawDirZ, u, v)){
			gazeUv[eye][0] = u;
			gazeUv[eye][1] = v;
		}
	}
	DriverLog("SwimProbe: resid=%.3f residRaw=%.3f rawLen=%.4f headVel=%.1f ageMs=%.1f "
		"dotL=(%.4f, %.4f) dotR=(%.4f, %.4f) gazeL=(%.4f, %.4f) gazeR=(%.4f, %.4f) "
		"dotHead=(%.4f, %.4f, %.4f) gazeRawHead=(%.4f, %.4f, %.4f)",
		resid, residRaw, gazeRawLen, settings.headVelDegS, settings.gazeAgeMs,
		dotUv[0][0], dotUv[0][1], dotUv[1][0], dotUv[1][1],
		gazeUv[0][0], gazeUv[0][1], gazeUv[1][0], gazeUv[1][1],
		settings.dotDirX, settings.dotDirY, settings.dotDirZ,
		settings.gazeRawDirX, settings.gazeRawDirY, settings.gazeRawDirZ);
}


// ---------------------------------------------------------------------------
// interactive distortion tuner
// ---------------------------------------------------------------------------
// the eye-tracked VOR probe failed as a fitter: eye-tracker error grows with
// eccentricity at the same magnitude as the lens residual, the published gaze
// is a single cyclopean ray (per-eye fits are illusory), and ET coverage dies
// around r~0.36 while the swim lives out to the corners. the human visual
// system has none of these limits: motion/vernier sensitivity is arcminute
// level across the whole lens. so the human becomes the null detector: while
// tuning, the frame's distortion is replaced by a per-band working curve
// edited live from the controllers, against the warped angular grid, until
// each highlighted band stops swimming during slow head rotation.
//
// controls (confirmed vrlink surface): joystick y nudges the active band's
// scale (squared response for fine work near center), A steps the band
// outward, B inward, X cycles linked/left/right eye editing (the ring only
// draws in the eyes being edited), Y resets the band to its activation
// value, holding either grip ~1.5s saves an importable profile json.

void DirectModeComponentShim::UpdateTuner(FrameProcessSettings &settings){
	bool enable = settings.config.distortion.tune.enable;
	deviceProvider.SetTunerInputActive(enable);
	if(!enable){
		if(tuner.active){
			tuner.active = false;
			DriverLog("Tuner: disabled (working curve dropped; the saved/configured profile applies again)");
		}
		return;
	}
	double now = NowSeconds();
	if(!tuner.active){
		// sanitize band radii from config
		tuner.bandR.clear();
		for(double r : settings.config.distortion.tune.bands){
			if(r > 0.02 && r < 1.2){ tuner.bandR.push_back(r); }
		}
		std::sort(tuner.bandR.begin(), tuner.bandR.end());
		if(tuner.bandR.empty()){
			tuner.bandR = {0.15, 0.22, 0.30, 0.38, 0.46, 0.55, 0.65};
		}
		int n = (int)tuner.bandR.size();
		tuner.scaleL.assign(n, 1.0);
		tuner.scaleR.assign(n, 1.0);
		// initialize each band from the CURRENT effective curve (gain
		// applied), so tuning refines whatever profile is loaded instead
		// of discarding it. missing per-eye keys fall back to the base
		// curve, matching the lut bake's ResolveCurve semantics.
		const StreamFrameDistortionConfig &d = settings.config.distortion;
		bool spline = d.mode == "spline";
		for(int eye = 0; eye < 2; eye++){
			double baseK1 = settings.config.k1;
			double baseK2 = settings.config.k2;
			const std::vector<StreamFrameDistortionPoint>* pts = &d.points;
			if(d.perEye){
				auto found = d.curves.find(eye == 0 ? "left" : "right");
				if(found != d.curves.end()){
					baseK1 = found->second.k1;
					baseK2 = found->second.k2;
					pts = &found->second.points;
				}
			}
			std::vector<StreamFrameDistortionPoint> sorted = *pts;
			std::sort(sorted.begin(), sorted.end(),
				[](const StreamFrameDistortionPoint &a, const StreamFrameDistortionPoint &b){ return a.r < b.r; });
			for(int i = 0; i < n; i++){
				double r = tuner.bandR[i];
				double scale;
				if(spline){
					scale = EvaluateDistortionCurve(sorted, r);
				}else{
					double r2 = r * r;
					scale = 1.0 + baseK1 * r2 + baseK2 * r2 * r2;
				}
				scale = 1.0 + d.gain * (scale - 1.0);
				if(scale < 0.85){ scale = 0.85; }
				if(scale > 1.15){ scale = 1.15; }
				(eye == 0 ? tuner.scaleL : tuner.scaleR)[i] = scale;
			}
		}
		tuner.initL = tuner.scaleL;
		tuner.initR = tuner.scaleR;
		tuner.band = 0;
		tuner.eyeMode = 0;
		tuner.prevBandOut = tuner.prevBandIn = tuner.prevEyeToggle = tuner.prevReset = false;
		tuner.lastTime = now;
		tuner.gripWasHigh = false;
		tuner.savedThisHold = false;
		tuner.lastNudgeLogTime = 0;
		tuner.active = true;
		DriverLog("Tuner: ACTIVE bands=%d (r %.2f..%.2f), initialized from the current curve. "
			"Controls: stick Y = adjust band, A/B = band out/in, X = eye linked/L/R, Y = reset band, hold grip 1.5s = save profile",
			n, tuner.bandR.front(), tuner.bandR.back());
	}
	int n = (int)tuner.bandR.size();
	double dt = now - tuner.lastTime;
	tuner.lastTime = now;
	if(dt < 0 || dt > 0.5){ dt = 0; }
	CustomHeadsetDeviceProvider::TunerInputState in;
	deviceProvider.GetTunerInput(in);
	// discrete actions on rising edges
	auto edge = [](bool current, bool &previous){
		bool rising = current && !previous;
		previous = current;
		return rising;
	};
	if(edge(in.bandOut, tuner.prevBandOut) && tuner.band < n - 1){
		tuner.band++;
		DriverLog("Tuner: band %d/%d r=%.2f (L=%.4f R=%.4f)", tuner.band + 1, n,
			tuner.bandR[tuner.band], tuner.scaleL[tuner.band], tuner.scaleR[tuner.band]);
	}
	if(edge(in.bandIn, tuner.prevBandIn) && tuner.band > 0){
		tuner.band--;
		DriverLog("Tuner: band %d/%d r=%.2f (L=%.4f R=%.4f)", tuner.band + 1, n,
			tuner.bandR[tuner.band], tuner.scaleL[tuner.band], tuner.scaleR[tuner.band]);
	}
	if(edge(in.eyeToggle, tuner.prevEyeToggle)){
		tuner.eyeMode = (tuner.eyeMode + 1) % 3;
		DriverLog("Tuner: editing %s", tuner.eyeMode == 0 ? "BOTH eyes (linked)" : (tuner.eyeMode == 1 ? "LEFT eye" : "RIGHT eye"));
	}
	if(edge(in.resetBand, tuner.prevReset)){
		if(tuner.eyeMode != 2){ tuner.scaleL[tuner.band] = tuner.initL[tuner.band]; }
		if(tuner.eyeMode != 1){ tuner.scaleR[tuner.band] = tuner.initR[tuner.band]; }
		DriverLog("Tuner: band %d reset to activation value (L=%.4f R=%.4f)",
			tuner.band + 1, tuner.scaleL[tuner.band], tuner.scaleR[tuner.band]);
	}
	// stick nudge. analog by default (deadzone + squared response); when
	// stepSize > 0, deterministic fixed steps every 100ms while deflected
	// past halfway ("one click at a time" fine nulling)
	double y = in.stickY;
	double magnitude = fabs(y);
	double delta = 0;
	double stepSize = settings.config.distortion.tune.stepSize;
	if(stepSize > 0){
		if(stepSize > 0.05){ stepSize = 0.05; }
		if(magnitude > 0.5){
			if(now - tuner.lastStepTime >= 0.1){
				tuner.lastStepTime = now;
				delta = (y > 0 ? 1.0 : -1.0) * stepSize;
			}
		}else{
			// re-arm so the first step after a fresh deflection is immediate
			tuner.lastStepTime = now - 0.1;
		}
	}else{
		double deadzone = 0.2;
		if(magnitude > deadzone && dt > 0){
			double normalized = (magnitude - deadzone) / (1.0 - deadzone);
			if(normalized > 1.0){ normalized = 1.0; }
			double rate = settings.config.distortion.tune.rate;
			if(rate < 0.005){ rate = 0.005; }
			if(rate > 0.5){ rate = 0.5; }
			delta = (y > 0 ? 1.0 : -1.0) * normalized * normalized * rate * dt;
		}
	}
	if(delta != 0){
		auto apply = [&](std::vector<double> &scales){
			scales[tuner.band] += delta;
			if(scales[tuner.band] < 0.85){ scales[tuner.band] = 0.85; }
			if(scales[tuner.band] > 1.15){ scales[tuner.band] = 1.15; }
		};
		if(tuner.eyeMode != 2){ apply(tuner.scaleL); }
		if(tuner.eyeMode != 1){ apply(tuner.scaleR); }
		if(stepSize > 0 || now - tuner.lastNudgeLogTime > 0.3){
			tuner.lastNudgeLogTime = now;
			DriverLog("Tuner: band %d/%d r=%.2f L=%.4f R=%.4f", tuner.band + 1, n,
				tuner.bandR[tuner.band], tuner.scaleL[tuner.band], tuner.scaleR[tuner.band]);
		}
	}
	// hold either grip to save; latch so one hold saves exactly once
	if(in.grip > 0.8){
		if(!tuner.gripWasHigh){
			tuner.gripWasHigh = true;
			tuner.gripHoldStart = now;
			tuner.savedThisHold = false;
		}else if(!tuner.savedThisHold && now - tuner.gripHoldStart > 1.5){
			tuner.savedThisHold = true;
			SaveTunedProfile(settings);
		}
	}else if(in.grip < 0.5){
		tuner.gripWasHigh = false;
	}
	// ---- take over this frame's distortion with the working curves ----
	StreamFrameDistortionConfig &d = settings.config.distortion;
	auto buildPoints = [&](const std::vector<double> &scales){
		std::vector<StreamFrameDistortionPoint> points;
		StreamFrameDistortionPoint origin;
		origin.r = 0.0;
		origin.scale = 1.0;
		points.push_back(origin);
		for(int i = 0; i < n; i++){
			StreamFrameDistortionPoint point;
			point.r = tuner.bandR[i];
			point.scale = scales[i];
			points.push_back(point);
		}
		return points;
	};
	d.mode = "spline";
	d.perEye = true;
	d.perAxis = false;
	d.gain = 1.0;
	d.points.clear();
	d.annulus.enable = false;
	d.curves["left"].k1 = 0;
	d.curves["left"].k2 = 0;
	d.curves["left"].points = buildPoints(tuner.scaleL);
	d.curves["right"].k1 = 0;
	d.curves["right"].k2 = 0;
	d.curves["right"].points = buildPoints(tuner.scaleR);
	// force the calibration view on (warped angular grid + warped overlays)
	// unless the user opted to bring their own overlays (forceGrid off:
	// tune against game content, or the world-locked grid variant)
	if(settings.config.distortion.tune.forceGrid){
		settings.config.eyeGaze.debugGrid = true;
		settings.config.eyeGaze.overlayWarped = true;
	}
	// band highlight for the shader (per-eye gating in ProcessEye)
	settings.tuneActive = true;
	settings.tuneRingR = tuner.bandR[tuner.band];
	settings.tuneEyeMode = tuner.eyeMode;
}

void DirectModeComponentShim::SaveTunedProfile(const FrameProcessSettings &settings){
	using nlohmann::json;
	int n = (int)tuner.bandR.size();
	auto pointsJson = [&](const std::vector<double> &scales){
		json points = json::array();
		points.push_back({{"r", 0.0}, {"scale", 1.0}});
		for(int i = 0; i < n; i++){
			// round through text once so the file matches what the log shows
			points.push_back({{"r", tuner.bandR[i]}, {"scale", (double)((long long)(scales[i] * 100000.0 + (scales[i] >= 0 ? 0.5 : -0.5))) / 100000.0}});
		}
		return points;
	};
	char stamp[32];
	time_t rawTime = time(nullptr);
	struct tm timeInfo;
#ifdef _WIN32
	localtime_s(&timeInfo, &rawTime);
#else
	localtime_r(&rawTime, &timeInfo);
#endif
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &timeInfo);
	json curves = {
		{"left", {{"k1", 0}, {"k2", 0}, {"points", pointsJson(tuner.scaleL)}}},
		{"right", {{"k1", 0}, {"k2", 0}, {"points", pointsJson(tuner.scaleR)}}},
	};
	json profile = {
		{"type", "streamFrameDistortionProfile"},
		{"version", 1},
		{"name", std::string("Tuned ") + stamp},
		{"distortion", {
			{"mode", "spline"},
			{"points", json::array()},
			{"perEye", true},
			{"perAxis", false},
			{"curves", curves},
		}},
		{"k1", 0},
		{"k2", 0},
		{"centerOffsetXLeft", settings.config.centerOffsetXLeft},
		{"centerOffsetXRight", settings.config.centerOffsetXRight},
		{"centerOffsetY", settings.config.centerOffsetY},
	};
	std::string folder = driverConfigLoader.GetConfigFolder() + "Distortion/";
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	std::string path = folder + "tuned-" + stamp + ".json";
	std::ofstream out(path);
	bool ok = false;
	if(out){
		out << profile.dump(2);
		ok = out.good();
	}
	if(ok){
		DriverLog("Tuner: SAVED profile to %s (import it from the GUI, or paste the block below into streamFrame)", path.c_str());
	}else{
		DriverLog("Tuner: FAILED to write %s, paste block below instead", path.c_str());
	}
	// paste-ready single-line block so a lost file never loses a tune
	json paste = {{"distortion", {{"mode", "spline"}, {"perEye", true}, {"gain", 1.0}, {"curves", curves}}}};
	DriverLog("Tuner: settings block: %s", paste.dump().c_str());
}

bool DirectModeComponentShim::GetActiveSettings(FrameProcessSettings &settings, bool &processAtSubmit){
	// live gaze for the frame about to be processed (dynamic pupil swim /
	// gaze debug). 100ms staleness guard: a brief hiccup degrades to the
	// static behavior instead of consuming stale gaze.
	EyeTrackingTap::Sample gaze;
	if(eyeTrackingTap.GetLatestSample(gaze, 0.1) && gaze.valid){
		settings.gazeValid = true;
		// raw gaze as published, for the swim probe (fitting must see the
		// unsmoothed signal; the adaptive smoothing lags during VOR)
		settings.gazeRawDirX = gaze.targetX;
		settings.gazeRawDirY = gaze.targetY;
		settings.gazeRawDirZ = gaze.targetZ;
		settings.gazeAgeMs = (NowSeconds() - gaze.receivedTime) * 1000.0;
		double dir[3] = { gaze.targetX, gaze.targetY, gaze.targetZ };
		// latency compensation: extrapolate along the recent gaze motion.
		// smoothed delta keeps saccade overshoot bounded; lead is clamped.
		double leadMs = settings.config.eyeGaze.predictionMs;
		if(leadMs < 0){ leadMs = 0; }
		if(leadMs > 100){ leadMs = 100; }
		if(leadMs > 0 && gazePrevValid && gaze.receivedTime > gazePrevTime){
			double dt = gaze.receivedTime - gazePrevTime;
			if(dt > 0.0005 && dt < 0.1){
				for(int i = 0; i < 3; i++){
					double instant = (dir[i] - gazePrevDir[i]) / dt;
					gazeVelEma[i] = gazeVelEma[i] * 0.6 + instant * 0.4;
					dir[i] += gazeVelEma[i] * leadMs / 1000.0;
				}
				double n = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
				if(n > 0.001){
					dir[0] /= n; dir[1] /= n; dir[2] /= n;
				}
			}
		}
		if(!gazePrevValid || gaze.receivedTime != gazePrevTime){
			gazePrevDir[0] = gaze.targetX;
			gazePrevDir[1] = gaze.targetY;
			gazePrevDir[2] = gaze.targetZ;
			gazePrevTime = gaze.receivedTime;
			gazePrevValid = true;
		}
		// speed-adaptive smoothing: the correction must follow FIXATIONS,
		// not sensor noise. large deltas pass almost unfiltered (the snap
		// lands inside the saccade, where vision is suppressed anyway);
		// sub-degree jitter is heavily damped so the corrected world is
		// rock solid while fixating.
		if(!gazeSmoothValid){
			gazeSmoothValid = true;
			gazeSmoothEma[0] = dir[0]; gazeSmoothEma[1] = dir[1]; gazeSmoothEma[2] = dir[2];
		}else{
			double d = gazeSmoothEma[0] * dir[0] + gazeSmoothEma[1] * dir[1] + gazeSmoothEma[2] * dir[2];
			if(d > 1.0){ d = 1.0; } if(d < -1.0){ d = -1.0; }
			double angle = acos(d);
			// full snap beyond ~3 degrees, 6% floor at fixation
			double alpha = 0.06 + angle / 0.05;
			if(alpha > 1.0){ alpha = 1.0; }
			for(int i = 0; i < 3; i++){
				gazeSmoothEma[i] = gazeSmoothEma[i] * (1.0 - alpha) + dir[i] * alpha;
			}
			double n = sqrt(gazeSmoothEma[0] * gazeSmoothEma[0] + gazeSmoothEma[1] * gazeSmoothEma[1] + gazeSmoothEma[2] * gazeSmoothEma[2]);
			if(n > 0.001){
				gazeSmoothEma[0] /= n; gazeSmoothEma[1] /= n; gazeSmoothEma[2] /= n;
			}
			dir[0] = gazeSmoothEma[0]; dir[1] = gazeSmoothEma[1]; dir[2] = gazeSmoothEma[2];
		}
		settings.gazeDirX = dir[0];
		settings.gazeDirY = dir[1];
		settings.gazeDirZ = dir[2];
	}
	// real per-eye frusta for the mapping (cached after the first query)
	for(int e = 0; e < 2; e++){
		float l, r, t, b;
		if(deviceProvider.GetHmdProjectionRaw(e, l, r, t, b)){
			settings.gazeProjValid = true;
			settings.gazeProj[e][0] = l; settings.gazeProj[e][1] = r;
			settings.gazeProj[e][2] = t; settings.gazeProj[e][3] = b;
		}
	}
	{
		std::lock_guard<std::mutex> configGuard(driverConfigLock);
		settings.config = driverConfig.streamFrame;
		processAtSubmit = driverConfig.streamFrame.processAtSubmitLayer;
		if(driverConfigLoader.info.isDashboardOpen && driverConfig.streamFrame.skipColorWhileDashboardOpen){
			settings.applyColor = false;
		}
	}
	// fixation dot direction for this frame's render pose and the head
	// angular velocity, both maintained in SubmitLayer
	settings.dotValid = dotHeadValid;
	settings.dotDirX = dotHeadDir[0];
	settings.dotDirY = dotHeadDir[1];
	settings.dotDirZ = dotHeadDir[2];
	settings.headVelDegS = headVelDegS;
	settings.headBasisValid = headBasisValid;
	for(int bi = 0; bi < 3; bi++){
		for(int bj = 0; bj < 3; bj++){
			settings.headBasis[bi][bj] = headBasisW[bi][bj];
		}
	}
	// interactive distortion tuner: may replace settings.config.distortion
	// with the live working curves and force the calibration overlays on,
	// so it must run before the activity checks below read the config
	UpdateTuner(settings);
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
	// debug/calibration overlays draw in the shader, so they must force
	// the pass on even when everything else is an identity transform
	// (previously the grid/ring only rendered when something else
	// happened to keep the pass active)
	colorActive |= config.eyeGaze.debugRing || config.eyeGaze.debugGrid
		|| config.eyeGaze.calibDot || config.eyeGaze.probeCapture;
	// the pass must also run while any dimming is applied
	settings.dimAmount = dimFactor;
	colorActive |= settings.dimAmount > 0.0001;
	bool spline = config.distortion.mode == "spline";
	auto curveActive = [spline](double k1, double k2, const std::vector<StreamFrameDistortionPoint> &points){
		if(spline){
			for(const auto &point : points){
				if(point.scale != 1.0){
					return true;
				}
			}
			return false;
		}
		return k1 != 0 || k2 != 0;
	};
	bool remapActive = curveActive(config.k1, config.k2, config.distortion.points);
	if(config.distortion.perEye || config.distortion.perAxis){
		for(const auto &pair : config.distortion.curves){
			if(curveActive(pair.second.k1, pair.second.k2, pair.second.points)){
				remapActive = true;
				break;
			}
		}
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
		// gaze tap read on the Present thread: this is the exact consumption
		// path the dynamic pupil-swim pass will use, so exercising it in the
		// heartbeat proves the plumbing end to end during recon. accept
		// samples up to 250ms old so a brief hiccup doesn't read as "no ET".
		EyeTrackingTap::Sample gaze;
		if(eyeTrackingTap.GetLatestSample(gaze, 0.25)){
			DriverLog("FrameComponentShim: gaze tap sample=%llu valid=%d tracked=%d target=(%.4f, %.4f, %.4f) rate=%.1fHz",
				(unsigned long long)gaze.sampleIndex, (int)gaze.valid, (int)gaze.tracked,
				gaze.targetX, gaze.targetY, gaze.targetZ, eyeTrackingTap.GetSampleRate());
		}
	}
	lastSyncTexture = syncTexture;
	// process the scene layer before the driver consumes it
	if(haveSceneLayer){
		FrameProcessSettings settings;
		bool processAtSubmit = false;
		if(GetActiveSettings(settings, processAtSubmit) && !processAtSubmit){
			MaybeLogSwimProbe(settings);
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
