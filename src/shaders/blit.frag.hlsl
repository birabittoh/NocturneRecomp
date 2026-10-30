// Pixel shader for the present blit.
//
// The guest's image is opaque by construction, so alpha is forced to one rather
// than carried through: a resolve destination can hold whatever the guest left
// in its alpha channel, and a swap chain image that is not opaque composites
// against the desktop on some paths.
//
// Applies the display ramp the guest's D3D library uploads on every swap when
// the title does not set one: sRGB decode followed by the BT.709 encode that
// VdGetCurrentDisplayGamma (type 2) asks for. See sub_82533C48, which builds it
// through sub_8252AED8 / sub_8252AD38 (sRGB sub_8252ABA0, BT.709 sub_8252AC50).
// The ramp lands in display controller registers the native renderer never
// reads, so without this the picture is darker than on the console.
//
// The player's gamma setting rides on top as a power on the result.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

[[vk::push_constant]]
struct Constants {
    float invGamma;
} constants;

float3 DisplayRamp(float3 c) {
    c = saturate(c);
    float3 lin = select(c > 0.04045, pow((c + 0.055) / 1.055, 2.4), c / 12.92);
    return select(lin < 0.018, lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099);
}

float4 PSMain(VSOutput input) : SV_TARGET {
    float3 c = DisplayRamp(sourceTexture.Sample(sourceSampler, input.uv).rgb);
    return float4(pow(c, constants.invGamma), 1.0);
}
