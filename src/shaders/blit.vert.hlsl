// Vertex shader for the present blit.
//
// A single oversized triangle covering clip space, generated from SV_VertexID,
// so the blit needs no vertex buffer and no input layout. Scaling is done by
// the viewport the draw is issued under, not here.
//
// The shader compiler is invoked with -fvk-invert-y for vertex shaders, so this
// is written for the D3D clip space convention and the Vulkan flip is applied
// for us, exactly as in imgui.vert.hlsl.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.uv = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
