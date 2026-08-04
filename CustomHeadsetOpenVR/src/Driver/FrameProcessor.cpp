#include "FrameProcessor.h"
#include "DriverLog.h"

#ifdef _WIN32

#include <d3dcompiler.h>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include "../Config/ConfigLoader.h"

#pragma comment(lib, "D3D11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "D3DCompiler.lib")

// log at most the first few occurrences of each kind of failure
#define PROCESSOR_ERROR(...) do{ if(errorCount++ < 20){ DriverLog(__VA_ARGS__); } }while(0)

// fullscreen triangle, no vertex buffer needed
static const char* vertexShaderSource = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID){
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
)";

// embedded fallback pixel shader. if vrlink_layer_ps.hlsl exists in the driver
// shader resources it is used instead and hot reloaded on change.
static const char* pixelShaderFallbackSource = R"(
// minimal fallback: distortion lut + saturation only. the full feature shader
// ships as vrlink_layer_ps.hlsl and is preferred when present.
cbuffer Params : register(b0){
	float saturation; float applyColor; float contrastMult; float contrastOffset;
	float contrastLinear; float outGamma; float casStrength; float casEnable;
	float annulusEnable; float annulusMin; float annulusMax; float annulusFeather;
	float ditherEnable; float lutMaxR; float aspect; float matrixEnable;
	float2 center; float2 boundsMin;
	float2 boundsSize; float2 texelSize;
	float4 colorMultiplier;
	float4 matR; float4 matG; float4 matB;
	float lutRowBase; float lutRowCount; float perAxisEnable; float dimAmount;
	float manualSrgb; float ditherLsb; float pad0; float pad1;
};
Texture2D<float4> tex : register(t0);
Texture2D<float4> lut : register(t1);
SamplerState samp : register(s0);

float3 LinearToSrgb(float3 c){
	c = max(c, 0.0);
	return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float3 SrgbToLinear(float3 c){
	c = max(c, 0.0);
	return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float SampleLutRow(float u, float row){
	return lut.SampleLevel(samp, float2(u, (row + 0.5) / lutRowCount), 0).x;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0{
	float2 p = uv - center;
	p.y *= aspect;
	float r = length(p);
	float u = r / lutMaxR;
	float s;
	if(perAxisEnable > 0.5){
		float wH = (p.x * p.x) / max(dot(p, p), 1e-9);
		s = SampleLutRow(u, lutRowBase) * wH + SampleLutRow(u, lutRowBase + 1) * (1.0 - wH);
	}else{
		s = SampleLutRow(u, lutRowBase);
	}
	if(annulusEnable > 0.5){
		float w = smoothstep(annulusMin - annulusFeather, annulusMin + annulusFeather, r)
			* (1.0 - smoothstep(annulusMax - annulusFeather, annulusMax + annulusFeather, r));
		s = 1.0 + (s - 1.0) * w;
	}
	p *= s;
	p.y /= aspect;
	float2 nSrc = p + center;
	float4 color = tex.SampleLevel(samp, nSrc * boundsSize + boundsMin, 0);
	if(manualSrgb > 0.5){
		color.rgb = SrgbToLinear(color.rgb);
	}
	if(any(nSrc < 0.0) || any(nSrc > 1.0)){
		color = float4(0, 0, 0, color.a);
	}
	if(applyColor > 0.5){
		float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
		color.rgb = lerp(gray.xxx, color.rgb, saturation);
	}
	color.rgb *= 1.0 - dimAmount;
	if(manualSrgb > 0.5){
		color.rgb = LinearToSrgb(color.rgb);
	}
	return color;
}
)";

// mirrors the cbuffer in the pixel shader, hlsl packing: 4 scalars per register,
// float2 pairs share registers, float4 rows for the color matrix
struct FrameProcessorConstants{
	float saturation; float applyColor; float contrastMult; float contrastOffset;
	float contrastLinear; float outGamma; float casStrength; float casEnable;
	float annulusEnable; float annulusMin; float annulusMax; float annulusFeather;
	float ditherEnable; float lutMaxR; float aspect; float matrixEnable;
	float center[2]; float boundsMin[2];
	float boundsSize[2]; float texelSize[2];
	float colorMultiplier[4];
	float matR[4]; float matG[4]; float matB[4];
	float lutRowBase; float lutRowCountF; float perAxisEnable; float dimAmount;
	float manualSrgb; float ditherLsb; float pad0; float pad1;
};

// map a layer texture format to the scratch format and shader mode used to
// process it. scratch must be in the same dxgi format family as the layer for
// the copies to be legal. rgba8/bgra8 use srgb typed views (hardware converts,
// shader sees linear). r10g10b10a2 has no srgb variant, so the shader decodes
// and re-encodes explicitly (manualSrgb) and dithers at the 10 bit lsb; the
// only difference vs the 8 bit path is that bilinear filtering happens on
// encoded values, which is visually negligible for near identity warps.
static bool MapLayerFormat(DXGI_FORMAT layerFormat, DXGI_FORMAT &scratchFormat, bool &manualSrgb, float &ditherLsb){
	switch(layerFormat){
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			scratchFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			manualSrgb = false;
			ditherLsb = 255.0f;
			return true;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			scratchFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			manualSrgb = false;
			ditherLsb = 255.0f;
			return true;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			scratchFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
			manualSrgb = true;
			ditherLsb = 1023.0f;
			return true;
		default:
			return false;
	}
}

static uint64_t NowMs(){
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// find the dxgi adapter SteamVR renders on, from the hmd's reported luid.
// on hybrid gpu systems the default adapter can be the integrated gpu, and a
// device created there cannot open the shared layer textures (or limps across
// adapters), which shows up as "processing enabled but nothing changes".
static IDXGIAdapter1* FindAdapterForHmd(){
	uint64_t luidValue = 0;
	vr::PropertyContainerHandle_t container = vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);
	vr::ETrackedPropertyError propError = vr::TrackedProp_Success;
	luidValue = vr::VRProperties()->GetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, &propError);
	if(propError != vr::TrackedProp_Success || luidValue == 0){
		DriverLog("FrameProcessor: hmd adapter luid unavailable (error %d), using default adapter", (int)propError);
		return nullptr;
	}
	IDXGIFactory1* factory = nullptr;
	if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory){
		return nullptr;
	}
	IDXGIAdapter1* found = nullptr;
	for(UINT i = 0; ; i++){
		IDXGIAdapter1* adapter = nullptr;
		if(factory->EnumAdapters1(i, &adapter) != S_OK || !adapter){
			break;
		}
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		uint64_t adapterLuid = ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32) | (uint64_t)desc.AdapterLuid.LowPart;
		if(adapterLuid == luidValue){
			DriverLog("FrameProcessor: matched hmd adapter %ls", desc.Description);
			found = adapter;
			break;
		}
		adapter->Release();
	}
	factory->Release();
	if(!found){
		DriverLog("FrameProcessor: no adapter matched hmd luid %llx, using default adapter", (unsigned long long)luidValue);
	}
	return found;
}

bool FrameProcessor::EnsureDevice(){
	if(device){
		return true;
	}
	if(deviceFailed){
		return false;
	}
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
	IDXGIAdapter1* adapter = FindAdapterForHmd();
	// with an explicit adapter the driver type must be unknown
	HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
	if(adapter){
		adapter->Release();
	}
	if(FAILED(hr)){
		deviceFailed = true;
		PROCESSOR_ERROR("FrameProcessor: failed to create D3D11 device: 0x%08X", (unsigned)hr);
		return false;
	}
	DriverLog("FrameProcessor: created D3D11 device");
	return true;
}

static std::string GetLayerShaderPath(){
	return driverConfigLoader.info.driverResources + "shaders/d3d11/vrlink_layer_ps.hlsl";
}

static uint64_t GetFileTime(const std::string &path){
	std::error_code ec;
	auto t = std::filesystem::last_write_time(path, ec);
	if(ec){
		return 0;
	}
	return (uint64_t)t.time_since_epoch().count();
}

bool FrameProcessor::EnsureShaders(){
	// vertex shader, sampler, constant buffer: once
	if(!vertexShader){
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "vs", nullptr, nullptr,
			"main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
		if(FAILED(hr) || !blob){
			if(errors){ PROCESSOR_ERROR("FrameProcessor: VS compile error: %s", (char*)errors->GetBufferPointer()); errors->Release(); }
			shaderFailed = true;
			return false;
		}
		hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vertexShader);
		blob->Release();
		if(FAILED(hr)){
			PROCESSOR_ERROR("FrameProcessor: CreateVertexShader failed: 0x%08X", (unsigned)hr);
			shaderFailed = true;
			return false;
		}

		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		if(FAILED(device->CreateSamplerState(&sd, &sampler))){
			shaderFailed = true;
			return false;
		}

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = (sizeof(FrameProcessorConstants) + 15) & ~15u;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if(FAILED(device->CreateBuffer(&bd, nullptr, &constantBuffer))){
			shaderFailed = true;
			return false;
		}
	}

	// pixel shader: prefer the hlsl file, hot reload when it changes, check its
	// mtime at most once a second
	uint64_t now = NowMs();
	bool checkFile = now - lastShaderCheckMs > 1000 || !pixelShader;
	if(checkFile){
		lastShaderCheckMs = now;
		std::string path = GetLayerShaderPath();
		uint64_t fileTime = GetFileTime(path);
		if(pixelShader && fileTime == pixelShaderFileTime){
			return true;
		}
		std::string source = pixelShaderFallbackSource;
		if(fileTime != 0){
			std::ifstream file(path, std::ios::binary);
			if(file){
				source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}else{
				fileTime = 0;
			}
		}
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(source.c_str(), source.size(), "vrlink_layer_ps", nullptr, nullptr,
			"main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
		if(FAILED(hr) || !blob){
			if(errors){
				DriverLog("FrameProcessor: PS compile error: %s", (char*)errors->GetBufferPointer());
				errors->Release();
			}
			// keep the previous shader if there is one so live edits with a typo
			// do not black out the processing
			pixelShaderFileTime = fileTime;
			if(pixelShader){
				return true;
			}
			shaderFailed = true;
			return false;
		}
		ID3D11PixelShader* newShader = nullptr;
		hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &newShader);
		blob->Release();
		if(FAILED(hr)){
			PROCESSOR_ERROR("FrameProcessor: CreatePixelShader failed: 0x%08X", (unsigned)hr);
			if(pixelShader){
				return true;
			}
			shaderFailed = true;
			return false;
		}
		if(pixelShader){
			pixelShader->Release();
		}
		pixelShader = newShader;
		pixelShaderFileTime = fileTime;
		shaderFailed = false;
		DriverLog("FrameProcessor: pixel shader ready (%s)", fileTime ? "from file" : "embedded");
	}
	return pixelShader != nullptr;
}

void FrameProcessor::ReleaseScratchSet(ScratchSet &set){
	if(set.inSRV){ set.inSRV->Release(); set.inSRV = nullptr; }
	if(set.in){ set.in->Release(); set.in = nullptr; }
	if(set.outRTV){ set.outRTV->Release(); set.outRTV = nullptr; }
	if(set.out){ set.out->Release(); set.out = nullptr; }
}

bool FrameProcessor::EnsureScratch(uint32_t width, uint32_t height, DXGI_FORMAT format){
	uint64_t key = ((uint64_t)format << 48) | ((uint64_t)width << 24) | (uint64_t)height;
	uint64_t now = NowMs();
	auto found = scratchSets.find(key);
	if(found != scratchSets.end()){
		found->second.lastUsedMs = now;
		scratchIn = found->second.in;
		scratchInSRV = found->second.inSRV;
		scratchOut = found->second.out;
		scratchOutRTV = found->second.outRTV;
		return true;
	}

	ScratchSet set;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;

	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if(FAILED(device->CreateTexture2D(&desc, nullptr, &set.in))){
		PROCESSOR_ERROR("FrameProcessor: failed to create scratchIn %ux%u", width, height);
		return false;
	}
	if(FAILED(device->CreateShaderResourceView(set.in, nullptr, &set.inSRV))){
		ReleaseScratchSet(set);
		return false;
	}
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	if(FAILED(device->CreateTexture2D(&desc, nullptr, &set.out))){
		PROCESSOR_ERROR("FrameProcessor: failed to create scratchOut %ux%u", width, height);
		ReleaseScratchSet(set);
		return false;
	}
	if(FAILED(device->CreateRenderTargetView(set.out, nullptr, &set.outRTV))){
		ReleaseScratchSet(set);
		return false;
	}
	set.lastUsedMs = now;

	// bound the cache: evict the least recently used entry beyond the cap
	while(scratchSets.size() >= maxScratchSets){
		auto lru = scratchSets.begin();
		for(auto it = scratchSets.begin(); it != scratchSets.end(); ++it){
			if(it->second.lastUsedMs < lru->second.lastUsedMs){
				lru = it;
			}
		}
		ReleaseScratchSet(lru->second);
		scratchSets.erase(lru);
	}
	auto inserted = scratchSets.emplace(key, set).first;
	scratchIn = inserted->second.in;
	scratchInSRV = inserted->second.inSRV;
	scratchOut = inserted->second.out;
	scratchOutRTV = inserted->second.outRTV;
	DriverLog("FrameProcessor: created scratch textures %ux%u format=%u (%zu sets cached)",
		width, height, (unsigned)format, scratchSets.size());
	return true;
}

static const int lutSize = 512;
static const float lutMaxRadius = 1.0f;

// non uniform catmull rom style hermite interpolation through sorted knots,
// flat outside the knot range
static double EvaluateCurve(const std::vector<StreamFrameDistortionPoint> &points, double r){
	if(points.empty()){
		return 1.0;
	}
	if(points.size() == 1 || r <= points.front().r){
		return r <= points.front().r ? points.front().scale : points.back().scale;
	}
	if(r >= points.back().r){
		return points.back().scale;
	}
	size_t i = 0;
	while(i + 2 < points.size() && r > points[i + 1].r){
		i++;
	}
	double r0 = points[i].r, r1 = points[i + 1].r;
	double v0 = points[i].scale, v1 = points[i + 1].scale;
	double h = r1 - r0;
	if(h <= 0){
		return v0;
	}
	// one sided or centered finite difference tangents
	double m0, m1;
	if(i == 0){
		m0 = (v1 - v0) / h;
	}else{
		double hr = points[i + 1].r - points[i - 1].r;
		m0 = hr > 0 ? (points[i + 1].scale - points[i - 1].scale) / hr : 0;
	}
	if(i + 2 >= points.size()){
		m1 = (v1 - v0) / h;
	}else{
		double hr = points[i + 2].r - points[i].r;
		m1 = hr > 0 ? (points[i + 2].scale - points[i].scale) / hr : 0;
	}
	double t = (r - r0) / h;
	double t2 = t * t, t3 = t2 * t;
	return (2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * h * m0
		+ (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * h * m1;
}

// resolve the curve to use for a given eye (0/1, or -1 when not per eye) and
// axis (0 horizontal / 1 vertical, or -1 when not per axis). a missing named
// curve falls back to the base curve.
struct EffectiveCurve{
	double k1 = 0;
	double k2 = 0;
	const std::vector<StreamFrameDistortionPoint>* points = nullptr;
};
static EffectiveCurve ResolveCurve(const StreamFrameConfig &config, int eye, int axis){
	EffectiveCurve result;
	result.k1 = config.k1;
	result.k2 = config.k2;
	result.points = &config.distortion.points;
	std::string key = "";
	if(eye >= 0 && axis >= 0){
		key = std::string(eye == 0 ? "left" : "right") + (axis == 0 ? "Horizontal" : "Vertical");
	}else if(eye >= 0){
		key = eye == 0 ? "left" : "right";
	}else if(axis >= 0){
		key = axis == 0 ? "horizontal" : "vertical";
	}
	if(!key.empty()){
		auto found = config.distortion.curves.find(key);
		if(found != config.distortion.curves.end()){
			result.k1 = found->second.k1;
			result.k2 = found->second.k2;
			result.points = &found->second.points;
		}
	}
	return result;
}

// serialize everything the lut depends on, for change detection
static std::string BuildLutKey(const StreamFrameConfig &config){
	std::string key = config.distortion.mode;
	key += config.distortion.perEye ? "|E" : "|e";
	key += config.distortion.perAxis ? "A" : "a";
	char buffer[64];
	auto appendCurve = [&](const EffectiveCurve &curve){
		snprintf(buffer, sizeof(buffer), "|%.9g,%.9g", curve.k1, curve.k2);
		key += buffer;
		for(const auto &point : *curve.points){
			snprintf(buffer, sizeof(buffer), ";%.9g:%.9g", point.r, point.scale);
			key += buffer;
		}
	};
	int eyeCount = config.distortion.perEye ? 2 : 1;
	int axisCount = config.distortion.perAxis ? 2 : 1;
	for(int eye = 0; eye < eyeCount; eye++){
		for(int axis = 0; axis < axisCount; axis++){
			appendCurve(ResolveCurve(config, config.distortion.perEye ? eye : -1, config.distortion.perAxis ? axis : -1));
		}
	}
	return key;
}

bool FrameProcessor::BakeLutIfNeeded(const StreamFrameConfig &config){
	std::string key = BuildLutKey(config);
	if(lutBaked && key == lastLutKey){
		return true;
	}

	int eyeCount = config.distortion.perEye ? 2 : 1;
	int axisCount = config.distortion.perAxis ? 2 : 1;
	int rowCount = eyeCount * axisCount;
	// recreate the texture when the row count changes
	if(lutTexture && rowCount != lutRowCount){
		if(lutSRV){ lutSRV->Release(); lutSRV = nullptr; }
		lutTexture->Release();
		lutTexture = nullptr;
	}
	if(!lutTexture){
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = lutSize;
		desc.Height = rowCount;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(device->CreateTexture2D(&desc, nullptr, &lutTexture))){
			PROCESSOR_ERROR("FrameProcessor: failed to create lut texture");
			return false;
		}
		if(FAILED(device->CreateShaderResourceView(lutTexture, nullptr, &lutSRV))){
			return false;
		}
		lutRowCount = rowCount;
	}

	std::vector<float> data(lutSize * rowCount);
	bool spline = config.distortion.mode == "spline";
	// row order: eye major, axis minor: [L], [L,R], [H,V] or [LH,LV,RH,RV]
	for(int eye = 0; eye < eyeCount; eye++){
		for(int axis = 0; axis < axisCount; axis++){
			int row = eye * axisCount + axis;
			EffectiveCurve curve = ResolveCurve(config, config.distortion.perEye ? eye : -1, config.distortion.perAxis ? axis : -1);
			std::vector<StreamFrameDistortionPoint> sortedPoints = *curve.points;
			std::sort(sortedPoints.begin(), sortedPoints.end(), [](const StreamFrameDistortionPoint &a, const StreamFrameDistortionPoint &b){
				return a.r < b.r;
			});
			for(int i = 0; i < lutSize; i++){
				double r = (double)i / (lutSize - 1) * lutMaxRadius;
				double scale;
				if(spline){
					scale = EvaluateCurve(sortedPoints, r);
				}else{
					double r2 = r * r;
					scale = 1.0 + curve.k1 * r2 + curve.k2 * r2 * r2;
				}
				data[row * lutSize + i] = (float)scale;
			}
		}
	}
	context->UpdateSubresource(lutTexture, 0, nullptr, data.data(), lutSize * sizeof(float), 0);

	lastLutKey = key;
	lutBaked = true;
	DriverLog("FrameProcessor: baked distortion lut (%s, %d rows)", spline ? "spline" : "k1k2", rowCount);
	return true;
}
ID3D11Texture2D* FrameProcessor::OpenShared(vr::SharedTextureHandle_t handle){
	auto it = openedTextures.find((uint64_t)handle);
	if(it != openedTextures.end()){
		return it->second;
	}
	ID3D11Texture2D* texture = nullptr;
	// legacy shared handle (observed unTextureFlags == 0)
	HRESULT hr = device->OpenSharedResource((HANDLE)(uintptr_t)handle, __uuidof(ID3D11Texture2D), (void**)&texture);
	if(FAILED(hr) || !texture){
		PROCESSOR_ERROR("FrameProcessor: OpenSharedResource failed for %llx: 0x%08X", (unsigned long long)handle, (unsigned)hr);
		return nullptr;
	}
	openedTextures[(uint64_t)handle] = texture;
	return texture;
}

void FrameProcessor::EvictTexture(vr::SharedTextureHandle_t handle){
	std::lock_guard<std::mutex> guard(lock);
	auto it = openedTextures.find((uint64_t)handle);
	if(it != openedTextures.end()){
		it->second->Release();
		openedTextures.erase(it);
	}
}

void FrameProcessor::EvictAll(){
	std::lock_guard<std::mutex> guard(lock);
	for(auto &pair : openedTextures){
		pair.second->Release();
	}
	openedTextures.clear();
	// scratch sets are cheap to repopulate and app teardown is the natural
	// moment to return the memory (sets are hundreds of MB at high supersample)
	for(auto &pair : scratchSets){
		ReleaseScratchSet(pair.second);
	}
	scratchSets.clear();
	scratchIn = nullptr;
	scratchInSRV = nullptr;
	scratchOut = nullptr;
	scratchOutRTV = nullptr;
}

bool FrameProcessor::ProcessEye(ID3D11Texture2D* texture, const vr::VRTextureBounds_t &bounds, int eye, const FrameProcessSettings &settings){
	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	DXGI_FORMAT mappedScratchFormat = DXGI_FORMAT_UNKNOWN;
	bool manualSrgb = false;
	float ditherLsb = 255.0f;
	if(!MapLayerFormat(desc.Format, mappedScratchFormat, manualSrgb, ditherLsb)){
		// log each unsupported format once per session, outside the error
		// budget, so a game launched late still reports why it is untouched
		if(skippedFormats.insert((unsigned)desc.Format).second){
			DriverLog("FrameProcessor: unsupported layer format %u, this app's frames are not processed", (unsigned)desc.Format);
		}
		return false;
	}
	if(!EnsureScratch(desc.Width, desc.Height, mappedScratchFormat)){
		return false;
	}

	// copy the layer into the scratch input (same size and format)
	context->CopyResource(scratchIn, texture);

	// constants
	const StreamFrameConfig &config = settings.config;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if(FAILED(context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))){
		return false;
	}
	FrameProcessorConstants constants = {};
	constants.saturation = (float)(config.saturation / 50.0);
	constants.applyColor = settings.applyColor ? 1.0f : 0.0f;
	// contrast precomputed like the compositor shader: col * mult + offset
	double contrastMult = config.contrast / 50.0;
	double contrastMid = config.contrastMidpoint / 100.0;
	constants.contrastMult = (float)contrastMult;
	constants.contrastOffset = (float)(-contrastMid * contrastMult + contrastMid);
	constants.contrastLinear = config.contrastLinear ? 1.0f : 0.0f;
	constants.outGamma = (float)config.gamma;
	constants.casStrength = (float)config.cas.strength;
	constants.casEnable = config.cas.enable ? 1.0f : 0.0f;
	constants.annulusEnable = config.distortion.annulus.enable ? 1.0f : 0.0f;
	constants.annulusMin = (float)config.distortion.annulus.rMin;
	constants.annulusMax = (float)config.distortion.annulus.rMax;
	constants.annulusFeather = (float)config.distortion.annulus.feather;
	constants.ditherEnable = config.dither ? 1.0f : 0.0f;
	constants.lutMaxR = lutMaxRadius;
	// row order is eye major, axis minor
	int axisCount = config.distortion.perAxis ? 2 : 1;
	constants.lutRowBase = config.distortion.perEye ? (float)(eye * axisCount) : 0.0f;
	constants.lutRowCountF = (float)lutRowCount;
	constants.perAxisEnable = config.distortion.perAxis ? 1.0f : 0.0f;
	constants.dimAmount = (float)settings.dimAmount;
	constants.manualSrgb = manualSrgb ? 1.0f : 0.0f;
	constants.ditherLsb = ditherLsb;
	double centerOffsetX = eye == 0 ? config.centerOffsetXLeft : config.centerOffsetXRight;
	constants.center[0] = (float)(0.5 + centerOffsetX);
	constants.center[1] = (float)(0.5 + config.centerOffsetY);
	float uMin = (float)bounds.uMin, vMin = (float)bounds.vMin;
	float uSize = (float)(bounds.uMax - bounds.uMin), vSize = (float)(bounds.vMax - bounds.vMin);
	if(uSize == 0){ uSize = 1; }
	if(vSize == 0){ vSize = 1; }
	constants.boundsMin[0] = uMin;
	constants.boundsMin[1] = vMin;
	constants.boundsSize[0] = uSize;
	constants.boundsSize[1] = vSize;
	constants.texelSize[0] = 1.0f / desc.Width;
	constants.texelSize[1] = 1.0f / desc.Height;
	constants.aspect = desc.Width != 0 ? (float)((double)desc.Height * vSize / ((double)desc.Width * uSize)) : 1.0f;
	constants.colorMultiplier[0] = (float)config.colorMultiplier.r;
	constants.colorMultiplier[1] = (float)config.colorMultiplier.g;
	constants.colorMultiplier[2] = (float)config.colorMultiplier.b;
	constants.colorMultiplier[3] = 1.0f;
	if(config.srgbMatrix.size() == 9){
		constants.matrixEnable = 1.0f;
		for(int i = 0; i < 3; i++){
			constants.matR[i] = (float)config.srgbMatrix[i];
			constants.matG[i] = (float)config.srgbMatrix[3 + i];
			constants.matB[i] = (float)config.srgbMatrix[6 + i];
		}
	}else{
		constants.matrixEnable = 0.0f;
		constants.matR[0] = 1.0f;
		constants.matG[1] = 1.0f;
		constants.matB[2] = 1.0f;
	}
	memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(constantBuffer, 0);

	// draw a fullscreen triangle into the bounds region of the scratch output.
	// with full 0-1 bounds this is the whole texture; with a shared side by side
	// texture each eye only touches its own half.
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = uMin * desc.Width;
	viewport.TopLeftY = vMin * desc.Height;
	viewport.Width = uSize * desc.Width;
	viewport.Height = vSize * desc.Height;
	viewport.MaxDepth = 1.0f;
	context->ClearState();
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vertexShader, nullptr, 0);
	context->PSSetShader(pixelShader, nullptr, 0);
	ID3D11ShaderResourceView* srvs[2] = { scratchInSRV, lutSRV };
	context->PSSetShaderResources(0, 2, srvs);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetConstantBuffers(0, 1, &constantBuffer);
	context->RSSetViewports(1, &viewport);
	context->OMSetRenderTargets(1, &scratchOutRTV, nullptr);
	context->Draw(3, 0);
	context->ClearState();

	// copy only the bounds region back into the layer texture
	D3D11_BOX box = {};
	box.left = (UINT)(uMin * desc.Width);
	box.top = (UINT)(vMin * desc.Height);
	box.right = (UINT)((uMin + uSize) * desc.Width);
	box.bottom = (UINT)((vMin + vSize) * desc.Height);
	box.back = 1;
	context->CopySubresourceRegion(texture, 0, box.left, box.top, 0, scratchOut, 0, &box);
	return true;
}

bool FrameProcessor::ProcessSceneLayer(vr::SharedTextureHandle_t leftEye, vr::SharedTextureHandle_t rightEye,
	const vr::VRTextureBounds_t &leftBounds, const vr::VRTextureBounds_t &rightBounds,
	vr::SharedTextureHandle_t syncTexture, const FrameProcessSettings &settings){
	std::lock_guard<std::mutex> guard(lock);

	// re-arm the diagnostic budget every 5 minutes so problems in apps
	// launched later in the session are not silenced by earlier errors
	uint64_t nowMs = NowMs();
	if(nowMs - lastErrorResetMs > 300000){
		lastErrorResetMs = nowMs;
		errorCount = 0;
	}

	if(!EnsureDevice() || !EnsureShaders()){
		return false;
	}
	if(!BakeLutIfNeeded(settings.config)){
		return false;
	}

	// acquire the frame via the sync texture keyed mutex. this guarantees the
	// app has finished rendering the layer textures, and our release followed by
	// vrlink's own acquire orders its reads after our writes.
	ID3D11Texture2D* sync = OpenShared(syncTexture);
	if(!sync){
		return false;
	}
	IDXGIKeyedMutex* mutex = nullptr;
	if(FAILED(sync->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&mutex)) || !mutex){
		PROCESSOR_ERROR("FrameProcessor: sync texture has no keyed mutex, skipping processing");
		return false;
	}
	// base timeout from config; after a skipped frame escalate so a transient
	// long hold costs one flash instead of a visible streak of them. clamped
	// to keep a misconfigured value from stalling the pipeline.
	int baseTimeout = settings.config.syncTimeoutMs;
	if(baseTimeout < 1){ baseTimeout = 1; }
	if(baseTimeout > 100){ baseTimeout = 100; }
	uint32_t timeout = (uint32_t)baseTimeout;
	if(consecutiveSyncSkips > 0){
		uint32_t escalated = (uint32_t)baseTimeout * 3;
		timeout = escalated < 15 ? 15 : escalated;
	}
	HRESULT hr = mutex->AcquireSync(0, timeout);
	if(hr != S_OK){
		// timeout or abandoned: skip this frame rather than stall the pipeline
		consecutiveSyncSkips++;
		mutex->Release();
		PROCESSOR_ERROR("FrameProcessor: AcquireSync returned 0x%08X, skipping frame (%d consecutive)", (unsigned)hr, consecutiveSyncSkips);
		return false;
	}
	if(consecutiveSyncSkips > 0){
		DriverLog("FrameProcessor: sync recovered after %d skipped frames", consecutiveSyncSkips);
		consecutiveSyncSkips = 0;
	}

	bool ok = true;
	ID3D11Texture2D* left = OpenShared(leftEye);
	ID3D11Texture2D* right = OpenShared(rightEye);
	if(left){
		ok &= ProcessEye(left, leftBounds, 0, settings);
	}
	bool boundsDiffer = memcmp(&leftBounds, &rightBounds, sizeof(vr::VRTextureBounds_t)) != 0;
	if(right && (right != left || boundsDiffer)){
		ok &= ProcessEye(right, rightBounds, 1, settings);
	}

	// submit our commands to the GPU before releasing the mutex. without this the
	// commands sit in the immediate context (this device never presents, so it
	// almost never kicks on its own), ReleaseSync signals before any of our work,
	// and the driver encodes the untouched texture.
	context->Flush();

	mutex->ReleaseSync(0);
	mutex->Release();
	return ok;
}

#endif // _WIN32
