# Galaxy XR native host integration

This is a project-defined Windows host extension for the Galaxy XR Steam Link
integration. It is not an official Samsung, Valve, Steam Link, OpenVR, or
OpenXR network protocol.

## Safety boundary

- `galaxyXR.enable=false` starts no listener, installs no Galaxy hook, changes
  no property, creates no eye component, and changes no display or pose value.
- No authenticated session means the wrappers are pass-through.
- A HELLO must identify package `com.valvesoftware.steamlinkvr`, manufacturer
  `Samsung`, and model `Samsung Galaxy XR`.
- The listener stays closed until `allowedClients` contains at least one exact
  nonzero `versionCode`/`apkSha256`/`bridgeSha256` record. HELLO must match all
  three values from the same record; cross-paired hashes are rejected.
- `listenAddress` must be a usable unicast IPv4 assigned to this PC. Invalid or
  stale adapter addresses are reported once and retried at most every five seconds.
- The TCP control peer and UDP source IPv4 address must match.
- Only one session is accepted. Sequence duplicates, reordering, wrap, stale
  samples, invalid counts, non-finite values, bad quaternion norms, and bad
  authentication tags are rejected.
- `vrlink_compat` retains the exact existing DLL hash, RVA, 15-byte prologue,
  MSVC string-layout bounds, and exact-model checks. Unknown Valve binaries are
  never guessed or patched.

## GXRP v1.0

Native Steam Link 5002318/5002322 need the optional `Galaxy XR native telemetry`
APK patch. It installs only `libgxr_xr_bridge.so` and its implicit API-layer
manifest, preserving Valve's native controller/hand configuration. The patch
requires the PC LAN IPv4 and a private 64-hex pairing token. Its control and
tracking ports are fixed to the host defaults, 29981 and 29982, so there is no
port setup. After the final APK is signed, enroll it and configure the host in
one step from this directory:

```powershell
.\tools\Set-GalaxyXRNativeTelemetryHost.ps1 `
  -ApkPath C:\path\SteamLink-GalaxyXR.apk
```

The setup reads the version, PC address, pairing token, and fixed ports from
the final APK with Android `apkanalyzer`. It hashes the final APK and the exact
embedded `libgxr_xr_bridge.so`, then transactionally updates
`%APPDATA%\GalaxyXR\CustomHeadset\settings.json`. Other settings and admissions
for other Steam Link builds are preserved. A non-secret admission report is
also written beside the APK; it deliberately contains no pairing token.

If `apkanalyzer` or Java is unavailable, pass the three non-hash values that
were selected when patching. Ports still require no input:

```powershell
.\tools\Set-GalaxyXRNativeTelemetryHost.ps1 `
  -ApkPath C:\path\SteamLink-GalaxyXR.apk `
  -VersionCode 5002322 `
  -ListenAddress 192.168.1.27 `
  -PairingTokenHex YOUR_64_HEX_PAIRING_TOKEN
```

Run this command again whenever the APK is rebuilt or re-signed, because either
operation changes its exact admission hash. SteamVR deployment and restart are
separate, explicitly authorized operations; this setup tool performs neither.

The available precompiled bridge reads the pairing token from application
manifest metadata. The patched APK is therefore a private, single-user
artifact: anyone who obtains it can extract the token and impersonate the
client. Do not commit, upload, or distribute it. GXRP authentication protects
against network peers that do not possess the APK; it does not protect a
distributed artifact. A distributable production design requires bridge source
changes that provision the token at runtime from app-private,
Android-Keystore-backed storage.

Control uses TCP frames prefixed by a little-endian `u32 messageBytes`.
Tracking/presentation uses one UDP datagram per envelope and payload.

The 68-byte envelope is:

| Offset | Field |
|---:|---|
| 0 | ASCII `GXRP` |
| 4 | major `u16`, minor `u16` |
| 8 | message type `u16`, header bytes `u16` |
| 12 | payload bytes `u32` |
| 16 | session ID `[16]` |
| 32 | sequence `u64` |
| 40 | flags `u32` |
| 44 | client monotonic nanoseconds `i64` |
| 52 | HMAC-SHA256 tag truncated to 16 bytes |

All integers and IEEE-754 floats are little-endian and every field is
serialized explicitly. The tag covers the header with a zero tag plus the
payload. HELLO has a zero session ID and is PSK-authenticated. HELLO_ACK
returns the host nonce and session ID and is PSK-authenticated. The session key
is:

`HMAC-SHA256(PSK, "GXRP-SESSION-V1" || clientNonce32 || hostNonce32 || sessionId16)`

HELLO_ACK does not activate the session. The client must prove possession of
that newly derived key by sending the first CAPABILITIES frame under the fresh
session ID and a higher sequence; only then can identity, display, gaze, face,
or pose behavior activate. Zero or recently reused client nonces are refused.

Control payloads are limited to 4096 bytes; UDP payloads to 2048 bytes. Golden
vectors live under `protocol/golden/` and are shared with the Android build.
The host rejects a later minor revision because v1.0 has no extension/TLV area
from which optional fields could be skipped safely.

The v1.0 payload order is fixed:

| Type | Payload, in little-endian field order |
|---|---|
| HELLO | client nonce, feature bits, version code, clock-conversion flag, package/version, APK hash, bridge version/hash, manufacturer/model/serial/fingerprint, runtime name/version |
| HELLO_ACK | host nonce, selected version, accepted/reason, payload/rate limits, tracking port, host version, loaded driver-DLL SHA-256 |
| CAPABILITIES | revision/features/permissions/view config, view/overlay/timing flags, current and supported refresh rates, activity/product/extensions, then each view's dimensions/sample count/FOV |
| CLOCK | request `t0`; response `t0,t1,t2` in monotonic nanoseconds |
| TRACKING | sample/timing/space/revision/validity fields, two eye states and poses, then face time/validity/state/source, 68 weights, and three confidences |
| PRESENTATION | client frame/timing markers, submitted/decoder/surface dimensions, decoded/host frame IDs, optional photon time, correlation flags |
| DIAGNOSTICS | capability revision, sample/send/drop/reconnect/error counters, queue high-water and last accepted sequences |
| DISCONNECT | reason, may-resume flag and last control/tracking sequences |

Every variable string is a `u16` byte count followed by UTF-8 bytes and is
bounded by the codec. Exact limits and validation are authoritative in
`GalaxyXRProtocol.{h,cpp}`.

GXRP v1 assigns capability/permission bit 0 to eye tracking, bit 1 to visual
face tracking, and permission bit 2 to audio capture. Native publication additionally requires the corresponding
`XR_ANDROID_eye_tracking` or `XR_ANDROID_face_tracking` token in the enabled
extension list. Visual face frames require face permission. Audio frames require
record-audio permission and the exact `XR_ANDROID_face_tracking_data_source`
token; multimodal frames require that token and both permissions. Revocation
invalidates output immediately.

## Coordinates and gaze

`baseSpaceId=1` means canonical HMD/view space: metres, right-handed, +X right,
+Y up, and -Z forward. The client must transform runtime eye poses into this
space before transmission because v1 does not carry a base-to-HMD transform.
Other base-space IDs remain diagnostic-only and are not published.

The host rotates local `(0,0,-1)` by each valid eye orientation, computes the
closest points between binocular rays, and rejects parallel, behind-eye,
non-finite, and over-10-metre convergence. A bounded fallback distance is used
for valid non-converging rays. Shut/invalid eyes and stale data publish inactive
and invalid state; they never publish false centered gaze.

Negotiated per-view dimensions and FOV can override render-target size and
projection only after authentication. GXRP v1.0 does not describe the opaque
VRLink compositor texture layout, so the host deliberately preserves the
wrapped driver's eye viewport instead of assuming side-by-side packing.

## Face shared memory

`Local\CustomHeadsetOpenVR.GalaxyXR.Face.v1` contains a packed, versioned
`FaceSharedMemoryV1` snapshot. Consumers should open it read-only and use the
even generation counter as a seqlock: read generation, copy, read generation
again, and accept only equal even values. The schema retains 68 Android XR
weights, confidence `[lower,left-upper,right-upper]`, validity, state, source,
session ID, sequence, and client/host timestamps. Tongue out/left/right/up/down
are indices 63/64/65/66/67.

`GalaxyXRFaceOscAdapter.exe` is a separate read-only consumer and is never
launched by the driver. It refuses to run without `--enable`, defaults to
loopback, accepts only loopback/RFC1918 IPv4 destinations, limits output to
1–120 Hz, and sends one OSC message of at most 512 bytes per new snapshot.
The `/galaxyxr/face/frame/v1` message has type tags
`,iiihhh` followed by 71 `f` tags: valid, state, source, sequence, client time,
host time, 68 weights, then three confidences. This preserves all five tongue
values. It is a project schema suitable for an explicit downstream mapping;
the OSC schema itself is not native VRCFaceTracking or Valve FB2. Native VRCFT
v5 integration is provided separately by
`VRCFT/GalaxyXR.VRCFaceTracking`, which reads the lossless shared memory and
maps all 68 Android XR parameters to Unified Expressions. The existing FB2
compatibility path remains lossy for tongue left/right.

## What static validation cannot prove

The host build and deterministic tests prove codec parity, authentication
vectors, strict decoding, clock math, canonical face integrity, and fail-closed
configuration paths. They do not prove live Android permissions/extensions,
gaze sign on hardware, DFR consumption, exact remote photon time, the complete
render/encode/decode/surface chain, controller prediction quality, audio,
restart/disconnect behavior, or rollback. Those require an authorized physical
Galaxy XR and SteamVR validation run.
