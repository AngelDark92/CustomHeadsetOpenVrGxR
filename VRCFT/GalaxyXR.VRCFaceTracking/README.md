# Galaxy XR VRCFaceTracking v5 module

This x64 .NET 7 module reads the driver's versioned, read-only
`Local\CustomHeadsetOpenVR.GalaxyXR.Face.v1` mapping. It accepts only a stable
even seqlock generation, schema v1, a valid tracking state, 68 finite Android
XR parameters, three finite confidences, and a sequence that is still moving.
Invalid or stale input clears every published expression and returns the module
to `Idle`.

`AndroidXrUnifiedMapper.cs` maps every index from the official
`XrFaceParameterIndicesANDROID` ordering. Eye openness and gaze use indices
12-21; all five tongue channels remain distinct. A few Android shapes have no
one-to-one Unified Expressions shape, so the mapping deliberately fans them
out (left/right mouth movement and lip pucker) instead of discarding them.

Run `tools/Acquire-VRCFT-5.2.3-SDK.ps1`, then build this project. Install the
resulting DLL into `%APPDATA%\VRCFaceTracking\CustomLibs`. The main project
installer performs that copy transactionally when the module artifact exists.
