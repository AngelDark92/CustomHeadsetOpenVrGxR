#pragma once
#include "openvr_driver.h"
#include <d3d11.h>
#include <cstdint>
#include <map>
#include <set>
#include <mutex>
#include <string>

// ============================================================================
// ReconLogger — passive, opt-in instrumentation of the vrlink (Steam Link)
// consumption path, to answer three questions that decide future work:
//
//   1. ZERO-COPY v3 feasibility: where EXACTLY does vrlink read our layer
//      texture? (a CopyResource/CopySubresourceRegion into its own staging /
//      NV12 target, or an SRV bind into a colour-convert draw?) The answer is
//      the one point where pixels can be substituted WITHOUT touching vrlink's
//      handle bookkeeping or NVENC surface registration — the two walls that
//      killed zero-copy v1 (handle lookup) and v2 (encoder reset loop).
//
//   2. NVENC control surface: which encode parameters does vrlink set, and can
//      we influence bitrate / codec / rate control from our position?
//
//   3. Per-eye ET wire: does the device stream per-eye gaze that vrlink
//      collapses to the cyclopean ray before publishing? If so, intercepting
//      pre-collapse yields per-eye ET with NO device APK.
//
// This module ONLY OBSERVES. It installs vtable hooks on the vrlink D3D11
// immediate context (and logs NVENC session creation if the export is
// resolvable), records what it sees with heavy rate-limiting, and always
// forwards to the original function unchanged. Nothing is substituted, no
// resource is created, no encode is altered. A crash here would be as bad as
// a crash in the real path, so every hook is a thin logging wrapper with no
// allocation on the hot path beyond a bounded map insert.
//
// GATED: does nothing unless streamFrame.reconLogger.enable is true. Off by
// default. Designed to run for ONE disposable session, then be removed.
//
// Lock discipline: reconLock is a LEAF lock. Nothing inside a reconLock scope
// calls back into vrlink, the FrameProcessor, or any openvr entry point
// (concurrency law: never call out while holding a lock). The hooks call the
// trampoline OUTSIDE the lock.
// ============================================================================

class ReconLogger{
public:
	static ReconLogger& Get();

	// called by FrameProcessor once it has opened a layer texture: registers
	// the D3D11 device/context that OWNS the app-side layers (that is OUR
	// processing device, not vrlink's). Used only to tell the two devices
	// apart in logs.
	void NoteProcessingDevice(ID3D11Device* device);

	// install the passive context hooks. The D3D11 immediate-context vtable
	// is SHARED across every ID3D11DeviceContext in the process (all point at
	// the same d3d11.dll implementation), so hooking our OWN context's vtable
	// also intercepts vrlink's context calls — device-agnostic and robust.
	// Safe to call repeatedly; installs exactly once. Pass our processing
	// context.
	void InstallOnce(ID3D11DeviceContext* anyContext);

	// register a known layer texture pointer (as opened on vrlink's device)
	// so the context hooks can recognise reads of it. Best-effort; the hooks
	// also log any copy/bind involving a texture of the scene layer's size.
	void NoteLayerDimensions(uint32_t width, uint32_t height);

	bool Enabled() const { return enabled; }
	void SetEnabled(bool on){ enabled = on; }

	// public because the MinHook detours are FREE functions (MinHook needs
	// plain function pointers), and they classify + log through the
	// singleton. internal helpers in spirit, public by necessity.
	// classify a texture we see in a copy/bind: matches the layer size? on
	// vrlink's device? returns a short human tag for the log.
	std::string ClassifyTexture(ID3D11Resource* resource);
	// bounded logging helper (respects maxPerKind per key)
	void LogKind(const char* site, const std::string& detail);


private:
	ReconLogger() = default;
	ReconLogger(const ReconLogger&) = delete;
	ReconLogger& operator=(const ReconLogger&) = delete;

	void InstallContextHooks(ID3D11DeviceContext* context);
	void TryLogNvenc();

	std::mutex reconLock;
	bool enabled = false;
	bool contextHooksInstalled = false;
	bool nvencLogged = false;
	ID3D11Device* processingDevice = nullptr;
	ID3D11Device* vrlinkDevice = nullptr;

	// scene layer dimensions, to recognise reads of the layer even when we
	// do not have the exact vrlink-side pointer
	uint32_t layerWidth = 0;
	uint32_t layerHeight = 0;

	// rate-limiting: log each distinct (call-site, resourceKind) a bounded
	// number of times, then go silent for that combo
	std::map<std::string, uint32_t> logCounts;
	static constexpr uint32_t maxPerKind = 8;

	// hook trampoline storage and static thunks need access
	friend struct ReconContextHooks;


};
