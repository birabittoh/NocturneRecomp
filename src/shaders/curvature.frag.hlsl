// Pixel shader for the Screen Curvature setting (screen_curvature): bends the
// PS1 picture like a CRT tube's glass, with rounded corners, once the screen
// filter (if any) has drawn it. Redraws the picture's rect from a copy of the
// target, in the mod filters' layout (docs/making-mods.md, "Screen filters"),
// with the amount, 0 to 1, where their intensity goes.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState linearSampler : register(s1);
[[vk::binding(2, 0)]] SamplerState pointSampler : register(s2);

[[vk::push_constant]]
struct Constants {
    float4 sourceRect;   // the picture in sourceTexture's uv: offset, size
    float2 textureSize;  // sourceTexture, in pixels
    float2 outputSize;   // the picture as drawn, in pixels
    float curvature;     // 0 (flat) to 1
    uint frameCount;
    float2 pad;
} constants;

// At full curvature: how far the edges' middles are pulled in, and the
// corners' radius, both in halves of the picture's height.
static const float MAX_BULGE = 0.09;
static const float MAX_CORNER = 0.14;

float4 PSMain(VSOutput input) : SV_TARGET {
    float k = constants.curvature;
    float2 c = input.uv * 2.0 - 1.0;
    c *= 1.0 + k * MAX_BULGE * float2(c.y * c.y, c.x * c.x);
    float2 p = c * 0.5 + 0.5;

    // 1 inside the tube's rounded rectangle, 0 outside, with a pixel of soft
    // edge.
    float2 aspect = float2(constants.outputSize.x / constants.outputSize.y, 1.0);
    float2 a = abs(c) * aspect;
    float radius = k * MAX_CORNER;
    float d = length(max(a - (aspect - radius), 0.0)) - radius;
    float inside = saturate(-d * constants.outputSize.y * 0.5);

    float2 uv = constants.sourceRect.xy + saturate(p) * constants.sourceRect.zw;
    float3 color = sourceTexture.SampleLevel(linearSampler, uv, 0).rgb;
    return float4(color * inside, 1.0);
}
