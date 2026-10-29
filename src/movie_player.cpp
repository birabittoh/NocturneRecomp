#include "generated/nocturnerecomp_init.h"

#include "movie_player.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/audio/asf_movie_decoder.h>
#include <rex/audio/raw_pcm_player.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "target.h"

// ---------------------------------------------------------------------------
// The JP release's story movies on World's code.
//
// JP plays three movies from DATA/video that World cut: OPN_WS before the
// Richter prologue, and TYK_WS / END_WS, the castle collapsing at the end
// (JP's video table at 0x8309BBC4: LOGO15XA, OPN_WS, TYK_WS, END_WS; there is
// no LOGO15XA in the package). JP's game code starts its XMV player from
// state machines World still has:
//
//  - Title mode 5 (JP sub_822B0EA0): new game runs it on the way to the
//    prologue. JP fades a picture in and out, plays OPN_WS, then calls
//    sub_822AE128; World's mode 5 (dispatcher sub_822521C0) calls the same
//    step, sub_8224ED70, straight away.
//  - The ending (JP sub_823F0078, World sub_82391F48): state 0 picks movie 3
//    (END_WS) when dword_83164F14 is set, else 2 (TYK_WS), in
//    dword_831751A0; state 1 plays it. World's state 1 still calls the PSX
//    STR player (sub_822ED3E0), which finds no stream and gives up.
//
// JP draws the movie over a game that keeps running, with the game's sound
// suspended. Here the hooks hold their state machine on the playing step
// instead, so nothing advances underneath, and the movie is decoded and drawn
// on the host (rex::audio::AsfMovieDecoder, MovieOverlay). Like JP's, the
// picture fills the PS1 box: JP builds its XMV quad from the four corners of
// the Change Screen Size rect (JP dword_82B32C90, sub_825E01E0), the rect the
// game's layers are drawn in, so it follows the player's stretch and the
// border outside the box stays on screen. Start skips it, as in JP. A folder without the movie files (World) runs the vanilla code.
//
// Hooks wired up in config/nocturnerecomp_movie_hooks.toml.
// ---------------------------------------------------------------------------

REXCVAR_DECLARE(bool, skip_richter);  // intro_skips.cpp

extern "C" REX_FUNC(sub_82234BF0);  // SOTNSoundSuspend
extern "C" REX_FUNC(sub_82234CC8);  // SOTNSoundUnsuspend

namespace nocturne {
namespace {

constexpr uint32_t kMovieIndex = 0x831751A0;   // dword_831751A0
constexpr uint32_t kStrPlayerOn = 0x8318EE20;  // dword_8318EE20
constexpr size_t kFramesAhead = 8;

// The Change Screen Size rect (LTRB, guest display pixels) and the full display
// it stretches towards; see graphics_settings.cpp.
constexpr uint32_t kStretchRect = 0x82882C68;
constexpr uint32_t kStretchRectMax = 0x82882C98;

uint32_t ReadU32(uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(
      REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address));
}

void WriteU32(uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(
      REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address), value);
}

// Calls a recompiled guest function with an isolated context borrowed from the
// current thread (see tu_hooks.cpp's CallGuest). Midasm hooks get no context.
void CallGuest(void (*fn)(PPCContext&, uint8_t*)) {
  auto* parent = rex::runtime::ThreadState::Get()->context();
  PPCContext ctx{};
  ctx.r1 = parent->r1;
  ctx.r1.u32 -= 0x70;
  ctx.r13 = parent->r13;
  ctx.fpscr = parent->fpscr;
  fn(ctx, REX_KERNEL_STATE()->memory()->virtual_membase());
  parent->fpscr = ctx.fpscr;
}

struct Frame {
  std::vector<uint8_t> rgba;
  double pts = 0.0;
};

// One movie at a time: a worker decodes ahead, the overlay shows the frame
// due, the guest hooks start it and wait for it.
class Movie {
 public:
  enum class Phase { kIdle, kPlaying, kDone };

  ~Movie() { Finish(); }

  void Start(std::filesystem::path path) {
    Finish();
    stop_ = false;
    {
      std::lock_guard lock(mutex_);
      frames_.clear();
      decode_done_ = false;
      started_ = false;
    }
    phase_ = Phase::kPlaying;
    worker_ = std::thread([this, path = std::move(path)] { Decode(path); });
  }

  // Ends playback early.
  void Skip() {
    stop_ = true;
    space_.notify_all();
    audio_.Stop();
    phase_ = Phase::kDone;
  }

  // Back to idle once the hook has seen kDone.
  void Finish() {
    stop_ = true;
    space_.notify_all();
    if (worker_.joinable())
      worker_.join();
    audio_.Stop();
    std::lock_guard lock(mutex_);
    frames_.clear();
    phase_ = Phase::kIdle;
  }

  Phase phase() {
    Update();
    return phase_;
  }

  // The frame due now, if it changed since the last call. Overlay thread.
  bool TakeFrame(Frame& out, uint32_t& width, uint32_t& height) {
    Update();
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kPlaying || !started_)
      return false;
    const double now = Elapsed();
    bool taken = false;
    while (!frames_.empty() && frames_.front().pts <= now) {
      out = std::move(frames_.front());
      frames_.pop_front();
      taken = true;
    }
    if (taken)
      space_.notify_all();
    width = width_;
    height = height_;
    return taken;
  }

  bool showing() const { return phase_ == Phase::kPlaying; }

 private:
  void Decode(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    rex::audio::AsfMovieDecoder decoder;
    if (bytes.empty() || !decoder.Open(std::move(bytes))) {
      REXLOG_ERROR("movies: can't play {}", path.string());
      phase_ = Phase::kDone;
      return;
    }
    std::vector<int16_t> pcm;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    const bool has_audio = decoder.DecodeAudio(pcm, sample_rate, channels);
    {
      std::lock_guard lock(mutex_);
      width_ = decoder.width();
      height_ = decoder.height();
      duration_ = decoder.duration();
    }
    if (stop_)
      return;
    if (has_audio)
      audio_.PlayInt16(std::move(pcm), sample_rate, channels);
    {
      std::lock_guard lock(mutex_);
      start_ = std::chrono::steady_clock::now();
      started_ = true;
    }
    REXLOG_INFO("movies: playing {} ({:.1f} s)", path.filename().string(), duration_);

    Frame frame;
    while (!stop_ && decoder.NextVideoFrame(frame.rgba, frame.pts)) {
      std::unique_lock lock(mutex_);
      space_.wait(lock, [&] { return stop_ || frames_.size() < kFramesAhead; });
      if (stop_)
        break;
      frames_.push_back(std::move(frame));
      frame = {};
    }
    std::lock_guard lock(mutex_);
    decode_done_ = true;
  }

  double Elapsed() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

  // Ends playback once every frame is out and the movie's length has passed.
  void Update() {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kPlaying || !started_)
      return;
    if (decode_done_ && frames_.empty() && Elapsed() >= duration_) {
      audio_.Stop();
      phase_ = Phase::kDone;
    }
  }

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable space_;
  std::deque<Frame> frames_;
  std::atomic<bool> stop_{false};
  std::atomic<Phase> phase_{Phase::kIdle};
  bool decode_done_ = false;
  bool started_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  double duration_ = 0.0;
  std::chrono::steady_clock::time_point start_;
  rex::audio::RawPcmPlayer audio_;
};

Movie g_movie;
rex::input::InputSystem* g_input = nullptr;
bool g_start_was_down = true;

bool StartDown() {
  if (!g_input)
    return false;
  rex::input::X_INPUT_STATE state{};
  if (g_input->GetState(0, &state) != 0)
    return false;
  return (uint16_t(state.gamepad.buttons) & rex::input::X_INPUT_GAMEPAD_START) != 0;
}

enum class Hold { kNoMovie, kPlaying, kFinished };

// Starts |name| from JP's DATA/video on the first call and holds the caller
// while it plays. Without JP installed the vanilla code runs.
Hold HoldForMovie(const char* name) {
  switch (g_movie.phase()) {
    case Movie::Phase::kIdle: {
      if (JpDir().empty())
        return Hold::kNoMovie;
      const auto path = JpDir() / "DATA" / "video" / name;
      std::error_code ec;
      if (!std::filesystem::is_regular_file(path, ec))
        return Hold::kNoMovie;
      CallGuest(sub_82234BF0);
      g_start_was_down = true;  // a press that started the game doesn't skip
      g_movie.Start(path);
      return Hold::kPlaying;
    }
    case Movie::Phase::kPlaying: {
      const bool down = StartDown();
      if (down && !g_start_was_down)
        g_movie.Skip();
      g_start_was_down = down;
      return Hold::kPlaying;
    }
    case Movie::Phase::kDone:
      g_movie.Finish();
      CallGuest(sub_82234CC8);
      return Hold::kFinished;
  }
  return Hold::kNoMovie;
}

class MovieOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit MovieOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_movie.showing()) {
      textures_.clear();
      return;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    if (g_movie.TakeFrame(frame_, width, height) && imgui_drawer()->immediate_drawer()) {
      auto texture = imgui_drawer()->immediate_drawer()->CreateTexture(
          width, height, rex::ui::ImmediateTextureFilter::kLinear, false, frame_.rgba.data());
      if (texture) {
        // Earlier frames may still be in flight on the GPU; keep a few.
        textures_.push_back(std::move(texture));
        while (textures_.size() > 4)
          textures_.pop_front();
      }
    }

    ImVec2 min;
    ImVec2 max;
    if (!BoxOnWindow(io.DisplaySize, min, max))
      return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (textures_.empty()) {
      draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, 255));
      return;
    }
    draw->AddImage(reinterpret_cast<ImTextureID>(textures_.back().get()), min, max);
  }

  // The PS1 box in window pixels. The presenter letterboxes the guest display
  // into the window at its own aspect ratio, so the box is mapped through the
  // same fit.
  static bool BoxOnWindow(const ImVec2& window, ImVec2& min, ImVec2& max) {
    const auto rect = [](uint32_t base, int i) {
      return float(int32_t(ReadU32(base + 4 * i)));
    };
    const float display_x = rect(kStretchRectMax, 0);
    const float display_y = rect(kStretchRectMax, 1);
    const float display_w = rect(kStretchRectMax, 2) - display_x;
    const float display_h = rect(kStretchRectMax, 3) - display_y;
    if (display_w <= 0 || display_h <= 0)
      return false;
    const float scale = std::min(window.x / display_w, window.y / display_h);
    const float x0 = (window.x - display_w * scale) * 0.5f;
    const float y0 = (window.y - display_h * scale) * 0.5f;
    min = ImVec2(x0 + (rect(kStretchRect, 0) - display_x) * scale,
                 y0 + (rect(kStretchRect, 1) - display_y) * scale);
    max = ImVec2(x0 + (rect(kStretchRect, 2) - display_x) * scale,
                 y0 + (rect(kStretchRect, 3) - display_y) * scale);
    return max.x > min.x && max.y > min.y;
  }

 private:
  Frame frame_;
  std::deque<std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
};

}  // namespace

void BindMovies(rex::Runtime* runtime) {
  g_input = static_cast<rex::input::InputSystem*>(runtime->input_system());
}

std::unique_ptr<rex::ui::ImGuiDialog> CreateMovieOverlay(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<MovieOverlay>(drawer);
}

}  // namespace nocturne

// Title dispatcher sub_822521C0, mode 5's `b sub_8224ED70`: OPN_WS first.
// True holds mode 5 (returns from the dispatcher, 0x822523A0). The opening
// introduces the Richter prologue, so skip_richter skips it too.
bool MovieOpeningHook() {
  if (REXCVAR_GET(skip_richter))
    return false;
  return nocturne::HoldForMovie("OPN_WS.wmv") == nocturne::Hold::kPlaying;
}

// sub_82391F48 state 1, before `bl sub_822ED3E0` (the PSX STR player): the
// movie state 0 picked. True holds state 1 (to the epilogue, 0x82392304).
// When it ends, the STR player is switched off (it returns at once when
// dword_8318EE20 is 0) and the rest of state 1 runs as after a played movie.
bool MovieEndingHook() {
  using nocturne::Hold;
  const uint32_t index = nocturne::ReadU32(nocturne::kMovieIndex);
  const char* name = index == 2 ? "TYK_WS.wmv" : index == 3 ? "END_WS.wmv" : nullptr;
  if (!name)
    return false;
  switch (nocturne::HoldForMovie(name)) {
    case Hold::kPlaying:
      return true;
    case Hold::kFinished:
      nocturne::WriteU32(nocturne::kStrPlayerOn, 0);
      return false;
    case Hold::kNoMovie:
      return false;
  }
  return false;
}
