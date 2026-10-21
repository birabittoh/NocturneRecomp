// Pixel shader for the SDK's overlay drawer.
//
// Colour-only draws still sample: the drawer binds a 1x1 opaque white texture
// when ImmediateDraw::texture is null, so there is no second pipeline and no
// branch here.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

[[vk::binding(0, 0)]] Texture2D<float4> uiTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState uiSampler : register(s1);

float4 PSMain(VSOutput input) : SV_TARGET {
    return input.color * uiTexture.Sample(uiSampler, input.uv);
}
