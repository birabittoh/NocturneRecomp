#include "jp_voices.h"

#include <filesystem>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "target.h"

REXCVAR_DEFINE_BOOL(jp_voices, true, "Game",
                    "Japanese voices, when the JP release is installed")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nocturne {
namespace {

constexpr const char* kGuestSoundPath = "game:\\DATA\\sound";
constexpr const char* kMount = "\\Device\\NocturneJpSound";
// Bigger in JP (it has twice the cues), so it tells the two folders apart.
constexpr const char* kProbeBank = "sd_koa2.xsb";

}  // namespace

void InstallJpVoices(rex::Runtime* runtime) {
  const std::filesystem::path& jp = JpDir();
  if (jp.empty() || !runtime || !runtime->file_system())
    return;
  std::error_code ec;
  if (std::filesystem::equivalent(jp, GameDataRoot(), ec)) {
    REXLOG_INFO("jp voices: JP is the mounted folder, nothing to link");
    return;
  }
  if (!REXCVAR_GET(jp_voices)) {
    REXLOG_INFO("jp voices: off, playing World's");
    return;
  }

  const std::filesystem::path jp_sound = jp / "DATA" / "sound";
  if (!std::filesystem::is_directory(jp_sound, ec)) {
    REXLOG_WARN("jp voices: {} is missing", jp_sound.string());
    return;
  }

  auto* vfs = runtime->file_system();
  rex::filesystem::Entry* world_sound = vfs->ResolvePath(kGuestSoundPath);
  if (!world_sound) {
    REXLOG_WARN("jp voices: no {} to replace", kGuestSoundPath);
    return;
  }
  const std::string world_sound_path = world_sound->absolute_path();

  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMount, jp_sound, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("jp voices: could not mount {}", jp_sound.string());
    return;
  }
  // Shadow the directory, not each file: see font_overlay.h.
  if (!vfs->RegisterSymbolicLink(world_sound_path, kMount)) {
    REXLOG_WARN("jp voices: could not link {}", world_sound_path);
    vfs->UnregisterDevice(kMount);
    return;
  }

  // Check through the open path's own steps: resolve the parent, then the child.
  rex::filesystem::Entry* parent = vfs->ResolvePath("game:\\data\\sound");
  rex::filesystem::Entry* probe = parent ? parent->GetChild(kProbeBank) : nullptr;
  const auto jp_size = std::filesystem::file_size(jp_sound / kProbeBank, ec);
  if (!probe || ec || probe->size() != jp_size) {
    REXLOG_WARN("jp voices: {} does not resolve to JP's copy", kProbeBank);
    return;
  }
  REXLOG_INFO("jp voices: DATA\\sound now reads {}", jp_sound.string());
}

}  // namespace nocturne
