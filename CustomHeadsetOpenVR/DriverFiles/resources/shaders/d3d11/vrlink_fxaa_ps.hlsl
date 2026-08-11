// vrlink_fxaa_ps.hlsl — FXAA quality pre-pass (fxaaMode = quality).
// Runs BEFORE the warp/CAS pass, in unwarped source space on the axis
// aligned texel grid FXAA's edge search assumes, writing an AA-resolved
// copy of the bounds region into the fx intermediate. The main pass then
// samples fully resolved neighborhoods, removing the fast path's
// compromise (CAS sharpening raw neighbors around an AA center).
// SHIP-TOGETHER: this file, vrlink_layer_ps.hlsl and the dll move as a
// set; the cbuffer below must stay byte-identical to the layer shader's.

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
SamplerState samp : register(s0);

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
float FxaaLuma(float3 c){
	return sqrt(dot(c, float3(0.299, 0.587, 0.114)));
}
// bounds mapped source sample; srgb typed views decode in hardware,
// manualSrgb layers decode here (mirrors SampleWarped in the main pass)
float4 SampleSrc(float2 uvSrcNorm){
	float4 c = tex.SampleLevel(samp, uvSrcNorm * boundsSize + boundsMin, 0);
	if(manualSrgb > 0.5){
		c.rgb = SrgbToLinear(c.rgb);
	}
	return c;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0{
	float4 color = SampleSrc(uv);
	float2 tf = texelSize / boundsSize;
	float lNW = FxaaLuma(SampleSrc(uv + float2(-tf.x, -tf.y)).rgb);
	float lNE = FxaaLuma(SampleSrc(uv + float2( tf.x, -tf.y)).rgb);
	float lSW = FxaaLuma(SampleSrc(uv + float2(-tf.x,  tf.y)).rgb);
	float lSE = FxaaLuma(SampleSrc(uv + float2( tf.x,  tf.y)).rgb);
	float lM  = FxaaLuma(color.rgb);
	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
	if(lMax - lMin >= max(0.0625, lMax * 0.125)){
		lNE += 1.0 / 384.0;
		float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
		float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 512.0);
		float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
		dir = clamp(dir * rcpMin, -8.0, 8.0) * tf;
		float3 rgbA = 0.5 * (SampleSrc(uv + dir * (1.0 / 3.0 - 0.5)).rgb
			+ SampleSrc(uv + dir * (2.0 / 3.0 - 0.5)).rgb);
		float3 rgbB = rgbA * 0.5 + 0.25 * (SampleSrc(uv + dir * -0.5).rgb
			+ SampleSrc(uv + dir * 0.5).rgb);
		float lB = FxaaLuma(rgbB);
		color.rgb = (lB < lMin || lB > lMax) ? rgbA : rgbB;
	}
	if(manualSrgb > 0.5){
		color.rgb = LinearToSrgb(color.rgb);
	}
	return color;
}
