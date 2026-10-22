// Applies a randomizer seed (src/rando/rando.h) to the running game, and
// takes it back off, without booting a patched xex or restarting.
//
// A seed is a PatchSet: guest byte runs against the base image. Applying one
// writes the bytes into guest memory and then makes the code changes take
// effect:
//   * Recompiled functions execute natively and never re-read .text, so each
//     function whose instructions the seed changes is overridden (through
//     rex::runtime::FunctionDispatcher) with a thunk that interprets it from
//     guest memory (src/ppc_interpreter.h). Functions using instructions the
//     interpreter doesn't cover stay native with that patch inert; the count
//     is reported to the overlay.
//   * "Code caves" (new code the seed writes into zero space outside the
//     recompiled range, reached through patched branches) have no recompiled
//     function at all. Calls to them land in the dispatcher's fallback
//     resolver, which binds an interpreter thunk per entry point.
// Reverting restores the base bytes and every overridden function.
//
// History, kept because it was learned the hard way:
//   * The randomizer used to work by booting a patched Rando.xex. A comment
//     there warned that patching guest memory after boot hangs the game in
//     its text stream scanner (0x82427F88), blaming .data being reloaded from
//     disc. That attempt diffed the raw files, though, and the base xex is
//     AES-encrypted while the randomizer's output is plaintext, so it sprayed
//     garbage. A probe of the real diff (every changed run, at the main menu)
//     found live memory identical to the base image everywhere, and the seed
//     played correctly when applied there.
//   * Code and read-only data are mapped read-only in guest memory, with real
//     host page protection, so writes there go through a protection toggle.
//
// Applying and reverting happen on the guest thread between frames (the mod
// registry tick) and only while no save is loaded: the overlay posts a
// request, the tick performs or rejects it. The active seed is saved in the
// user data root and applied again at boot, before guest code runs, so a
// randomized save keeps its world across launches.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "rando/rando.h"

namespace rex {
class Runtime;
}

namespace nocturne {

class RandoRuntime {
 public:
  // Call from OnPostLoadXexImage: the module is mapped and no guest code has
  // run. Registers the guest tick and applies the saved active seed, if any.
  void Bind(rex::Runtime* runtime, const std::filesystem::path& game_data_root,
            const std::filesystem::path& user_data_root);

  // Overlay requests, performed by the next guest tick. Rejected while a
  // save is loaded.
  void RequestApply(std::shared_ptr<const rando::Result> seed);
  void RequestRevert();

  struct State {
    bool supported = false;
    std::string unsupported_reason;
    bool request_pending = false;
    std::shared_ptr<const rando::Result> active;  // null: vanilla
    // Patched functions the interpreter couldn't take over; those changes
    // are inert.
    size_t inactive_code_patches = 0;
    // Outcome of the last request or of the boot-time restore.
    std::string message;
  };
  State state() const;

  // True while the game has a save loaded; seeds can't change then. Safe to
  // call from any thread once bound.
  bool SaveLoaded() const;

 private:
  void Tick();
  bool Apply(std::shared_ptr<const rando::Result> seed, std::string* error);
  void Revert();
  void SaveActive() const;

  rex::Runtime* runtime_ = nullptr;
  std::filesystem::path base_xex_;
  std::filesystem::path active_path_;
  std::shared_ptr<const rando::BaseXex> base_;

  mutable std::mutex mutex_;
  State state_;
  enum class Request { kNone, kApply, kRevert } request_ = Request::kNone;
  std::shared_ptr<const rando::Result> requested_;
};

RandoRuntime& GetRandoRuntime();

}  // namespace nocturne
