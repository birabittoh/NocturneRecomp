// Builds MottZilla's SOTN_XB_RANDO core (ext/SOTN_XB_RANDO, the Source/ files
// other than main.cpp, which is that project's Win32 GUI) into this exe.
// xb_rando.cpp #includes its own xb_candlesetup.h and xb_relic_locations.cpp,
// so it is compiled as one translation unit here, unmodified.
//
// The core patches a decrypted default.xex through stdio. Rather than editing
// the upstream source, its stdio calls are redirected by macros to the
// wrappers below:
//   * the xex it opens (XB_Randomize's path argument) is kMemoryXexPath, a
//     host buffer set with SetMemoryXex; seeks, reads and writes go there and
//     nothing touches the disk;
//   * "spoilers.txt" is captured into a string (TakeSpoilers).
// Any other FILE* is forwarded to real stdio.
//
// rand()/srand() are redirected too, to a private copy of MSVC's generator
// with MSVC's per-thread state. glibc's rand() differs from MSVC's (so the
// same seed name gave different seeds per platform) and is shared by every
// thread in the process. Upstream only seeds the relic thread's state,
// which starts at 1 in a new thread just as it does here; the generator
// thread's state is seeded from the seed number (SeedGeneratorRng), so a
// whole run is reproducible.
//
// Its Win32 GUI hooks (LogMsg, DoMessages, ...) are provided by rando_host.cpp.
//
// Randomizer by MottZilla: https://github.com/MottZilla/SOTN_XB_RANDO

#include "rando_core.h"

// Everything xb_rando.cpp includes, ahead of the macros below, so the macros
// only rewrite the core's own calls.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <process.h>
#include <string>
#include <windows.h>

#ifndef _WIN32
#include "compat/itoa.h"
#endif

namespace nocturne::rando::detail {
namespace {

struct MemoryFile {
  std::vector<uint8_t>* data = nullptr;
  size_t pos = 0;
  size_t limit = 0;
  bool out_of_range = false;
};

struct SpoilerFile {
  std::string text;
};

MemoryFile g_xex;
SpoilerFile g_spoilers;

std::FILE* XexHandle() { return reinterpret_cast<std::FILE*>(&g_xex); }
std::FILE* SpoilerHandle() { return reinterpret_cast<std::FILE*>(&g_spoilers); }

thread_local uint32_t g_rand_state = 1;

}  // namespace

void SetMemoryXex(std::vector<uint8_t>* data, size_t limit) {
  g_xex = {};
  g_xex.data = data;
  // (No std::min: <windows.h> defines a min macro.)
  g_xex.limit = !data ? 0 : limit < data->size() ? limit : data->size();
}

bool MemoryXexOutOfRange() { return g_xex.out_of_range; }

std::string TakeSpoilers() { return std::move(g_spoilers.text); }

void SeedGeneratorRng(uint32_t seed) { g_rand_state = seed; }

std::FILE* OpenForRando(const char* path, const char* mode) {
  if (std::strcmp(path, kMemoryXexPath) == 0) {
    return g_xex.data ? XexHandle() : nullptr;
  }
  if (std::strcmp(path, "spoilers.txt") == 0) {
    g_spoilers.text.clear();
    return SpoilerHandle();
  }
  return std::fopen(path, mode);
}

int RandoFclose(std::FILE* f) {
  if (f == XexHandle() || f == SpoilerHandle()) {
    return 0;
  }
  return std::fclose(f);
}

int RandoFseek(std::FILE* f, long offset, int origin) {
  if (f != XexHandle()) {
    return std::fseek(f, offset, origin);
  }
  long base = origin == SEEK_CUR   ? static_cast<long>(g_xex.pos)
              : origin == SEEK_END ? static_cast<long>(g_xex.limit)
                                   : 0;
  if (base + offset < 0) {
    g_xex.out_of_range = true;
    return -1;
  }
  g_xex.pos = static_cast<size_t>(base + offset);
  return 0;
}

long RandoFtell(std::FILE* f) {
  if (f != XexHandle()) {
    return std::ftell(f);
  }
  return static_cast<long>(g_xex.pos);
}

int RandoFgetc(std::FILE* f) {
  if (f != XexHandle()) {
    return std::fgetc(f);
  }
  if (g_xex.pos >= g_xex.limit) {
    g_xex.out_of_range = true;
    return EOF;
  }
  return (*g_xex.data)[g_xex.pos++];
}

int RandoFputc(int c, std::FILE* f) {
  if (f == SpoilerHandle()) {
    g_spoilers.text.push_back(static_cast<char>(c));
    return static_cast<unsigned char>(c);
  }
  if (f != XexHandle()) {
    return std::fputc(c, f);
  }
  if (g_xex.pos >= g_xex.limit) {
    g_xex.out_of_range = true;
    return EOF;
  }
  (*g_xex.data)[g_xex.pos++] = static_cast<uint8_t>(c);
  return static_cast<unsigned char>(c);
}

size_t RandoFwrite(const void* ptr, size_t size, size_t count, std::FILE* f) {
  if (f != XexHandle() && f != SpoilerHandle()) {
    return std::fwrite(ptr, size, count, f);
  }
  const auto* p = static_cast<const unsigned char*>(ptr);
  for (size_t i = 0; i < size * count; ++i) {
    if (RandoFputc(p[i], f) == EOF) {
      return i / (size ? size : 1);
    }
  }
  return count;
}

int RandoFputs(const char* s, std::FILE* f) {
  if (f != XexHandle() && f != SpoilerHandle()) {
    return std::fputs(s, f);
  }
  return RandoFwrite(s, 1, std::strlen(s), f) == std::strlen(s) ? 0 : EOF;
}

// MSVC's rand(): RAND_MAX is 0x7FFF there as well.
int RandoRand() {
  g_rand_state = g_rand_state * 214013u + 2531011u;
  return static_cast<int>((g_rand_state >> 16) & 0x7FFF);
}

void RandoSrand(unsigned seed) { g_rand_state = seed; }

}  // namespace nocturne::rando::detail

#define fopen(path, mode) nocturne::rando::detail::OpenForRando(path, mode)
#define fclose(f) nocturne::rando::detail::RandoFclose(f)
#define fseek(f, offset, origin) nocturne::rando::detail::RandoFseek(f, offset, origin)
#define ftell(f) nocturne::rando::detail::RandoFtell(f)
#define fgetc(f) nocturne::rando::detail::RandoFgetc(f)
#define fputc(c, f) nocturne::rando::detail::RandoFputc(c, f)
#define fwrite(ptr, size, count, f) nocturne::rando::detail::RandoFwrite(ptr, size, count, f)
#define fputs(s, f) nocturne::rando::detail::RandoFputs(s, f)
#define rand() nocturne::rando::detail::RandoRand()
#define srand(seed) nocturne::rando::detail::RandoSrand(seed)

#include "xb_rando.cpp"

namespace nocturne::rando::detail {

void WriteEnhancements(const Enhancements& e) {
  fXEX = fopen(kMemoryXexPath, "r+b");
  if (!fXEX) {
    return;
  }
  // The two that check their option themselves.
  const int fast_warps = XB_FastWarps, anti_freeze = XB_AntiFreeze;
  XB_FastWarps = XB_AntiFreeze = 1;
  if (e.Has(Enhancement::kFastWarps)) XB_ApplyFastWarps();
  if (e.Has(Enhancement::kAntiFreeze)) XB_ApplyAntiFreeze();
  XB_FastWarps = fast_warps;
  XB_AntiFreeze = anti_freeze;

  if (e.Has(Enhancement::kRemoveClockStatue)) XB_RemoveClockStatue();
  if (e.Has(Enhancement::kBoostDropRates)) {
    // What XB_DoDropsRando writes for XB_BoostDropRates, without the drops.
    for (int i = 0; i < 398; ++i) {
      fseek(fXEX, OFS_ENEMYDEFS + (i * 0x24) + 0x1A, SEEK_SET);
      XB_Write_u16_be(16);
      XB_Write_u16_be(32);
    }
  }
  if (e.Has(Enhancement::kDeathOnVacation)) XB_ApplyDeathOnVacation();
  if (e.Has(Enhancement::kSkipMariaAlchemy)) XB_RemoveMariaAlchemyMeeting();
  if (e.Has(Enhancement::kOuterWallElevator)) XB_FixOuterWallElevator();
  if (e.Has(Enhancement::kScyllaSoftlock)) XB_FixScyllaSoftlock();
  if (e.Has(Enhancement::kBridgeInput)) XB_PatchBridgeInput();
  if (e.Has(Enhancement::kCatacombsEscape)) XB_PatchMines();
  if (e.Has(Enhancement::kReverseLibraryCard)) XB_ApplyReverseLibraryCardPatch();
  if (e.Has(Enhancement::kClearFile)) XB_PatchClearFile();
  if (e.Has(Enhancement::kMapTracker)) {
    // XB_ApplyMapTrackerPatch for the "Guarded" location set (relics only),
    // which is what the vanilla castle has; inline so as not to log to the
    // Generator.
    fseek(fXEX, MapTracker_CodeBlock_Ofs, SEEK_SET);
    fwrite(&MapTracker_CodeBlock[0], sizeof(MapTracker_CodeBlock), 1, fXEX);
    fseek(fXEX, 0x24035C + 0x4000, SEEK_SET);
    XB_Write_u32_be(XB_CalcBranch(0x24035C, 0x87FE70));
    fseek(fXEX, MapTracker_LocData_Ofs, SEEK_SET);
    fwrite(&MapTracker_LocData[0], sizeof(MapTracker_LocData), 1, fXEX);
    fseek(fXEX, MapTracker_LocData_Ofs + MapTracker_Castle1Terminate_Ofs, SEEK_SET);
    XB_Write_u16_be(0);
    fseek(fXEX, MapTracker_LocData_Ofs + MapTracker_Castle2Terminate_Ofs, SEEK_SET);
    XB_Write_u16_be(0);
  }
  // From XB_PresetWrites, XB_RandomizeShopItems and XB_ApplyBountyHunterPatches.
  if (e.Has(Enhancement::kClockRoomDoor)) {
    XB_WritePatch(0x3163BC, &XB_PATCHCLOCKROOMOPEN[0], sizeof(XB_PATCHCLOCKROOMOPEN));
  }
  if (e.Has(Enhancement::kKeyItemsStay)) {
    fseek(fXEX, 0x347AD0, SEEK_SET);
    XB_Write_u32_be(0x60000000);
  }
  if (e.Has(Enhancement::kFreeJewelOfOpen)) {
    fseek(fXEX, OFS_SHOPLIST + 4, SEEK_SET);
    XB_Write_u32_be(0);
  }
  if (e.Has(Enhancement::kCheapDuplicator)) {
    fseek(fXEX, 0xD42234, SEEK_SET);
    XB_Write_u32_be(15000);
  }
  if (e.Has(Enhancement::kInfiniteItems)) {
    for (long ofs : {0xE43351, 0xE42F31, 0xE42F89, 0xE42DD1, 0xE41CF9}) {
      fseek(fXEX, ofs, SEEK_SET);
      fputc(0, fXEX);
    }
  }
  if (e.Has(Enhancement::kSkipMariaSilverRing)) {
    fseek(fXEX, 0xCE0596, SEEK_SET);
    XB_Write_u16_be(0x0008);
    XB_Write_u16_be(0x0001);
  }
  if (e.Has(Enhancement::kDemonCardDoor)) {
    for (long ofs : {0xCE3A8C, 0xCE3E6E, 0xCE3A82, 0xCE3E64}) {
      fseek(fXEX, ofs + 4, SEEK_SET);
      XB_Write_u16_be(0);
    }
  }
  if (e.alchemy_vats) {
    // Its rand() rolls from the chosen shuffle instead of the seed's state.
    const uint32_t state = g_rand_state;
    g_rand_state = e.alchemy_vats;
    XB_RandomizeAlchemyVats();
    g_rand_state = state;
  }
  switch (e.game_mode) {
    case GameMode::kBatMaster:
      XB_ApplyBatMasterPatches();
      break;
    case GameMode::kLegDay:
      XB_ApplyLegDayPatches();
      break;
    case GameMode::kRichter:
      XB_ApplyRichterPatches();
      break;
    default:
      break;
  }
  if (e.wing_smash_color >= 1 && e.wing_smash_color <= kWingSmashColorCount) {
    // XB_RandomizeWingSmashTrail with the color chosen instead of rolled.
    fseek(fXEX, 0xDEB73C, SEEK_SET);
    XB_Write_u16_be(WS_COLORS[e.wing_smash_color - 1]);
  }
  fclose(fXEX);
  fXEX = NULL;
}

}  // namespace nocturne::rando::detail
