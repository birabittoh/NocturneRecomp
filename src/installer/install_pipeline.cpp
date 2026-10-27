#include "install_pipeline.h"

#include <fstream>
#include <iterator>
#include <vector>

#include <rex/logging.h>

#include "intro_text.h"
#include "release_id.h"

namespace nocturne {
namespace {

namespace fs = std::filesystem;

constexpr const char* kExecutable = "default.xex";

bool IsContentUri(const std::string& path) {
  return path.starts_with("content://");
}

void Report(const InstallHooks& hooks, Phase phase, PhaseState state) {
  if (hooks.phase)
    hooks.phase(phase, state);
}

// A file picked inside an extracted folder means the folder.
fs::path Resolve(const std::string& picked) {
  fs::path path(picked);
  std::error_code ec;
  if (!IsContentUri(picked) && fs::is_regular_file(path, ec) &&
      SameFileName(path.filename().string(), kExecutable))
    return path.parent_path();
  return path;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

fs::path ChildNamed(const fs::path& dir, std::string_view name) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (SameFileName(entry.path().filename().string(), name))
      return entry.path();
  }
  return {};
}

// Known releases by their default.xex hash.
SourceInfo FromHash(const std::vector<uint8_t>& file, const std::string& unknown) {
  const std::string hash = Sha256Hex(file);
  const Release* release = FindRelease(hash);
  if (!release) {
    REXLOG_WARN("Unknown default.xex: {}", hash);
    return {{}, unknown};
  }
  SourceInfo info{release->name, {}, release->languages};
  info.id = release->id;
  info.startable = release->startable;
  if (!release->supported)
    info.error = TrErr("err_unsupported", info.release);
  return info;
}

// Prepared folders are taken as they are, named when their executable says
// which release they came from.
SourceInfo IdentifyGameDirectory(const fs::path& dir) {
  std::vector<uint8_t> file;
  const fs::path xex = ChildNamed(dir, kExecutable);
  if (xex.empty() || !ReadFile(xex, file))
    return {};
  SourceInfo info = FromHash(file, {});
  info.error.clear();
  return info;
}

std::string RunLaterPhases(const fs::path& dir, const InstallHooks& hooks) {
  (void)dir;
  (void)hooks;
  return {};
}

}  // namespace

const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::kExtract: return "Extraction";
  }
  return "";
}

bool IsGameDirectory(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / kExecutable, ec) &&
         fs::is_directory(dir / "MEDIA", ec);
}

bool NeedsConversion(const fs::path&) {
  return false;
}

bool NeedsPatching(const fs::path&) {
  return false;
}

bool IsOutdated(const fs::path&) {
  return false;
}

SourceInfo IdentifySource(const std::string& picked) {
  std::error_code ec;
  const fs::path path = Resolve(picked);
  if (!IsContentUri(picked) && fs::is_directory(path, ec)) {
    if (!IsGameDirectory(path))
      return {{}, TrErr("err_not_extracted")};
    return IdentifyGameDirectory(path);
  }
  std::vector<uint8_t> xex;
  if (std::string error = ReadPackageFile(picked, kExecutable, xex); !error.empty())
    return {{}, error};
  SourceInfo info = FromHash(xex, TrErr("err_xbla_unknown"));
  info.required_bytes = MeasurePackage(picked);
  return info;
}

std::string InstallDirectory(const fs::path& dir, const InstallHooks& hooks) {
  Report(hooks, Phase::kExtract, PhaseState::kSkipped);
  return RunLaterPhases(dir, hooks);
}

std::string Install(const std::string& picked, const fs::path& assets, const InstallHooks& hooks,
                    fs::path& dir) {
  Report(hooks, Phase::kExtract, PhaseState::kActive);
  std::string error = ExtractPackage(picked, assets, kExecutable, hooks.progress);
  if (error.empty() && !IsGameDirectory(assets))
    error = TrErr("err_incomplete");
  if (!error.empty())
    return error;
  Report(hooks, Phase::kExtract, PhaseState::kDone);
  dir = assets;
  return RunLaterPhases(dir, hooks);
}

}  // namespace nocturne
