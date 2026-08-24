# CustomHeadsetOpenVrGxR working guide

Read `Docs/PROJECT-ARCHITECTURE.md` before changing this checkout. It describes
the runtime data flow and has a "where to make a change" table.

## Non-negotiable boundaries

- Work only inside this repository unless the user explicitly expands scope.
- `driver_vrlink` remains the real wireless HMD/controller owner. This driver
  observes and wraps its devices; it must not register duplicate Galaxy XR
  devices or replace VRLink transport/compositor ownership.
- Galaxy XR activation is fail closed. Do not relax the authenticated GXRP
  session, exact APK/bridge hashes, pairing key, capability revision, Samsung
  model, serial match, or verified VRLink-hook gates.
- Native eye data belongs on the activated HMD property container at
  `/eyetracking`. Publish invalid immediately on stale/disconnect; never invent
  center gaze.
- Face data is the complete Android XR 68-float frame in
  `Local\CustomHeadsetOpenVR.GalaxyXR.Face.v1`. Preserve the seqlock layout and
  all five tongue channels. VRCFT mapping belongs in `VRCFT/`, not the driver.
- Generic active-driver fallbacks use `{CustomHeadsetOpenVR}` paths. Galaxy XR
  identity, icons, profiles, and normal render models use the resource-only
  `{galaxyxrresources}` package. Do not write resources into `driver_vrlink`.
- Driver builds must use `DeployToSteamVR=false` unless live deployment was
  explicitly requested. Building is not runtime proof.
- Do not run ADB, install an APK, mutate SteamVR, or restart SteamVR merely to
  perform static validation. Record static and live evidence separately.
- Preserve unrelated dirty work and pinned submodule revisions.

## Required checks for Galaxy XR changes

1. Validate every modified JSON resource.
2. Build the driver Release x64 and Win32 with `DeployToSteamVR=false`.
3. Build and run `GalaxyXRTests` x64 and Win32.
4. Build `VRCFT/GalaxyXR.VRCFaceTracking` when its code or shared-memory schema
   changes.
5. Run `cargo check` and the Angular production build when installer/UI code
   changes.
6. State clearly whether a real SteamVR + Steam Link + headset session was run.
