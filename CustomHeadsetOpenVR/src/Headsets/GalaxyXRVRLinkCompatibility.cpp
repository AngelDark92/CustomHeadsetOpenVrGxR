#include "GalaxyXRVRLinkCompatibility.h"

#include "../Driver/DriverLog.h"

#ifdef _WIN32

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../../ThirdParty/minhook/include/MinHook.h"

namespace {
	constexpr wchar_t VRLinkModuleName[] = L"driver_vrlink.dll";
	constexpr uintptr_t GetHmdModelRva = 0x009E5830;
	constexpr int EyeCapableWirelessCapabilityClass = 8;
	constexpr char GalaxyXRModel[] = "Samsung Galaxy XR";
	constexpr size_t GalaxyXRModelLength = sizeof(GalaxyXRModel) - 1;
	constexpr size_t MsvcStringSizeOffset = 16;
	constexpr size_t MsvcStringCapacityOffset = 24;
	constexpr size_t MsvcStringSmallCapacity = 15;
	constexpr size_t MaximumAcceptedStringCapacity = 4096;

	constexpr std::array<unsigned char, 32> SupportedDllSha256 = {
		0x8D, 0x8C, 0x72, 0xD2, 0x2F, 0x1A, 0x50, 0x7E,
		0x66, 0x1E, 0x7B, 0x61, 0x2E, 0x90, 0xC9, 0x41,
		0x6D, 0x12, 0xDF, 0x5B, 0xEA, 0xA5, 0xCB, 0x3B,
		0x76, 0x4D, 0x34, 0x4D, 0xDB, 0x3D, 0xA2, 0x68,
	};

	constexpr std::array<unsigned char, 15> SupportedGetHmdModelPrologue = {
		0x48, 0x89, 0x5C, 0x24, 0x18,
		0x48, 0x89, 0x74, 0x24, 0x20,
		0x57, 0x48, 0x83, 0xEC, 0x40,
	};

	using GetHmdModelFunction = int (*)(const void* modelString);
	std::atomic<GetHmdModelFunction> originalGetHmdModel{nullptr};

	template<typename T>
	bool ReadCurrentProcessMemory(const void* address, T& value){
		SIZE_T bytesRead = 0;
		return address != nullptr &&
			ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof(value), &bytesRead) != FALSE &&
			bytesRead == sizeof(value);
	}

	bool ReadCurrentProcessBytes(const void* address, void* destination, size_t length){
		SIZE_T bytesRead = 0;
		return address != nullptr && destination != nullptr &&
			ReadProcessMemory(GetCurrentProcess(), address, destination, length, &bytesRead) != FALSE &&
			bytesRead == length;
	}

	bool IsExactGalaxyXRModel(const void* modelString){
		if(modelString == nullptr){
			return false;
		}

		const auto* objectBytes = static_cast<const unsigned char*>(modelString);
		size_t length = 0;
		size_t capacity = 0;
		if(!ReadCurrentProcessMemory(objectBytes + MsvcStringSizeOffset, length) ||
			!ReadCurrentProcessMemory(objectBytes + MsvcStringCapacityOffset, capacity)){
			return false;
		}

		if(length != GalaxyXRModelLength ||
			capacity < length ||
			capacity > MaximumAcceptedStringCapacity){
			return false;
		}

		const char* characters = reinterpret_cast<const char*>(modelString);
		if(capacity > MsvcStringSmallCapacity &&
			!ReadCurrentProcessMemory(modelString, characters)){
			return false;
		}

		std::array<char, GalaxyXRModelLength + 1> observed{};
		if(!ReadCurrentProcessBytes(characters, observed.data(), observed.size())){
			return false;
		}

		return observed[GalaxyXRModelLength] == '\0' &&
			std::memcmp(observed.data(), GalaxyXRModel, GalaxyXRModelLength) == 0;
	}

	int GetHmdModelHook(const void* modelString){
		const GetHmdModelFunction original =
			originalGetHmdModel.load(std::memory_order_acquire);
		int result = 0;
		if(IsExactGalaxyXRModel(modelString)){
			// Capability class 8 is a private VRLink capability reuse for this
			// eye-capable wireless path. It is not a public or transmitted Pico
			// model identity; the public model remains "Samsung Galaxy XR".
			result = EyeCapableWirelessCapabilityClass;
		}else{
			result = original ? original(modelString) : 0;
		}
		return result;
	}

	bool GetModulePath(HMODULE module, std::wstring& path){
		std::vector<wchar_t> buffer(1024);
		while(buffer.size() <= 32768){
			SetLastError(ERROR_SUCCESS);
			const DWORD length = GetModuleFileNameW(
				module,
				buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if(length == 0){
				return false;
			}
			if(length < buffer.size() - 1){
				path.assign(buffer.data(), length);
				return true;
			}
			buffer.resize(buffer.size() * 2);
		}
		return false;
	}

	bool ComputeFileSha256(const std::wstring& path, std::array<unsigned char, 32>& digest){
		HANDLE file = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_SEQUENTIAL_SCAN,
			nullptr);
		if(file == INVALID_HANDLE_VALUE){
			return false;
		}

		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::vector<unsigned char> hashObject;
		bool success = false;

		do{
			if(BCryptOpenAlgorithmProvider(
				&algorithm,
				BCRYPT_SHA256_ALGORITHM,
				nullptr,
				0) < 0){
				break;
			}

			DWORD objectLength = 0;
			DWORD hashLength = 0;
			DWORD resultLength = 0;
			if(BCryptGetProperty(
				algorithm,
				BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objectLength),
				sizeof(objectLength),
				&resultLength,
				0) < 0 ||
				BCryptGetProperty(
					algorithm,
					BCRYPT_HASH_LENGTH,
					reinterpret_cast<PUCHAR>(&hashLength),
					sizeof(hashLength),
					&resultLength,
					0) < 0 ||
				hashLength != digest.size()){
				break;
			}

			hashObject.resize(objectLength);
			if(BCryptCreateHash(
				algorithm,
				&hash,
				hashObject.data(),
				static_cast<ULONG>(hashObject.size()),
				nullptr,
				0,
				0) < 0){
				break;
			}

			std::array<unsigned char, 64 * 1024> buffer{};
			for(;;){
				DWORD bytesRead = 0;
				if(ReadFile(
					file,
					buffer.data(),
					static_cast<DWORD>(buffer.size()),
					&bytesRead,
					nullptr) == FALSE){
					break;
				}
				if(bytesRead == 0){
					if(BCryptFinishHash(
						hash,
						digest.data(),
						static_cast<ULONG>(digest.size()),
						0) >= 0){
						success = true;
					}
					break;
				}
				if(BCryptHashData(hash, buffer.data(), bytesRead, 0) < 0){
					break;
				}
			}
		}while(false);

		if(hash){
			BCryptDestroyHash(hash);
		}
		if(algorithm){
			BCryptCloseAlgorithmProvider(algorithm, 0);
		}
		CloseHandle(file);
		return success;
	}

	std::string Hex(const unsigned char* bytes, size_t length){
		static constexpr char Digits[] = "0123456789ABCDEF";
		std::string result(length * 2, '0');
		for(size_t index = 0; index < length; ++index){
			result[index * 2] = Digits[bytes[index] >> 4];
			result[index * 2 + 1] = Digits[bytes[index] & 0x0F];
		}
		return result;
	}
}

#endif

GalaxyXRVRLinkCompatibility galaxyXRVRLinkCompatibility;

void GalaxyXRVRLinkCompatibility::RunFrame(bool enabled){
#ifdef _WIN32
	if(!enabled){
		if(state == State::Active){
			Cleanup();
			if(state == State::Active){
				return;
			}
		}
		if(state != State::Refused &&
			state != State::InstalledDisabled){
			state = State::WaitingForEnable;
		}
		return;
	}

	if(state == State::Active || state == State::Refused){
		return;
	}
	if(!VerifyLoadedModule()){
		return;
	}
	if(state == State::InstalledDisabled){
		const MH_STATUS reenableStatus = MH_EnableHook(target);
		if(reenableStatus != MH_OK){
			DriverLog(
				"Galaxy XR VRLink compatibility refused: retained hook could not be re-enabled (%s).",
				MH_StatusToString(reenableStatus));
			state = State::Refused;
			return;
		}
		hookActive.store(true, std::memory_order_release);
		state = State::Active;
		DriverLog("Galaxy XR VRLink compatibility re-enabled for the verified module.");
		return;
	}

	const MH_STATUS initializeStatus = MH_Initialize();
	if(initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED){
		DriverLog(
			"Galaxy XR VRLink compatibility refused: MinHook initialization failed (%s); no hook installed.",
			MH_StatusToString(initializeStatus));
		target = nullptr;
		state = State::Refused;
		return;
	}

	GetHmdModelFunction createdOriginal = nullptr;
	const MH_STATUS createStatus = MH_CreateHook(
		target,
		reinterpret_cast<void*>(&GetHmdModelHook),
		reinterpret_cast<void**>(&createdOriginal));
	if(createStatus != MH_OK){
		DriverLog(
			"Galaxy XR VRLink compatibility refused: GetHmdModel hook creation failed (%s); no hook installed.",
			MH_StatusToString(createStatus));
		target = nullptr;
		originalGetHmdModel.store(nullptr, std::memory_order_release);
		state = State::Refused;
		return;
	}
	originalGetHmdModel.store(createdOriginal, std::memory_order_release);

	const MH_STATUS enableStatus = MH_EnableHook(target);
	if(enableStatus != MH_OK){
		MH_RemoveHook(target);
		DriverLog(
			"Galaxy XR VRLink compatibility refused: GetHmdModel hook enable failed (%s); hook removed.",
			MH_StatusToString(enableStatus));
		target = nullptr;
		originalGetHmdModel.store(nullptr, std::memory_order_release);
		state = State::Refused;
		return;
	}

	hookActive.store(true, std::memory_order_release);
	state = State::Active;
	DriverLog(
		"Galaxy XR VRLink compatibility active for verified driver_vrlink.dll SHA-256 %s; exact model \"%s\" uses private capability class %d.",
		Hex(SupportedDllSha256.data(), SupportedDllSha256.size()).c_str(),
		GalaxyXRModel,
		EyeCapableWirelessCapabilityClass);
#else
	(void)enabled;
#endif
}

void GalaxyXRVRLinkCompatibility::Cleanup(){
#ifdef _WIN32
	if(state == State::Active && target){
		HMODULE loadedModule = GetModuleHandleW(VRLinkModuleName);
		if(loadedModule == module){
			const MH_STATUS disableStatus = MH_DisableHook(target);
			if(disableStatus != MH_OK && disableStatus != MH_ERROR_DISABLED){
				DriverLog(
					"Galaxy XR VRLink compatibility cleanup: hook disable failed (%s).",
					MH_StatusToString(disableStatus));
				return;
			}
			// Keep MinHook's disabled record and trampoline for the process
			// lifetime. Removing or clearing it while a callback is unwinding
			// can turn a safe rollback into a use-after-free. Re-enable reuses
			// this exact verified target.
			hookActive.store(false, std::memory_order_release);
			state = State::InstalledDisabled;
			DriverLog(
				"Galaxy XR VRLink compatibility disabled; original trampoline retained for callback-safe rollback.");
			return;
		}else{
			DriverLog("Galaxy XR VRLink compatibility cleanup: driver_vrlink.dll was already unloaded; skipped unsafe hook write.");
			hookActive.store(false, std::memory_order_release);
			moduleVerified.store(false, std::memory_order_release);
			state = State::Refused;
			return;
		}
	}
#endif
	if(state != State::InstalledDisabled &&
		state != State::Refused){
		state = State::WaitingForEnable;
	}
}

bool GalaxyXRVRLinkCompatibility::VerifyLoadedModule(){
#if defined(_WIN32) && !defined(_WIN64)
	static bool loggedUnsupportedArchitecture = false;
	if(!loggedUnsupportedArchitecture){
		DriverLog(
			"Galaxy XR VRLink verification unavailable in Win32: exact compatibility contract is x64-only; preserving pass-through.");
		loggedUnsupportedArchitecture = true;
	}
	moduleVerified.store(false, std::memory_order_release);
	state = State::Refused;
	return false;
#elif defined(_WIN32)
	if(state == State::Refused){
		return false;
	}
	HMODULE loadedModule = GetModuleHandleW(VRLinkModuleName);
	if(!loadedModule){
		moduleVerified.store(false, std::memory_order_release);
		state = State::WaitingForModule;
		return false;
	}
	if(moduleVerified.load(std::memory_order_acquire) &&
		loadedModule == module){
		return true;
	}
	if(state == State::Active ||
		state == State::InstalledDisabled){
		DriverLog(
			"Galaxy XR VRLink verification refused: loaded module changed after hook creation; retained trampoline was not reused.");
		state = State::Refused;
		return false;
	}
	module = loadedModule;
	std::wstring modulePath;
	std::array<unsigned char, 32> observedHash{};
	if(!GetModulePath(loadedModule, modulePath) ||
		!ComputeFileSha256(modulePath, observedHash)){
		DriverLog("Galaxy XR VRLink verification refused: failed to hash loaded driver_vrlink.dll with Windows CNG.");
		state = State::Refused;
		return false;
	}
	if(observedHash != SupportedDllSha256){
		moduleVerified.store(false, std::memory_order_release);
		DriverLog(
			"Galaxy XR VRLink verification refused: unsupported driver_vrlink.dll SHA-256 %s.",
			Hex(observedHash.data(), observedHash.size()).c_str());
		state = State::Refused;
		return false;
	}
	target = reinterpret_cast<unsigned char*>(loadedModule) + GetHmdModelRva;
	std::array<unsigned char, SupportedGetHmdModelPrologue.size()> observedPrologue{};
	if(!ReadCurrentProcessBytes(target, observedPrologue.data(), observedPrologue.size()) ||
		observedPrologue != SupportedGetHmdModelPrologue){
		moduleVerified.store(false, std::memory_order_release);
		DriverLog(
			"Galaxy XR VRLink verification refused: prologue at RVA 0x%08llX was %s.",
			static_cast<unsigned long long>(GetHmdModelRva),
			Hex(observedPrologue.data(), observedPrologue.size()).c_str());
		target = nullptr;
		state = State::Refused;
		return false;
	}
	moduleVerified.store(true, std::memory_order_release);
	DriverLog(
		"Galaxy XR VRLink verification accepted exact SHA-256 %s and RVA/prologue contract.",
		Hex(SupportedDllSha256.data(), SupportedDllSha256.size()).c_str());
	return true;
#else
	return false;
#endif
}

bool GalaxyXRVRLinkCompatibility::IsModuleVerified() const{
	return moduleVerified.load(std::memory_order_acquire);
}

bool GalaxyXRVRLinkCompatibility::IsHookActive() const{
	return hookActive.load(std::memory_order_acquire);
}
