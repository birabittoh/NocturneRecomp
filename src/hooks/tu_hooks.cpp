#include "generated/nocturnerecomp_init.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/filesystem/vfs.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "hooks/tu_dialogue_patch.h"

// ---------------------------------------------------------------------------
// Title Update 1 ports.
//
// Everything below re-creates, on the *vanilla* image, a behaviour change the
// title update (assets/default.xexp) makes to the game code, so the update
// package is no longer needed for it. Each hook names the vanilla address it
// is wired to in config/nocturnerecomp_tu_hooks.toml; the patched-image equivalent is
// given for cross-reference with the title update's own codegen output.
// ---------------------------------------------------------------------------

namespace {

uint8_t* GuestPtr(uint32_t guest_address) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address);
}

uint32_t ReadU32(uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(GuestPtr(guest_address));
}

uint16_t ReadU16(uint32_t guest_address) {
  return rex::memory::load_and_swap<uint16_t>(GuestPtr(guest_address));
}

void WriteU16(uint32_t guest_address, uint16_t value) {
  rex::memory::store_and_swap<uint16_t>(GuestPtr(guest_address), value);
}

}  // namespace

// sub_82231A50 (patched 0x82231A50) is a small step state machine whose state
// byte lives at 0x831D20D8. After every step the update checks the word right
// behind it (0x831D20DC; patched 0x831D1E9C) and drops the state machine into
// state 3 when it is clear. The vanilla code returns straight
// from three places; this runs the new check on each of them. r28 is
// 0x831D0000 throughout the function.
void TuStateFlagCheck(PPCRegister& r28) {
  constexpr uint32_t kStateOffset = 0x20D8;
  constexpr uint32_t kFlagOffset = 0x20DC;
  if (ReadU32(r28.u32 + kFlagOffset) == 0) {
    *GuestPtr(r28.u32 + kStateOffset) = 3;
  }
}

// The same, for the step that found nothing: the vanilla `beq` (r3 == 0) went
// straight to the epilogue, the update runs the flag check first. Returns true
// when r3 == 0, which jumps to the epilogue (0x82231E28); false falls into the
// "device found" arm at 0x82231D98.
bool TuStateFlagCheckOnNoDevice(PPCRegister& r3, PPCRegister& r28) {
  if (r3.u32 != 0) {
    return false;
  }
  TuStateFlagCheck(r28);
  return true;
}

// The flag itself: the update turns the two one-line wrappers sub_8250E690
// and sub_8250E698 (patched 0x8250E848 / 0x8250E860; both pass 0 to an
// XMsgStartIORequest wrapper, messages 0x70005 / 0x70003) into "store 1 / 0 to
// 0x831D20DC, then do the same". State 3 of sub_82231A50 calls sub_8250E690 on
// its way to state 0x30, so the flag is raised while that state waits. Without
// these the flag stays 0 and the check bounces 0x30 back to 3 every frame: the
// game spins on a black screen (seen pausing in the prologue).
void TuStateFlagSet() {
  rex::memory::store_and_swap<uint32_t>(GuestPtr(0x831D20DC), 1);
}

void TuStateFlagClear() {
  rex::memory::store_and_swap<uint32_t>(GuestPtr(0x831D20DC), 0);
}

// sub_82248538 validates the controller layout twice (0x822485BC and
// 0x8224A228): the seven words at 0x83173C00 hold, per slot, which of the seven
// logical buttons it is bound to, and the layout is accepted when the bindings
// cover all of 0..6. The update moves that into one routine (patched
// sub_822411C8) which, on top of the verdict, refreshes the seven halfwords
// behind the table (0x83173C20..0x83173C2D): for slot k, the 16-bit button mask
// kButtonMask[binding[k]] (0x82E044B0; patched 0x82E04280). The verdict mirrors
// the old OR-of-bits loop, including its quirk that the seventh slot is
// replaced by 7 when some index is unused.
bool TuControlsLayoutValid() {
  constexpr uint32_t kTable = 0x83173C00;
  constexpr uint32_t kMasks = 0x82E044B0;
  constexpr int kSlots = 7;

  uint32_t binding[kSlots];
  bool seen[kSlots] = {};
  for (int k = 0; k < kSlots; ++k) {
    binding[k] = ReadU32(kTable + 4 * k);
    if (binding[k] < kSlots) {
      seen[binding[k]] = true;
    }
  }

  // The seventh slot only counts when every index 0..6 is in use; otherwise
  // the old loop OR-ed in bit 7 instead, which can never complete the set.
  uint32_t last = binding[kSlots - 1];
  for (int i = 0; i < kSlots; ++i) {
    if (!seen[i]) {
      last = kSlots;
      break;
    }
  }

  uint32_t covered = 0;
  for (int k = 0; k < kSlots - 1; ++k) {
    if (binding[k] < 32) {
      covered |= 1u << binding[k];
    }
  }
  if (last < 32) {
    covered |= 1u << last;
  }

  for (int k = 0; k < kSlots; ++k) {
    // The mask table has eight entries; a corrupt binding reads as "no button".
    WriteU16(kTable + 0x20 + 2 * k, binding[k] < 8 ? ReadU16(kMasks + 2 * binding[k]) : 0);
  }
  return covered == 0x7F;
}

// sub_823CBB60 (patched 0x823CBCD0) compares a sign-extended byte against
// 0xFF, which can never match; the update compares against -1 as intended.
// Site 0x823CBD5C: true takes the original `beq` target (0x823CC854), false
// skips the (dead) cmpwi/beq pair to 0x823CBD64.
bool TuSignedByteCompare(PPCRegister& r9) {
  return r9.s32 == -1;
}

// sub_824F66F8 (patched 0x824F6810) posts the "open Xbox Live logon" page
// switch with the page object's +0x148 field as argument; the update always
// passes 1. Run before `mr r5, r31` at 0x824F6738 (r31 is saved/restored by
// the function).
void TuLeaderboardLogonMode(PPCRegister& r31) {
  r31.u64 = 1;
}

// sub_8257E130 (patched 0x8257E308): the guest status that used to need to be
// exactly 1 for the "post page switch" branch at 0x8257E220 now also accepts 8.
bool TuStatusIs1Or8(PPCRegister& r3) {
  return r3.s32 == 1 || r3.s32 == 8;
}

// DISABLED: the hooks below are not wired in config/nocturnerecomp_tu_hooks.toml (the
// replacement lament.WMA is copyrighted and cannot be shipped); the cvar is no
// longer exposed in the settings overlay.
//
// The update swaps the credits music ("I Am the Wind", track "IAM") for
// "Admiration of Clan". That is two coupled changes -- the streamed file and
// the sound-test name -- so both follow the `credits_music_admiration` cvar
// (curated settings overlay), and both also require the replacement file to
// exist: without it the vanilla WMA plays and the vanilla name must stay.
REXCVAR_DEFINE_BOOL(credits_music_admiration, true, "Game",
                    "Play \"Admiration of Clan\" instead of \"I Am the Wind\" as the credits music");

namespace {

constexpr const char kLamentPath[] = "UPDATE:\\data\\sound\\lament.WMA";

bool AdmirationMusicActive() {
  if (!REXCVAR_GET(credits_music_admiration)) {
    return false;
  }
  static const bool available = REX_KERNEL_FS()->ResolvePath(kLamentPath) != nullptr;
  return available;
}

}  // namespace

// Track "IAM" streams the replacement instrumental UPDATE:\data\sound\lament.WMA
// when enabled and the update's data folder is mounted; the vanilla WMA stays
// otherwise. The music-path builder sub_8250E4F0 (patched 0x8250E610) has just
// assembled "<music dir><name>.WMA" at r1+0x80 when this runs (0x8250E584);
// r31 is the track name.
void TuLamentTrackRedirect(PPCRegister& r1, PPCRegister& r31) {
  const char* name = reinterpret_cast<const char*>(GuestPtr(r31.u32));
  if (std::strcmp(name, "IAM") != 0 || !AdmirationMusicActive()) {
    return;
  }
  std::memcpy(GuestPtr(r1.u32 + 0x80), kLamentPath, sizeof(kLamentPath));
}

// sub_823D9080 (patched 0x823D91F0): when the object's 0x100 flag is set the
// update plays sound effect 0x6CB (sub_82347BD0; vanilla sub_82347AE0) before
// clearing it. Runs at 0x823D90B0, only reached on that arm. Midasm hooks get
// no context, so the call uses an isolated one borrowed from the current
// thread, like rex::ppc::ImportFunction's auto-isolating call.
extern "C" REX_FUNC(sub_82347AE0);

// Calls a recompiled guest function with an isolated context borrowed from the
// current thread, returning r3. Midasm hooks get no context of their own.
uint64_t CallGuest(void (*fn)(PPCContext&, uint8_t*), uint64_t r3, uint64_t r4 = 0,
                   uint64_t r5 = 0) {
  auto* parent = rex::runtime::ThreadState::Get()->context();
  PPCContext ctx{};
  ctx.r1 = parent->r1;
  ctx.r1.u32 -= 0x70;
  ctx.r13 = parent->r13;
  ctx.fpscr = parent->fpscr;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, REX_KERNEL_STATE()->memory()->virtual_membase());
  parent->fpscr = ctx.fpscr;
  return ctx.r3.u64;
}

void TuPlayFlagSound() {
  CallGuest(sub_82347AE0, 0x6CB);
}

// The sound test shows each track's name through sub_82407B40, a table lookup
// (pointer table at 0x82D3E658) returning the string pointer in r3. The update
// renames the "I Am the Wind" entry (string at 0x82213F38) to "Admiration of
// Clan". The new name is 4 bytes longer than the old one, so it cannot be
// written over it; it lives in a guest allocation and the lookup result is
// swapped instead, which also leaves the table (part of .data, which the game
// reloads in places) alone. Names use the "big first letter" encoding
// (docs/reverse-engineering.md): first char = ASCII - 0x20, words separated by
// 0, terminated by 0xFF.
void TuSoundTestTrackName(PPCRegister& r3) {
  constexpr uint32_t kOldName = 0x82213F38;
  static const uint8_t kNewName[] = {'!', 'D', 'M', 'I', 'R', 'A', 'T', 'I', 'O', 'N',
                                     0,   'O', 'F', 0,   '#', 'L', 'A', 'N', 0xFF, 0};
  static uint32_t new_name = 0;
  if (r3.u32 != kOldName || !AdmirationMusicActive()) {
    return;
  }
  if (!new_name) {
    new_name = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(sizeof(kNewName));
    if (!new_name) {
      return;
    }
    std::memcpy(GuestPtr(new_name), kNewName, sizeof(kNewName));
  }
  r3.u32 = new_name;
}

// sub_82385DF0 (patched 0x82385EE0), state 6 / sub-state 2 of the Sword Lord's
// death: right before the five-piece spawn loop the update spawns one extra
// entity, but only while the global at 0x83133AEC (patched 0x831338AC) is 0xD.
// It is what makes the Vandal Sword appear. The entity comes from its own pool
// (sub_82377620 over r28+0x7800..r28+0x9000, 0xC0-byte slots) and is set up
// with the init routine sub_822F1B58 using the id in the halfword pair at
// 0x831CF35C (patched 0x831CF11C), facing like the boss (+0x14), shifted 0xE
// sideways and 0x16 up. When it exists the loop starts at index 1 instead of
// 0. Runs after the `or r29,r25,r25` at 0x82386690 that zeroes the index.
extern "C" REX_FUNC(sub_82377620);
extern "C" REX_FUNC(sub_822F1B58);

void TuExtraSpawn(PPCRegister& r28, PPCRegister& r29, PPCRegister& r31) {
  constexpr uint32_t kStageGlobal = 0x83133AEC;
  constexpr uint32_t kInitIdWord = 0x831CF35C;
  if (ReadU32(kStageGlobal) != 0xD) {
    return;
  }
  const uint32_t entity = static_cast<uint32_t>(
      CallGuest(sub_82377620, r28.u32 + 0x7800, r28.u32 + 0x9000));
  if (!entity) {
    return;
  }
  CallGuest(sub_822F1B58, ReadU32(kInitIdWord) & 0xFFFF, r31.u32, entity);
  const uint16_t facing = ReadU16(r31.u32 + 0x14);
  WriteU16(entity + 0x14, facing);
  WriteU16(entity, ReadU16(entity) + (facing ? 0xE : -0xE));
  WriteU16(entity + 4, ReadU16(entity + 4) - 0x16);
  r29.u64 = 1;
}

// Dialogue text. The update repacks the page blob at 0x82D58048..
// (page table at 0x82D5915C), so it cannot be copied. What changed in the
// dialogue pages is that every page's trailing descriptor loses its 0x0A byte
// ("3C 0A 11 .. 01" -> "3C 11 .. 01"). tu_dialogue_patch.h holds the
// equivalent in-place byte edits for the vanilla layout (the edit shifts only
// the descriptor's 2-4 bytes into its zero padding, so every page start and
// mid-page pointer keeps working). The update's shorter credits stream (at
// 0x82D589A8) is in that file too but DISABLED, as it goes with the credits
// song swap. The blob is part of .data, which the game reloads in places, so
// the patch is re-checked (memcmp) every time one of the readers runs: the
// page-table readers and the page interpreter sub_8239CD00.
void TuPatchDialoguePages() {
  for (const auto& patch : tu_dialogue::kPatches) {
    uint8_t* dst = GuestPtr(patch.address);
    if (std::memcmp(dst, patch.data, patch.size) != 0) {
      std::memcpy(dst, patch.data, patch.size);
    }
  }
}

// DISABLED (config hooks commented out, with the credits song swap): the
// update also scales the credits scroll by 3/4 in sub_8239CD00
// (patched 0x8239CE70): the timer increments for the 6 and 12 pixel line
// advances (0x18 -> 0x12, 0x30 -> 0x24) and the per-line timer reloads
// (0x4C -> 0x39, 4 -> 3). All four run after the original instruction.
void TuCreditsScrollTimer1(PPCRegister& r10) {
  r10.u64 -= 0x6;
}

void TuCreditsScrollTimer2(PPCRegister& r10) {
  r10.u64 -= 0xC;
}

void TuCreditsLineTimer(PPCRegister& r11) {
  r11.u64 = 0x39;
}

void TuCreditsRowTimer(PPCRegister& r11) {
  r11.u64 = 3;
}
