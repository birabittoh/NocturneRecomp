// Pixel shader for the present blit.
//
// The guest's image is opaque by construction, so alpha is forced to one rather
// than carried through: a resolve destination can hold whatever the guest left
// in its alpha channel, and a swap chain image that is not opaque composites
// against the desktop on some paths.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

float4 PSMain(VSOutput input) : SV_TARGET {
    return float4(sourceTexture.Sample(sourceSampler, input.uv).rgb, 1.0);
}
