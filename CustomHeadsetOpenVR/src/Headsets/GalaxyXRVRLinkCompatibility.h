#pragma once

#include <atomic>

// Windows-only, runtime compatibility for the one verified VRLink build.
// The hook is installed lazily after driver_vrlink.dll has been loaded and
// never modifies the DLL on disk.
class GalaxyXRVRLinkCompatibility{
public:
	void RunFrame(bool enabled);
	void Cleanup();
	bool VerifyLoadedModule();
	bool IsModuleVerified() const;
	bool IsHookActive() const;

private:
	enum class State{
		WaitingForEnable,
		WaitingForModule,
		InstalledDisabled,
		Active,
		Refused,
	};

	State state = State::WaitingForEnable;
	void* module = nullptr;
	void* target = nullptr;
	std::atomic<bool> moduleVerified{false};
	std::atomic<bool> hookActive{false};
};

extern GalaxyXRVRLinkCompatibility galaxyXRVRLinkCompatibility;
