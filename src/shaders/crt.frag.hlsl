// Pixel shader for the CRT screen filter (screen_filter = "crt"): scanlines
// whose beam widens on bright lines, an aperture grille mask, colour bleed,
// bloom and halation around bright areas and a vignette. It draws the picture
// flat: the Screen Curvature setting bends it afterwards.
//
// Drawn like a mod's filter (docs/making-mods.md, "Screen filters"), in their
// layout and with their constants: over the PS1 picture's rect, reading it from
// a copy of the frame. input.uv runs 0..1 across the picture,
// constants.sourceRect says where the picture is in sourceTexture, and the
// result replaces what is there. The effects (scanlines, mask, bloom,
// vignette) scale with the Filter Intensity setting (constants.intensity), so
// at its lowest the picture is close to untouched.

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
    float intensity;     // 0.05 to 1
    uint frameCount;
    float2 pad;
} constants;

// The PS1's display: one scanline per line of it, however it is stretched.
static const float2 PS1_SIZE = float2(256.0, 240.0);
// Where the picture is read: the finest the PS1 draws, its 512 wide mode (the
// game's text uses it), which reads the 256 wide one exactly too, two taps a
// pixel. Reading only 256 across would skip every other column of text.
static const float2 GRID = float2(512.0, 240.0);
// A grid step across, in PS1 pixels.
static const float GRID_X = PS1_SIZE.x / GRID.x;

static const float GAMMA_IN = 2.4;
static const float GAMMA_OUT = 2.4;

// How far each effect goes at full intensity.
static const float SCANLINE_DEPTH = 1.0;
static const float BEAM_MIN = 0.30;  // beam width on a dark line, in lines
static const float BEAM_MAX = 0.62;  // and on a bright one
static const float MASK_DEPTH = 0.30;
static const float BLOOM_SIGMA = 1.6;  // in PS1 pixels
static const float BLOOM_THRESHOLD = 0.25;  // linear brightness where it starts
static const float BLOOM_KNEE = 0.75;  // and how far above that it takes hold
static const float BLOOM_STRENGTH = 0.75;
static const float HALATION_STRENGTH = 0.06;
static const float VIGNETTE = 0.35;
// Colour bleed: the composite signal carries colour at a fraction of the
// brightness's bandwidth, so colour smears sideways over a few pixels while
// the picture's detail stays sharp.
static const float CHROMA_SIGMA = 1.4;  // in PS1 pixels
static const float CHROMA_GAIN = 1.25;  // makes up for what spreading thins out

float3 ToLinear(float3 c) { return pow(saturate(c), GAMMA_IN); }

// The picture at p (0..1 across it), as stored (gamma encoded, the way the
// signal carries it), clamped to its own edges so nothing next to it bleeds in.
float3 FetchSignal(float2 p) {
    float2 uv = constants.sourceRect.xy + saturate(p) * constants.sourceRect.zw;
    return sourceTexture.SampleLevel(linearSampler, uv, 0).rgb;
}

// The same, linear.
float3 Fetch(float2 p) { return ToLinear(FetchSignal(p)); }

// NTSC's YIQ: brightness in Y, colour in I and Q.
float3 ToYIQ(float3 c) {
    return float3(dot(c, float3(0.299, 0.587, 0.114)),
                  dot(c, float3(0.596, -0.274, -0.322)),
                  dot(c, float3(0.211, -0.523, 0.312)));
}

float3 FromYIQ(float3 c) {
    return float3(dot(c, float3(1.0, 0.956, 0.621)),
                  dot(c, float3(1.0, -0.272, -0.647)),
                  dot(c, float3(1.0, -1.106, 1.703)));
}

// A Gaussian of `sigma` at `d`, brought down to zero at `reach`, so that a
// tap leaving the window as the point moves fades out instead of jumping.
float Kernel(float d2, float sigma, float reach) {
    float k = -0.5 / (sigma * sigma);
    return max(exp(d2 * k) - exp(reach * reach * k), 0.0);
}

// PS1 line `row`, decoded like an analog signal: brightness softened a
// little across, colour smeared over a few pixels, so a lone coloured pixel
// tints its neighbours (a sprite's one-pixel red eye fills the eye).
//
// The taps are the line's own pixels, read at their centres, and only their
// weights follow x. Taps that moved with x would read the same pixels over
// whole stretches and then jump, which stripes the picture column by column.
float3 Row(float x, float row) {
    float y = (row + 0.5) / PS1_SIZE.y;
    float q = x * GRID.x - 0.5;
    float base = floor(q);
    float f = q - base;
    float luma = 0.0, luma_weight = 0.0;
    float2 sharp = 0.0, chroma = 0.0;
    float chroma_weight = 0.0;
    [unroll] for (int i = -7; i <= 8; ++i) {
        float3 c = ToYIQ(FetchSignal(float2((base + i + 0.5) / GRID.x, y)));
        float d = (i - f) * GRID_X;
        float d2 = d * d;
        float wl = Kernel(d2, 0.25, 0.75);
        float wc = Kernel(d2, CHROMA_SIGMA, 4.0);
        luma += c.x * wl;
        sharp += c.yz * wl;
        luma_weight += wl;
        chroma += c.yz * wc;
        chroma_weight += wc;
    }
    luma /= luma_weight;
    sharp /= luma_weight;
    chroma *= CHROMA_GAIN / chroma_weight;
    chroma = lerp(sharp, chroma, constants.intensity);
    return ToLinear(FromYIQ(float3(luma, chroma)));
}

// The beam's profile at `distance` lines from its centre: Gaussian, wider when
// the line is brighter, as a real beam blooms with its current.
float3 Beam(float distance, float3 color) {
    float3 width = lerp(BEAM_MIN, BEAM_MAX, sqrt(saturate(color)));
    float3 d = distance / width;
    return exp(-d * d);
}

float3 Scanlines(float2 p, float depth) {
    float y = p.y * PS1_SIZE.y - 0.5;
    float row = floor(y);
    float f = y - row;
    float3 a = Row(p.x, row);
    float3 b = Row(p.x, row + 1.0);
    float3 lit = a * Beam(f, a) + b * Beam(1.0 - f, b);
    // The beams give back a little under what they cover; made up so that
    // the lines darken the gaps more than they dim the picture.
    lit *= 1.35;
    float3 even = lerp(a, b, f);
    return lerp(even, lit, depth);
}

// How much of a colour blooms: nothing below the threshold, then rising
// steeply, so that bright colours spill far more than mid tones do.
float BrightPass(float3 c) {
    float m = max(c.r, max(c.g, c.b));
    float t = saturate((m - BLOOM_THRESHOLD) / BLOOM_KNEE);
    return t * t * (3.0 - 2.0 * t) * (1.0 + m);
}

// A Gaussian blur of the picture over its own pixels, read at their centres
// with weights that follow p (see Row), so the glow is smooth rather than
// offset copies of what is bright or blocks a PS1 pixel wide. Gives the
// bright-passed blur (bloom) and the plain one (halation).
void Glow(float2 p, out float3 bloom, out float3 haze) {
    float2 q = p * GRID - 0.5;
    float2 base = floor(q);
    float2 f = q - base;
    bloom = 0.0;
    haze = 0.0;
    float weight = 0.0;
    [unroll] for (int y = -2; y <= 3; ++y) {
        [unroll] for (int x = -5; x <= 6; ++x) {
            float2 d = (float2(x, y) - f) * float2(GRID_X, 1.0);
            float w = Kernel(dot(d, d), BLOOM_SIGMA, 3.0);
            float3 c = Fetch((base + float2(x, y) + 0.5) / GRID);
            bloom += c * (w * BrightPass(c));
            haze += c * w;
            weight += w;
        }
    }
    bloom /= weight;
    haze /= weight;
}

// An aperture grille: vertical red, green and blue stripes on the output's
// own pixels.
float3 Mask(float2 pixel, float depth) {
    int stripe = int(fmod(floor(pixel.x), 3.0));
    float3 mask = 1.0 - depth;
    if (stripe == 0) mask.r = 1.0;
    else if (stripe == 1) mask.g = 1.0;
    else mask.b = 1.0;
    // Keeps the average brightness where it was.
    return mask / (1.0 - depth * 2.0 / 3.0);
}

float4 PSMain(VSOutput input) : SV_TARGET {
    float k = constants.intensity;
    float2 p = input.uv;

    float3 color = Scanlines(p, SCANLINE_DEPTH * k);

    float3 bloom, haze;
    Glow(p, bloom, haze);
    // Bloom: the bright parts spill over; halation: a faint haze of all of it.
    color += bloom * BLOOM_STRENGTH * k;
    color += haze * HALATION_STRENGTH * k;

    color *= Mask(input.position.xy, MASK_DEPTH * k);

    float2 v = p * (1.0 - p);
    float vignette = pow(saturate(v.x * v.y * 16.0), 0.25);
    color *= lerp(1.0, vignette, VIGNETTE * k);

    color = pow(saturate(color), 1.0 / GAMMA_OUT);
    return float4(color, 1.0);
}
