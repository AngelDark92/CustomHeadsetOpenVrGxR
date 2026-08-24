# Galaxy XR integration architecture

## Runtime ownership

Steam Link's Valve driver, `driver_vrlink`, owns streaming, the HMD, controllers,
poses, compositor interaction, and Steam Link's eye/face OSC output. This project
does not wrap or replace those devices.

`GalaxyXRResources/DriverFiles` is a Valve-style external-vendor companion:

- `driver.vrdrivermanifest` uses `resourceOnly: true`, `alwaysActivate: true`,
  and an empty `hmd_presence` list.
- `default.vrsettings` provides the `vrlink_<Build.PRODUCT>` Galaxy identity,
  resource paths, display defaults, and eye capability flags.
- `driver.vrresources` maps the HMD/controller icons, input profiles, bindings,
  and render models under `{galaxyxrresources}`.

There is no GXRP network service, custom eye publisher, shared-memory face path,
desktop IP, pairing secret, APK hash enrollment, or private VRLink hook.

## Face and tongue tracking

The APK's GXR Face Bridge translates Galaxy XR
`XR_ANDROID_face_tracking` data to the `XR_FB_face_tracking2` interface Steam
Link already understands. Steam Link sends eye and face data through OSC.

Users who want VRCFaceTracking install the matching Galaxy XR LinkFT module and
configure Steam Link Advanced Settings:

1. Enable OSC.
2. Enable Share eye data.
3. Enable Share face data.
4. Set OSC output port to 9015.

LinkFT receives that stream locally on the PC. Base VRCFaceTracking without the
matching LinkFT module is not sufficient.

## Installer and migration

The Galaxy XR GUI installs only the tracked `galaxyxrresources` package into the
managed driver-package directory and registers its exact path with
`vrpathreg`. Receipt schema v3 owns exactly that one package.

Receipt schemas v1 and v2 remain readable only for safe migration and cleanup.
An upgrade may remove an earlier active `CustomHeadsetOpenVR` package and
`GalaxyXR.VRCFaceTracking.dll` only after their exact receipt paths, manifests,
file trees, and hashes prove ownership. Unrelated drivers, VRCFT modules, SteamVR
settings, and user data remain untouched. Every registration mutation is
exact-path and rollback-capable, and SteamVR must be closed.

## Build and validation

`tools/Build-GalaxyXR.ps1` validates the tracked resource tree, stages it under
`output/galaxyxrresources`, and optionally builds the GUI. It never deploys to
SteamVR.

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Build-GalaxyXR.ps1 -SkipGui
```

When GUI or installer code changes, also run Rust tests/checks and the Angular
production build. Static success does not prove live SteamVR recognition,
Steam Link OSC output, eye tracking, face expressions, or tongue channels; those
require a headset run.
