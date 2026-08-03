// Processing applied to direct mode scene layer textures before the streaming
// driver (e.g. vrlink / Steam Link) composites and encodes them.
//
// This file is hot reloaded: save it while SteamVR is running and the change
// applies within about a second. If it fails to compile, the previous working
// shader stays active and the error is written to the vrserver log.
//
// Tuning the distortion pre perturbation ("wobble" fix):
// - The saturation/k1/k2/center values come from the streamFrame section of
//   settings.json via the constant buffer below, so normally you tune by
//   editing settings.json (also live reloaded). Edit this file when you want
//   to change the *model*, e.g. add a k3 term or asymmetric correction.
// - k1 affects the whole field roughly quadratically with radius, k2 mostly
//   the periphery. Start with k1 steps of +-0.005 while fixating a straight
//   line and rotating your head; pick the sign that reduces the swimming.

cbuffer Params : register(b0){
	float saturation;     // saturation / 50, so 1 = neutral
	float k1;
	float k2;
	float applyColor;     // 1 or 0 (0 while the dashboard is open by default)
	float2 center;        // distortion center in bounds normalized uv
	float2 boundsMin;     // valid region of the texture (usually 0,0)
	float2 boundsSize;    // usually 1,1
	float aspect;         // region height / width, makes the radius isotropic
	float pad;
};
Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0{
	// the viewport covers exactly the bounds region, so uv is already bounds normalized
	float2 n = uv;
	// radial pre perturbation: output pixel n samples input at center + p * s
	float2 p = n - center;
	p.y *= aspect;
	float r2 = dot(p, p);
	float s = 1.0 + k1 * r2 + k2 * r2 * r2;
	p *= s;
	p.y /= aspect;
	float2 nSrc = p + center;
	float2 uvSrc = nSrc * boundsSize + boundsMin;
	float4 color = tex.SampleLevel(samp, uvSrc, 0);
	// black outside the valid region instead of clamped streaks
	if(any(nSrc < 0.0) || any(nSrc > 1.0)){
		color = float4(0, 0, 0, color.a);
	}
	if(applyColor > 0.5){
		// linear space saturation (srgb views decode/encode around this shader)
		float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
		color.rgb = lerp(gray.xxx, color.rgb, saturation);
	}
	return color;
}
