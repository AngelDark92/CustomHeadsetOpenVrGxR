#include "ZeroCopy.h"
#include "DriverLog.h"
#include <chrono>

static double NowSeconds(){
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count() / 1000000.0;
}

ZeroCopyV3& ZeroCopyV3::Get(){
	static ZeroCopyV3 instance;
	return instance;
}

bool ZeroCopyV3::IsProcessingDevice(ID3D11Device* device){
	std::lock_guard<std::mutex> guard(zLock);
	return device != nullptr && device == processingDevice;
}

void ZeroCopyV3::SetProcessingDevice(ID3D11Device* device){
	std::lock_guard<std::mutex> guard(zLock);
	processingDevice = device;
}

void ZeroCopyV3::PublishShadow(uint32_t width, uint32_t height, int index, HANDLE sharedHandle){
	if(index < 0 || index >= slots){ return; }
	uint64_t key = ((uint64_t)width << 32) | height;
	bool announce = false;
	ID3D11Resource* toRelease = nullptr;
	ID3D11ShaderResourceView* toReleaseSRV = nullptr;
	{
		std::lock_guard<std::mutex> guard(zLock);
		Entry &e = entries[key];
		if(e.handle[index] != sharedHandle){
			// (re)registered: drop any vrlink-side open of the old handle
			// (the actual Release happens OUTSIDE the lock — a COM release
			// can run destruction work, and nothing runs under our locks)
			toRelease = e.opened[index];
			e.opened[index] = nullptr;
			toReleaseSRV = e.openedSRV[index];
			e.openedSRV[index] = nullptr;
			e.handle[index] = sharedHandle;
			announce = (index == 0);
		}
	}
	if(toReleaseSRV){
		toReleaseSRV->Release();
	}
	if(toRelease){
		toRelease->Release();
	}
	if(announce){
		DriverLog("zero-copy v3: shadow set registered for %ux%u", width, height);
	}
}

void ZeroCopyV3::MarkFresh(uint32_t width, uint32_t height, int index){
	if(index < 0 || index >= slots){ return; }
	uint64_t key = ((uint64_t)width << 32) | height;
	std::lock_guard<std::mutex> guard(zLock);
	auto found = entries.find(key);
	if(found == entries.end()){ return; }
	found->second.freshIndex = index;
	found->second.freshTime = NowSeconds();
}

ID3D11Resource* ZeroCopyV3::AcquireRedirect(uint32_t width, uint32_t height, ID3D11Device* device){
	if(!Armed() || !device){ return nullptr; }
	uint64_t key = ((uint64_t)width << 32) | height;
	HANDLE toOpen = nullptr;
	int idx = -1;
	{
		std::lock_guard<std::mutex> guard(zLock);
		if(device == processingDevice){
			// our own layer->scratch copy shares the staging signature;
			// never redirect our own reads (feedback loop)
			return nullptr;
		}
		auto found = entries.find(key);
		if(found == entries.end()){
			noEntry++;
			return nullptr;
		}
		Entry &e = found->second;
		idx = e.freshIndex;
		if(idx < 0 || NowSeconds() - e.freshTime > 0.15){
			stale++;
			return nullptr;
		}
		if(e.openedDevice == device && e.opened[idx]){
			return e.opened[idx];
		}
		toOpen = e.handle[idx];
		if(!toOpen){
			openFail++;
			return nullptr;
		}
	}
	// open OUTSIDE the lock. ID3D11Device methods are thread safe, and this
	// runs on vrlink's thread which is already mid D3D work on that device.
	ID3D11Resource* opened = nullptr;
	HRESULT hr = device->OpenSharedResource(toOpen, __uuidof(ID3D11Resource), (void**)&opened);
	if(FAILED(hr) || !opened){
		std::lock_guard<std::mutex> guard(zLock);
		openFail++;
		return nullptr;
	}
	{
		ID3D11Resource* deferred[slots + 1] = {};
		int deferredCount = 0;
		ID3D11ShaderResourceView* deferredSRV[slots] = {};
		int deferredSRVCount = 0;
		ID3D11Resource* result = nullptr;
		{
			std::lock_guard<std::mutex> guard(zLock);
			auto found = entries.find(key);
			if(found != entries.end()){
				Entry &e = found->second;
				if(e.openedDevice != device){
					// device changed (or first open): drop stale opens
					for(int i = 0; i < slots; i++){
						if(e.opened[i]){
							deferred[deferredCount++] = e.opened[i];
							e.opened[i] = nullptr;
						}
						if(e.openedSRV[i]){
							deferredSRV[deferredSRVCount++] = e.openedSRV[i];
							e.openedSRV[i] = nullptr;
						}
					}
					e.openedDevice = device;
				}
				if(e.opened[idx]){
					deferred[deferredCount++] = e.opened[idx];
				}
				e.opened[idx] = opened;
				result = opened;
			}
		}
		// releases OUTSIDE the lock (COM release can run destruction work)
		for(int i = 0; i < deferredSRVCount; i++){
			deferredSRV[i]->Release();
		}
		for(int i = 0; i < deferredCount; i++){
			deferred[i]->Release();
		}
		if(result){
			return result;
		}
	}
	opened->Release();
	return nullptr;
}

ID3D11ShaderResourceView* ZeroCopyV3::AcquireRedirectSRV(uint32_t width, uint32_t height, ID3D11Device* device){
	// resolve the fresh shadow resource on this device first (handles the
	// device guard, freshness window, lazy open and all bookkeeping)
	ID3D11Resource* res = AcquireRedirect(width, height, device);
	if(!res){ return nullptr; }
	uint64_t key = ((uint64_t)width << 32) | height;
	int idx = -1;
	{
		std::lock_guard<std::mutex> guard(zLock);
		auto found = entries.find(key);
		if(found == entries.end()){ return nullptr; }
		Entry &e = found->second;
		idx = e.freshIndex;
		if(idx < 0 || e.opened[idx] != res){
			// raced with a refresh; skip this frame (passthrough)
			return nullptr;
		}
		if(e.openedSRV[idx]){
			return e.openedSRV[idx];
		}
	}
	// create OUTSIDE the lock (thread-safe device method). null desc =
	// default view of the whole resource in its own (fully typed) format —
	// the shadow was created with the layer's exact format, so this
	// matches what vrlink's own layer SRV sees.
	ID3D11ShaderResourceView* srv = nullptr;
	if(FAILED(device->CreateShaderResourceView(res, nullptr, &srv)) || !srv){
		std::lock_guard<std::mutex> guard(zLock);
		openFail++;
		return nullptr;
	}
	ID3D11ShaderResourceView* toRelease = nullptr;
	ID3D11ShaderResourceView* result = nullptr;
	{
		std::lock_guard<std::mutex> guard(zLock);
		auto found = entries.find(key);
		if(found != entries.end()){
			Entry &e = found->second;
			if(e.freshIndex == idx && e.opened[idx] == res){
				toRelease = e.openedSRV[idx];
				e.openedSRV[idx] = srv;
				result = srv;
			}
		}
	}
	if(result){
		if(toRelease){ toRelease->Release(); }
		return result;
	}
	srv->Release();
	return nullptr;
}

void ZeroCopyV3::CountRedirect(){
	bool logIt = false;
	uint32_t n = 0;
	{
		std::lock_guard<std::mutex> guard(zLock);
		redirects++;
		n = redirects;
		// first few redirects, then a heartbeat every 1000
		if(redirectsLogged < 3 || (n % 1000) == 0){
			redirectsLogged++;
			logIt = true;
		}
	}
	if(logIt){
		DriverLog("zero-copy v3: redirect active (%u so far) — vrlink is encoding the pre-warped shadow", n);
	}
}

void ZeroCopyV3::CountPassthrough(const char* reason){
	bool logIt = false;
	uint32_t s = 0, ne = 0, of = 0;
	{
		std::lock_guard<std::mutex> guard(zLock);
		if(passLogged < 6){
			passLogged++;
			logIt = true;
			s = stale; ne = noEntry; of = openFail;
		}
	}
	if(logIt){
		DriverLog("zero-copy v3: passthrough (%s) — unprocessed frame shipped (stale=%u noEntry=%u openFail=%u)",
			reason, s, ne, of);
	}
}

void ZeroCopyV3::MaybeHeartbeat(){
	if(!Armed()){ return; }
	bool logIt = false;
	uint32_t r = 0, s = 0, ne = 0, of = 0, nm = 0;
	{
		std::lock_guard<std::mutex> guard(zLock);
		double now = NowSeconds();
		if(now - lastHeartbeat >= 30.0){
			lastHeartbeat = now;
			logIt = true;
			r = redirects; s = stale; ne = noEntry; of = openFail; nm = nearMissLogged;
		}
	}
	if(logIt){
		DriverLog("zero-copy v3: heartbeat — redirects=%u stale=%u noEntry=%u openFail=%u nearMiss=%u (all-zero counters while armed mean the staging copy is NOT reaching the hook)",
			r, s, ne, of, nm);
	}
}

void ZeroCopyV3::NearMiss(const char* detail){
	bool logIt = false;
	{
		std::lock_guard<std::mutex> guard(zLock);
		if(nearMissLogged < 8){
			nearMissLogged++;
			logIt = true;
		}
	}
	if(logIt){
		DriverLog("zero-copy v3: near-miss (%s) — a staging-like copy was NOT redirected for this reason", detail);
	}
}
