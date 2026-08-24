# CustomHeadsetOpenVrGxR architecture and change map

## What this project is

This repository builds one active SteamVR server-driver package named
`CustomHeadsetOpenVR` plus one resource-only companion named
`galaxyxrresources`. For Samsung Galaxy XR, the active driver augments the HMD and controller
objects already created by Valve's `driver_vrlink`; it does not create a second
wireless streaming stack. An authenticated companion bridge on the headset
sends identity, display, timing, pose, eye, and face frames through GXRP. The
Windows driver admits those frames, exposes SteamVR-facing HMD resources and
native eye data, and publishes a lossless face frame for the bundled
VRCFaceTracking v5 module.

The integration is deliberately disabled by default. SteamVR still gets the
existing CustomHeadsetOpenVR behavior unless `galaxyXR.enable` is configured
and all runtime admission gates pass.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `CustomHeadsetOpenVR/` | Native OpenVR server driver and packaged SteamVR files. |
| `CustomHeadsetOpenVR/src/Driver/` | OpenVR provider, hooks, device interception, and frame lifecycle. |
| `CustomHeadsetOpenVR/src/Headsets/GalaxyXR.*` | Galaxy HMD shim: activation, identity, display forwarding/override. |
| `CustomHeadsetOpenVR/src/Headsets/GalaxyXRVRLinkCompatibility.*` | Exact-build, x64-only VRLink compatibility verifier/hook. |
| `CustomHeadsetOpenVR/src/GalaxyXR/` | GXRP protocol, transport, admission profile, clock, display, pose, eye, face, diagnostics. |
| `CustomHeadsetOpenVR/DriverFiles/` | Active DLL package; contains runtime shaders and controller-model source copies used for writable scale/tuning variants, but no Galaxy identity/input/icon defaults. |
| `GalaxyXRResources/DriverFiles/` | Tracked source for the resource-only Valve external-vendor package: Galaxy product settings, inputs, icons, and models. |
| `VRCFT/GalaxyXR.VRCFaceTracking/` | VRCFaceTracking v5 consumer and Android XR-to-Unified Expressions mapping. |
| `ThirdParty/VRCFaceTracking/5.2.3.0/` | Pinned SDK/Core assemblies, license, and provenance. |
| `CustomHeadsetGUI/` | Angular UI plus Tauri/Rust native installer. |
| `tests/GalaxyXRProtocolTests.cpp` | Host protocol, security, clock, mapping-layout, and fuzz regression tests. |
| `protocol/golden/` | Cross-platform GXRP envelope and KDF fixtures. |
| `tools/` | Resource validation, VRCFT SDK acquisition, and optional diagnostic OSC adapter. |
| `output/` | Generated `CustomHeadsetOpenVR` and `galaxyxrresources` packages. Never treat it as source. |

## How SteamVR loads it

`CustomHeadsetOpenVR/DriverFiles/driver.vrdrivermanifest` declares the active
driver package. A build copies `DriverFiles` into
`output/CustomHeadsetOpenVR`, then adds the platform DLL under `bin/win64` or
`bin/win32`. SteamVR loads that DLL as an OpenVR server driver and calls the
provider in `src/Driver/DeviceProvider.cpp`.

SteamVR loads the sibling `galaxyxrresources` manifest before device activation
because it is `resourceOnly`, `alwaysActivate`, has empty `hmd_presence`, and
contains no DLL. SteamVR resolves Galaxy icons, render models, input profiles,
and product defaults relative to `{galaxyxrresources}`:

- `resources/settings/default.vrsettings` supplies Galaxy identity, display,
  and controller defaults for the VRLink-related settings sections.
- `resources/driver.vrresources` maps HMD/controller states to Galaxy icons.
- `resources/input/` defines the HMD and controller input profiles.
- `resources/rendermodels/galaxy_xr_hmd/` contains the stylized headset model;
  `vst_controller_left|right/` contain controller models and textures.
- `resources/icons/galaxyxr/` contains prebuilt HMD/controller status icons.

The checked-in prebuilt status icons under
`GalaxyXRResources/DriverFiles/resources/icons/galaxyxr/` are authoritative.
The normal build stages that resource-only source tree unchanged; it does not
synthesize or recolor icon files.

## Runtime flow

1. SteamVR loads `CustomHeadsetOpenVR` beside `driver_vrlink`.
2. The provider's tracked-device hook sees VRLink's HMD/controllers and keeps
   the original objects as their owners.
3. The resource-only package classifies `xrvst2`/`xrvst2ue` as Galaxy XR before
   activation; this static identity does not authenticate tracking data.
4. When `galaxyXR.enable` is true, the provider wraps the HMD with
   `GalaxyXRShim`. Existing GenericHeadset behavior remains beneath it.
5. `GalaxyXRSystem::RunFrame` configures the bounded host services. It starts
   no transport until pairing and exact client provenance are configured.
6. The headset bridge connects on GXRP control TCP (default 29981), proves the
   pairing key and exact client identity, receives a derived session key, then
   confirms a revisioned, nonempty capability snapshot.
7. Authenticated tracking arrives on GXRP UDP (default 29982). Packets are
   HMAC-authenticated, session-bound, sequence-checked, size-bounded, and
   rejected when stale or tied to the wrong capability revision.
8. Only when the authenticated Galaxy profile also matches the activated HMD
   serial and the VRLink compatibility verifier reports a known active hook may
   the shim expose display/eye/face data or apply timing/pose changes.
9. Disconnect, authentication loss, staleness, deactivation, or cleanup
   invalidates eye/face output; the resource-only static identity remains.

## GXRP security and wire ownership

`GalaxyXRProtocol.*` defines GXRP v1: a fixed 68-byte envelope, bounded payload,
16-byte authentication tag, protocol version, message type, session ID,
sequence, and client monotonic timestamp. `GalaxyXRTransport.*` owns TCP/UDP
sockets and handshake state. `GalaxyXRProfile.*` owns the admitted session and
requires:

- package `com.valvesoftware.steamlinkvr`;
- manufacturer `Samsung` and model `Samsung Galaxy XR`;
- a nonzero session and increasing capability revision;
- one or two nonempty view descriptions;
- tracking tied to the current capability revision.

Configuration additionally requires a 32-byte pairing key, at least one
`allowedClients` record containing an exact version code plus nonzero APK and
bridge SHA-256 digests, and a hash of the loaded host DLL. Legacy single-record
settings migrate in memory. Secrets are read from `%APPDATA%/GalaxyXR/CustomHeadset` by
default and must never be logged or committed.

## Headset identity, display, and pose

`Headsets/GalaxyXR.cpp` is a shim around the real VRLink HMD. It saves original
OpenVR properties before applying the public Samsung Galaxy XR identity and
restores them on loss/deactivation. Its display callbacks ask
`GalaxyXRDisplay` for authenticated view geometry; otherwise they pass through
to VRLink unchanged.

`GalaxyXRPoseTiming` may adjust poses already supplied by VRLink. It never owns
tracking. Prediction is separately configured for HMD/controllers and is
fail-closed when the session or clock mapping is unavailable.

`GalaxyXRVRLinkCompatibility.cpp` contains an exact x64 build verifier and the
private compatibility hook. An unknown DLL, Win32 build, missing signature, or
inactive hook leaves the Galaxy override disabled. Add a new supported VRLink
build only with its verified file hash, exact code bytes/RVA, and regression
evidence; never use a broad pattern match.

## Native eye tracking

`GalaxyXREyePublisher` creates `/eyetracking` on the property container of the
activated HMD, never on hard-coded device 0. Android XR gaze is transformed to
OpenVR's `VREyeTrackingData_t`. The publisher uses host `steady_clock`, bounds
the age by `eye.staleAfterMs`, retries binding, and publishes an invalid sample
on stale/disconnect instead of synthesized center gaze.

The `eye.source` setting selects ownership. `android_xr` is the real Galaxy
path; `vrlink_compat` is reserved for the verified compatibility source;
`synthetic` is diagnostic only; `off` is the safe default.

## Face tracking and VRCFaceTracking

The driver preserves every Android XR face parameter in its specification
order. `GalaxyXRFaceOutput` creates the local memory mapping
`Local\CustomHeadsetOpenVR.GalaxyXR.Face.v1`. `FaceSharedMemoryV1` contains:

- magic/version/structure size;
- an even/odd seqlock generation;
- session ID and sample sequence;
- client and host monotonic timestamps;
- validity/state/source flags;
- 68 float weights and three confidence floats.

Writers commit an odd generation while copying and an even generation when the
snapshot is stable. Any layout change is a versioned protocol change: update
the C++ structure, C# reader, tests, documentation, and module together.

`VRCFT/GalaxyXR.VRCFaceTracking` is a VRCFaceTracking v5 `ExtTrackingModule`.
Its reader opens the mapping read-only, performs stable seqlock reads, validates
all sizes/counts/floats, and clears output after 250 ms without a new sequence.
`AndroidXrUnifiedMapper.cs` maps the 68 Android XR indices to Unified
Expressions, including eye openness/gaze and tongue out/left/right/up/down.
This module is the native VRCFT integration; the OSC adapter in `tools/` is an
optional project diagnostic and is not a VRCFT-native protocol.

## Configuration and safe defaults

The schema lives in `src/Config/Config.h`; JSON parsing lives in
`src/Config/ConfigLoader.cpp`. User settings are read from
`%APPDATA%/GalaxyXR/CustomHeadset/settings.json` in the Galaxy XR build. The
`galaxyXR` section is canonical; legacy `galaxyXr` native fields are accepted
only as lower-precedence migration input. It groups:

- top level: enable, negotiated identity, serial match, public identity, and
  VRLink compatibility mode;
- `telemetry`: listen address/ports, pairing source, allowed version, exact APK
  and bridge hashes;
- `eye`: source, diagnostic pattern, distance, freshness;
- `face`: lossless shared memory and optional compatibility/diagnostic flags;
- `display`, `timing`, `prediction`, and `diagnostics`.

Defaults are intentionally inert: Galaxy is disabled, pairing is mandatory,
the host listens only on loopback, admission hashes are empty, eye is off, and
VRLink compatibility is off. The serial default is the exact known
`VRLINKHMDGALAXYXR` identity; override it only when a captured VRLink device
uses a different exact serial. A minimal real configuration must supply locally
measured provenance; do not put example secrets or hashes in the repository.

## Installer and package transaction

The Angular service locates both sibling SteamVR packages and the optional VRCFT module,
then invokes Rust commands through `tauri_wrapper.ts`.
`src-tauri/src/driver_installer.rs` validates both package identities and required
resources, rejects links, hashes the full source tree, copies it into a
same-volume staging directory, re-hashes it, renames the previous installation
to a backup, activates the stage, validates again, and rolls back on activation
failure. SteamVR driver enablement is changed only after installation succeeds.
Before APK enrollment changes settings, a read-only backend preflight validates
SteamVR is stopped, both source graphs, `vrpathreg`, ownership, and registration
conflicts. Enrollment is restored if the later package transaction fails.
The commit writes an app-owned, Galaxy-profile install receipt with the exact
package tree identities and any owned VRCFT module hash. Readiness and later
replacement re-hash both installed trees against this receipt. The active DLL
package stays non-resource-only; the companion stays binary-free and resource-only.
An existing package without this receipt is treated as unowned and is not
overwritten; move or remove a legacy installation explicitly before the first
managed install.

Uninstall requires that receipt and refuses a drifted/unowned driver or module,
then renames owned artifacts to tombstones before deletion. It does not delete
an arbitrary similarly named directory. The VRCFT module is staged and
hash-checked into `%APPDATA%/VRCFaceTracking/CustomLibs` only when VRCFT is
already installed; an existing same-named module must match the prior receipt.

The GUI transaction does not prove SteamVR recognized the device. Installation,
SteamVR restart, APK/ADB work, and a real headset session are separate live
operations and need separate logs.

The driver writes a one-second, redacted runtime heartbeat to
`%APPDATA%/GalaxyXR/CustomHeadset/galaxyxr-status.json`. The GUI treats its
package/admission checks as setup only; live readiness comes from this heartbeat
and becomes stale after five seconds. It includes state/counters only, never a
pairing key or eye/face sample arrays.

## Where to make a change

| Goal | Primary file(s) | Also update/verify |
| --- | --- | --- |
| Change Galaxy config/default | `src/Config/Config.h`, `ConfigLoader.cpp` | GUI type/default if exposed; docs and config tests. |
| Change admission/handshake | `GalaxyXRProtocol.*`, `GalaxyXRTransport.*`, `GalaxyXRProfile.*` | Android bridge, golden vectors, protocol tests. |
| Change wire structure | `GalaxyXRProtocol.*`, `GalaxyXRTypes.h` | Android encoder/decoder, golden fixtures, bounds/fuzz tests. |
| Change identity or SteamVR properties | `GalaxyXRResources/.../default.vrsettings`, `Headsets/GalaxyXR.cpp` | Native APK identity patch, icons, models, live property capture. |
| Support a new VRLink DLL | `GalaxyXRVRLinkCompatibility.cpp` | Exact x64 hash/bytes/RVA tests; fail-closed unknown-build test. |
| Change view size/FOV/timing | `GalaxyXRDisplay.*` | capability payload/tests; preserve pass-through fallback. |
| Change pose prediction | `GalaxyXRPoseTiming.*` | device registry/clock tests and real motion capture. |
| Change native eye output | `GalaxyXREyePublisher.*` | Android gaze convention tests and stale-invalid test. |
| Change face shared memory | `GalaxyXRFaceOutput.*` | C# reader, VRCFT module, layout/one-hot/stale tests. |
| Change Android-to-VRCFT mapping | `VRCFT/.../AndroidXrUnifiedMapper.cs` | Khronos enum order and 68 one-hot/tongue tests. |
| Change HMD/controller artwork | `GalaxyXRResources/DriverFiles/resources/icons/galaxyxr/` | `driver.vrresources`; replace the prebuilt assets and inspect every state. |
| Change controller inputs | `GalaxyXRResources/DriverFiles/resources/input/` | render-model paths, bindings, SteamVR input test. |
| Change headset/controller model | `GalaxyXRResources/.../rendermodels/` | profile JSON and SteamVR render-model inspection. |
| Change install behavior | `src-tauri/src/driver_installer.rs` | Angular wrapper/service, rollback tests, cargo/Angular builds. |
| Change provider integration | `Driver/DeviceProvider.cpp` | lifecycle ordering, pass-through behavior, x64/x86 driver builds. |
| Add native Galaxy source | `CustomHeadsetOpenVR.vcxproj` | solution/test project and both architectures. |

The current precompiled Android GXRP bridge accepts its pairing token only from
APK manifest metadata. Treat every telemetry-enabled APK as a private,
single-user artifact: possession reveals the token. A distributable build needs
bridge source changes for runtime, app-private Android Keystore provisioning.

## Build and verification

Use a Developer PowerShell or an explicit MSBuild path. These commands do not
deploy to SteamVR:

The repeatable entry point is `tools/Build-GalaxyXR.ps1`. It compiles the active
driver with `VENDOR_GALAXYXR`, validates the
committed active and companion resource graphs, stages `GalaxyXRResources/DriverFiles`
as `output/galaxyxrresources`, builds/tests both native architectures,
builds the VRCFT module, stages that module under `output/VRCFT`, and runs
`npm run build` in `CustomHeadsetGUI` (Tauri release exe) unless `-SkipGui` is
specified. Status icons are taken directly from the prebuilt assets in
`GalaxyXRResources/DriverFiles/resources/icons/`.

```powershell
git submodule update --init --recursive
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild .\CustomHeadsetOpenVR\CustomHeadsetOpenVR.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:ExternalCompilerOptions=/DVENDOR_GALAXYXR /p:DeployToSteamVR=false
& $msbuild .\CustomHeadsetOpenVR\CustomHeadsetOpenVR.vcxproj /m /p:Configuration=Release /p:Platform=Win32 /p:ExternalCompilerOptions=/DVENDOR_GALAXYXR /p:DeployToSteamVR=false
& $msbuild .\GalaxyXRTests.vcxproj /m /p:Configuration=Release /p:Platform=x64
& .\Release\GalaxyXRTests.exe
& $msbuild .\GalaxyXRTests.vcxproj /m /p:Configuration=Release /p:Platform=Win32
& .\Release\GalaxyXRTests.exe
```

Build the face module with a .NET 7 SDK:

```powershell
dotnet build .\VRCFT\GalaxyXR.VRCFaceTracking\GalaxyXR.VRCFaceTracking.csproj -c Release
```

Validate the GUI/native installer:

```powershell
Push-Location .\CustomHeadsetGUI\src-tauri
cargo check
Pop-Location
Push-Location .\CustomHeadsetGUI
npm run build
Pop-Location
```

Before calling runtime integration successful, retain evidence for all of:

1. both packages exist exactly once under SteamVR and only the active package loads a DLL;
2. VRLink remains the source driver while Galaxy identity/resources appear;
3. HMD and both controller icons/models/input profiles resolve;
4. authenticated GXRP capability/tracking sequences remain fresh;
5. `/eyetracking` is on the activated HMD and becomes invalid when stale;
6. the VRCFT module loads, reads increasing sequences, maps all required face
   channels, and clears on disconnect;
7. shutdown/restart restores identity and leaves no stale stage/backup.

Static builds and protocol tests are necessary, but they are not this live
proof.
