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
	float pad2;
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

float4 SampleWarped(float2 uvSrcNorm){
	return tex.SampleLevel(samp, uvSrcNorm * boundsSize + boundsMin, 0);
}

// sample one curve row of the lut. row centers avoid bleed between curves.
float SampleLutRow(float u, float row){
	return lut.SampleLevel(samp, float2(u, (row + 0.5) / lutRowCount), 0).x;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0{
	// the viewport covers exactly the bounds region, so uv is already bounds normalized
	// ---- distortion resample ----
	float2 p = uv - center;
	p.y *= aspect;
	float r = length(p);
	// radial scale from the baked curve rows (k1k2 polynomial or spline).
	// per axis blends the horizontal and vertical curves by the squared cosine
	// of the ring angle, giving an elliptic correction.
	float u = r / lutMaxR;
	float s;
	if(perAxisEnable > 0.5){
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
	float2 nSrc = p + center;
	float4 color = SampleWarped(nSrc);
	bool outside = any(nSrc < 0.0) || any(nSrc > 1.0);

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
		g = saturate(g + noise / 255.0);
		color.rgb = SrgbToLinear(g);
	}

	return color;
}
