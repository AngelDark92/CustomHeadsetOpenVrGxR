#pragma once
#include "openvr_driver.h"
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

#ifdef _WIN32

#include <d3d11.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// settings snapshot copied from driverConfig once per frame
struct FrameProcessSettings{
	bool enable = false;
	bool applyColor = true;      // false while dashboard is open (compositor shader already applied it)
	double saturation = 50;      // 50 = neutral, same semantics as customShader.saturation
	double k1 = 0;               // radial distortion pre perturbation
	double k2 = 0;
	double centerOffsetXLeft = 0;   // optical center offset from texture center, uv units
	double centerOffsetXRight = 0;
	double centerOffsetY = 0;
};

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
	bool EnsureScratch(uint32_t width, uint32_t height);
	ID3D11Texture2D* OpenShared(vr::SharedTextureHandle_t handle);
	bool ProcessEye(ID3D11Texture2D* texture, const vr::VRTextureBounds_t &bounds, int eye, const FrameProcessSettings &settings);

	std::mutex lock;
	bool deviceFailed = false;

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

	// scratch textures, recreated when the layer size changes
	uint32_t scratchWidth = 0;
	uint32_t scratchHeight = 0;
	ID3D11Texture2D* scratchIn = nullptr;      // copy of the layer texture, sampled by the shader
	ID3D11ShaderResourceView* scratchInSRV = nullptr;
	ID3D11Texture2D* scratchOut = nullptr;     // render target, copied back into the layer texture
	ID3D11RenderTargetView* scratchOutRTV = nullptr;

	// cache of opened shared resources by handle value. handles can be reused
	// after destruction, so DestroySwapTextureSet must evict.
	std::map<uint64_t, ID3D11Texture2D*> openedTextures;

	// rate limited error logging
	uint64_t errorCount = 0;
};

#else

// non windows stub
struct FrameProcessSettings{
	bool enable = false;
	bool applyColor = true;
	double saturation = 50;
	double k1 = 0;
	double k2 = 0;
	double centerOffsetXLeft = 0;
	double centerOffsetXRight = 0;
	double centerOffsetY = 0;
};

class FrameProcessor{
public:
	bool ProcessSceneLayer(vr::SharedTextureHandle_t, vr::SharedTextureHandle_t,
		const vr::VRTextureBounds_t &, const vr::VRTextureBounds_t &,
		vr::SharedTextureHandle_t, const FrameProcessSettings &){ return false; }
	void EvictTexture(vr::SharedTextureHandle_t){}
	void EvictAll(){}
};

#endif
