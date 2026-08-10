#pragma once
#include <windows.h>
#include <atomic>

// ============================================================================
// NvencTap — OBSERVE-ONLY recon of vrlink's NVENC usage. This slice never
// modifies a single byte of any NVENC parameter; it exists to answer, from a
// field log, the questions the D3D-side recon cannot:
//
//   1. Does vrlink register the LAYER texture directly with NVENC
//      (nvEncRegisterResource with layer dimensions)? If yes, that is the
//      v3c substitution site — the consumption point that produced
//      all-zero v3/v3b counters (no D3D copy, no SRV bind to intercept).
//   2. What are the exact encoder init/reconfigure parameters (rate
//      control, QP, profile)? That is the parameter surface for the black
//      floor / banding work: the same observe -> adjust path as the
//      Black Floor Clamp, one layer deeper.
//
// Mechanism: MinHook on the nvEncodeAPI64.dll export
// NvEncodeAPICreateInstance. Our detour calls the original, then wraps
// selected entries of the returned caller-owned function list. Wrapped
// shims log (bounded) and ALWAYS forward unchanged. Struct layouts vary
// by API version, so decoding is two-tier: best-effort fields that have
// been stable across public headers (version, GUIDs, width, height,
// pitch, resource pointer), plus a VirtualQuery-guarded hex dump for
// exact offline decoding against the matching header.
//
// Timing caveat (documented in the GUI tip): vrlink may call
// NvEncodeAPICreateInstance before our hook lands. Enable the toggle in
// settings BEFORE launching SteamVR; resolution changes also recreate
// encoders and re-enter the hooked path.
// ============================================================================

class NvencTap{
public:
	static NvencTap& Get();

	// attempt installation; cheap and self-throttled (1/s), safe to call
	// every frame while the toggle is on. logs once on success/failure.
	void TryInstall();
	bool Installed() const { return installed.load(std::memory_order_relaxed); }

private:
	NvencTap() = default;
	NvencTap(const NvencTap&) = delete;
	NvencTap& operator=(const NvencTap&) = delete;

	std::atomic<bool> installed{false};
	std::atomic<bool> failedPermanently{false};
	double lastAttempt = 0;
};
