/**
 * @file        touch_controls_overlay.cpp
 * @brief       Touch pad overlay. See touch_controls_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "touch_controls_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(double, touch_opacity);

namespace nocturne::touch {

namespace {

// Held controls brighten
constexpr float kPressedBoost = 2.0f;

constexpr float kPi = 3.14159265f;

enum class FaceGlyph { kCross, kCircle, kSquare, kTriangle };

struct FaceStyle {
  ImVec4 color;
  const char* label;
  FaceGlyph glyph;
};

bool GetFaceStyle(uint16_t button, TouchFaceStyle face_style, FaceStyle* style) {
  const bool ps = face_style == TouchFaceStyle::kPlayStation;
  switch (button) {
    case X_INPUT_GAMEPAD_A:
      *style = ps ? FaceStyle{{0.490f, 0.651f, 0.949f, 1.0f}, "", FaceGlyph::kCross}
                  : FaceStyle{{0.369f, 0.682f, 0.227f, 1.0f}, "A", FaceGlyph::kCross};
      return true;
    case X_INPUT_GAMEPAD_B:
      *style = ps ? FaceStyle{{0.957f, 0.392f, 0.408f, 1.0f}, "", FaceGlyph::kCircle}
                  : FaceStyle{{0.761f, 0.231f, 0.231f, 1.0f}, "B", FaceGlyph::kCircle};
      return true;
    case X_INPUT_GAMEPAD_X:
      *style = ps ? FaceStyle{{0.902f, 0.553f, 0.792f, 1.0f}, "", FaceGlyph::kSquare}
                  : FaceStyle{{0.243f, 0.471f, 0.761f, 1.0f}, "X", FaceGlyph::kSquare};
      return true;
    case X_INPUT_GAMEPAD_Y:
      *style = ps ? FaceStyle{{0.282f, 0.788f, 0.643f, 1.0f}, "", FaceGlyph::kTriangle}
                  : FaceStyle{{0.847f, 0.635f, 0.118f, 1.0f}, "Y", FaceGlyph::kTriangle};
      return true;
    default:
      return false;
  }
}

ImVec4 LerpColor(const ImVec4& a, const ImVec4& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
          a.w + (b.w - a.w) * t};
}

ImU32 ColorWithAlpha(ImVec4 color, float alpha) {
  color.w = std::clamp(alpha, 0.0f, 1.0f);
  return ImGui::GetColorU32(color);
}


ImFont* FaceButtonFont() {
  for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {
    if (std::strcmp(font->GetDebugName(), "Touch controls") == 0) {
      return font;
    }
  }
  return ImGui::GetFont();
}


ImU32 FillColor(float alpha, bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f), 1.0f);
  return ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, a));
}

ImU32 EdgeColor(float alpha, bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f) * 1.6f, 1.0f);
  return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a));
}

// The shoulder, trigger and menu plates mirror shoulder() and menu_button() in
// scripts/gen-face-buttons.py, redrawn as vectors so they follow touch_scale.
struct Shape {
  std::vector<ImVec2> pts;    // -1..1 across the control, before rounding
  std::vector<float> radius;  // corner radius per point, in control half heights
};

// Replaces every corner with a short curve; stays convex if the input is.
std::vector<ImVec2> RoundShape(const Shape& shape, ImVec2 mid, ImVec2 half, float side) {
  const size_t n = shape.pts.size();
  std::vector<ImVec2> px(n);
  for (size_t i = 0; i < n; ++i) {
    px[i] = ImVec2(mid.x + shape.pts[i].x * side * half.x, mid.y + shape.pts[i].y * half.y);
  }
  std::vector<ImVec2> out;
  for (size_t i = 0; i < n; ++i) {
    const ImVec2 v = px[i];
    const ImVec2 a = px[(i + n - 1) % n];
    const ImVec2 b = px[(i + 1) % n];
    const float la = std::hypot(a.x - v.x, a.y - v.y);
    const float lb = std::hypot(b.x - v.x, b.y - v.y);
    const float k = std::min(shape.radius[i] * half.y, 0.5f * std::min(la, lb));
    const ImVec2 pa(v.x + (a.x - v.x) / la * k, v.y + (a.y - v.y) / la * k);
    const ImVec2 pb(v.x + (b.x - v.x) / lb * k, v.y + (b.y - v.y) / lb * k);
    constexpr int kSteps = 6;
    for (int s = 0; s <= kSteps; ++s) {
      const float t = float(s) / float(kSteps);
      const float w0 = (1 - t) * (1 - t), w1 = 2 * t * (1 - t), w2 = t * t;
      out.emplace_back(w0 * pa.x + w1 * v.x + w2 * pb.x, w0 * pa.y + w1 * v.y + w2 * pb.y);
    }
  }
  return out;
}

// Vertical gradient across a convex outline. Colour is linear in y, so the
// fan's per-vertex colours interpolate exactly.
void FillGradient(ImDrawList* draw_list, const std::vector<ImVec2>& pts, const ImVec4& top,
                  const ImVec4& bottom) {
  float y0 = FLT_MAX, y1 = -FLT_MAX;
  for (const ImVec2& p : pts) {
    y0 = std::min(y0, p.y);
    y1 = std::max(y1, p.y);
  }
  const int n = int(pts.size());
  draw_list->PrimReserve((n - 2) * 3, n);
  const ImDrawIdx base = ImDrawIdx(draw_list->_VtxCurrentIdx);
  for (int i = 1; i + 1 < n; ++i) {
    draw_list->PrimWriteIdx(base);
    draw_list->PrimWriteIdx(ImDrawIdx(base + i));
    draw_list->PrimWriteIdx(ImDrawIdx(base + i + 1));
  }
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  for (const ImVec2& p : pts) {
    const float t = std::clamp((p.y - y0) / std::max(y1 - y0, 1.0f), 0.0f, 1.0f);
    draw_list->PrimWriteVtx(p, uv, ImGui::GetColorU32(LerpColor(top, bottom, t)));
  }
}

// A gradient body over a dark outline.
void DrawPlate(ImDrawList* draw_list, const std::vector<ImVec2>& pts, float edge,
               const ImVec4& edge_color, const ImVec4& top, const ImVec4& bottom) {
  draw_list->AddPolyline(pts.data(), int(pts.size()), ImGui::GetColorU32(edge_color),
                         ImDrawFlags_Closed, edge * 2.0f);
  FillGradient(draw_list, pts, top, bottom);
}

// Bold white lettering in a black outline, as the game letters its glyphs.
void DrawOutlinedText(ImDrawList* draw_list, const char* text, ImVec2 mid, float size,
                      float alpha) {
  ImFont* font = FaceButtonFont();
  size = std::min(std::round(size), font->LegacySize);
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 pos(std::round(mid.x - ts.x * 0.5f), std::round(mid.y - ts.y * 0.5f));
  const ImU32 outline = ColorWithAlpha(ImVec4(0, 0, 0, 1), alpha);
  const float r = std::max(1.0f, size * 0.07f);
  constexpr int kSteps = 12;
  for (int i = 0; i < kSteps; ++i) {
    const float angle = 2.0f * kPi * float(i) / float(kSteps);
    draw_list->AddText(font, size,
                       ImVec2(pos.x + std::cos(angle) * r, pos.y + std::sin(angle) * r), outline,
                       text);
  }
  const ImU32 fill = ColorWithAlpha(ImVec4(1, 1, 1, 1), alpha);
  for (float dx : {-0.03f * size, 0.0f, 0.03f * size}) {
    draw_list->AddText(font, size, ImVec2(pos.x + dx, pos.y), fill, text);
  }
}

float TextWidth(const char* text, float size) {
  ImFont* font = FaceButtonFont();
  size = std::min(std::round(size), font->LegacySize);
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

void DrawPillIcon(ImDrawList* draw_list, const TouchControl& c, bool ps, float alpha,
                  bool pressed) {
  const float a = std::min(alpha * (pressed ? 2.6f : 2.0f), 1.0f);
  const ImVec2 half(c.half_width, c.half_height);
  const ImVec2 mid(c.cx, c.cy + (pressed ? c.half_height * 0.06f : 0.0f));
  const float edge = std::max(1.5f, c.half_height * 0.07f);
  const ImVec4 black(0, 0, 0, a);

  const bool left = c.trigger == TouchTrigger::kLeft || c.button == X_INPUT_GAMEPAD_LEFT_SHOULDER;
  const float side = left ? 1.0f : -1.0f;  // shapes are drawn as the left hand ones
  const bool trigger = c.trigger != TouchTrigger::kNone;
  const bool shoulder = trigger || c.button == X_INPUT_GAMEPAD_LEFT_SHOULDER ||
                        c.button == X_INPUT_GAMEPAD_RIGHT_SHOULDER;

  std::string label = c.label;
  if (ps && shoulder && label.size() == 2) {
    label[0] = left ? 'L' : 'R';
    label[1] = trigger ? '2' : '1';
  }
  const float letter_alpha = std::min(alpha * 2.1f, 1.0f);
  const float text_size = c.half_height * 1.15f;

  if (shoulder) {
    Shape shape;
    if (ps) {
      const float bottom = trigger ? 1.1f : 0.5f;
      shape = {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, {0.5f, 0.5f, bottom, bottom}};
    } else if (trigger) {
      shape = {{{-0.724f, -1}, {0.862f, -1}, {1, 1}, {-1, 1}, {-1, -0.61f}},
               {0.3f, 0.3f, 0.3f, 0.3f, 0.3f}};
    } else {
      shape = {{{1, -1}, {1, 1}, {-0.44f, 1}, {-1, 0.43f}, {-1, -0.43f}, {-0.44f, -1}},
               {0.2f, 0.2f, 0.5f, 0.5f, 0.5f, 0.5f}};
    }
    const std::vector<ImVec2> pts = RoundShape(shape, mid, half, side);
    if (ps) {
      DrawPlate(draw_list, pts, edge, black, ImVec4(0.48f, 0.48f, 0.52f, a),
                ImVec4(0.20f, 0.20f, 0.22f, a));
      const float inset = c.half_height * 0.12f;
      const ImVec2 inner_half(std::max(half.x - inset, 1.0f), std::max(half.y - inset, 1.0f));
      FillGradient(draw_list, RoundShape(shape, mid, inner_half, side),
                   ImVec4(0.34f, 0.34f, 0.37f, a), ImVec4(0.06f, 0.06f, 0.07f, a));
    } else if (trigger) {
      DrawPlate(draw_list, pts, edge, black, ImVec4(0.88f, 0.89f, 0.92f, a),
                ImVec4(0.40f, 0.41f, 0.45f, a));
      // The slot near the top edge.
      const float sx = mid.x - side * half.x * 0.05f;
      draw_list->AddRectFilled(ImVec2(sx - half.x * 0.38f, mid.y - half.y * 0.90f),
                               ImVec2(sx + half.x * 0.38f, mid.y - half.y * 0.62f),
                               ColorWithAlpha(ImVec4(0.12f, 0.12f, 0.13f, 1), a), half.y * 0.12f);
    } else {
      DrawPlate(draw_list, pts, edge, black, ImVec4(0.97f, 0.97f, 0.98f, a),
                ImVec4(0.55f, 0.55f, 0.57f, a));
    }
    if (pressed) {
      draw_list->AddPolyline(pts.data(), int(pts.size()),
                             ColorWithAlpha(ImVec4(1, 1, 1, 1), alpha * 0.85f), ImDrawFlags_Closed,
                             2.0f);
    }
    const float tx = mid.x + side * half.x * (ps ? 0.0f : (trigger ? -0.02f : 0.13f));
    DrawOutlinedText(draw_list, label.c_str(), ImVec2(tx, mid.y), text_size, letter_alpha);
    return;
  }

  // Keyed on the label rather than the button: a layout may send either
  // button from either pill (see touch_layout.h's remap handling).
  if (c.label == "START" || c.label == "BACK") {
    // The badge, pointing right for Start and left for Back, then the name.
    const char* name = c.label.c_str();
    const float dir = c.label == "START" ? 1.0f : -1.0f;
    const float r = c.half_height * 0.8f;
    const float gap = c.half_height * 0.3f;
    const float text_w = TextWidth(name, text_size);
    const float x0 = mid.x - (r * 2.0f + gap + text_w) * 0.5f;
    const ImVec2 bc(x0 + r, mid.y);
    if (ps) {
      const std::vector<ImVec2> tri = {ImVec2(bc.x + dir * r * 0.9f, bc.y),
                                       ImVec2(bc.x - dir * r * 0.8f, bc.y - r * 0.95f),
                                       ImVec2(bc.x - dir * r * 0.8f, bc.y + r * 0.95f)};
      DrawPlate(draw_list, tri, edge, black, ImVec4(0.46f, 0.46f, 0.50f, a),
                ImVec4(0.18f, 0.18f, 0.21f, a));
    } else {
      std::vector<ImVec2> disc;
      for (int i = 0; i < 32; ++i) {
        const float ang = 2.0f * kPi * float(i) / 32.0f;
        disc.emplace_back(bc.x + std::cos(ang) * r, bc.y + std::sin(ang) * r);
      }
      DrawPlate(draw_list, disc, edge, black, ImVec4(0.94f, 0.95f, 0.97f, a),
                ImVec4(0.52f, 0.53f, 0.58f, a));
      const float t = r * 0.5f;
      draw_list->AddTriangleFilled(ImVec2(bc.x + dir * t * 1.3f, bc.y),
                                   ImVec2(bc.x - dir * t * 0.9f, bc.y - t * 1.25f),
                                   ImVec2(bc.x - dir * t * 0.9f, bc.y + t * 1.25f),
                                   ColorWithAlpha(ImVec4(0.36f, 0.37f, 0.39f, 1), a));
    }
    DrawOutlinedText(draw_list, name, ImVec2(x0 + r * 2.0f + gap + text_w * 0.5f, mid.y),
                     text_size, letter_alpha);
    return;
  }

  // Menu: two square windows, the front one over the back one.
  const float s = c.half_height * 0.62f;
  const float shift = s * 0.55f;
  for (int i = 0; i < 2; ++i) {
    const float off = (i == 0 ? -shift : shift);
    const ImVec2 centre(mid.x + off, mid.y + off);
    const Shape shape = {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, {0.15f, 0.15f, 0.15f, 0.15f}};
    const std::vector<ImVec2> pts = RoundShape(shape, centre, ImVec2(s, s), 1.0f);
    DrawPlate(draw_list, pts, edge, black, ImVec4(0.97f, 0.97f, 0.98f, a),
              ImVec4(0.55f, 0.55f, 0.57f, a));
    if (pressed) {
      draw_list->AddPolyline(pts.data(), int(pts.size()),
                             ColorWithAlpha(ImVec4(1, 1, 1, 1), alpha * 0.85f), ImDrawFlags_Closed,
                             2.0f);
    }
  }
}

// Two right-pointing triangles, the fast forward glyph.
void DrawFastForwardIcon(ImDrawList* draw_list, float cx, float cy, float radius, float alpha,
                         bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f) * 1.8f, 1.0f);
  const ImU32 color = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a));
  const float h = radius * 0.42f;
  const float w = radius * 0.38f;
  for (int i = 0; i < 2; ++i) {
    const float x = cx + (i == 0 ? -w : w) - w * 0.5f;
    draw_list->AddTriangleFilled(ImVec2(x, cy - h), ImVec2(x, cy + h), ImVec2(x + w, cy), color);
  }
}

float SmoothStep(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Fills an ellipse as one mesh of nested rings shrinking toward `focus`, so a
// translucent gradient covers each pixel once instead of stacking into bands.
// `shade(t, dir)` gives the colour at ring t (0 at the focus, 1 at the edge)
// in direction dir; a one pixel fringe fades the edge out like ImGui's own AA.
template <typename Shade>
void FillShadedEllipse(ImDrawList* draw_list, ImVec2 center, ImVec2 radii, ImVec2 focus,
                       Shade shade) {
  constexpr int kRings = 16;
  constexpr int kSegments = 48;
  constexpr int kLoops = kRings + 2;  // focus, rings, fringe
  draw_list->PrimReserve((kLoops - 1) * kSegments * 6, kLoops * kSegments);
  const ImDrawIdx base = ImDrawIdx(draw_list->_VtxCurrentIdx);
  for (int loop = 0; loop + 1 < kLoops; ++loop) {
    for (int s = 0; s < kSegments; ++s) {
      const int n = (s + 1) % kSegments;
      const ImDrawIdx a = ImDrawIdx(base + loop * kSegments + s);
      const ImDrawIdx b = ImDrawIdx(base + loop * kSegments + n);
      const ImDrawIdx c = ImDrawIdx(base + (loop + 1) * kSegments + n);
      const ImDrawIdx d = ImDrawIdx(base + (loop + 1) * kSegments + s);
      draw_list->PrimWriteIdx(a);
      draw_list->PrimWriteIdx(b);
      draw_list->PrimWriteIdx(c);
      draw_list->PrimWriteIdx(a);
      draw_list->PrimWriteIdx(c);
      draw_list->PrimWriteIdx(d);
    }
  }
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  for (int loop = 0; loop < kLoops; ++loop) {
    const bool fringe = loop == kLoops - 1;
    const float t = fringe ? 1.0f : float(loop) / float(kRings);
    for (int s = 0; s < kSegments; ++s) {
      const float angle = 2.0f * kPi * float(s) / float(kSegments);
      const ImVec2 dir(std::cos(angle), std::sin(angle));
      const float grow = fringe ? 0.5f : (loop == kRings ? -0.5f : 0.0f);
      const ImVec2 ring_center(focus.x + (center.x - focus.x) * t,
                               focus.y + (center.y - focus.y) * t);
      const ImVec2 pos(ring_center.x + dir.x * (radii.x * t + grow),
                       ring_center.y + dir.y * (radii.y * t + grow));
      ImVec4 color = shade(t, dir);
      if (fringe) {
        color.w = 0.0f;
      }
      draw_list->PrimWriteVtx(pos, uv, ImGui::GetColorU32(color));
    }
  }
}

void DrawFaceButton(ImDrawList* draw_list, const FaceStyle& style, float cx, float cy, float radius,
                    float alpha, bool pressed) {
  const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 black(0.0f, 0.0f, 0.0f, 1.0f);
  const ImVec4 highlight = LerpColor(style.color, white, 0.40f);
  const ImVec4 shadow = LerpColor(style.color, black, 0.40f);
  const ImVec4 glow = LerpColor(style.color, white, 0.25f);
  const float face_alpha = std::min(alpha * (pressed ? 2.6f : 2.0f), 1.0f);
  // The art sits just inside the control's radius; a held button sinks a little.
  const float face_radius = radius * (pressed ? 0.94f : 0.98f);
  const float u = face_radius / 60.0f;  // one pixel of the 128 px reference art
  const ImVec2 center(cx, cy + (pressed ? radius * 0.025f : 0.0f));

  // Lit from above, darkening toward the rim, with light passing through the
  // plastic and catching the lower edge.
  FillShadedEllipse(
      draw_list, center, ImVec2(face_radius, face_radius),
      ImVec2(center.x, center.y - face_radius * 0.35f), [&](float t, ImVec2 dir) {
        ImVec4 color = LerpColor(highlight, style.color, SmoothStep(0.0f, 0.6f, t));
        color = LerpColor(color, shadow, SmoothStep(0.55f, 1.0f, t));
        const float lower = std::max(dir.y, 0.0f);
        color = LerpColor(color, glow, 0.7f * lower * lower * SmoothStep(0.7f, 0.95f, t));
        color.w = face_alpha;
        return color;
      });

  // Soft edged sheen fading out toward the middle of the dome.
  const float gloss_alpha = std::min(alpha * (pressed ? 0.45f : 1.1f), 0.75f);
  const ImVec2 gloss_center(center.x, center.y - face_radius * 0.42f);
  const ImVec2 gloss_radii(face_radius * 0.62f, face_radius * 0.40f);
  FillShadedEllipse(draw_list, gloss_center, gloss_radii, gloss_center,
                    [&](float t, ImVec2 dir) {
                      const float y = 0.5f - 0.5f * dir.y * t;
                      ImVec4 color = white;
                      color.w = gloss_alpha * (0.15f + 0.85f * y * y) *
                                (1.0f - SmoothStep(0.55f, 1.0f, t));
                      return color;
                    });
  // A thin highlight inside a thick black rim.
  draw_list->AddCircle(center, face_radius - 3.5f * u, ColorWithAlpha(white, face_alpha * 0.45f),
                       64, 1.5f * u);
  draw_list->AddCircle(center, face_radius - 1.2f * u, ColorWithAlpha(black, face_alpha), 64,
                       5.0f * u);
  if (pressed) {
    draw_list->AddCircle(center, face_radius + 2.0f, ColorWithAlpha(white, alpha * 0.85f), 48,
                         2.0f);
  }

  // Cream letter in a dark outline of the button's own shade, over a drop shadow.
  ImFont* font = FaceButtonFont();
  const float font_size = std::min(std::round(face_radius * 1.2f), font->LegacySize);
  const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, style.label);
  const ImVec2 text_pos(std::round(center.x - text_size.x * 0.5f),
                        std::round(center.y - text_size.y * 0.5f));
  const float letter_alpha = std::min(alpha * 2.1f, 1.0f);
  draw_list->AddText(font, font_size, ImVec2(text_pos.x, text_pos.y + u),
                     ColorWithAlpha(black, letter_alpha * 0.42f), style.label);
  const ImU32 outline = ColorWithAlpha(LerpColor(black, style.color, 0.45f), letter_alpha);
  constexpr int kOutlineSteps = 16;
  for (int i = 0; i < kOutlineSteps; ++i) {
    const float angle = 2.0f * kPi * float(i) / float(kOutlineSteps);
    draw_list->AddText(font, font_size,
                       ImVec2(text_pos.x + std::cos(angle) * 2.5f * u,
                              text_pos.y + std::sin(angle) * 2.5f * u),
                       outline, style.label);
  }
  // Drawn at a half pixel spread to thicken the lighter face into a bold weight.
  const ImU32 letter = ColorWithAlpha(ImVec4(0.98f, 0.957f, 0.886f, 1.0f), letter_alpha);
  for (float dx : {-0.5f * u, 0.0f, 0.5f * u}) {
    draw_list->AddText(font, font_size, ImVec2(text_pos.x + dx, text_pos.y), letter, style.label);
  }
}

void DrawFaceGlyph(ImDrawList* draw_list, FaceGlyph glyph, ImVec2 c, float size, ImU32 color,
                   float thickness) {
  switch (glyph) {
    case FaceGlyph::kCross: {
      const float d = size * 0.62f;
      draw_list->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), color, thickness);
      draw_list->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), color, thickness);
      break;
    }
    case FaceGlyph::kCircle:
      draw_list->AddCircle(c, size * 0.72f, color, 32, thickness);
      break;
    case FaceGlyph::kSquare: {
      const float h = size * 0.60f;
      draw_list->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), color, 0.0f, 0,
                         thickness);
      break;
    }
    case FaceGlyph::kTriangle: {
      // Dropped a little so its centroid reads as centred.
      const float r = size * 0.82f;
      const float y = c.y + size * 0.06f;
      draw_list->AddTriangle(ImVec2(c.x, y - r), ImVec2(c.x + r * 0.866f, y + r * 0.5f),
                             ImVec2(c.x - r * 0.866f, y + r * 0.5f), color, thickness);
      break;
    }
  }
}

// Matte black dish with a coloured symbol, lit from above: the bezel catches
// the light on top while the hollow inside brightens toward the bottom.
void DrawPlayStationButton(ImDrawList* draw_list, const FaceStyle& style, float cx, float cy,
                           float radius, float alpha, bool pressed) {
  const ImVec4 white(1.0f, 1.0f, 1.0f, 1.0f);
  const ImVec4 black(0.0f, 0.0f, 0.0f, 1.0f);
  const ImVec4 bezel_lit(0.30f, 0.30f, 0.33f, 1.0f);
  const ImVec4 bezel_dark(0.11f, 0.11f, 0.12f, 1.0f);
  const ImVec4 well_top(0.04f, 0.04f, 0.05f, 1.0f);
  const ImVec4 well_bottom(0.13f, 0.13f, 0.15f, 1.0f);
  const float face_alpha = std::min(alpha * (pressed ? 2.6f : 2.0f), 1.0f);
  const float face_radius = radius * (pressed ? 0.94f : 0.98f);
  const ImVec2 center(cx, cy + (pressed ? radius * 0.025f : 0.0f));

  FillShadedEllipse(draw_list, center, ImVec2(face_radius, face_radius), center,
                    [&](float t, ImVec2 dir) {
                      const float y = 0.5f + 0.5f * dir.y * t;
                      const ImVec4 well = LerpColor(well_top, well_bottom, y);
                      const ImVec4 bezel = LerpColor(bezel_lit, bezel_dark, 0.5f + 0.5f * dir.y);
                      ImVec4 color = LerpColor(well, bezel, SmoothStep(0.74f, 0.9f, t));
                      color.w = face_alpha;
                      return color;
                    });
  draw_list->AddCircle(center, face_radius, ColorWithAlpha(black, face_alpha * 0.8f), 48, 2.0f);
  draw_list->AddCircle(center, face_radius + 1.5f, ColorWithAlpha(white, alpha * 0.30f), 48,
                       1.0f);

  const float glyph_size = face_radius * 0.66f;
  const float thickness = std::max(2.0f, face_radius * (pressed ? 0.19f : 0.17f));
  if (pressed) {
    draw_list->AddCircleFilled(center, face_radius * 0.82f,
                               ColorWithAlpha(style.color, alpha * 0.35f), 48);
    draw_list->AddCircle(center, face_radius + 2.0f, ColorWithAlpha(style.color, alpha * 1.6f), 48,
                         2.0f);
  }
  DrawFaceGlyph(draw_list, style.glyph, ImVec2(center.x, center.y + 1.5f), glyph_size,
                ColorWithAlpha(black, alpha * 0.6f), thickness);
  const ImVec4 glyph_color = pressed ? LerpColor(style.color, white, 0.35f) : style.color;
  DrawFaceGlyph(draw_list, style.glyph, center, glyph_size,
                ColorWithAlpha(glyph_color, std::min(alpha * 2.4f, 1.0f)), thickness);
}

// One arrowhead of a D-pad, pointing away from the centre along (dx, dy).
void DrawDpadArrow(ImDrawList* draw_list, float cx, float cy, float radius, float dx, float dy,
                   float alpha, bool lit) {
  const float tip = radius * 0.72f;
  const float base = radius * 0.36f;
  const float half = radius * 0.20f;
  // Perpendicular to the direction, for the two base corners.
  const float px = -dy;
  const float py = dx;
  const ImVec2 a(cx + dx * tip, cy + dy * tip);
  const ImVec2 b(cx + dx * base + px * half, cy + dy * base + py * half);
  const ImVec2 c(cx + dx * base - px * half, cy + dy * base - py * half);
  draw_list->AddTriangleFilled(a, b, c, EdgeColor(alpha, lit));
}

}  // namespace

TouchControlsOverlay::TouchControlsOverlay(rex::ui::ImGuiDrawer* drawer, TouchInputDriver* driver,
                                           InputSystem* input_system)
    : rex::ui::ImGuiDialog(drawer), driver_(driver), input_system_(input_system) {}

void TouchControlsOverlay::OnDraw(ImGuiIO& io) {
  if (!driver_ || !input_system_) {
    return;
  }
  bool assigned = false;
  for (const auto& view : input_system_->SnapshotDevices()) {
    if (view.device.kind == DeviceKind::kTouch && view.guest_user_mask != 0) {
      assigned = true;
      break;
    }
  }
  if (!assigned) {
    return;
  }
  TouchVisualState state;
  if (!driver_->GetVisualState(&state)) {
    return;
  }

  const float width = io.DisplaySize.x;
  const float height = io.DisplaySize.y;
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }

  const float alpha = float(std::clamp(REXCVAR_GET(touch_opacity), 0.0, 1.0)) * state.fade;
  if (alpha <= 0.0f) {
    return;
  }

  // The driver hit-tested against the window in pixels; ImGui draws in logical
  // points. Rebuilding the layout here rather than scaling the driver's copy
  // keeps both sides reading the same source of truth for the geometry.
  const TouchLayout layout = driver_->BuildLayout(width, height);

  // Foreground list so the pad sits above the game and above any overlay
  // window, drawn without a host window of its own so it never takes input:
  // touches belong to the driver, which sees them straight from the window.
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  for (size_t i = 0; i < layout.controls.size(); ++i) {
    const TouchControl& c = layout.controls[i];
    const bool pressed = (state.pressed_mask & (uint64_t(1) << i)) != 0;
    const uint16_t held = i < state.control_buttons.size() ? state.control_buttons[i] : 0;

    switch (c.shape) {
      case TouchControl::Shape::kCircle: {
        FaceStyle face_style;
        if (GetFaceStyle(c.button, layout.face_style, &face_style)) {
          if (layout.face_style == TouchFaceStyle::kPlayStation) {
            DrawPlayStationButton(draw_list, face_style, c.cx, c.cy, c.half_width, alpha, pressed);
          } else {
            DrawFaceButton(draw_list, face_style, c.cx, c.cy, c.half_width, alpha, pressed);
          }
        } else {
          draw_list->AddCircleFilled(ImVec2(c.cx, c.cy), c.half_width, FillColor(alpha, pressed),
                                     32);
          draw_list->AddCircle(ImVec2(c.cx, c.cy), c.half_width, EdgeColor(alpha, pressed), 32,
                               2.0f);
          if (c.fast_forward) {
            DrawFastForwardIcon(draw_list, c.cx, c.cy, c.half_width, alpha, pressed);
          }
        }
        break;
      }

      case TouchControl::Shape::kPill: {
        DrawPillIcon(draw_list, c, layout.face_style == TouchFaceStyle::kPlayStation, alpha,
                     pressed);
        break;
      }

      case TouchControl::Shape::kSquare: {
        const ImVec2 lo(c.cx - c.half_width, c.cy - c.half_width);
        const ImVec2 hi(c.cx + c.half_width, c.cy + c.half_width);
        draw_list->AddRectFilled(lo, hi, FillColor(alpha, pressed), c.half_width * 0.2f);
        draw_list->AddRect(lo, hi, EdgeColor(alpha, pressed), c.half_width * 0.2f, 0, 2.0f);
        ImFont* font = ImGui::GetFont();
        const float size = c.half_width * 0.8f;
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, c.label.c_str());
        draw_list->AddText(font, size, ImVec2(c.cx - ts.x * 0.5f, c.cy - ts.y * 0.5f),
                           EdgeColor(alpha, pressed), c.label.c_str());
        break;
      }

      case TouchControl::Shape::kDpad:
        draw_list->AddCircleFilled(ImVec2(c.cx, c.cy), c.half_width, FillColor(alpha, false), 32);
        draw_list->AddCircle(ImVec2(c.cx, c.cy), c.half_width, EdgeColor(alpha, pressed), 32, 2.0f);
        // Each arrow lights on its own, so a diagonal lights two of them and
        // the drawing matches what the guest is being told.
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 0.0f, -1.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_UP) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 0.0f, 1.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_DOWN) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, -1.0f, 0.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_LEFT) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 1.0f, 0.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_RIGHT) != 0);
        break;
    }
  }

  for (size_t i = 0; i < layout.sticks.size() && i < state.sticks.size(); ++i) {
    const TouchStick& stick = layout.sticks[i];
    const TouchStickVisual& visual = state.sticks[i];
    // A free-placed stick has no resting position, so it only exists on screen
    // while a finger is planting it. A pinned one is always drawn.
    if (!visual.active && !stick.fixed) {
      continue;
    }
    const ImVec2 centre(visual.cx * width, visual.cy * height);
    const ImVec2 knob(visual.knob_cx * width, visual.knob_cy * height);
    draw_list->AddCircleFilled(centre, stick.radius, FillColor(alpha, false), 48);
    draw_list->AddCircle(centre, stick.radius, EdgeColor(alpha, false), 48, 2.0f);
    draw_list->AddCircleFilled(knob, stick.radius * 0.42f, FillColor(alpha, visual.active), 32);
    draw_list->AddCircle(knob, stick.radius * 0.42f, EdgeColor(alpha, visual.active), 32, 2.0f);
  }
}

}  // namespace nocturne::touch
