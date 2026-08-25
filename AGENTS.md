# CustomHeadsetOpenVrGxR working guide

Read `Docs/PROJECT-ARCHITECTURE.md` before changing this checkout. It describes
the runtime data flow and has a "where to make a change" table.

## Non-negotiable boundaries

- Work only inside this repository unless the user explicitly expands scope.
- `driver_vrlink` remains the real wireless HMD/controller owner. The active
  `CustomHeadsetOpenVR` utility driver provides settings and frame processing
  without creating a duplicate HMD/controller; `galaxyxrresources` provides the
  external-vendor identity assets.
- Face and tongue data use Steam Link's OSC sharing plus the matching external
  LinkFT VRCFaceTracking module. Do not reintroduce GXRP, embedded PC addresses,
  pairing tokens, APK hash enrollment, or a bundled VRCFT module.
- Active-driver fallbacks use `{CustomHeadsetOpenVR}` paths. Galaxy XR identity,
  icons, profiles, and normal render models use the resource-only
  `{galaxyxrresources}` package. Do not write resources into `driver_vrlink`.
- Driver builds must use `DeployToSteamVR=false` unless live deployment was
  explicitly requested. Building is not runtime proof.
- Do not run ADB, install an APK, mutate SteamVR, or restart SteamVR merely to
  perform static validation. Record static and live evidence separately.
- Preserve unrelated dirty work and pinned submodule revisions.

## Required checks for Galaxy XR changes

1. Validate every modified JSON resource.
2. Run `tools/Build-GalaxyXR.ps1 -SkipGui` to build and validate both driver packages.
3. Run `cargo check` and the Angular production build when installer/UI code
   changes.
4. State clearly whether a real SteamVR + Steam Link + headset session was run.
