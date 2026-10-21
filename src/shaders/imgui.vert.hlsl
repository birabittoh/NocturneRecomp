// Vertex shader for the SDK's overlay drawer (ImmediateDrawer / ImGui).
//
// Vertices arrive in ui::ImmediateVertex layout, which is deliberately the same
// as ImDrawVert: float2 position, float2 uv, packed RGBA8 colour. Positions are
// in the overlay's coordinate space (pixels), so the only transform is the
// orthographic scale and translate handed down as push constants.
//
// Note the shader compiler is invoked with -fvk-invert-y for vertex shaders, so
// this is written for the D3D clip space convention and the Vulkan flip is
// applied for us.

struct VSInput {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

[[vk::push_constant]]
struct Constants {
    float2 scale;
    float2 translate;
} constants;

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position * constants.scale + constants.translate, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
