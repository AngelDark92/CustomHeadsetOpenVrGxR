// Processing applied to direct mode scene layer textures before the streaming
// driver (e.g. vrlink / Steam Link) composites and encodes them.
//
// Hot reloaded: save while SteamVR is running and it applies within about a
// second. A compile error keeps the previous working shader active and writes
// the error to the vrserver log. All values below arrive from the streamFrame
// section of settings.json (also live reloaded) via the constant buffer.
//
// Pipeline: distortion resample (lut curve, optional annulus mask, then the
// dense displacement map) -> CAS sharpening -> color chain (matrix,
// saturation, tint, contrast, gamma) -> brightness -> dither -> overlays ->
// calibration pattern -> dimming -> blackout -> output. The texture views
// are srgb, so values here are linear.

cbuffer Params : register(b0){
	float saturation;      // saturation / 50, 1 = neutral
	float applyColor;      // 0 while the dashboard is open by default
	float contrastMult;    // contrast / 50
	float contrastOffset;  // precomputed midpoint offset
	float contrastLinear;  // 1 = apply contrast in linear space
	float outGamma;        // 2.2 = neutral
	float casStrength;     // 0..1
	float casEnable;
	float annulusEnable;
	float annulusMin;
	float annulusMax;
	float annulusFeather;
	float ditherEnable;
	float lutMaxR;         // radius covered by the lut, currently 1.0
	float aspect;          // region height / width, makes the radius isotropic
	float matrixEnable;
	float2 center;         // distortion center in bounds normalized uv
	float2 boundsMin;      // valid region of the texture (usually 0,0)
	float2 boundsSize;     // usually 1,1
	float2 texelSize;      // 1 / texture size, for CAS neighbor sampling
	float4 colorMultiplier;
	float4 matR;           // rows of the 3x3 linear rgb color matrix
	float4 matG;
	float4 matB;
	float lutRowBase;      // first lut row for this eye (rows: eye major, axis minor)
	float lutRowCount;     // total rows in the lut texture (1, 2 or 4)
	float perAxisEnable;   // blend a horizontal and a vertical curve around the ring
	float dimAmount;       // stationary dimming, 0 bright to 1 black
	float manualSrgb;      // 1 = views are not srgb typed (e.g. 10 bit layers):
	                       // decode after sampling, re-encode before output
	float ditherLsb;       // quantization steps of the output encode (255 or 1023)
	float vibrance;        // -1..1, 0 = off (promoted from pad0, offsets unchanged)
	float pad1;
	float gazeU;           // mapped gaze point for THIS eye, output uv space
	float gazeV;
	float gazeRing;        // 1 = draw the gaze debug ring
	float debugGrid;       // 0 off, 1 uv grid, 2 angular grid
	float projL;           // per-eye projection frustum tangents (raw,
	float projR;           // y-down convention), for the angular grid
	float projT;
	float projB;
	float gridSpacingRad;  // angular grid line spacing
	// world-locked fixation dot (VOR swim probe target), bounds
	// normalized uv per eye; dotMode 0 off, 1 on
	float dotU;
	float dotV;
	float dotMode;
	// 1 = draw grid + fixation dot in CONTENT space (the source uv the
	// distortion samples), so the profile warps them like scene content:
	// the grid then validates profiles by straightness, and the warped dot
	// closes the swim probe loop (probe residual scores the profile)
	float overlayWarped;
	// interactive tuner band highlight: ring radius in the same
	// aspect-corrected radius space the distortion curve is indexed by;
	// mode 0 off, 1 on (per-eye gating happens cpu-side, so linked/left/
	// right edit modes read directly as which eyes show the ring), alpha
	// is the configured ring opacity
	float tuneRingR;
	float tuneRingMode;
	float tuneRingAlpha;
	// head basis columns (head axes in world) and the world-locked grid
	// flag: the angular grid is then drawn at fixed world azimuth and
	// elevation, so it stays put while the head rotates — the exact
	// stimulus the swim nulling task wants
	float3 hbx; float gridWorldLock;
	float3 hby; float padD;
	float3 hbz; float padE;
	// per-eye whole-image alignment shift (prism correction), cpu-resolved
	// for THIS eye: constant offset on the source sample uv after the
	// distortion warp, so the eye's entire image translates. corrects
	// binocular misalignment when an eye sits off its lens axis; the CAS
	// neighborhood, edge blanking and warped overlays all follow the
	// shifted content automatically.
	// band segments: segCount > 0 = N angular lut rows per eye, sampled
	// with periodic (wrap) interpolation between segment centers. the
	// tuner's active segment (tuneSegIdx >= 0) restricts the band ring
	// to that sector so what you see is exactly what you edit.
	float alignShiftU; float alignShiftV; float segCount; float tuneSegIdx;
	// per-band layouts: the CURRENT band's segment count for the sector
	// highlight (segCount above stays the lut ROW count for sampling)
	// fxaaEnable promoted from padH (cbuffer size and every prior offset
	// unchanged); the USAGE expression below is the capability token
	float tuneSegCount; float fxaaEnable; float padI; float padJ;
	// black floor: ramp bar enable, range remap mode (0/1/2), shadow
	// lift floor in sRGB code units (-1 = lift disabled), knee code
	float bfRampBar; float bfRangeMode; float bfShadowFloor; float bfKnee;
	// sboys camera grid: opaque background flag; bfBlackPoint =
	// adjustable black point in sRGB code units (0 = off)
	float gridOpaque; float bfBlackPoint; float padL; float padM;
	// dense displacement map (camera-measured correction): enable + the
	// array slice for this eye. brightness = general multiplier on
	// linear rgb, applied at all times. blackout = force black output.
	float dispEnable; float dispEye; float brightness; float blackout;
	// gray-code sweep pattern (camera auto calibration): index (-1 off,
	// 0 black, 1 white, then per axis / bit / inverse), bits per axis,
	// pattern white level (linear), 1 = this eye shows it (else black)
	float calibPattern; float calibBits; float calibLevel; float calibEyeActive;
	// capture mode: desaturate + dim scene under the grid; sboys grid
	// line level (linear); captureOtherBlack = 1 on the non-calibration
	// eye while capture mode is on (that eye renders black)
	float gridDesat; float gridLevel; float captureOtherBlack; float padO;
};
Texture2D<float4> tex : register(t0);
Texture2D<float4> lut : register(t1);
// displacement map, slice 0 left / 1 right, R = du, G = dv (source sample
// offset in bounds uv), 256x256 texels covering the eye's uv square
Texture2DArray<float2> dispMap : register(t2);
SamplerState samp : register(s0);

// exact piecewise srgb conversions, used for gamma space operations and dither
float3 LinearToSrgb(float3 c){
	c = max(c, 0.0);
	float3 lo = c * 12.92;
	float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
	return lerp(lo, hi, step(0.0031308, c));
}
float3 SrgbToLinear(float3 c){
	c = max(c, 0.0);
	float3 lo = c / 12.92;
	float3 hi = pow((c + 0.055) / 1.055, 2.4);
	return lerp(lo, hi, step(0.04045, c));
}

// interleaved gradient noise, stable per output pixel
float InterleavedGradientNoise(float2 pixel){
	return frac(52.9829189 * frac(0.06711056 * pixel.x + 0.00583715 * pixel.y));
}

// perceptual luma for FXAA edge detection (sqrt approximates the gamma
// the algorithm's thresholds were tuned in; samples here are linear)
float FxaaLuma(float3 c){
	return sqrt(dot(c, float3(0.299, 0.587, 0.114)));
}

float4 SampleWarped(float2 uvSrcNorm){
	float4 c = tex.SampleLevel(samp, uvSrcNorm * boundsSize + boundsMin, 0);
	// srgb typed views decode in hardware; manualSrgb layers arrive encoded.
	// (filtering then happens on encoded values, negligible for these warps)
	if(manualSrgb > 0.5){
		c.rgb = SrgbToLinear(c.rgb);
	}
	return c;
}

// angular coordinate for band segments, shared by lut sampling and the
// sector highlight so they can never disagree: 0 at screen right (+x),
// increasing towards screen down (+y in uv), in turns (0..1).
float SegmentTurns(float2 p){
	return frac(atan2(p.y, p.x) / 6.28318530718 + 1.0);
}

// sample one curve row of the lut. row centers avoid bleed between curves.
float SampleLutRow(float u, float row){
	return lut.SampleLevel(samp, float2(u, (row + 0.5) / lutRowCount), 0).x;
}

// band segments: N angular rows per eye, values authored at segment
// CENTERS, cosine-interpolated periodically between adjacent centers —
// smooth around the ring with no seam at the wrap.
float SampleLutSegmented(float u, float turns){
	float n = segCount;
	float f = turns * n - 0.5;
	float s0 = floor(f);
	float w = f - s0;
	w = 0.5 - 0.5 * cos(w * 3.14159265359);
	float rowA = lutRowBase + frac((s0 + n) / n) * n;
	float rowB = lutRowBase + frac((s0 + 1.0 + n) / n) * n;
	return lerp(SampleLutRow(u, rowA), SampleLutRow(u, rowB), w);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0{
	// the viewport covers exactly the bounds region, so uv is already bounds normalized
	// ---- distortion resample ----
	float2 p = uv - center;
	p.y *= aspect;
	float r = length(p);
	// segment angle from the PRE-scale aspect-corrected offset, computed
	// once and shared with the ring sector (p is reassigned by the warp)
	float segTurns = SegmentTurns(p);
	// radial scale from the baked curve rows (k1k2 polynomial or spline).
	// per axis blends the horizontal and vertical curves by the squared cosine
	// of the ring angle, giving an elliptic correction.
	float u = r / lutMaxR;
	float s;
	if(segCount > 0.5){
		s = SampleLutSegmented(u, segTurns);
	}else if(perAxisEnable > 0.5){
		float wH = (p.x * p.x) / max(dot(p, p), 1e-9);
		s = SampleLutRow(u, lutRowBase) * wH + SampleLutRow(u, lutRowBase + 1) * (1.0 - wH);
	}else{
		s = SampleLutRow(u, lutRowBase);
	}
	if(annulusEnable > 0.5){
		// diagnostic band: only apply the displacement within [annulusMin, annulusMax],
		// feathered so the mask boundary squashes smoothly instead of shearing
		float w = smoothstep(annulusMin - annulusFeather, annulusMin + annulusFeather, r)
			* (1.0 - smoothstep(annulusMax - annulusFeather, annulusMax + annulusFeather, r));
		s = 1.0 + (s - 1.0) * w;
	}
	p *= s;
	p.y /= aspect;
	float2 nSrc = p + center + float2(alignShiftU, alignShiftV);
	// ---- dense displacement map, composed after the radial warp. indexed
	// by OUTPUT uv (the space the camera measures in); the offset is a
	// source sample offset so content appears moved by -disp. texel
	// centers sit on the lattice edges (texel 0 = uv 0, texel 255 = uv 1)
	if(dispEnable > 0.5){
		float2 duv = uv * (255.0 / 256.0) + (0.5 / 256.0);
		nSrc += dispMap.SampleLevel(samp, float3(duv, dispEye), 0).xy;
	}
	// overlay coordinate: output space normally; content space when warped
	// overlays are on (overlays then displace exactly like sampled content)
	float2 ovUv = (overlayWarped > 0.5) ? nSrc : uv;
	float4 color = SampleWarped(nSrc);
	bool outside = any(nSrc < 0.0) || any(nSrc > 1.0);

	// ---- FXAA (compact 3.11 console variant), BEFORE CAS: resolve the
	// center color along the detected local edge so sharpening enhances a
	// clean edge instead of amplifying the staircase. flat areas exit at
	// the contrast gate after 4 corner taps; CAS then sharpens the
	// AA-resolved center against its raw cross neighbors (single pass
	// compromise: the neighborhood is not itself AA-resolved).
	if(fxaaEnable > 0.5 && !outside){
		float2 tf = texelSize / boundsSize;
		float lNW = FxaaLuma(SampleWarped(nSrc + float2(-tf.x, -tf.y)).rgb);
		float lNE = FxaaLuma(SampleWarped(nSrc + float2( tf.x, -tf.y)).rgb);
		float lSW = FxaaLuma(SampleWarped(nSrc + float2(-tf.x,  tf.y)).rgb);
		float lSE = FxaaLuma(SampleWarped(nSrc + float2( tf.x,  tf.y)).rgb);
		float lM  = FxaaLuma(color.rgb);
		float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
		float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
		if(lMax - lMin >= max(0.0625, lMax * 0.125)){
			lNE += 1.0 / 384.0;
			float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
			float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 512.0);
			float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
			dir = clamp(dir * rcpMin, -8.0, 8.0) * tf;
			float3 rgbA = 0.5 * (SampleWarped(nSrc + dir * (1.0 / 3.0 - 0.5)).rgb
				+ SampleWarped(nSrc + dir * (2.0 / 3.0 - 0.5)).rgb);
			float3 rgbB = rgbA * 0.5 + 0.25 * (SampleWarped(nSrc + dir * -0.5).rgb
				+ SampleWarped(nSrc + dir * 0.5).rgb);
			float lB = FxaaLuma(rgbB);
			color.rgb = (lB < lMin || lB > lMax) ? rgbA : rgbB;
		}
	}

	// ---- CAS sharpening (per channel, compact FidelityFX style) ----
	if(casEnable > 0.5 && !outside){
		// neighbors around the warped sample position. the warp is near identity
		// so a plain one texel cross in source space is accurate enough.
		float2 t = texelSize / boundsSize;
		float3 up = SampleWarped(nSrc + float2(0, -t.y)).rgb;
		float3 dn = SampleWarped(nSrc + float2(0, t.y)).rgb;
		float3 lf = SampleWarped(nSrc + float2(-t.x, 0)).rgb;
		float3 rt = SampleWarped(nSrc + float2(t.x, 0)).rgb;
		float3 mn = min(min(up, dn), min(lf, min(rt, color.rgb)));
		float3 mx = max(max(up, dn), max(lf, max(rt, color.rgb)));
		float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 0.0001)));
		float peak = -1.0 / lerp(8.0, 5.0, saturate(casStrength));
		float3 w = amp * peak;
		color.rgb = saturate((color.rgb + w * (up + dn + lf + rt)) / (1.0 + 4.0 * w));
	}

	if(outside){
		color = float4(0, 0, 0, color.a);
	}

	// ---- color chain ----
	if(applyColor > 0.5){
		if(matrixEnable > 0.5){
			color.rgb = mul(float3x3(matR.xyz, matG.xyz, matB.xyz), color.rgb);
		}
		float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
		color.rgb = lerp(gray.xxx, color.rgb, saturation);
		// vibrance: saturation change weighted toward the LEAST saturated
		// pixels. positive enriches muted colors while already vivid ones
		// (HSV sat ~ 1) are nearly untouched — clips far later than raw
		// saturation. negative pushes muted colors toward gray while vivid
		// accents survive. the (mx-mn)/mx mask is scale invariant, so
		// evaluating it on linear values only softens the mask curve vs a
		// gamma space evaluation; the op itself matches the saturation
		// lerp above and stacks with it.
		if(abs(vibrance) > 0.001){
			float vmx = max(color.r, max(color.g, color.b));
			float vmn = min(color.r, min(color.g, color.b));
			float vsat = saturate((vmx - vmn) / max(vmx, 0.0001));
			float vgray = dot(color.rgb, float3(0.299, 0.587, 0.114));
			color.rgb = lerp(vgray.xxx, color.rgb, 1.0 + vibrance * (1.0 - vsat));
		}
		color.rgb *= colorMultiplier.rgb;
		if(contrastLinear > 0.5){
			color.rgb = color.rgb * contrastMult + contrastOffset;
		}else{
			float3 g = LinearToSrgb(color.rgb);
			g = g * contrastMult + contrastOffset;
			color.rgb = SrgbToLinear(g);
		}
		if(abs(outGamma - 2.2) > 0.001){
			float3 g = LinearToSrgb(color.rgb);
			g = pow(max(g, 0.0), 2.2 / outGamma);
			color.rgb = SrgbToLinear(g);
		}
	}

	// ---- general brightness (always on, independent of applyColor) ----
	if(abs(brightness - 1.0) > 0.001){
		color.rgb *= brightness;
	}

	// ---- dither ----
	// ---- black floor: near-black diagnostic ramps + fixes. the ramp
	// bar is drawn FIRST so its patches ride through the range remap,
	// the shadow lift, the dither and the encoder exactly like game
	// shadows do — what the eye reads off the strips is the honest
	// end-to-end near-black transfer. two strips per eye: screen
	// center (the foveal encode region when looking straight ahead)
	// and near the bottom (peripheral region); comparing them isolates
	// the foveated encoder's per-region quantization floor. ----
	if(bfRampBar > 0.5){
		// strips are SCREEN-locked on purpose: the encoder's QP regions
		// are screen-space, so a world-locked bar would wander across
		// region boundaries and confound the reading. horizontally
		// centered (x 0.30-0.70) so the whole ramp sits inside the
		// lens sweet spot; the peripheral strip rides at y ~0.8, below
		// the foveal encode square but still comfortably visible.
		bool stripA = uv.y > 0.47 && uv.y < 0.53;   // foveal
		bool stripB = uv.y > 0.78 && uv.y < 0.84;   // peripheral
		if((stripA || stripB) && uv.x > 0.30 && uv.x < 0.70){
			float fx = (uv.x - 0.30) / 0.40;
			float patch = min(floor(fx * 17.0), 16.0);
			float g = patch * 2.0 / 255.0; // sRGB codes 0..32 step 2
			color.rgb = SrgbToLinear(float3(g, g, g));
			// white tick row at the strip top marking codes 0/8/16/24/32
			float stripTop = stripA ? 0.47 : 0.78;
			if(uv.y - stripTop < 0.006 && fmod(patch, 4.0) < 0.5){
				color.rgb = 1.0;
			}
		}
	}
	if(bfRangeMode > 0.5 || bfShadowFloor >= 0.0 || bfBlackPoint > 0.01){
		float3 g = LinearToSrgb(color.rgb);
		if(bfShadowFloor >= 0.0){
			// shadow-only lift: linear squeeze below the knee mapping
			// [0, knee] -> [floor, knee], identity above. dark content
			// rises above the OLED/encoder floor; midtones untouched.
			float knee = max(bfKnee, bfShadowFloor + 0.5) / 255.0;
			float fl = bfShadowFloor / 255.0;
			float3 low = fl + g * ((knee - fl) / knee);
			g = lerp(low, g, step(knee, g));
		}
		if(bfBlackPoint > 0.01){
			// adjustable black point: remap [bp, 255] -> [0, 255].
			// calibrate with the ramp bar: raise until the two darkest
			// patches just merge, then back off one notch. same math
			// as rangeMode expand but with a CHOSEN pivot instead of
			// the fixed 16-code chop.
			float bp = min(bfBlackPoint, 48.0) / 255.0;
			g = saturate((g - bp) / (1.0 - bp));
		}
		if(bfRangeMode > 1.5){
			// expand: decode as if limited range (fix for grey blacks /
			// clipped whites when the chain double-applies limited)
			g = saturate((g * 255.0 - 16.0) / 219.0);
		}else if(bfRangeMode > 0.5){
			// compress into limited range before encode (fix when the
			// display decodes full-range video as limited)
			g = (16.0 + 219.0 * g) / 255.0;
		}
		color.rgb = SrgbToLinear(g);
	}

	if(ditherEnable > 0.5){
		// one quantization step of noise in the srgb domain, where the 8 bit
		// encode happens, to break up banding in dark gradients
		float noise = InterleavedGradientNoise(pos.xy) - 0.5;
		float3 g = LinearToSrgb(color.rgb);
		g = saturate(g + noise / max(ditherLsb, 1.0));
		color.rgb = SrgbToLinear(g);
	}

	// ---- calibration grid: the straight line reference for pupil swim
	// tuning. uv mode: lines every 0.1 uv. angular mode: lines at exact
	// multiples of gridSpacingRad of visual angle through the real
	// frustum (sboy-style distortion photos: every rendered line has a
	// known angular position, so a photo through the lens reads
	// distortion error directly). axes emphasized for pose recovery. ----
	if(debugGrid > 2.5){
		// ---- sboys camera-calibration pattern (FovCalibration.shader
		// port): per-axis visual angles through the real frustum,
		// hue-coded lines every gridSpacingRad. each line's COLOR
		// encodes its absolute angular index (hue = frac(n/6), n =
		// round(|angle|/step)), so a photo through the lens identifies
		// every line without counting from center — which also lets the
		// fit script solve residual camera pose jointly with the
		// distortion residual. bright full-length axis cross (|angle| <
		// 0.1 deg) for centering the camera; optional opaque grey
		// background so the camera sees only the pattern. angular
		// convention identical to the repo shader: angleH = atan2(x,-z)
		// = atan(tx), up-positive vertical via -ty. ----
		float tx = projL + ovUv.x * (projR - projL);
		float ty = projT + ovUv.y * (projB - projT);
		float aH = atan(tx) * 57.29577951308232;
		float aV = atan(-ty) * 57.29577951308232;
		float stepDeg = max(gridSpacingRad * 57.29577951308232, 0.25);
		if(gridOpaque > 0.5){
			color.rgb = SrgbToLinear(float3(0.1, 0.1, 0.1));
		}else if(gridDesat > 0.5){
			// capture mode: keep the world as a reference but make it
			// grey and dim so the hue-coded lines cannot be confused
			// with scene content in the camera view
			float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
			color.rgb = lum * 0.35;
		}
		// white axis cross first; colored lines never overlap it
		// (their box index is 0 there and 0 is skipped, like sboys)
		if(abs(aH) < 0.1 || abs(aV) < 0.1){
			color.rgb = gridLevel;
		}
		float lineW = 0.05; // fraction of one step, sboys default
		float modH = frac(aH / stepDeg);
		float modV = frac(aV / stepDeg);
		float boxN = 0.0;
		// dominant-axis priority resolves crossings deterministically
		if((modH <= lineW || modH >= 1.0 - lineW) && abs(aH) >= abs(aV)){
			boxN = round(abs(aH) / stepDeg);
		}
		if((modV <= lineW || modV >= 1.0 - lineW) && abs(aV) > abs(aH)){
			boxN = round(abs(aV) / stepDeg);
		}
		if(boxN > 0.5){
			float hue = frac(boxN / 6.0);
			// HUEtoRGB, matching the source shader's coding exactly
			float3 hc = saturate(float3(
				abs(hue * 6.0 - 3.0) - 1.0,
				2.0 - abs(hue * 6.0 - 2.0),
				2.0 - abs(hue * 6.0 - 4.0)));
			color.rgb = SrgbToLinear(hc) * gridLevel;
		}
	}else if(debugGrid > 1.5){
		float tx = projL + ovUv.x * (projR - projL);
		float ty = projT + ovUv.y * (projB - projT);
		float ax, ay;
		if(gridWorldLock > 0.5){
			// world-locked: transform the pixel's view direction into world
			// space (raw tangents are y-down, head y is up, forward is -z)
			// and grid on world azimuth/elevation. lines counter-rotate
			// against head motion, i.e. they stay fixed in the world.
			float3 d = normalize(float3(tx, -ty, -1.0));
			float3 wd = hbx * d.x + hby * d.y + hbz * d.z;
			ax = atan2(wd.x, -wd.z);
			ay = asin(clamp(wd.y, -1.0, 1.0));
		}else{
			ax = atan(tx);
			ay = atan(ty);
		}
		float2 f = float2(abs(frac(ax / gridSpacingRad + 0.5) - 0.5),
			abs(frac(ay / gridSpacingRad + 0.5) - 0.5)) * gridSpacingRad;
		float2 fw = float2(max(fwidth(ax), 1e-5), max(fwidth(ay), 1e-5));
		float lineMask = max(1.0 - smoothstep(fw.x, 2.5 * fw.x, f.x),
			1.0 - smoothstep(fw.y, 2.5 * fw.y, f.y));
		// axes (0 degrees) doubled width and brighter, for locating the
		// optical axis and recovering camera pose from photos
		float axisMask = max(1.0 - smoothstep(2.0 * fw.x, 5.0 * fw.x, abs(ax)),
			1.0 - smoothstep(2.0 * fw.y, 5.0 * fw.y, abs(ay)));
		color.rgb = lerp(color.rgb, float3(0.15, 1.0, 0.45), lineMask * 0.45);
		color.rgb = lerp(color.rgb, float3(1.0, 1.0, 1.0), axisMask * 0.7);
	}else if(debugGrid > 0.5){
		float2 cell = frac(ovUv * 10.0);
		float2 distToLine = min(cell, 1.0 - cell);
		float nearLine = min(distToLine.x, distToLine.y / max(aspect, 0.001));
		float lineMask = 1.0 - smoothstep(0.012, 0.03, nearLine);
		color.rgb = lerp(color.rgb, float3(0.15, 1.0, 0.45), lineMask * 0.45);
	}

	// ---- gaze debug ring: live calibration of the gaze -> viewport
	// mapping the dynamic pupil swim pass will reuse ----
	if(gazeRing > 0.5){
		float2 gd = uv - float2(gazeU, gazeV);
		gd.y *= aspect;
		float gr = length(gd);
		float ring = smoothstep(0.005, 0.002, abs(gr - 0.025));
		color.rgb = lerp(color.rgb, float3(1.0, 0.15, 0.1), ring * 0.85);
	}

	// ---- world-locked fixation dot: the VOR swim probe target. cyan
	// filled dot with a thin halo ring, sized to sit inside the red gaze
	// ring so gaze-vs-target alignment is readable at a glance ----
	if(dotMode > 0.5){
		float2 dd = ovUv - float2(dotU, dotV);
		dd.y *= aspect;
		float dr = length(dd);
		if(dotMode > 2.5){
			// controller tip marker (aligner): magenta dot + fine halo. the
			// nulling cue is this marker freezing while the physical tip is
			// planted and the controller swirls around it.
			float dotMask = 1.0 - smoothstep(0.003, 0.006, dr);
			float haloMask = smoothstep(0.003, 0.001, abs(dr - 0.012));
			color.rgb = lerp(color.rgb, float3(1.0, 0.2, 0.9), max(dotMask, haloMask * 0.8));
		}else if(dotMode > 1.5){
			// distortion center cross (center tune): thin amber crosshair
			// with an open middle so the breathing still-point stays visible
			float armX = (1.0 - smoothstep(0.0012, 0.0028, abs(dd.x))) * step(abs(dd.y), 0.035) * step(0.006, abs(dd.y));
			float armY = (1.0 - smoothstep(0.0012, 0.0028, abs(dd.y))) * step(abs(dd.x), 0.035) * step(0.006, abs(dd.x));
			color.rgb = lerp(color.rgb, float3(1.0, 0.8, 0.2), max(armX, armY) * 0.8);
		}else{
			// fixation dot (VOR swim probe target)
			float dotMask = 1.0 - smoothstep(0.004, 0.007, dr);
			float haloMask = smoothstep(0.004, 0.0015, abs(dr - 0.016));
			color.rgb = lerp(color.rgb, float3(0.1, 0.9, 1.0), max(dotMask, haloMask * 0.7));
		}
	}

	// ---- interactive tuner band ring: marks the radius the active spline
	// knot acts at, using the r computed for the lut lookup above so ring
	// and knot can never disagree. drawn in output space intentionally:
	// the band is defined over output radii, and the surrounding content
	// (and warped grid) moving against a fixed ring is the nulling cue ----
	if(tuneRingMode > 0.5){
		float ringDist = abs(r - tuneRingR);
		float ringMask = 1.0 - smoothstep(0.003, 0.007, ringDist);
		float ringA = tuneRingAlpha;
		// segment editing: full brightness only inside the active sector,
		// strongly dimmed elsewhere (the ring stays visible for context)
		if(tuneSegCount > 0.5 && tuneSegIdx > -0.5){
			float seg = floor(segTurns * tuneSegCount);
			if(abs(seg - tuneSegIdx) > 0.5){
				ringA *= 0.12;
			}
		}
		color.rgb = lerp(color.rgb, float3(1.0, 0.65, 0.1), ringMask * ringA);
	}

	// ---- gray-code sweep pattern (camera auto calibration): replaces the
	// output entirely, in OUTPUT uv space so it encodes exactly which uv
	// the panel shows at each physical position, independent of any warp
	// or color setting. the non-calibration eye is black. ----
	if(calibPattern > -0.5){
		float3 pat = 0.0;
		if(calibEyeActive > 0.5){
			int idx = (int)round(calibPattern);
			if(idx == 1){
				pat = calibLevel;
			}else if(idx >= 2){
				int bits = clamp((int)round(calibBits), 1, 12);
				int k = idx - 2;
				int axis = k / (2 * bits);
				int bit = (k / 2) % bits;
				int inv = k % 2;
				float coord = axis == 0 ? uv.x : uv.y;
				uint levels = (uint)(1 << bits);
				uint code = (uint)clamp((int)floor(coord * levels), 0, (int)levels - 1);
				uint gray = code ^ (code >> 1);
				uint b = (gray >> (uint)(bits - 1 - bit)) & 1u;
				if(inv == 1){ b = 1u - b; }
				pat = b == 1u ? calibLevel : 0.0;
			}
		}
		color.rgb = pat;
	}

	// ---- capture mode: the other eye is black ----
	if(captureOtherBlack > 0.5){
		color.rgb = 0.0;
	}

	// ---- stationary dimming (uniform fade to black, no uneven oled wear) ----
	color.rgb *= 1.0 - dimAmount;

	// ---- blackout: panel protection while a camera rig stays mounted.
	// last word, nothing after it touches the color ----
	if(blackout > 0.5){
		color.rgb = 0.0;
	}

	// manualSrgb output is written through a non srgb view: encode explicitly
	if(manualSrgb > 0.5){
		color.rgb = LinearToSrgb(color.rgb);
	}

	return color;
}
