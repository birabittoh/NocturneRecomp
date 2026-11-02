// Pixel shader for the JP release's movies (jp/movie_player.cpp), drawn into
// the PS1 box of the guest target ahead of the screen filter. The frame is
// copied as it is: the present blit's display ramp goes over it as over
// everything else the guest draws.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> movieTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState movieSampler : register(s1);

float4 PSMain(VSOutput input) : SV_TARGET {
    return float4(movieTexture.Sample(movieSampler, input.uv).rgb, 1.0);
}
