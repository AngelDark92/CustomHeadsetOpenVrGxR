// Processing applied to direct mode scene layer textures before the streaming
// driver (e.g. vrlink / Steam Link) composites and encodes them.
//
// Hot reloaded: save while SteamVR is running and it applies within about a
// second. A compile error keeps the previous working shader active and writes
// the error to the vrserver log. All values below arrive from the streamFrame
// section of settings.json (also live reloaded) via the constant buffer.
//
// Pipeline: distortion resample (lut curve, optional annulus mask) -> CAS
// sharpening -> color chain (matrix, saturation, tint, contrast, gamma) ->
// dither -> output. The texture views are srgb, so values here are linear.

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
	float pad0;
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
};
Texture2D<float4> tex : register(t0);
Texture2D<float4> lut : register(t1);
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

	// ---- dither ----
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
	if(debugGrid > 1.5){
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

	// ---- stationary dimming (uniform fade to black, no uneven oled wear) ----
	color.rgb *= 1.0 - dimAmount;

	// manualSrgb output is written through a non srgb view: encode explicitly
	if(manualSrgb > 0.5){
		color.rgb = LinearToSrgb(color.rgb);
	}

	return color;
}
