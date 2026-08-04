#include "../DriverLog.h"
#include "Hooking.h"
#include "InterfaceHookInjector.h"
#include "../DeviceProvider.h"
#include "../EyeTrackingTap.h"

static CustomHeadsetDeviceProvider *Driver = nullptr;

static Hook<void*(*)(vr::IVRDriverContext *, const char *, vr::EVRInitError *)> 
	GetGenericInterfaceHook("IVRDriverContext::GetGenericInterface");

static Hook<void(*)(vr::IVRServerDriverHost *, uint32_t, const vr::DriverPose_t &, uint32_t)>
	TrackedDevicePoseUpdatedHook005("IVRServerDriverHost005::TrackedDevicePoseUpdated");

static Hook<void(*)(vr::IVRServerDriverHost *, uint32_t, const vr::DriverPose_t &, uint32_t)>
	TrackedDevicePoseUpdatedHook006("IVRServerDriverHost006::TrackedDevicePoseUpdated");

static Hook<void(*)(vr::IVRServerDriverHost *_this, const char *pchDeviceSerialNumber, vr::ETrackedDeviceClass eDeviceClass, vr::ITrackedDeviceServerDriver *pDriver)>
	TrackedDeviceAddedHook006("IVRServerDriverHost006::TrackedDeviceAdded");

// eye tracking tap: intercept driver-side gaze publication (vrlink publishes
// gaze into vrserver through these two IVRDriverInput_004 entries)
static Hook<vr::EVRInputError(*)(vr::IVRDriverInput *, vr::PropertyContainerHandle_t, const char *, vr::VRInputComponentHandle_t *)>
	CreateEyeTrackingComponentHook004("IVRDriverInput004::CreateEyeTrackingComponent");

static Hook<vr::EVRInputError(*)(vr::IVRDriverInput *, vr::VRInputComponentHandle_t, const vr::VREyeTrackingData_t *, double)>
	UpdateEyeTrackingComponentHook004("IVRDriverInput004::UpdateEyeTrackingComponent");

static void DetourTrackedDevicePoseUpdated005(vr::IVRServerDriverHost *_this, uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize)
{
	//TRACE("ServerTrackedDeviceProvider::DetourTrackedDevicePoseUpdated(%d)", unWhichDevice);
	auto pose = newPose;
	if (Driver->HandleDevicePoseUpdated(unWhichDevice, pose))
	{
		TrackedDevicePoseUpdatedHook005.originalFunc(_this, unWhichDevice, pose, unPoseStructSize);
	}
}

static void DetourTrackedDevicePoseUpdated006(vr::IVRServerDriverHost *_this, uint32_t unWhichDevice, const vr::DriverPose_t &newPose, uint32_t unPoseStructSize)
{
	//TRACE("ServerTrackedDeviceProvider::DetourTrackedDevicePoseUpdated(%d)", unWhichDevice);
	auto pose = newPose;
	if (Driver->HandleDevicePoseUpdated(unWhichDevice, pose))
	{
		TrackedDevicePoseUpdatedHook006.originalFunc(_this, unWhichDevice, pose, unPoseStructSize);
	}
}

static void DetourTrackedDeviceAdded006(vr::IVRServerDriverHost *_this, const char *pchDeviceSerialNumber, vr::ETrackedDeviceClass eDeviceClass, vr::ITrackedDeviceServerDriver *pDriver)
{
	if (Driver->HandleDeviceAdded(pchDeviceSerialNumber, eDeviceClass, pDriver))  
	{
		TrackedDeviceAddedHook006.originalFunc(_this, pchDeviceSerialNumber, eDeviceClass, pDriver);
		// // Add 19 more copies with random serial numbers
		// for (int i = 0; i < 19; i++)
		// {
		// 	char serial[32]{};
		// 	snprintf(serial, sizeof(serial), "FIM-%d-%d", i, rand());
		// 	TrackedDeviceAddedHook006.originalFunc(_this, serial, eDeviceClass, pDriver);
		// }
	}
}

static vr::EVRInputError DetourCreateEyeTrackingComponent004(vr::IVRDriverInput *_this, vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle)
{
	auto error = CreateEyeTrackingComponentHook004.originalFunc(_this, ulContainer, pchName, pHandle);
	eyeTrackingTap.OnCreateComponent(ulContainer, pchName,
		pHandle ? *pHandle : vr::k_ulInvalidInputComponentHandle, error);
	return error;
}

static vr::EVRInputError DetourUpdateEyeTrackingComponent004(vr::IVRDriverInput *_this, vr::VRInputComponentHandle_t ulComponent, const vr::VREyeTrackingData_t *pEyeTrackingData, double fTimeOffset)
{
	auto error = UpdateEyeTrackingComponentHook004.originalFunc(_this, ulComponent, pEyeTrackingData, fTimeOffset);
	eyeTrackingTap.OnUpdateComponent(ulComponent, pEyeTrackingData, fTimeOffset);
	return error;
}

static void *DetourGetGenericInterface(vr::IVRDriverContext *_this, const char *pchInterfaceVersion, vr::EVRInitError *peError)
{
	Driver->driverContexts.insert(_this);  // Store the driver context for later use
	
	// TRACE("ServerTrackedDeviceProvider::DetourGetGenericInterface(%s)", pchInterfaceVersion);
	auto originalInterface = GetGenericInterfaceHook.originalFunc(_this, pchInterfaceVersion, peError);

	std::string iface(pchInterfaceVersion);
	if (iface == "IVRServerDriverHost_005")
	{
		if (!IHook::Exists(TrackedDevicePoseUpdatedHook005.name))
		{
			TrackedDevicePoseUpdatedHook005.CreateHookInObjectVTable(originalInterface, 1, &DetourTrackedDevicePoseUpdated005);
			IHook::Register(&TrackedDevicePoseUpdatedHook005);
		}
	}
	else if (iface == "IVRServerDriverHost_006")
	{
		if (!IHook::Exists(TrackedDevicePoseUpdatedHook006.name))
		{
			TrackedDevicePoseUpdatedHook006.CreateHookInObjectVTable(originalInterface, 1, &DetourTrackedDevicePoseUpdated006);
			IHook::Register(&TrackedDevicePoseUpdatedHook006);
		}
		if (!IHook::Exists(TrackedDeviceAddedHook006.name))
		{
			TrackedDeviceAddedHook006.CreateHookInObjectVTable(originalInterface, 0, &DetourTrackedDeviceAdded006);
			IHook::Register(&TrackedDeviceAddedHook006);
		}
	}
	else if (iface == "IVRDriverInput_004")
	{
		// IVRDriverInput_004 vtable order (openvr_driver.h declaration order,
		// no overloads): 0 CreateBooleanComponent, 1 UpdateBooleanComponent,
		// 2 CreateScalarComponent, 3 UpdateScalarComponent,
		// 4 CreateHapticComponent, 5 CreateSkeletonComponent,
		// 6 UpdateSkeletonComponent, 7 CreatePoseComponent,
		// 8 UpdatePoseComponent, 9 CreateEyeTrackingComponent,
		// 10 UpdateEyeTrackingComponent
		if (!IHook::Exists(CreateEyeTrackingComponentHook004.name))
		{
			CreateEyeTrackingComponentHook004.CreateHookInObjectVTable(originalInterface, 9, &DetourCreateEyeTrackingComponent004);
			IHook::Register(&CreateEyeTrackingComponentHook004);
		}
		if (!IHook::Exists(UpdateEyeTrackingComponentHook004.name))
		{
			UpdateEyeTrackingComponentHook004.CreateHookInObjectVTable(originalInterface, 10, &DetourUpdateEyeTrackingComponent004);
			IHook::Register(&UpdateEyeTrackingComponentHook004);
		}
	}

	return originalInterface;
}

void InjectHooks(CustomHeadsetDeviceProvider *driver, vr::IVRDriverContext *pDriverContext)
{
	Driver = driver;

	// auto err = MH_Initialize();
	// if (err == MH_OK)
	// {
	// 	GetGenericInterfaceHook.CreateHookInObjectVTable(pDriverContext, 0, &DetourGetGenericInterface);
	// 	IHook::Register(&GetGenericInterfaceHook);
	// }
	// else
	// {
	// 	DriverLog("MH_Initialize error: %s", MH_StatusToString(err));
	// }
	GetGenericInterfaceHook.CreateHookInObjectVTable(pDriverContext, 0, &DetourGetGenericInterface);
	IHook::Register(&GetGenericInterfaceHook);
}

void DisableHooks()
{
	IHook::DestroyAll();
	// MH_Uninitialize();
}