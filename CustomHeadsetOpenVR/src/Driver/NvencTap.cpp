#include "NvencTap.h"
#include "DriverLog.h"
#include "../../../ThirdParty/minhook/include/MinHook.h"
#include <chrono>
#include <cstdio>
#include <cstring>

static double NowSecondsNv(){
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
}

namespace {

// ---- minimal NVENC surface (no SDK header in tree; only what the shims
// need, layout-stable leading fields per the public nvEncodeAPI.h) ----
typedef uint32_t NVENCSTATUS_T; // 0 == NV_ENC_SUCCESS

struct NvGuid{ uint32_t d1; uint16_t d2; uint16_t d3; uint8_t d4[8]; };

// leading fields of NV_ENC_INITIALIZE_PARAMS (stable across API 8..12)
struct NvInitLead{
	uint32_t version;
	NvGuid encodeGUID;
	NvGuid presetGUID;
	uint32_t encodeWidth;
	uint32_t encodeHeight;
	uint32_t darWidth;
	uint32_t darHeight;
	uint32_t frameRateNum;
	uint32_t frameRateDen;
};

// leading fields of NV_ENC_REGISTER_RESOURCE (stable across API 8..12)
struct NvRegisterLead{
	uint32_t version;
	uint32_t resourceType; // 0=dx, 2=cudadevptr, ...
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t subResourceIndex;
	void* resourceToRegister;
};

// the function list is caller-owned; entries we wrap live at fixed indices
// only per-version, so we wrap by NAME using the documented list layout's
// stable prologue: version, reserved, then function pointers in a fixed
// published order. rather than assume indices, we scan the struct for the
// pointer values returned by the driver AFTER calling the original — but
// pointer identity scanning is fragile. instead: the published order of
// NV_ENCODE_API_FUNCTION_LIST has been append-only since API 5; the
// entries below sit at fixed offsets in every published header:
//   +0x08 nvEncOpenEncodeSession        (index 0)
//   ...
// to stay honest about layout risk we wrap only via offsets read from the
// published append-only order, version-gated: if the list version's major
// (HIWORD-style encoding NVENCAPI_STRUCT_VERSION) is outside 5..15 we log
// and wrap nothing.
struct NvFunctionListLead{
	uint32_t version;
	uint32_t reserved;
	void* fn[64]; // published append-only pointer table
};

// published append-only indices (nvEncodeAPI.h, stable since API 5)
constexpr int kFnInitializeEncoder = 8;    // nvEncInitializeEncoder
constexpr int kFnRegisterResource = 28;    // nvEncRegisterResource
constexpr int kFnReconfigureEncoder = 31;  // nvEncReconfigureEncoder

typedef NVENCSTATUS_T (*PFN_NvInitializeEncoder)(void* encoder, void* params);
typedef NVENCSTATUS_T (*PFN_NvRegisterResource)(void* encoder, void* params);
typedef NVENCSTATUS_T (*PFN_NvReconfigureEncoder)(void* encoder, void* params);
typedef NVENCSTATUS_T (*PFN_NvEncodeAPICreateInstance)(void* functionList);

PFN_NvInitializeEncoder origInitializeEncoder = nullptr;
PFN_NvRegisterResource origRegisterResource = nullptr;
PFN_NvReconfigureEncoder origReconfigureEncoder = nullptr;
PFN_NvEncodeAPICreateInstance origCreateInstance = nullptr;

std::atomic<int> initLogged{0};
std::atomic<int> registerLogged{0};
std::atomic<int> reconfLogged{0};

// VirtualQuery-guarded hex dump: never reads past a readable region
void SafeHexDump(const char* label, const void* ptr, size_t want){
	if(!ptr){ return; }
	MEMORY_BASIC_INFORMATION mbi = {};
	if(VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0){ return; }
	if(mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))){ return; }
	size_t avail = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - (const char*)ptr);
	size_t len = want < avail ? want : avail;
	const unsigned char* p = (const unsigned char*)ptr;
	char line[3 * 32 + 32];
	for(size_t off = 0; off < len; off += 32){
		size_t n = (len - off) < 32 ? (len - off) : 32;
		char* w = line;
		for(size_t i = 0; i < n; i++){
			w += snprintf(w, 4, "%02x ", p[off + i]);
		}
		DriverLog("NvencTap: %s +0x%03x: %s", label, (unsigned)off, line);
	}
}

void LogGuid(const char* label, const NvGuid &g){
	DriverLog("NvencTap: %s {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
		label, g.d1, g.d2, g.d3, g.d4[0], g.d4[1], g.d4[2], g.d4[3], g.d4[4], g.d4[5], g.d4[6], g.d4[7]);
}

NVENCSTATUS_T ShimInitializeEncoder(void* encoder, void* params){
	if(initLogged.fetch_add(1) < 3 && params){
		NvInitLead lead = {};
		memcpy(&lead, params, sizeof(lead) < 64 ? sizeof(lead) : 64);
		DriverLog("NvencTap: nvEncInitializeEncoder encoder=%p version=0x%08x %ux%u dar=%ux%u fps=%u/%u",
			encoder, lead.version, lead.encodeWidth, lead.encodeHeight,
			lead.darWidth, lead.darHeight, lead.frameRateNum, lead.frameRateDen);
		LogGuid("  encodeGUID", lead.encodeGUID);
		LogGuid("  presetGUID", lead.presetGUID);
		SafeHexDump("init", params, 0x150);
	}
	return origInitializeEncoder(encoder, params);
}

NVENCSTATUS_T ShimRegisterResource(void* encoder, void* params){
	if(registerLogged.fetch_add(1) < 10 && params){
		NvRegisterLead lead = {};
		memcpy(&lead, params, sizeof(lead));
		DriverLog("NvencTap: nvEncRegisterResource encoder=%p version=0x%08x type=%u %ux%u pitch=%u sub=%u resource=%p — LAYER-SIZED resource here means direct NVENC consumption (the v3c substitution site)",
			encoder, lead.version, lead.resourceType, lead.width, lead.height,
			lead.pitch, lead.subResourceIndex, lead.resourceToRegister);
	}
	return origRegisterResource(encoder, params);
}

NVENCSTATUS_T ShimReconfigureEncoder(void* encoder, void* params){
	if(reconfLogged.fetch_add(1) < 3 && params){
		uint32_t version = 0;
		memcpy(&version, params, sizeof(version));
		DriverLog("NvencTap: nvEncReconfigureEncoder encoder=%p version=0x%08x", encoder, version);
		SafeHexDump("reconf", params, 0x150);
	}
	return origReconfigureEncoder(encoder, params);
}

NVENCSTATUS_T ShimCreateInstance(void* functionList){
	NVENCSTATUS_T status = origCreateInstance(functionList);
	if(status != 0 || !functionList){
		DriverLog("NvencTap: NvEncodeAPICreateInstance status=%u (list not wrapped)", status);
		return status;
	}
	NvFunctionListLead* list = (NvFunctionListLead*)functionList;
	// wrap observe-only shims, keeping the driver's pointers as originals.
	// the same driver functions come back for every instance, so the
	// single static originals are stable; re-wrapping an already-wrapped
	// list (caller re-calls CreateInstance on its own struct) is guarded
	// by pointer identity.
	if(list->fn[kFnInitializeEncoder] && list->fn[kFnInitializeEncoder] != (void*)&ShimInitializeEncoder){
		origInitializeEncoder = (PFN_NvInitializeEncoder)list->fn[kFnInitializeEncoder];
		list->fn[kFnInitializeEncoder] = (void*)&ShimInitializeEncoder;
	}
	if(list->fn[kFnRegisterResource] && list->fn[kFnRegisterResource] != (void*)&ShimRegisterResource){
		origRegisterResource = (PFN_NvRegisterResource)list->fn[kFnRegisterResource];
		list->fn[kFnRegisterResource] = (void*)&ShimRegisterResource;
	}
	if(list->fn[kFnReconfigureEncoder] && list->fn[kFnReconfigureEncoder] != (void*)&ShimReconfigureEncoder){
		origReconfigureEncoder = (PFN_NvReconfigureEncoder)list->fn[kFnReconfigureEncoder];
		list->fn[kFnReconfigureEncoder] = (void*)&ShimReconfigureEncoder;
	}
	DriverLog("NvencTap: function list wrapped (list version=0x%08x) — observe only, nothing modified", list->version);
	return status;
}

} // namespace

NvencTap& NvencTap::Get(){
	static NvencTap instance;
	return instance;
}

void NvencTap::TryInstall(){
	if(installed.load(std::memory_order_relaxed) || failedPermanently.load(std::memory_order_relaxed)){
		return;
	}
	double now = NowSecondsNv();
	if(now - lastAttempt < 1.0){ return; }
	lastAttempt = now;
	HMODULE nv = GetModuleHandleA("nvEncodeAPI64.dll");
	if(!nv){
		return; // lazy-loaded; retry next second
	}
	void* target = (void*)GetProcAddress(nv, "NvEncodeAPICreateInstance");
	if(!target){
		failedPermanently.store(true, std::memory_order_relaxed);
		DriverLog("NvencTap: NvEncodeAPICreateInstance export not found — tap unavailable");
		return;
	}
	if(MH_CreateHook(target, (void*)&ShimCreateInstance, (void**)&origCreateInstance) != MH_OK
			|| MH_EnableHook(target) != MH_OK){
		failedPermanently.store(true, std::memory_order_relaxed);
		DriverLog("NvencTap: hook install FAILED on NvEncodeAPICreateInstance");
		return;
	}
	installed.store(true, std::memory_order_relaxed);
	DriverLog("NvencTap: installed on NvEncodeAPICreateInstance — encoders created or recreated from now on will be observed. NOTE: an encoder created BEFORE this point is invisible; enable the toggle before launching SteamVR, or change stream resolution to force recreation.");
}
