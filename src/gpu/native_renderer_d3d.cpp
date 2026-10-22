// nocturnerecomp - ReXGlue Recompiled Project
//
// The guest hooks. The Xbox 360 D3D9 runtime is statically linked into the
// game, so there are no imports to hook: the functions below are located by
// address (named in nocturnerecomp_d3d9.toml, which carries the evidence for
// each) and replaced with REX_HOOK_RAW. Each hook runs the guest's own code
// first (via __imp__<name>) so the device object keeps its shadows of every
// register the renderer reads back, then observes the call.
//
// Nothing here consumes the command buffer the guest writes packets into: the
// draws are replayed from the device's shadows (native_renderer_draw.cpp). The
// waits that assume a GPU is draining it are the only thing that has to be
// short-circuited.

#include <rex/hook.h>
#include <rex/logging.h>

#include <rex/chrono/clock.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "generated/nocturnerecomp_init.h"

#include "guest_shaders.h"
#include "native_renderer.h"
#include "native_renderer_draw.h"
#include "native_renderer_plume.h"

// The recompiled bodies the hooks below wrap. The strong REX_HOOK_RAW
// definitions override the weak generated aliases, so calling __imp__ runs the
// original.
extern "C" REX_FUNC(__imp__D3DDevice__Swap);

extern "C" REX_FUNC(__imp__D3DDevice__EndVertices);
extern "C" REX_FUNC(__imp__D3DDevice__DrawVertices);
extern "C" REX_FUNC(__imp__D3DDevice__DrawIndexedVertices);
extern "C" REX_FUNC(__imp__D3DDevice__BeginVertices);
extern "C" REX_FUNC(__imp__D3DDevice__Resolve);
extern "C" REX_FUNC(__imp__D3DDevice__ClearF);
extern "C" REX_FUNC(__imp__D3DDevice__SetRenderTarget);
extern "C" REX_FUNC(__imp__D3DDevice__SetTexture);
extern "C" REX_FUNC(__imp__D3DDevice__SetVertexShader);
extern "C" REX_FUNC(__imp__D3DDevice__SetPixelShader);

namespace {
// Per frame call counts, logged at Swap while the call pattern is being
// learned.
struct FrameStats {
  std::atomic<uint32_t> draws{0}, indexed{0}, begin_vertices{0}, end_vertices{0}, resolves{0}, render_targets{0},
      textures{0}, vertex_shaders{0}, pixel_shaders{0};
  uint32_t frame = 0;
} g_stats;
}  // namespace
void FlushInline(uint8_t* base);

REX_HOOK_RAW(D3DDevice__DrawVertices) {
  FlushInline(base);
  REXLOG_TRACE("DrawVertices prim={} start={} count={}", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  ++g_stats.draws;
  __imp__D3DDevice__DrawVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice__DrawIndexedVertices) {
  FlushInline(base);
  REXLOG_TRACE("DrawIndexed prim={} base={} start={} count={}", ctx.r4.u32, ctx.r5.u32,
               ctx.r6.u32, ctx.r7.u32);
  ++g_stats.indexed;
  __imp__D3DDevice__DrawIndexedVertices(ctx, base);
}

namespace {
struct InlineBlock {
  uint32_t device = 0, prim = 0, count = 0, stride = 0, data = 0;
} g_inline;

uint32_t U32(uint8_t* base, uint32_t address) { return REX_LOAD_U32(address); }
float F32(uint8_t* base, uint32_t address) {
  uint32_t v = REX_LOAD_U32(address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint64_t Fnv(uint8_t* base, uint32_t address, uint32_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < size; ++i) {
    h ^= *(base + address + i);
    h *= 0x100000001B3ull;
  }
  return h;
}

// Shader object -> microcode hash. The container header is copied into the
// object at +872 (vertex) / +40 (pixel), microcode right after it.
void DescribeShader(uint8_t* base, const char* kind, uint32_t object, uint32_t header_offset) {
  if (!object) {
    REXLOG_INFO("  {} shader: none", kind);
    return;
  }
  const uint32_t header = object + header_offset;
  const uint32_t header_size = U32(base, header + 4);
  const uint32_t ucode_size = U32(base, header + 8);
  // The microcode lives behind a pointer in the object: vertex at +0x20,
  // pixel at +0x18 (the arguments the shader flush hands its emit helper).
  uint32_t ptr = U32(base, object + (header_offset == 872 ? 0x20 : 0x18));
  const uint32_t ucode = ptr >= 0x80000000u ? ptr : (0xC0000000u | (ptr & 0x1FFFFFFFu));
  {
    static const uint8_t vs_pat[8] = {0xf0, 0x55, 0x40, 0x03, 0x00, 0x00, 0x12, 0x00};
    for (uint32_t o = 0; o < 8192; o += 4)
      if (!std::memcmp(base + ucode + o, vs_pat, 8))
        REXLOG_INFO("  {} file ucode found at ptr+{}", kind, o);
  }
  REXLOG_INFO("  {} shader obj={:08X} magic={:08X} hdr={} ucode={} ptr={:08X} fnv(ucode)={:016X}",
              kind, object, U32(base, header), header_size, ucode_size, ptr,
              Fnv(base, ucode, ucode_size));
}

void TraceDraw(uint8_t* base, const InlineBlock& b) {
  const uint32_t dev = b.device;
  REXLOG_INFO("draw prim={} count={} stride={} data={:08X} vs slot={} ps slot={}", b.prim, b.count,
              b.stride, b.data, nocturne::FindGuestVertexShader(base, U32(base, dev + 0x3080)),
              nocturne::FindGuestPixelShader(base, U32(base, dev + 0x307c)));
  DescribeShader(base, "vs", U32(base, dev + 0x3080), 872);
  DescribeShader(base, "ps", U32(base, dev + 0x307c), 40);
  const uint32_t decl = U32(base, dev + 0x2d14);
  if (decl) {
    const uint32_t n = U32(base, decl + 24);
    REXLOG_INFO("  decl={:08X} elements={}", decl, n);
    for (uint32_t i = 0; i < n && i < 16; ++i) {
      const uint32_t e = decl + 52 + 12 * i;
      REXLOG_INFO("    [{}] stream/offset={:08X} type={:08X} usage={} idx={}", i, U32(base, e),
                  U32(base, e + 4), *(base + e + 9), *(base + e + 10));
    }
  }
  for (uint32_t s = 0; s < 4; ++s) {
    if (!U32(base, dev + 4 * (s + 0xbfa)))
      continue;
    const uint32_t f = dev + 0x400 + 24 * s;
    REXLOG_INFO("  tex{} tex={:08X} fetch={:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", s,
                U32(base, dev + 4 * (s + 0xbfa)), U32(base, f), U32(base, f + 4), U32(base, f + 8),
                U32(base, f + 12), U32(base, f + 16), U32(base, f + 20));
  }
  REXLOG_INFO("  regs depth={:08X} blend={:08X} colorctl={:08X} mode={:08X} cmask={:08X} aref={:08X}",
              U32(base, dev + 10420), U32(base, dev + 10424), U32(base, dev + 10428),
              U32(base, dev + 10440), U32(base, dev + 10332), U32(base, dev + 10372));
  for (uint32_t c = 0; c < 6; ++c) {
    const uint32_t a = dev + 1792 + 16 * c;
    REXLOG_INFO("  vc{} = {} {} {} {}", c, F32(base, a), F32(base, a + 4), F32(base, a + 8),
                F32(base, a + 12));
  }
  for (uint32_t c = 0; c < 3; ++c) {
    const uint32_t a = dev + 5888 + 16 * c;
    REXLOG_INFO("  pc{} = {} {} {} {}", c, F32(base, a), F32(base, a + 4), F32(base, a + 8),
                F32(base, a + 12));
  }
  for (uint32_t v = 0; v < 2 && v < b.count; ++v) {
    std::string line;
    for (uint32_t w = 0; w < b.stride / 4 && w < 12; ++w)
      line += fmt::format(" {}", F32(base, b.data + v * b.stride + 4 * w));
    REXLOG_INFO("  v{}:{}", v, line);
  }
}
}  // namespace

bool g_inline_pending = false;

// The game never calls EndVertices (the draw packet is emitted by BeginVertices
// itself), so a block is consumed at the next hooked D3D entry, by which time
// the guest has finished filling it.
void FlushInline(uint8_t* base) {
  if (!g_inline_pending)
    return;
  g_inline_pending = false;
  {
    const uint32_t vs = U32(base, g_inline.device + 0x3080), ps = U32(base, g_inline.device + 0x307c);
    static uint32_t last_vs = 0, last_ps = 0;
    if ((nocturne::FindGuestVertexShader(base, vs) < 0 || nocturne::FindGuestPixelShader(base, ps) < 0) &&
        (vs != last_vs || ps != last_ps)) {
      last_vs = vs;
      last_ps = ps;
      REXLOG_INFO("draw with a shader outside the pack at frame {}:", g_stats.frame);
      TraceDraw(base, g_inline);
    }
  }
  if (g_stats.frame >= 300 && g_stats.frame < 302)
    TraceDraw(base, g_inline);
  nocturne::GuestDrawInline(base, {g_inline.device, g_inline.prim, g_inline.count, g_inline.stride,
                                   g_inline.data});
}

REX_HOOK_RAW(D3DDevice__BeginVertices) {
  FlushInline(base);
  ++g_stats.begin_vertices;
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__D3DDevice__BeginVertices(ctx, base);
  g_inline = {dev, prim, count, stride, ctx.r3.u32};
  g_inline_pending = true;
}

REX_HOOK_RAW(D3DDevice__EndVertices) {
  ++g_stats.end_vertices;
  __imp__D3DDevice__EndVertices(ctx, base);
  FlushInline(base);
}

REX_HOOK_RAW(D3DDevice__Resolve) {
  FlushInline(base);
  ++g_stats.resolves;
  if (g_stats.frame >= 300 && g_stats.frame < 302) {
    const uint32_t dev = ctx.r3.u32;
    const uint32_t rt = U32(base, dev + 0x2f80);
    const uint32_t cc = ctx.r10.u32;
    REXLOG_INFO("resolve flags={:X} src={:08X} dest={:08X} point={:08X} r8={:X} r9={:X} rt0={:08X} "
                "rt0 size={:08X} pitch={:08X} clear={:08X} ({} {} {} {})",
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, rt,
                rt ? U32(base, rt + 36) : 0, rt ? U32(base, rt + 24) : 0, cc,
                cc ? F32(base, cc) : 0.0f, cc ? F32(base, cc + 4) : 0.0f,
                cc ? F32(base, cc + 8) : 0.0f, cc ? F32(base, cc + 12) : 0.0f);
  }
  // (device, flags, source rect, destination texture, destination point,
  // level, slice, clear colour, ...).
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, destination = ctx.r6.u32,
                 clear_color = ctx.r10.u32;
  __imp__D3DDevice__Resolve(ctx, base);
  nocturne::GuestResolve(base, dev, flags, destination, clear_color);
}

// Clear reaches this once per rect (or once with none), so it covers both.
REX_HOOK_RAW(D3DDevice__ClearF) {
  FlushInline(base);
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, color = ctx.r6.u32;
  __imp__D3DDevice__ClearF(ctx, base);
  nocturne::GuestClear(base, dev, flags, rect, color);
}

REX_HOOK_RAW(D3DDevice__SetRenderTarget) {
  FlushInline(base);
  REXLOG_TRACE("SetRenderTarget index={} surface={:08X}", ctx.r4.u32, ctx.r5.u32);
  ++g_stats.render_targets;
  __imp__D3DDevice__SetRenderTarget(ctx, base);
}

REX_HOOK_RAW(D3DDevice__SetTexture) {
  FlushInline(base);
  ++g_stats.textures;
  __imp__D3DDevice__SetTexture(ctx, base);
}

REX_HOOK_RAW(D3DDevice__SetVertexShader) {
  FlushInline(base);
  ++g_stats.vertex_shaders;
  const uint32_t shader = ctx.r4.u32;
  __imp__D3DDevice__SetVertexShader(ctx, base);
  nocturne::FindGuestVertexShader(base, shader);
}

REX_HOOK_RAW(D3DDevice__SetPixelShader) {
  FlushInline(base);
  ++g_stats.pixel_shaders;
  const uint32_t shader = ctx.r4.u32;
  __imp__D3DDevice__SetPixelShader(ctx, base);
  nocturne::FindGuestPixelShader(base, shader);
}

// The game's simulation is tied to the frame rate, and with no GPU vblank to
// pace Swap nothing stops it from running at the display rate. Hold the guest
// thread to 60 frames per second (scaled by the fast-forward time scalar).
static void LimitFrameRate() {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  double scalar = rex::chrono::Clock::guest_time_scalar();
  if (!(scalar > 0.0)) scalar = 1.0;
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / (60.0 * scalar)));
  const auto now = Clock::now();
  if (next == Clock::time_point{} || now - next > period * 4) {
    next = now;  // first frame, or a hitch: don't try to catch up
  }
  next += period;
  if (next > now) std::this_thread::sleep_until(next);
}

// The guest's final frame boundary: (device, ...). Present the host frame once
// the guest's own swap has run.
REX_HOOK_RAW(D3DDevice__Swap) {
  FlushInline(base);
  __imp__D3DDevice__Swap(ctx, base);

  if (++g_stats.frame % 120 == 0) {
    REXLOG_INFO(
        "d3d: frame {} draws={} indexed={} immediate={} end={} resolves={} rt={} tex={} vs={} ps={}",
        g_stats.frame, g_stats.draws.exchange(0), g_stats.indexed.exchange(0),
        g_stats.begin_vertices.exchange(0), g_stats.end_vertices.exchange(0), g_stats.resolves.exchange(0),
        g_stats.render_targets.exchange(0), g_stats.textures.exchange(0),
        g_stats.vertex_shaders.exchange(0), g_stats.pixel_shaders.exchange(0));
  }
  nocturne::PlumePresentFrame();
  nocturne::NotifyGuestFrame();
  LimitFrameRate();
}

// Swap brackets its frame throttle (spin while the submitted counter runs more
// than 15 frames ahead of the retired one) with Begin/Poll/End. Poll returns 1
// to keep spinning and, once a 5000 timebase tick window expires, calls the GPU
// hang recovery. Returning 0 makes every such loop exit normally. One hook
// covers every waiting caller, and keeps the hang path unreachable, which it
// otherwise would not be, since with no GPU every wait exceeds the window.
REX_HOOK_RAW(D3DDevice__ThrottleWait_Poll) {
  (void)base;
  ctx.r3.u64 = 0;
}

// The guest emits a wait-for-idle packet, submits it, and busy waits until the
// GPU clears it; nothing does, so this returns success immediately. Reached
// from the renderer init through SetRingBufferParameters, which drains the
// previous ring before replacing it. SetRingBufferParameters itself is not
// stubbed: running it keeps the ring memory allocated and the command buffer
// pointers valid, so the guest's packet writers write somewhere harmless.
REX_HOOK_RAW(D3DDevice__BlockUntilGpuIdle) {
  (void)base;
  ctx.r3.u64 = 0;
}
