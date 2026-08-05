#pragma once
#include "openvr_driver.h"
#include "../Config/Config.h"
#include <cstdint>

// Phase 2: GPU processing of direct mode layer textures before the headset
// driver (vrlink) consumes them.
//
// Contract (observed from the vrlink direct mode path):
// - Layer textures are per eye, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, legacy shared
//   handles (unTextureFlags == 0), full 0-1 bounds, no depth.
// - Synchronization between the app device and the driver device is done via a
//   keyed mutex on the sync texture passed to Present. Acquiring it guarantees
//   the app's rendering into the layer textures has completed; our release
//   followed by vrlink's own acquire orders vrlink's reads after our writes.
//
// Everything is fail safe: any error results in the frame being forwarded
// unprocessed and a (rate limited) log line.

// settings snapshot copied from driverConfig once per frame
struct FrameProcessSettings{
	StreamFrameConfig config = {};
	// false while dashboard is open (compositor shader already applies the color
	// adjustments to the flattened scene in that state). does not gate cas/dither.
	bool applyColor = true;
	// stationary dimming factor, 0 bright to 1 black, applied after everything
	double dimAmount = 0;
	// live gaze from the eye tracking tap, sampled on the Present thread.
	// unit direction in HMD space (origin is the head origin; vrlink
	// publishes a combined ray with zero origin), valid=false when the tap
	// has no fresh sample.
	bool gazeValid = false;
	double gazeDirX = 0;
	double gazeDirY = 0;
	double gazeDirZ = -1;
	// real per-eye projection frusta from the HMD display component
	// ([eye][left,right,top,bottom], OpenVR raw convention: y-down
	// tangents). when valid the gaze mapping uses these — the same math
	// the runtime uses for GetEyeTrackedFoveationCenter — instead of the
	// symmetric tangent knobs.
	bool gazeProjValid = false;
	float gazeProj[2][4] = {};
};

#ifdef _WIN32

#include <d3d11.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class FrameProcessor{
public:
	// process both eye textures of the scene layer. syncTexture is the direct
	// mode sync texture whose keyed mutex guards the frame. returns true if the
	// frame was processed, false if it was skipped (frame is still valid).
	bool ProcessSceneLayer(vr::SharedTextureHandle_t leftEye, vr::SharedTextureHandle_t rightEye,
		const vr::VRTextureBounds_t &leftBounds, const vr::VRTextureBounds_t &rightBounds,
		vr::SharedTextureHandle_t syncTexture, const FrameProcessSettings &settings);

	// drop cached opened resources for a destroyed swap texture handle
	void EvictTexture(vr::SharedTextureHandle_t handle);
	// drop everything (e.g. DestroyAllSwapTextureSets); cheap, caches repopulate
	void EvictAll();

private:
	bool EnsureDevice();
	bool EnsureShaders();
	bool EnsureScratch(uint32_t width, uint32_t height, DXGI_FORMAT format);
	ID3D11Texture2D* OpenShared(vr::SharedTextureHandle_t handle);
	bool ProcessEye(ID3D11Texture2D* texture, const vr::VRTextureBounds_t &bounds, int eye, const FrameProcessSettings &settings);

	std::mutex lock;
	bool deviceFailed = false;
	// consecutive frames skipped on sync acquire timeout, drives the
	// escalating timeout that breaks flash streaks under load
	int consecutiveSyncSkips = 0;

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	// fullscreen triangle vertex shader + processing pixel shader
	ID3D11VertexShader* vertexShader = nullptr;
	ID3D11PixelShader* pixelShader = nullptr;
	ID3D11SamplerState* sampler = nullptr;
	ID3D11Buffer* constantBuffer = nullptr;
	// mtime of the hlsl file the current pixel shader was compiled from (0 = embedded fallback)
	uint64_t pixelShaderFileTime = 0;
	// last time the hlsl file mtime was checked, to avoid a stat call every frame
	uint64_t lastShaderCheckMs = 0;
	bool shaderFailed = false;

	// distortion curve lookup table, rebaked when the distortion settings change
	bool BakeLutIfNeeded(const StreamFrameConfig &config);
	ID3D11Texture2D* lutTexture = nullptr;
	ID3D11ShaderResourceView* lutSRV = nullptr;
	// serialized copy of the settings the current lut was baked from
	std::string lastLutKey = "";
	// number of curve rows in the lut texture (1, 2 or 4)
	int lutRowCount = 1;
	bool lutBaked = false;

	// scratch texture cache, keyed by size + format family. multiple sizes are
	// live simultaneously during app transitions and dashboard flattening
	// (scene frames alternate between vrcompositor's set and the app's set);
	// a single set caused ~half a GB of alloc/free per flip, under the sync
	// keyed mutex, which showed up as multi second stutter at app launches.
	struct ScratchSet{
		ID3D11Texture2D* in = nullptr;
		ID3D11ShaderResourceView* inSRV = nullptr;
		ID3D11Texture2D* out = nullptr;
		ID3D11RenderTargetView* outRTV = nullptr;
		uint64_t lastUsedMs = 0;
	};
	static constexpr size_t maxScratchSets = 4;
	std::map<uint64_t, ScratchSet> scratchSets;
	static void ReleaseScratchSet(ScratchSet &set);
	// layer formats already reported as unsupported (log each once, not
	// against the errorCount budget, so per-game skips stay visible)
	std::set<unsigned> skippedFormats;
	// periodic re-arm of the errorCount budget so a game launched late in a
	// session still gets its 20 diagnostic lines
	uint64_t lastErrorResetMs = 0;
	// non owning aliases into the cache entry selected by the last
	// EnsureScratch call, consumed by ProcessEye
	ID3D11Texture2D* scratchIn = nullptr;      // copy of the layer texture, sampled by the shader
	ID3D11ShaderResourceView* scratchInSRV = nullptr;
	ID3D11Texture2D* scratchOut = nullptr;     // render target, copied back into the layer texture
	ID3D11RenderTargetView* scratchOutRTV = nullptr;

	// cache of opened shared resources by handle value. handles can be reused
	// after destruction, so DestroySwapTextureSet must evict.
	std::map<uint64_t, ID3D11Texture2D*> openedTextures;

	// rate limited error logging
	uint64_t errorCount = 0;
	// gaze ring state logging
	bool gazeRingWasActive = false;
	uint64_t lastGazeRingLogMs = 0;
};

#else

// non windows stub
class FrameProcessor{
public:
	bool ProcessSceneLayer(vr::SharedTextureHandle_t, vr::SharedTextureHandle_t,
		const vr::VRTextureBounds_t &, const vr::VRTextureBounds_t &,
		vr::SharedTextureHandle_t, const FrameProcessSettings &){ return false; }
	void EvictTexture(vr::SharedTextureHandle_t){}
	void EvictAll(){}
};

#endif
