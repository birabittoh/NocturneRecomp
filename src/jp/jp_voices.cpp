#include "generated/nocturnerecomp_init.h"

#include "jp/jp_voices.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "core/target.h"

REXCVAR_DEFINE_STRING(voices, "auto", "Game",
                      "Whose voices to play when both releases are installed: world, jp, or "
                      "auto (JP's while the language is Japanese)")
    .allowed({"auto", "jp", "world"});

namespace nocturne {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kLanguageJapanese = 2;

constexpr const char* kGuestSoundPath = "game:\\DATA\\sound";
constexpr const char* kMount = "\\Device\\NocturneJpSound";

// The per-stage bank table: 44-byte rows, the .xsb then the .xwb byte count
// first. Read by hd_read_trans (sub_82252A30) for every stage load. Rows 70
// to 79 are placeholders, the same in both releases, that name no real bank.
constexpr uint32_t kStageTable = 0x82E00CF8;
constexpr uint32_t kStageRowSize = 44;
constexpr uint32_t kStageRows = 80;
// Its first row is sd_rou in World: a check that this is the image we know.
constexpr std::pair<uint32_t, uint32_t> kStageRow0 = {1745, 708608};

// The resident banks (Alucard, Richter, the menus...): 20-byte rows of path,
// load type (3 .xsb, 4 .xwb), pointer to the buffer kind, byte count.
constexpr uint32_t kResidentTable = 0x82E042A0;
constexpr uint32_t kResidentRowSize = 20;
constexpr uint32_t kResidentRows = 22;
constexpr uint32_t kLoadSoundBank = 3;
constexpr uint32_t kLoadWaveBank = 4;

// hd_read_trans's load descriptor (path, type, buffer, size, ...), the
// pointer to the one in use, and the buffer kind of a sound load. Stage banks
// are kind 3.
constexpr uint32_t kDescriptor = 0x82E7FD54;
constexpr uint32_t kDescriptorInUse = 0x82E7FD50;
constexpr uint32_t kLoadKind = 0x8313BC2C;
constexpr uint32_t kKinds = 4;
constexpr uint32_t kStageKind = 3;

// What sub_824FBE98 allocates for each kind's wave bank, in World.
constexpr std::array<uint32_t, kKinds> kWorldWaveBuffer = {0xE1000, 0xF000, 0x55800, 0x18E000};

struct Sizes {
  uint32_t xsb = 0;
  uint32_t xwb = 0;
};

bool g_active = false;
// Every table entry the two releases disagree on: guest address, World's
// byte count, JP's.
struct TableValue {
  uint32_t address;
  uint32_t world;
  uint32_t jp;
};
std::vector<TableValue> g_table;
rex::filesystem::VirtualFileSystem* g_vfs = nullptr;
std::string g_world_sound_path;
bool g_linked = false;
std::atomic<bool> g_dirty{false};
std::array<uint32_t, kKinds> g_wave_buffer{};  // sizes to allocate
std::array<uint32_t, kKinds> g_sound_buffer{};  // guest addresses of ours

rex::memory::Memory* Memory() {
  return REX_KERNEL_STATE()->memory();
}

uint32_t Load(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(Memory()->TranslateVirtual(address));
}

void Store(uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(Memory()->TranslateVirtual(address), value);
}

uint8_t LoadByte(uint32_t address) {
  return *Memory()->TranslateVirtual<const uint8_t*>(address);
}

std::string LoadString(uint32_t address) {
  return address ? std::string(Memory()->TranslateVirtual<const char*>(address)) : std::string();
}

uint32_t FileSize(const fs::path& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? 0 : static_cast<uint32_t>(size);
}

// "rou" -> the sizes of sd_rou.xsb/.xwb in `dir`, for every bank there.
std::map<std::string, Sizes> BankSizes(const fs::path& dir) {
  std::map<std::string, Sizes> banks;
  std::error_code ec;
  for (const auto& item : fs::directory_iterator(dir, ec)) {
    const fs::path& path = item.path();
    const std::string stem = path.stem().string();
    if (stem.rfind("sd_", 0) != 0)
      continue;
    Sizes& sizes = banks[stem.substr(3)];
    if (path.extension() == ".xsb")
      sizes.xsb = FileSize(path);
    else if (path.extension() == ".xwb")
      sizes.xwb = FileSize(path);
  }
  return banks;
}

// Records JP's byte counts for both tables (applied by UseVoices). A stage row is matched to its bank by
// World's sizes rather than by the name it gives: some rows name sd_rou but
// carry sd_tou's sizes, and JP's table has JP's sd_tou sizes there too.
// Records the biggest bank of each kind, for the buffers. False when the
// image isn't the one these addresses were read from.
bool BuildTables(const fs::path& world_sound, const fs::path& jp_sound) {
  if (Load(kStageTable) != kStageRow0.first || Load(kStageTable + 4) != kStageRow0.second ||
      LoadString(Load(kResidentTable)) != "game:\\data\\sound\\sd_alk.xsb") {
    REXLOG_WARN("jp voices: the bank tables are not where they should be");
    return false;
  }
  const auto world = BankSizes(world_sound);
  const auto jp = BankSizes(jp_sound);
  std::map<std::pair<uint32_t, uint32_t>, std::string> by_world_sizes;
  for (const auto& [name, sizes] : world)
    by_world_sizes[{sizes.xsb, sizes.xwb}] = name;

  std::array<uint32_t, kKinds> sound_size{};
  std::array<uint32_t, kKinds> wave_size = kWorldWaveBuffer;

  for (uint32_t row = 0; row < kStageRows; ++row) {
    const uint32_t entry = kStageTable + row * kStageRowSize;
    const auto bank = by_world_sizes.find({Load(entry), Load(entry + 4)});
    // World's banks are read into the same buffers when they play.
    if (bank != by_world_sizes.end())
      sound_size[kStageKind] = std::max(sound_size[kStageKind], Load(entry));
    const auto jp_bank = bank == by_world_sizes.end() ? jp.end() : jp.find(bank->second);
    if (jp_bank == jp.end() || !jp_bank->second.xsb || !jp_bank->second.xwb)
      continue;
    g_table.push_back({entry, Load(entry), jp_bank->second.xsb});
    g_table.push_back({entry + 4, Load(entry + 4), jp_bank->second.xwb});
    sound_size[kStageKind] = std::max(sound_size[kStageKind], jp_bank->second.xsb);
    wave_size[kStageKind] = std::max(wave_size[kStageKind], jp_bank->second.xwb);
  }

  for (uint32_t row = 0; row < kResidentRows; ++row) {
    const uint32_t entry = kResidentTable + row * kResidentRowSize;
    const uint32_t type = Load(entry + 4);
    if (type != kLoadSoundBank && type != kLoadWaveBank)
      continue;
    const std::string path = LoadString(Load(entry));
    const uint32_t size = FileSize(jp_sound / path.substr(path.rfind('\\') + 1));
    const uint32_t kind = LoadByte(Load(entry + 8));
    if (kind >= kKinds)
      continue;
    auto& biggest = type == kLoadSoundBank ? sound_size[kind] : wave_size[kind];
    biggest = std::max(biggest, Load(entry + 12));
    if (!size)
      continue;
    g_table.push_back({entry + 12, Load(entry + 12), size});
    biggest = std::max(biggest, size);
  }

  for (uint32_t kind = 0; kind < kKinds; ++kind) {
    g_sound_buffer[kind] = Memory()->SystemHeapAlloc(sound_size[kind], 0x100);
    if (!g_sound_buffer[kind]) {
      REXLOG_WARN("jp voices: could not allocate a {}-byte sound bank buffer", sound_size[kind]);
      return false;
    }
  }
  g_wave_buffer = wave_size;
  return true;
}

bool WantJp() {
  const std::string choice = REXCVAR_GET(voices);
  if (choice == "jp")
    return true;
  if (choice == "world")
    return false;
  return rex::cvar::Query<uint32_t>("user_language") == kLanguageJapanese;
}

// Puts one release's byte counts in the tables and links DATA\sound to JP's
// folder or not. Banks already loaded keep playing; each takes the choice
// from its next load (a stage's on entering it, a character's or familiar's
// when it is next loaded).
void UseVoices(bool jp) {
  for (const auto& value : g_table)
    Store(value.address, jp ? value.jp : value.world);
  if (jp && !g_linked) {
    // Shadow the directory, not each file: see font_overlay.h.
    g_linked = g_vfs->RegisterSymbolicLink(g_world_sound_path, kMount);
    if (!g_linked)
      REXLOG_WARN("jp voices: could not link {}", g_world_sound_path);
  } else if (!jp && g_linked) {
    g_vfs->UnregisterSymbolicLink(g_world_sound_path);
    g_linked = false;
  }
  REXLOG_INFO("jp voices: playing {} voices", g_linked ? "JP's" : "World's");
}

}  // namespace

bool JpVoicesAvailable() {
  return JpOverlaysAvailable();
}

void InstallJpVoices(rex::Runtime* runtime) {
  if (!JpVoicesAvailable() || !runtime || !runtime->file_system())
    return;

  const fs::path& jp = JpDir();
  std::error_code ec;
  const fs::path jp_sound = jp / "DATA" / "sound";
  const fs::path world_sound = GameDataRoot() / "DATA" / "sound";
  if (!fs::is_directory(jp_sound, ec)) {
    REXLOG_WARN("jp voices: {} is missing", jp_sound.string());
    return;
  }

  auto* vfs = runtime->file_system();
  rex::filesystem::Entry* world_entry = vfs->ResolvePath(kGuestSoundPath);
  if (!world_entry) {
    REXLOG_WARN("jp voices: no {} to replace", kGuestSoundPath);
    return;
  }

  if (!BuildTables(world_sound, jp_sound))
    return;

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMount, jp_sound, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("jp voices: could not mount {}", jp_sound.string());
    return;
  }
  g_vfs = vfs;
  g_world_sound_path = world_entry->absolute_path();
  // The buffers are sized for JP's banks whichever plays, so the choice can
  // change at any time.
  g_active = true;
  REXLOG_INFO("jp voices: {} available (wave buffers {:X} {:X} {:X} {:X})", jp_sound.string(),
              g_wave_buffer[0], g_wave_buffer[1], g_wave_buffer[2], g_wave_buffer[3]);
  UseVoices(WantJp());
  auto mark = [](std::string_view, std::string_view) { g_dirty.store(true); };
  rex::cvar::RegisterChangeCallback("voices", mark);
  rex::cvar::RegisterChangeCallback("user_language", mark);
}

void ApplyPendingVoices() {
  if (!g_active || !g_dirty.exchange(false))
    return;
  UseVoices(WantJp());
}

}  // namespace nocturne

// sub_824FBE98, at each of its four allocations: r3 is the byte count of one
// kind's wave bank buffer. JP's banks need more.
void JpVoicesWaveBuffer(PPCRegister& r3) {
  using namespace nocturne;
  if (!g_active)
    return;
  for (uint32_t kind = 0; kind < kKinds; ++kind) {
    if (r3.u32 == kWorldWaveBuffer[kind]) {
      r3.u64 = g_wave_buffer[kind];
      return;
    }
  }
}

// hd_read_trans, after logging the load and before reading: a sound bank
// goes into our buffer for its kind instead of World's static one.
void JpVoicesSoundBuffer() {
  using namespace nocturne;
  if (!g_active || Load(kDescriptorInUse) != kDescriptor ||
      Load(kDescriptor + 4) != kLoadSoundBank)
    return;
  const uint32_t kind = Load(kLoadKind);
  if (kind < kKinds)
    Store(kDescriptor + 8, g_sound_buffer[kind]);
}
