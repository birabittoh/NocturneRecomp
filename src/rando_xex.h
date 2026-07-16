// Randomizer support (formerly the rando_patch mod; made a base feature).
//
// SOTN_XB_RANDO takes a decrypted default.xex and writes a patched
// "Rando.xex" of identical size/layout -- no relocation or resizing, just
// bytes changed in place (see game/SOTN_XB_RANDO_Feb_6th_2025/
// GettingStarted.txt). Because the layout is identical, applying the
// randomizer is exactly "boot the patched image instead of the base one":
// redirect the module path in ReXApp::OnLoadXexImage (called before
// Runtime::LoadXexImage maps anything into guest memory), and every table
// the randomizer touched loads exactly as a console booting Rando.xex would
// see it.
//
// Timing is load-bearing -- do NOT reimplement this as a post-launch guest-
// memory patch. The mod this replaces tried applying (address, bytes) diffs
// to live guest memory and hit two distinct failure modes before settling
// here:
//   1. A guest-address translation bug (PE section raw pointers are
//      relative to the XEX's PE base at header_size=0x4000, not to the
//      file start), which sprayed every write 0x4000 bytes off target.
//   2. Even with correct addresses, parts of .data (dialogue/entity
//      opcode-stream buffers around guest 0x82Dxxxxx) are reloaded from
//      disc shortly after boot; patching them after guest code has run
//      corrupts the live streams and hangs the game's stream scanner
//      (guest 0x82427F88 spins forever on unhandled opcode bytes).
// Booting the patched image sidesteps both by construction.
//
// Booting the patched image only covers data, though. The randomizer's
// .text diffs are real instruction patches (nop'd branches, changed
// immediates -- an earlier investigation claimed codegen output was
// byte-identical either way; that was wrong), and recompiled code executes
// natively without ever re-reading .text from guest memory, so those
// changes are inert in the booted image. InstallRandoOverrides (defined in
// src/rando_xex.cpp) closes that gap at runtime: it diffs the patched xex
// against the base image, maps every differing instruction to its
// recompiled function, and overrides each affected function with a thunk
// that interprets it straight from guest memory (src/ppc_interpreter.h) --
// so any randomizer output works, with no per-seed build step. (An earlier
// build-time approach shipped pre-recompiled override bodies for one
// specific Rando.xex; the interpreter replaces it.)

#pragma once

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

// Filename of a randomizer-patched xex inside the game data root (e.g.
// SOTN_XB_RANDO's "Rando.xex"). Empty = disabled.
REXCVAR_DECLARE(std::string, rando_xex_name);

namespace nocturne {

// Called from OnLoadXexImage. `module_path` is the guest VFS path of the
// module about to be loaded (e.g. "game:\default.xex", where "game:" maps to
// `game_data_root`); on success it's rewritten to point at the configured
// patched xex. Validation happens against the host-side files: the patched
// xex must exist in the game data root, be a XEX2 image, and match the base
// image's size (the randomizer patches in place, so any size difference
// means it isn't a same-layout variant and guest addresses wouldn't line
// up). It also refuses on a title-update build (a "<module>p" delta patch
// staged next to the boot module. On any mismatch this warns and leaves the
// base image to boot.
//
// Returns true when the boot module was redirected, filling out_base_xex/
// out_patched_xex with the host paths of the two images; the caller must
// then also call InstallRandoOverrides once the runtime is up, or the
// randomizer's code changes stay inert (see the file header comment).
inline bool MaybeApplyRandoXex(const std::filesystem::path& game_data_root,
                               std::string& module_path,
                               std::filesystem::path* out_base_xex,
                               std::filesystem::path* out_patched_xex) {
  std::string configured = REXCVAR_GET(rando_xex_name);
  if (configured.empty()) {
    return false;
  }

  size_t sep = module_path.find_last_of("\\/");
  if (sep == std::string::npos) {
    REXLOG_WARN("[rando] unexpected module path '{}' -- booting the base image", module_path);
    return false;
  }
  std::string base_name = module_path.substr(sep + 1);

  // UserModule::LoadFromFile (rexglue-sdk/src/system/user_module.cpp) applies
  // any sibling "<module>p" file it finds next to the boot module as a XEX2
  // delta patch (title update), independently of and before any of this.
  // The rando .text diff below is computed against the *unpatched* base_xex,
  // so if a TU is staged, the diff and the resulting interpreted bounds are
  // against the wrong base image.
  std::error_code tu_ec;
  if (std::filesystem::exists(game_data_root / (base_name + "p"), tu_ec)) {
    REXLOG_WARN("[rando] {} is a title-update build ({}p is staged) -- randomizer .text "
                "interpretation only supports the vanilla base image; booting the base "
                "image without rando overrides",
                base_name, base_name);
    return false;
  }

  std::error_code ec;
  auto patched_size = std::filesystem::file_size(game_data_root / configured, ec);
  if (ec) {
    REXLOG_WARN("[rando] rando_xex_name is set but {} isn't readable -- booting the base image",
                (game_data_root / configured).string());
    return false;
  }
  auto base_size = std::filesystem::file_size(game_data_root / base_name, ec);
  if (!ec && patched_size != base_size) {
    REXLOG_WARN(
        "[rando] {} is {} bytes but {} is {} -- not a same-layout in-place variant; "
        "booting the base image",
        configured, patched_size, base_name, base_size);
    return false;
  }

  char magic[4] = {};
  std::FILE* f = std::fopen((game_data_root / configured).string().c_str(), "rb");
  bool is_xex2 = f && std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "XEX2", 4) == 0;
  if (f) {
    std::fclose(f);
  }
  if (!is_xex2) {
    REXLOG_WARN("[rando] {} isn't a XEX2 image -- booting the base image", configured);
    return false;
  }

  if (out_base_xex) {
    *out_base_xex = game_data_root / base_name;
  }
  if (out_patched_xex) {
    *out_patched_xex = game_data_root / configured;
  }
  module_path = module_path.substr(0, sep + 1) + configured;
  REXLOG_INFO("[rando] booting {} instead of {}", module_path, base_name);
  return true;
}

// Makes the patched image's .text changes take effect (see the file header
// comment): diffs `patched_xex` against `base_xex` over the code range,
// maps the differing instructions to recompiled functions, and overrides
// each one via rex::runtime::FunctionDispatcher with a thunk that
// interprets it from guest memory. Functions using instructions the
// interpreter doesn't cover are left native with a warning (that patch
// stays inert) rather than misexecuted. Call once after the module is
// loaded (every recompiled function's default dispatcher entry exists by
// then) and only when MaybeApplyRandoXex returned true -- guest memory
// must hold the patched image.
void InstallRandoOverrides(rex::Runtime* runtime, const std::filesystem::path& base_xex,
                           const std::filesystem::path& patched_xex);

}  // namespace nocturne
