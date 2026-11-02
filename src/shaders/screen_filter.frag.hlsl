// Pixel shader for the screen filter (the screen_filter cvar).
//
// The game draws the PS1 picture straight into its frame, within
// graphics.stretch_rect, and its own UI (the Xbox pause menu, its settings
// screens) on top of it afterwards. So the filter is drawn into the guest target
// between the two (see GuestFilterPs1Picture), under a viewport covering that
// rect: uv runs 0..1 across the PS1 picture, and the UI lands on top unfiltered.
//
// The output is a factor the target is multiplied by (the pipeline blends with
// dst * src), so the picture is never read back. It is laid out on the PS1's own
// 256x240 display, so a scanline is one line of the original picture however
// the rect is stretched.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// ScreenFilterKind (src/core/settings.h).
static const uint FILTER_SCANLINES = 1;
static const uint FILTER_LCD_GRID = 2;

static const float2 PS1_DISPLAY = float2(256.0, 240.0);
static const float PI = 3.14159265;

[[vk::push_constant]]
struct Constants {
    uint filter;
    // How dark the gaps between lines get, 0 to 1.
    float strength;
} constants;

// How much of a gap between two PS1 lines (or columns) this pixel covers, 0 in
// the middle of a line and 1 on the boundary: 0.5 + 0.5 cos(2 pi x), averaged
// over the pixel's footprint. The average of the cosine over a box of width w
// is the cosine scaled by sinc(w), so the pattern fades to an even tint as the
// lines approach a pixel apart instead of shimmering. x is in lines.
float Gap(float x, float footprint) {
    float w = clamp(footprint, 1e-4, 1.0);
    return 0.5 + 0.5 * cos(2.0 * PI * x) * (sin(PI * w) / (PI * w));
}

float4 PSMain(VSOutput input) : SV_TARGET {
    float2 ps1 = input.uv * PS1_DISPLAY;
    float2 footprint = fwidth(ps1);
    float factor = 1.0;
    if (constants.filter == FILTER_SCANLINES) {
        factor = 1.0 - constants.strength * Gap(ps1.y, footprint.y);
    } else if (constants.filter == FILTER_LCD_GRID) {
        factor = (1.0 - constants.strength * Gap(ps1.x, footprint.x)) *
                 (1.0 - constants.strength * Gap(ps1.y, footprint.y));
    }
    return float4(factor.xxx, 1.0);
}
