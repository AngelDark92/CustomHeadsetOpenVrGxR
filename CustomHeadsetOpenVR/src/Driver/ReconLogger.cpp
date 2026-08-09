#include "ReconLogger.h"
#include "DriverLog.h"
#include "../../../ThirdParty/minhook/include/MinHook.h"
#include <dxgi.h>

// ============================================================================
// Passive vtable hooks on vrlink's ID3D11DeviceContext. We hook the two
// methods through which a layer texture can be consumed:
//
//   CopyResource / CopySubresourceRegion  — vrlink staging the layer into an
//       NV12 or scratch target before NVENC (the most likely consumption point
//       and the ideal zero-copy v3 substitution site)
//   PSSetShaderResources / Draw           — vrlink colour-converting via a
//       pixel shader (the alternative consumption shape)
//
// We hook the context VTABLE (shared by all contexts of that device), so a
// single install covers vrlink's immediate context. Each thunk logs (bounded)
// then tail-calls the original. No allocation beyond a bounded map lookup;
// nothing calls back into vrlink or openvr under the lock.
// ============================================================================

namespace {

typedef void (STDMETHODCALLTYPE *CopyResource_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE *CopySubresourceRegion_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void (STDMETHODCALLTYPE *PSSetShaderResources_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);

CopyResource_t              origCopyResource = nullptr;
CopySubresourceRegion_t     origCopySubresourceRegion = nullptr;
PSSetShaderResources_t      origPSSetShaderResources = nullptr;

// vtable slot indices for ID3D11DeviceContext (stable COM layout, ID3D11Device
// context interface; verified against d3d11.h ordering)
constexpr int kVtblCopySubresourceRegion = 46;
constexpr int kVtblCopyResource          = 47;
constexpr int kVtblPSSetShaderResources  = 8;

void STDMETHODCALLTYPE HookCopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src){
	ReconLogger& r = ReconLogger::Get();
	if(r.Enabled()){
		std::string s = r.ClassifyTexture(src);
		std::string d = r.ClassifyTexture(dst);
		if(s.find("LAYER") != std::string::npos || d.find("LAYER") != std::string::npos){
			r.LogKind("CopyResource", "src=" + s + " dst=" + d);
		}
	}
	origCopyResource(ctx, dst, src);
}

void STDMETHODCALLTYPE HookCopySubresourceRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstSub,
		UINT dx, UINT dy, UINT dz, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box){
	ReconLogger& r = ReconLogger::Get();
	if(r.Enabled()){
		std::string s = r.ClassifyTexture(src);
		std::string d = r.ClassifyTexture(dst);
		if(s.find("LAYER") != std::string::npos || d.find("LAYER") != std::string::npos){
			char extra[96];
			snprintf(extra, sizeof(extra), " dstSub=%u srcSub=%u dstXY=(%u,%u)", dstSub, srcSub, dx, dy);
			r.LogKind("CopySubresourceRegion", "src=" + s + " dst=" + d + extra);
		}
	}
	origCopySubresourceRegion(ctx, dst, dstSub, dx, dy, dz, src, srcSub, box);
}

void STDMETHODCALLTYPE HookPSSetShaderResources(ID3D11DeviceContext* ctx, UINT start, UINT num,
		ID3D11ShaderResourceView* const* views){
	ReconLogger& r = ReconLogger::Get();
	if(r.Enabled() && views){
		for(UINT i = 0; i < num; i++){
			if(!views[i]){ continue; }
			ID3D11Resource* res = nullptr;
			views[i]->GetResource(&res);
			if(res){
				std::string tag = r.ClassifyTexture(res);
				if(tag.find("LAYER") != std::string::npos){
					char extra[48];
					snprintf(extra, sizeof(extra), " slot=%u", start + i);
					r.LogKind("PSSetShaderResources(bind)", tag + extra);
				}
				res->Release();
			}
		}
	}
	origPSSetShaderResources(ctx, start, num, views);
}

bool HookOneVtblEntry(void** vtbl, int index, void* detour, void** orig){
	void* target = vtbl[index];
	if(MH_CreateHook(target, detour, orig) != MH_OK){
		return false;
	}
	return MH_EnableHook(target) == MH_OK;
}

} // namespace

ReconLogger& ReconLogger::Get(){
	static ReconLogger instance;
	return instance;
}

void ReconLogger::NoteProcessingDevice(ID3D11Device* device){
	std::lock_guard<std::mutex> guard(reconLock);
	processingDevice = device;
}

void ReconLogger::NoteLayerDimensions(uint32_t width, uint32_t height){
	std::lock_guard<std::mutex> guard(reconLock);
	if(width && height && (width != layerWidth || height != layerHeight)){
		layerWidth = width;
		layerHeight = height;
	}
}

std::string ReconLogger::ClassifyTexture(ID3D11Resource* resource){
	if(!resource){ return "null"; }
	D3D11_RESOURCE_DIMENSION dim;
	resource->GetType(&dim);
	if(dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D){
		return "non2d";
	}
	ID3D11Texture2D* tex = nullptr;
	if(FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex){
		return "tex?";
	}
	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);
	tex->Release();
	char buf[160];
	// no lock: layerWidth/Height are written rarely and read racily on
	// purpose (worst case a mislabel in a log line, never a crash)
	bool sizeMatch = (desc.Width == layerWidth && desc.Height == layerHeight && layerWidth != 0);
	snprintf(buf, sizeof(buf), "%s[%ux%u fmt=%u arr=%u bind=0x%x misc=0x%x]",
		sizeMatch ? "LAYER" : "tex",
		desc.Width, desc.Height, (unsigned)desc.Format, desc.ArraySize,
		(unsigned)desc.BindFlags, (unsigned)desc.MiscFlags);
	return std::string(buf);
}

void ReconLogger::LogKind(const char* site, const std::string& detail){
	// decide under the lock, emit OUTSIDE it (concurrency law: no callout —
	// not even DriverLog — while holding a lock)
	bool emit = false;
	bool last = false;
	{
		std::lock_guard<std::mutex> guard(reconLock);
		std::string key = std::string(site) + "|" + detail.substr(0, 24);
		uint32_t& n = logCounts[key];
		if(n < maxPerKind){
			n++;
			emit = true;
			last = (n == maxPerKind);
		}
	}
	if(emit){
		DriverLog("Recon: %s %s%s", site, detail.c_str(),
			last ? "  [further identical lines suppressed]" : "");
	}
}

void ReconLogger::InstallOnce(ID3D11DeviceContext* anyContext){
	if(!enabled || !anyContext){ return; }
	{
		std::lock_guard<std::mutex> guard(reconLock);
		if(contextHooksInstalled){ return; }
		contextHooksInstalled = true; // claim the install; do the work below
	}
	// The immediate-context vtable is shared process-wide, so hooking this
	// context's vtable intercepts vrlink's context too. Install OUTSIDE the
	// lock (MinHook calls; concurrency law: never call out under the lock).
	InstallContextHooks(anyContext);
	TryLogNvenc();
}

void ReconLogger::InstallContextHooks(ID3D11DeviceContext* context){
	void** vtbl = *reinterpret_cast<void***>(context);
	bool a = HookOneVtblEntry(vtbl, kVtblCopyResource, (void*)&HookCopyResource, (void**)&origCopyResource);
	bool b = HookOneVtblEntry(vtbl, kVtblCopySubresourceRegion, (void*)&HookCopySubresourceRegion, (void**)&origCopySubresourceRegion);
	bool c = HookOneVtblEntry(vtbl, kVtblPSSetShaderResources, (void*)&HookPSSetShaderResources, (void**)&origPSSetShaderResources);
	DriverLog("Recon: context hooks installed (CopyResource:%s CopySubresourceRegion:%s PSSetShaderResources:%s) on context %p",
		a ? "ok" : "FAIL", b ? "ok" : "FAIL", c ? "ok" : "FAIL", (void*)context);
	DriverLog("Recon: watching for reads of the %ux%u layer — the first CopyResource/CopySubresourceRegion whose SRC is a LAYER is vrlink's consumption point (zero-copy v3 substitution site)",
		layerWidth, layerHeight);
}

void ReconLogger::TryLogNvenc(){
	{
		std::lock_guard<std::mutex> guard(reconLock);
		if(nvencLogged){ return; }
		nvencLogged = true;
	}
	// vrlink links NVENC dynamically; if the module is resident we can at
	// least confirm which one and its path (parameter interception would be a
	// follow-up slice, not this recon pass).
	HMODULE nv = GetModuleHandleA("nvEncodeAPI64.dll");
	if(nv){
		char path[MAX_PATH] = {0};
		GetModuleFileNameA(nv, path, MAX_PATH);
		DriverLog("Recon: NVENC module resident: %s (parameter interception is a follow-up if v3 needs it)", path);
	}else{
		DriverLog("Recon: nvEncodeAPI64.dll not resident yet (vrlink may load it lazily on first encode)");
	}
}
