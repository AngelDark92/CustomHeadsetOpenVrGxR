# Stream Frame Processing Setup and Calibration

Processing applied to the streamed frames of a direct mode headset (Steam Link / vrlink, Tested on a Samsung Galaxy XR) right before the driver encodes them. Unlike the
custom shader, it applies at all times, not only while the SteamVR dashboard is open. All settings apply live within about a second.

## Install

1. Unpack the entire release zip (the GUI folder and the `CustomHeadsetOpenVR` folder must stay next to each other).
2. Run `CustomHeadsetGUI.exe`, go to About, press install driver.
3. If you previously installed another copy of the driver, while the installation should overwrite it, if you want to make sure or have any issues, remove CustomHeadsetOpenVR from steamvr/drivers, then install via the gui
4. Restart SteamVR. The Streamed Headset page in the GUI has everything.

## Recommended configuration

- Streamed Headset page: Enable on.
- Custom Shader (device pages): leave **disabled**. The Streamed Headset processing replaces it for streamed HMDs. The page shows a warning if both are active with color adjustments.
- Skip Color While Dashboard Open: leave **off** (default).

## Feature notes

- **Saturation / Contrast / Gamma / Tint**: same semantics as the original shader (50 and 2.2 are neutral).
- **CAS Sharpening**: applied before the video encode.
- **Dither**: helps banding in dark gradients. The encoder eats some of it, but low amplitude noise before quantization still helps.
- **Color Matrix**: advanced gamut/white point correction. Empty = off.

## Distortion correction (static)

Symptom: fixate a point, rotate your head, the world ripples or swims. Most likely cause: the headset's built-in lens correction is slightly off; the error is a function of distance from the optical center. The correction applies a small counter-warp to the streamed image.

The curve sets a radial scale per ring around the optical center: 1.0 leaves that ring untouched, above 1.0 pulls its content toward the center, below pushes it outward. Real corrections are within about a percent of 1.0.

Suggested calibration workflow(WIP):

1. Stand in front of straight lines or text (the SteamVR construct grid works, or testHMD works).
2. Start in k1/k2 mode. Fixate a grid/text, rotate your head, nudge k1 in ±0.005 steps, Refine periphery with k2.
3. Press "Convert k1/k2 to spline" to keep the shape and gain per-radius control points. Drag points, double click adds, right click removes.
4. Use the Annulus Tuning Band to isolate one radius band while tuning its control point (diagnostic only, disable for normal use).
5. If the residual differs between eyes or directions, enable Per Eye and/or Per Axis curves. The existing curve is copied as a starting point; tabs above the plot select which curve you are editing, siblings draw dimmed.
6. Optical Center Offsets move where the rings are anchored, per eye, if the residual is asymmetric around the center.

## Troubleshooting

Check `Steam\logs\vrserver.txt`:

- `FrameComponentShim: wrapping IVRDriverDirectModeComponent`, the frame path is hooked. Missing: the headset driver is not direct mode, or the driver did not load (check for duplicate registrations).
- `FrameProcessor: matched hmd adapter <gpu>`, processing runs on the GPU SteamVR renders on. `no adapter matched` / `failed to create D3D11 device`: report with your GPU setup.
- `FrameProcessor: OpenSharedResource failed`, shared texture access problem, usually wrong adapter (see above).
- `FrameProcessor: AcquireSync returned 0x00000102` occasionally is normal (busy frame skipped), constant spam is a problem you should report.
- `FrameProcessor: baked distortion lut (...)` appears whenever curve settings change.
- `FrameProcessor: PS compile error: ...`, a live edit of `resources\shaders\d3d11\vrlink_layer_ps.hlsl` has a typo; the previous
  working shader stays active.

"Changes nothing": make sure the Streamed Headset page's Enable is on and you
are testing with something non-neutral (saturation 0 is unmissable). The
dashboard itself is never processed; judge the world behind or around it.

If you have any issues, please open an issue on the GitHub page linked in the GUI's about page, and attach your Steam\logs\vrserver.txt .
Please don't open them on the original sboys repository.
