// nocturnerecomp - ReXGlue Recompiled Project
//
// The game's one tessellated draw: a screen wide strip with a scrolling texture
// (the smoke at the top of the menus), drawn by sub 0x82581990 as a single quad
// patch of four control points through TesselationVS. The Xenos tessellator
// generates the vertices and TesselationVS evaluates the patch for each one, so
// with no GPU there is nothing to run that shader on. Instead the patch is
// evaluated here on a fixed grid and the result is drawn as an ordinary quad
// list through FrameworkVS, which is a pass through of the same four outputs.
//
// What TesselationVS computes per generated vertex (u, v), from the shader
// microcode (see scripts/gen-guest-shaders.py for why it is not in the pack):
//   * position, colour and both texture coordinates, bilinear between the
//     control points in loop order (0 at u=v=0, 1 at u=1, 2 at u=v=1, 3 at v=1);
//   * the alpha of the result replaced by a fade that depends only on the
//     interpolated position and on c0/c1: c1 holds the x range of the ramps
//     (c1.xy the left edge, c1.zw the right edge), c0 the y range likewise.
//     Equal bounds make a ramp zero wide, i.e. no fade, which is what the game
//     sets while the strip is fully visible.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/logging.h>

#include "guest_shaders.h"
#include "native_renderer_draw.h"

namespace nocturne {
namespace {

constexpr uint32_t kVertexFloatShadow = 1792;
constexpr uint32_t kStreamStrides = 0x2FD8;
constexpr uint32_t kPrimQuadList = 13;
constexpr uint64_t kFrameworkVertexKey = 0xE18AA685724DEF4Full;
constexpr uint32_t kControlPointStride = 32;
constexpr float kCellSize = 16.0f;  // pixels per generated cell, at most
constexpr uint32_t kMaxCells = 160;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// The hardware multiply: a zero operand gives zero even against an infinity.
float XeMul(float a, float b) { return std::min(std::fabs(a), std::fabs(b)) == 0.0f ? 0.0f : a * b; }

// cndeq: a == 0 ? b : c.
float CndEq(float a, float b, float c) { return a == 0.0f ? b : c; }

struct ControlPoint {
  float position[3];
  uint32_t color;
  float texcoord0[2];
  float texcoord1[2];
};

struct FadeConstants {
  float c0[4], c1[4], zero;  // zero is c255.x
};

// The alpha TesselationVS exports for a vertex at (x, y) whose interpolated
// alpha is `alpha`, instruction for instruction.
float Fade(const FadeConstants& k, float x, float y, float alpha) {
  const float *c0 = k.c0, *c1 = k.c1;
  const float x_right_over = x > c1[2], x_right_end = x > c1[3];
  const float x_left_end = c1[1] > x, x_left_over = c1[0] > x;
  const float y_over_end = y > c0[3];
  const float y_top_end = c0[1] > y, y_top_over = c0[0] > y;
  const float ramp_y = XeMul(y - c0[0], 1.0f / (c0[1] - c0[0]));
  const float ramp_right = XeMul(x - c1[2], 1.0f / (c1[3] - c1[2]));
  const float ramp_left = XeMul(x - c1[0], 1.0f / (c1[1] - c1[0]));

  float w = alpha;
  w = CndEq(x_left_end, w, ramp_left);
  w = CndEq(x_left_over, w, k.zero);
  w = CndEq(x_right_over, w, ramp_right);
  w = CndEq(x_right_end, w, k.zero);
  w = CndEq(y_top_end, w, ramp_y);
  w = CndEq(y_top_over, w, k.zero);
  if (y_over_end != 0.0f) {
    w = 0.0f;  // -|r| > 0 is never true
  } else {
    const float bottom = XeMul(y - c0[2], 1.0f / (c0[3] - c0[2]));
    w = CndEq(float(y > c0[2]), w, bottom);
  }
  return w;
}

struct Vertex {
  float position[3];
  uint32_t color;
  float texcoord0[2];
  float texcoord1[2];
};
static_assert(sizeof(Vertex) == kControlPointStride);

Vertex Evaluate(const ControlPoint (&cp)[4], const FadeConstants& k, float u, float v) {
  // Weights of the corners at (0,0), (1,0), (1,1), (0,1).
  const float w[4] = {(1 - u) * (1 - v), u * (1 - v), u * v, (1 - u) * v};
  Vertex out{};
  float channel[4] = {};
  for (int i = 0; i < 4; ++i) {
    for (int a = 0; a < 3; ++a)
      out.position[a] += w[i] * cp[i].position[a];
    for (int a = 0; a < 2; ++a) {
      out.texcoord0[a] += w[i] * cp[i].texcoord0[a];
      out.texcoord1[a] += w[i] * cp[i].texcoord1[a];
    }
    for (int c = 0; c < 4; ++c)
      channel[c] += w[i] * float((cp[i].color >> (8 * c)) & 0xFFu);
  }
  const float alpha = Fade(k, out.position[0], out.position[1], channel[3] / 255.0f);
  channel[3] = std::isnan(alpha) ? 0.0f : std::clamp(alpha, 0.0f, 1.0f) * 255.0f;
  for (int c = 0; c < 4; ++c)
    out.color |= uint32_t(std::lround(std::clamp(channel[c], 0.0f, 255.0f))) << (8 * c);
  return out;
}

}  // namespace

void GuestDrawTessellatedQuad(uint8_t* base, uint32_t device, uint32_t control_points) {
  if (base[device + kStreamStrides] != kControlPointStride / 4) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("native_renderer: tessellated draw with a stride of {} bytes is not handled",
                  base[device + kStreamStrides] * 4);
    }
    return;
  }
  static const int vertex_slot = FindGuestVertexShaderByKey(kFrameworkVertexKey);
  if (vertex_slot < 0)
    return;

  ControlPoint cp[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t a = control_points + kControlPointStride * i;
    for (int c = 0; c < 3; ++c)
      cp[i].position[c] = LoadFloat(base, a + 4 * c);
    cp[i].color = Load32(base, a + 12);
    for (int c = 0; c < 2; ++c) {
      cp[i].texcoord0[c] = LoadFloat(base, a + 16 + 4 * c);
      cp[i].texcoord1[c] = LoadFloat(base, a + 24 + 4 * c);
    }
  }
  FadeConstants k;
  for (int c = 0; c < 4; ++c) {
    k.c0[c] = LoadFloat(base, device + kVertexFloatShadow + 4 * c);
    k.c1[c] = LoadFloat(base, device + kVertexFloatShadow + 16 + 4 * c);
  }
  k.zero = LoadFloat(base, device + kVertexFloatShadow + 16 * 255);

  float min_x = cp[0].position[0], max_x = min_x, min_y = cp[0].position[1], max_y = min_y;
  for (int i = 1; i < 4; ++i) {
    min_x = std::min(min_x, cp[i].position[0]);
    max_x = std::max(max_x, cp[i].position[0]);
    min_y = std::min(min_y, cp[i].position[1]);
    max_y = std::max(max_y, cp[i].position[1]);
  }
  const uint32_t cells_u = std::clamp(uint32_t(std::ceil((max_x - min_x) / kCellSize)), 1u, kMaxCells);
  const uint32_t cells_v = std::clamp(uint32_t(std::ceil((max_y - min_y) / kCellSize)), 1u, kMaxCells);

  // The grid's points, then each cell as one quad (loop order, like the guest's).
  std::vector<Vertex> grid((cells_u + 1) * (cells_v + 1));
  for (uint32_t j = 0; j <= cells_v; ++j)
    for (uint32_t i = 0; i <= cells_u; ++i)
      grid[j * (cells_u + 1) + i] = Evaluate(cp, k, float(i) / cells_u, float(j) / cells_v);
  std::vector<Vertex> quads;
  quads.reserve(size_t(cells_u) * cells_v * 4);
  for (uint32_t j = 0; j < cells_v; ++j) {
    for (uint32_t i = 0; i < cells_u; ++i) {
      const uint32_t at = j * (cells_u + 1) + i;
      quads.push_back(grid[at]);
      quads.push_back(grid[at + 1]);
      quads.push_back(grid[at + cells_u + 2]);
      quads.push_back(grid[at + cells_u + 1]);
    }
  }

  GuestInlineDraw draw;
  draw.device = device;
  draw.primitive = kPrimQuadList;
  draw.count = uint32_t(quads.size());
  draw.stride = kControlPointStride;
  draw.host_vertices = reinterpret_cast<const uint8_t*>(quads.data());
  draw.vertex_slot = vertex_slot;
  GuestDrawInline(base, draw);
}

}  // namespace nocturne
