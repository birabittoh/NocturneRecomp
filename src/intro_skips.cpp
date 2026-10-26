// Two shortcuts through the long road to gameplay:
//
//   skip_intros   boot straight to the Xbox 360 main menu, past the Konami
//                 and Digital Eclipse logos and the disclaimer;
//   skip_richter  go from name entry straight to the Alucard intro, past the
//                 Richter prologue.
//
// Both only take paths the game itself has. The hooks are wired up in
// nocturnerecomp_config.toml.
#include "generated/nocturnerecomp_init.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(skip_intros, false, "Game",
                    "Skip the logos and disclaimer and boot straight to the main menu");
REXCVAR_DEFINE_BOOL(skip_richter, false, "Game",
                    "Skip the Richter prologue: start a new game at the Alucard intro");

namespace {

uint8_t* GuestPtr(uint32_t guest_address) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual(guest_address);
}

void WriteU32(uint32_t guest_address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(GuestPtr(guest_address), value);
}

uint32_t ReadU32(uint32_t guest_address) {
  return rex::memory::load_and_swap<uint32_t>(GuestPtr(guest_address));
}

// The PS1 game's globals, at their sotn-decomp names.
constexpr uint32_t kStageId = 0x83133AEC;            // g_StageId
constexpr uint32_t kPlayableCharacter = 0x83133AF4;  // g_PlayableCharacter, 0: Alucard
constexpr uint32_t kStageSt0 = 0x1F;                 // Richter prologue
constexpr uint32_t kStageNo3 = 0x41;                 // Castle Entrance, first visit

// g_Status fields the prologue rewards are computed from.
constexpr uint32_t kStatusHp = 0x83174B7C;
constexpr uint32_t kStatusHpMax = 0x83174B80;
constexpr uint32_t kStatusHearts = 0x83174B84;
constexpr uint32_t kStatusSubweapon = 0x83174BD8;
// The prologue HUD's notes: set when Richter's HP bar went up (Maria's
// potion), and his hearts when the Dracula fight began.
constexpr uint32_t kPrologueHealed = 0x8314E924;
constexpr uint32_t kPrologueFightHearts = 0x8314E92C;
// g_Settings' time attack records, 32 words, (h * 100 + m) * 100 + s; the
// first is the prologue's Dracula.
constexpr uint32_t kTimeAttackRecords = 0x83173C30;
constexpr uint32_t kTimeAttackRecordCount = 32;
constexpr uint32_t kTimeAttackUnlocks = 0x83173D08;  // two words next to them

// Set when name entry was redirected to NO3; consumed by the player init
// that follows.
bool g_prologue_skipped = false;

}  // namespace

// The boot logos page (front-end page 0, ctor sub_825C6320) runs three timed
// fade-in / hold / fade-out sets: Konami (+0x228..+0x230), Digital Eclipse
// (+0x21C..+0x224) and the disclaimer (+0x240..+0x248), plus a set only some
// languages use (+0x234..+0x23C). Its update (sub_825C64D0) works through
// whichever is nonzero, and once all are zero it posts the switch to page 1,
// the main menu. Pressing A or Start zeroes the current logo's set
// (sub_825C5E78); this zeroes all of them on the first update.
void SkipIntrosLogoUpdate(PPCRegister& r3) {
  if (!REXCVAR_GET(skip_intros)) {
    return;
  }
  for (uint32_t offset = 0x21C; offset <= 0x248; offset += 4) {
    WriteU32(r3.u32 + offset, 0);
  }
}

// Name entry (sub_82397068), on confirming the name, picks the first stage:
// ST0 for Alucard, NO3 for the "RICHTER" name (r10 = g_PlayableCharacter ?
// NO3 : ST0, stored to g_StageId at 0x823975B4). The prologue itself ends the
// same way, ST0 -> NO3 (sub_823A4CE8), so sending Alucard to NO3 here is the
// prologue's own ending without the prologue.
void SkipRichterStageSelect(PPCRegister& r10) {
  g_prologue_skipped = false;
  if (!REXCVAR_GET(skip_richter) || r10.u32 != kStageSt0) {
    return;
  }
  r10.u64 = kStageNo3;
  g_prologue_skipped = true;
}

// The stage loader (sub_82250D10) calls the player init sub_8225AED0
// (InitStatsAndGear) at 0x822514E8. Loading NO3 as Alucard it hands out the
// prologue rewards, judged on what Richter left behind: his HP against his
// max, his hearts, whether Maria healed him, his hearts going into the
// Dracula fight and the Dracula time attack record. With no prologue played
// those are all zero, which reads as the best possible run (+5 to every stat
// and more), and the time attack records were never cleared either (the
// Richter init does that). So stand in an unremarkable run first: Richter
// beat Dracula on low HP in six minutes with hearts to spare, which earns
// +2 STR and +1 CON and no items.
void SkipRichterBeforeInit() {
  if (!g_prologue_skipped) {
    return;
  }
  if (ReadU32(kStageId) != kStageNo3 || ReadU32(kPlayableCharacter) != 0) {
    g_prologue_skipped = false;
    return;
  }
  for (uint32_t i = 0; i < kTimeAttackRecordCount; ++i) {
    WriteU32(kTimeAttackRecords + 4 * i, 0);
  }
  WriteU32(kTimeAttackUnlocks, 0);
  WriteU32(kTimeAttackUnlocks + 4, 0);
  WriteU32(kTimeAttackRecords, 600);  // 0:06:00
  WriteU32(kStatusHp, 10);
  WriteU32(kStatusHpMax, 50);
  WriteU32(kStatusHearts, 10);
  WriteU32(kStatusSubweapon, 0);
  WriteU32(kPrologueHealed, 0);
  WriteU32(kPrologueFightHearts, 10);
}

// Right after the init: take the stand-in Dracula record back out, so the
// time attack screen doesn't show a prologue that was never played.
void SkipRichterAfterInit() {
  if (!g_prologue_skipped) {
    return;
  }
  g_prologue_skipped = false;
  WriteU32(kTimeAttackRecords, 0);
}
