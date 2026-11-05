#include "generated/nocturnerecomp_init.h"

#include "jp/movie_player.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
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

#include "core/target.h"

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
//    prologue. JP fades a title card in and out ("JOURNEY BACK TO 1792 / and
//    the Transylvanian countryside of Romania." over 「1792年 トランシルバニア
//    地方」), plays OPN_WS, then calls sub_822AE128; World's mode 5
//    (dispatcher sub_822521C0) calls the same step, sub_8224ED70, straight
//    away. The card is ported below on World's functions (OpeningCard).
//  - The ending (JP sub_823F0078, World sub_82391F48): state 0 picks movie 3
//    (END_WS) when dword_83164F14 is set, else 2 (TYK_WS), in
//    dword_831751A0; state 1 plays it. World's state 1 still calls the PSX
//    STR player (sub_822ED3E0), which finds no stream and gives up.
//
// JP draws the movie over a game that keeps running, with the game's sound
// suspended. Here the hooks hold their state machine on the playing step
// instead, so nothing advances underneath, and the movie is decoded and drawn
// on the host (rex::audio::AsfMovieDecoder) and drawn by the native renderer
// into the guest's frame, under the screen filter (MoviePicture). Like JP's, the
// picture fills the PS1 box: JP builds its XMV quad from the four corners of
// the Change Screen Size rect (JP dword_82B32C90, sub_825E01E0), the rect the
// game's layers are drawn in, so it follows the player's stretch and the
// border outside the box stays on screen. Start skips it once the game is cleared, as JP's opening does. A folder without the movie files (World) runs the vanilla code.
//
// Hooks wired up in config/nocturnerecomp_movie_hooks.toml.
// ---------------------------------------------------------------------------

REXCVAR_DECLARE(bool, skip_richter);  // intro_skips.cpp

extern "C" REX_FUNC(sub_82234BF0);  // SOTNSoundSuspend
extern "C" REX_FUNC(sub_82234CC8);  // SOTNSoundUnsuspend
extern "C" REX_FUNC(sub_82239918);  // LoadGfx(id), JP sub_82299758
extern "C" REX_FUNC(sub_8223B470);  // AllocPrimitives(type, count), JP sub_8229B2B8
extern "C" REX_FUNC(sub_8223B6A8);  // FreePrimitives(index), JP sub_8229B4F0

namespace nocturne {
namespace {

constexpr uint32_t kMovieIndex = 0x831751A0;   // dword_831751A0
constexpr uint32_t kStrPlayerOn = 0x8318EE20;  // dword_8318EE20
constexpr uint32_t kGameCleared = 0x83133AFC;  // JP dword_833F6F04 (player_status.cpp)
constexpr size_t kFramesAhead = 8;

uint32_t ReadU32(uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(
      REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address));
}

void WriteU32(uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(
      REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address), value);
}

uint8_t* Guest(uint32_t guest_address) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(guest_address);
}

void WriteU16(uint32_t guest_address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(Guest(guest_address), value);
}

// Calls a recompiled guest function with an isolated context borrowed from the
// current thread (see tu_hooks.cpp's CallGuest). Midasm hooks get no context.
uint32_t CallGuest(void (*fn)(PPCContext&, uint8_t*), uint32_t r3 = 0, uint32_t r4 = 0) {
  auto* parent = rex::runtime::ThreadState::Get()->context();
  PPCContext ctx{};
  ctx.r1 = parent->r1;
  ctx.r1.u32 -= 0x70;
  ctx.r13 = parent->r13;
  ctx.fpscr = parent->fpscr;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  fn(ctx, REX_KERNEL_STATE()->memory()->virtual_membase());
  parent->fpscr = ctx.fpscr;
  return ctx.r3.u32;
}

struct Frame {
  std::vector<uint8_t> rgba;
  double pts = 0.0;
};

// One movie at a time: a worker decodes ahead, the renderer shows the frame
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
      current_.reset();
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
    current_.reset();
    phase_ = Phase::kIdle;
  }

  Phase phase() {
    Update();
    return phase_;
  }

  // The frame due now. False when nothing is playing; out.rgba stays null
  // until the first frame is due. Called by the renderer, once a frame.
  bool Picture(MoviePicture& out) {
    Update();
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kPlaying)
      return false;
    out.rgba = current_;
    out.width = width_;
    out.height = height_;
    out.serial = serial_;
    return true;
  }

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

  // Moves on to the frame due now, and ends playback once every frame is out
  // and the movie's length has passed. Both the hooks and the renderer call
  // it, so the movie runs to its end even on a frame nothing draws.
  void Update() {
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::kPlaying || !started_)
      return;
    const double now = Elapsed();
    bool taken = false;
    while (!frames_.empty() && frames_.front().pts <= now) {
      current_ = std::make_shared<const std::vector<uint8_t>>(std::move(frames_.front().rgba));
      frames_.pop_front();
      taken = true;
    }
    if (taken) {
      ++serial_;
      space_.notify_all();
    }
    if (decode_done_ && frames_.empty() && now >= duration_) {
      audio_.Stop();
      phase_ = Phase::kDone;
    }
  }

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable space_;
  std::deque<Frame> frames_;
  std::shared_ptr<const std::vector<uint8_t>> current_;
  uint64_t serial_ = 0;
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
// ---------------------------------------------------------------------------
// The title card before OPN_WS: JP sub_822B0EA0, states 0-4.
//
// Two textured quads (POLY_GT4) from texture page 0x1C (VRAM 768,256, 4bpp)
// at CLUT 0, which gfx 0x1A (World's DRA table still has it) loads with a
// 16-step grey palette: the English lines (232x32 from v 0) in the middle of
// a 320x240 screen and the Japanese one (192x16 from v 32) below. After a 64
// frame wait the English fades in a step a frame, the Japanese appears at
// 0x60; after 128 frames at full both fade out, the Japanese going at 0x40.
// JP shows the Japanese line in every language; here only in Japanese.
// JP's globals sit 0x2C8E9E0 higher than World's, in the same layout.
// ---------------------------------------------------------------------------
constexpr uint32_t kGfxBusy = 0x83164F08;   // JP dword_8342D8E8
constexpr uint32_t kGfxOwner = 0x83164F04;  // JP dword_8342D8E4
constexpr uint32_t kGfxMode = 0x83174634;   // JP dword_8343D014
constexpr uint32_t kGfxSlots = 0x8316AFE0;  // JP 0x834339C0: 32 of 0x40
constexpr uint32_t kGfxQueue = 0x8316AF60;  // JP 0x83433940
constexpr uint32_t kPrims = 0x831751E0;     // JP 0x8343DBC0: the primitive pool
constexpr uint32_t kPrimSize = 0x38;
constexpr uint32_t kDisplay = 0x8318F660;   // JP 0x83458040: the draw/display envs
constexpr uint32_t kPs1Language = 0x8313BC20;  // the PS1 side's language index
constexpr uint32_t kPs1Japanese = 1;

// World's primitive (the PSX Primitive, 4 bytes further on).
constexpr uint32_t kPrimNext = 0x00;
constexpr uint32_t kPrimXY[] = {0x0C, 0x18, 0x24, 0x30};  // s16 x, y per vertex
constexpr uint32_t kPrimUV[] = {0x10, 0x1C, 0x28, 0x34};  // u8 u, v per vertex
constexpr uint32_t kPrimRGB[] = {0x08, 0x14, 0x20, 0x2C};  // u8 r, g, b per vertex
constexpr uint32_t kPrimTimer = 0x17;                      // p1, JP's wait counter
constexpr uint32_t kPrimTpage = 0x1E;
constexpr uint32_t kPrimDrawMode = 0x36;
constexpr uint16_t kDrawShown = 0, kDrawColors = 4, kDrawHide = 8;

// JP sub_822ABE20: a 320x240 screen (World only has 256, 360 and 512 wide).
// {offset from kDisplay, size, value}, as JP stores them.
struct DisplayStore {
  uint32_t offset;
  uint8_t size;
  uint16_t value;
};
constexpr DisplayStore kDisplay320[] = {
    {0x4, 2, 0},       {0x6, 2, 20},      {0x8, 2, 320},     {0xA, 2, 207},
    {0xC, 2, 0},       {0xE, 2, 0},       {0x10, 2, 0},      {0x12, 2, 0},
    {0x14, 2, 0},      {0x16, 2, 0},      {0x1A, 1, 0},      {0x1B, 1, 1},
    {0x1C, 1, 1},      {0x1D, 1, 0},      {0x1E, 1, 0},      {0x1F, 1, 0},
    {0x64, 2, 0},      {0x66, 2, 256},    {0x68, 2, 320},    {0x6A, 2, 240},
    {0x6C, 2, 0},      {0x6E, 2, 0},      {0x70, 2, 320},    {0x72, 2, 240},
    {0x75, 1, 0},      {0x1BAFC, 2, 0},   {0x1BAFE, 2, 276}, {0x1BB00, 2, 320},
    {0x1BB02, 2, 207}, {0x1BB04, 2, 0},   {0x1BB06, 2, 256}, {0x1BB08, 2, 0},
    {0x1BB0A, 2, 0},   {0x1BB0C, 2, 0},   {0x1BB0E, 2, 0},   {0x1BB12, 1, 0},
    {0x1BB13, 1, 1},   {0x1BB14, 1, 1},   {0x1BB15, 1, 0},   {0x1BB16, 1, 0},
    {0x1BB17, 1, 0},   {0x1BB5C, 2, 0},   {0x1BB5E, 2, 0},   {0x1BB60, 2, 320},
    {0x1BB62, 2, 240}, {0x1BB64, 2, 0},   {0x1BB66, 2, 0},   {0x1BB68, 2, 320},
    {0x1BB6A, 2, 240}, {0x1BB6D, 1, 0},
};

void SetDisplay320() {
  for (const auto& store : kDisplay320) {
    if (store.size == 1)
      *Guest(kDisplay + store.offset) = uint8_t(store.value);
    else
      WriteU16(kDisplay + store.offset, store.value);
  }
}

// An axis-aligned quad: (x0, y0)-(x1, y1) on screen, (u0, v0)-(u1, v1) in
// page 0x1C.
void SetQuad(uint32_t prim, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t u0, uint8_t v0,
             uint8_t u1, uint8_t v1) {
  const int16_t xs[] = {x0, x1, x0, x1}, ys[] = {y0, y0, y1, y1};
  const uint8_t us[] = {u0, u1, u0, u1}, vs[] = {v0, v0, v1, v1};
  for (int i = 0; i < 4; ++i) {
    WriteU16(prim + kPrimXY[i], uint16_t(xs[i]));
    WriteU16(prim + kPrimXY[i] + 2, uint16_t(ys[i]));
    *Guest(prim + kPrimUV[i]) = us[i];
    *Guest(prim + kPrimUV[i] + 1) = vs[i];
  }
  WriteU16(prim + kPrimTpage, 0x1C);
}

void SetGrey(uint32_t prim, uint8_t level) {
  for (const uint32_t rgb : kPrimRGB)
    std::memset(Guest(prim + rgb), level, 3);
}

class OpeningCard {
 public:
  bool fresh() const { return step_ == Step::kSetUp; }
  bool done() const { return step_ == Step::kDone; }
  void Reset() { step_ = Step::kSetUp; }

  // One game frame of the card. False once it is over.
  bool Run() {
    switch (step_) {
      case Step::kSetUp:
        return SetUp();
      case Step::kWaitIn:
      case Step::kHold: {
        uint8_t& timer = *Guest(english_ + kPrimTimer);
        if (--timer == 0)
          step_ = step_ == Step::kWaitIn ? Step::kFadeIn : Step::kFadeOut;
        return true;
      }
      case Step::kFadeIn: {
        const uint8_t level = Level() + 1;
        SetGrey(english_, level);
        if (level == 0x60 && ReadU32(kPs1Language) == kPs1Japanese)
          WriteU16(japanese_ + kPrimDrawMode, kDrawShown);
        if (level == 0x80) {
          *Guest(english_ + kPrimTimer) = 0x80;
          step_ = Step::kHold;
        }
        return true;
      }
      case Step::kFadeOut: {
        const uint8_t level = Level() - 1;
        SetGrey(english_, level);
        if (level == 0x40)
          WriteU16(japanese_ + kPrimDrawMode, kDrawHide);
        if (level != 0)
          return true;
        End();
        return false;
      }
      case Step::kDone:
        return false;
    }
    return false;
  }

  // Frees the quads, if they were made.
  void End() {
    if (step_ != Step::kSetUp && step_ != Step::kDone)
      CallGuest(sub_8223B6A8, index_);
    step_ = Step::kDone;
  }

 private:
  enum class Step { kSetUp, kWaitIn, kFadeIn, kHold, kFadeOut, kDone };

  uint8_t Level() const { return *Guest(english_ + kPrimRGB[0]); }

  bool SetUp() {
    if (ReadU32(kGfxBusy) != 0)
      return true;
    // Drop the gfx loads still pending and queue the card's palette.
    WriteU32(kGfxQueue + 0xC, 0);
    for (uint32_t i = 0; i < 32; ++i)
      WriteU16(kGfxSlots + 8 + i * 0x40, 0);
    CallGuest(sub_82239918, 0x1A);
    WriteU32(kGfxOwner, 1);
    WriteU32(kGfxMode, 0x18);
    SetDisplay320();
    const int16_t index = int16_t(CallGuest(sub_8223B470, 4, 2));
    if (index < 0) {
      REXLOG_WARN("movies: no primitives for the opening card");
      step_ = Step::kDone;
      return false;
    }
    index_ = uint32_t(index);
    english_ = kPrims + index_ * kPrimSize;
    japanese_ = ReadU32(english_ + kPrimNext);

    SetQuad(english_, 44, 96, 276, 128, 0, 0, 232, 32);
    SetGrey(english_, 0);
    *Guest(english_ + kPrimTimer) = 0x40;
    WriteU16(english_ + kPrimDrawMode, kDrawColors);

    SetQuad(japanese_, 60, 208, 252, 224, 0, 32, 192, 48);
    SetGrey(japanese_, 0x80);
    WriteU16(japanese_ + kPrimDrawMode, kDrawHide);
    REXLOG_INFO("movies: opening card (primitives {})", index_);
    step_ = Step::kWaitIn;
    return true;
  }

  Step step_ = Step::kSetUp;
  uint32_t index_ = 0;
  uint32_t english_ = 0;
  uint32_t japanese_ = 0;
};

OpeningCard g_card;

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
// while it plays; Start skips it when |skippable|. Without JP installed the
// vanilla code runs.
Hold HoldForMovie(const char* name, bool skippable) {
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
      if (skippable && down && !g_start_was_down)
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

}  // namespace

void BindMovies(rex::Runtime* runtime) {
  g_input = static_cast<rex::input::InputSystem*>(runtime->input_system());
}

bool CurrentMoviePicture(MoviePicture& out) { return g_movie.Picture(out); }

}  // namespace nocturne

// Title dispatcher sub_822521C0, mode 5's `b sub_8224ED70`: JP's title card,
// then OPN_WS. True holds mode 5 (returns from the dispatcher, 0x822523A0).
// Start skips both, but only once the game has been cleared: JP's mode 5
// checks Start against the clear flag. The opening introduces the Richter
// prologue, so skip_richter skips it too.
bool MovieOpeningHook() {
  using namespace nocturne;
  if (REXCVAR_GET(skip_richter))
    return false;
  if (!g_card.done()) {
    std::error_code ec;
    if (JpDir().empty() ||
        !std::filesystem::is_regular_file(JpDir() / "DATA" / "video" / "OPN_WS.wmv", ec))
      return false;
    if (g_card.fresh())
      g_start_was_down = true;  // a press that started the game doesn't skip
    const bool down = StartDown();
    const bool pressed = down && !g_start_was_down;
    g_start_was_down = down;
    if (pressed && ReadU32(kGameCleared) != 0) {
      g_card.End();
      g_card.Reset();
      return false;
    }
    if (g_card.Run())
      return true;
  }
  if (HoldForMovie("OPN_WS.wmv", ReadU32(kGameCleared) != 0) == Hold::kPlaying)
    return true;
  g_card.Reset();
  return false;
}

// sub_82391F48 state 1, before `bl sub_822ED3E0` (the PSX STR player): the
// movie state 0 picked. True holds state 1 (to the epilogue, 0x82392304).
// When it ends, the STR player is switched off (it returns at once when
// dword_8318EE20 is 0) and the rest of state 1 runs as after a played movie.
// Start skips it once the game is cleared, like the opening.
bool MovieEndingHook() {
  using nocturne::Hold;
  const uint32_t index = nocturne::ReadU32(nocturne::kMovieIndex);
  const char* name = index == 2 ? "TYK_WS.wmv" : index == 3 ? "END_WS.wmv" : nullptr;
  if (!name)
    return false;
  switch (nocturne::HoldForMovie(name, nocturne::ReadU32(nocturne::kGameCleared) != 0)) {
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
