// Applies a randomizer seed (src/rando/rando.h) to the running game, and
// takes it back off, without booting a patched xex or restarting.
//
// A seed is a PatchSet: guest byte runs against the base image. Applying one
// writes the bytes into guest memory. Recompiled functions never re-read
// their instructions from guest memory, so the code changes take effect
// through the hooks of src/rando_hooks.h: every instruction the randomizer
// can patch, and every code cave it can branch to, is compiled in already,
// and applying a world tells those hooks which patch each site now holds.
// Patched instructions no hook knows stay inert; the count is reported to
// the overlay. Reverting turns the hooks off and restores the base bytes.
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
// registry tick) and only at the title / main menu: the overlay posts a
// request, the tick performs or rejects it. The chosen seed (what New Game
// gets) is saved in the user data root and applied again at boot, before
// guest code runs.
//
// Each save slot also remembers the world it was written in
// (user_root/rando_slots/<slot>.txt). Loading a save applies that world on
// the guest thread just before the game unpacks the save, whatever seed is
// chosen for New Game; back at the title screen the New Game seed returns.
// Saves with no record (made before this existed) load as vanilla.
//
// The enhancements (src/enhancements.h) are written over whichever world is
// in memory, vanilla or seed, so what guest memory holds is always one
// composed PatchSet (rando::ComposeWorld). They are game settings, not part
// of a seed or a save: a change made while a game runs is applied on the next
// tick, where a seed waits for the title screen.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "rando/rando.h"

namespace rex {
class Runtime;
}

namespace nocturne {

class RandoRuntime {
 public:
  // Call from OnPostLoadXexImage: the module is mapped and no guest code has
  // run. Keeps a copy of the image as the randomizer's base, registers the
  // guest tick and applies the saved active seed, if any.
  void Bind(rex::Runtime* runtime, const std::filesystem::path& user_data_root);

  // The image as loaded, which seeds are generated from; null when the
  // randomizer isn't supported.
  std::shared_ptr<const rando::BaseXex> base() const { return base_; }

  // Overlay requests, performed by the next guest tick. Rejected unless the
  // game is at the title / main menu.
  void RequestApply(std::shared_ptr<const rando::Result> seed);
  void RequestRevert();

  // Save-slot binding, from the guest hooks at the bottom of the .cpp (guest
  // thread). A save is bound to the world it was written in; loading it
  // brings that world back, and the title screen restores the New Game one.
  void OnSaveRead(const std::string& slot);
  void OnSaveLoading();
  void OnSaveWritten(const std::string& slot);
  void OnSaveDeleted(const std::string& slot);

  struct State {
    bool supported = false;
    std::string unsupported_reason;
    bool request_pending = false;
    std::shared_ptr<const rando::Result> active;  // in guest memory now; null: vanilla
    // Enhancements in guest memory now, over `active`.
    rando::Enhancements enhancements;
    // What New Game gets (the overlay's choice); null: vanilla. Differs
    // from `active` only while a save bound to another world is loaded.
    std::shared_ptr<const rando::Result> new_game;
    // Patched instructions no hook knows; those changes are inert.
    size_t inactive_code_patches = 0;
    // Outcome of the last request or of the boot-time restore.
    std::string message;
  };
  State state() const;

  // True at the title / main menu, the only place seeds can change: from
  // New Game on (file select included) the game is running and may already
  // have read the data a seed patches. Safe to call from any thread once
  // bound.
  bool InFrontEnd() const;

 private:
  void Tick();
  // Callers hold apply_mutex_: the tick and the save hooks can run on
  // different threads.
  // Puts `seed` (null: vanilla) with the current enhancements in memory.
  bool Apply(std::shared_ptr<const rando::Result> seed, std::string* error);
  // Back to the base image: no seed and no enhancements.
  void Revert();
  // Apply, unless that world is already in memory.
  bool ApplyWorld(std::shared_ptr<const rando::Result> seed, std::string* error);
  // The vanilla game with the enhancements, or the bare base image if even
  // that fails.
  void ApplyVanilla();

  // Seed files: options plus "hash=", or "vanilla=1". Loading regenerates
  // the seed (cached) and refuses it if the hash no longer matches.
  std::shared_ptr<const rando::Result> LoadSeedFile(const std::filesystem::path& path,
                                                    std::string* error);
  void WriteSeedFile(const std::filesystem::path& path,
                     const std::shared_ptr<const rando::Result>& seed) const;
  static bool IsVanillaFile(const std::filesystem::path& path);
  std::filesystem::path SlotPath(const std::string& slot) const;

  rex::Runtime* runtime_ = nullptr;
  std::filesystem::path active_path_;
  std::filesystem::path slots_dir_;
  std::shared_ptr<const rando::BaseXex> base_;  // set by Bind, then constant

  std::mutex apply_mutex_;
  std::vector<std::shared_ptr<const rando::Result>> seed_cache_;  // under apply_mutex_
  std::string last_read_slot_;  // guest thread
  // What guest memory holds over the base image; null: nothing.
  std::shared_ptr<const rando::PatchSet> applied_;  // under apply_mutex_
  // The enhancements that last failed to apply; not retried until they change.
  std::optional<rando::Enhancements> failed_enhancements_;  // under apply_mutex_
  bool was_front_end_ = false;  // tick

  mutable std::mutex mutex_;
  State state_;
  enum class Request { kNone, kApply, kRevert } request_ = Request::kNone;
  std::shared_ptr<const rando::Result> requested_;
};

RandoRuntime& GetRandoRuntime();

}  // namespace nocturne
