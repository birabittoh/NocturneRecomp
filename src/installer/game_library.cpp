#include "game_library.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <sstream>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include "install_pipeline.h"

REXCVAR_DEFINE_STRING(game_version, "", "Game",
                      "A game directory to start in place of the library's releases, for this "
                      "run only")
    .hidden();

namespace nocturne {
namespace {

namespace fs = std::filesystem;

constexpr const char* kInfoFile = "nocturne-version.toml";
constexpr const char* kMigrating = "migrating.toml";
constexpr const char* kRootXex = "default.xex";
constexpr const char* kRootXexSource = "default.xex.source";
// Library order.
constexpr std::array<std::string_view, 2> kOrder = {"xbla-world", "xbla-jp"};
// IDA databases stay next to the root executable they were made from.
constexpr std::array<std::string_view, 7> kIdaFiles = {".i64", ".idb", ".id0", ".id1",
                                                       ".id2", ".nam", ".til"};

std::string Utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

fs::path FromUtf8(std::string_view text) {
  return fs::path(std::u8string(text.begin(), text.end()));
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return bool(out);
}

std::string TomlString(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out + "\"";
}

bool IsVersionFolder(const fs::path& dir) {
  std::error_code ec;
  return fs::is_directory(dir, ec) &&
         (IsGameDirectory(dir) || fs::is_regular_file(dir / kInfoFile, ec));
}

// What a flat tree's move into its version folder leaves in the root.
bool StaysInRoot(const fs::path& root, const fs::path& entry) {
  const std::string name = entry.filename().string();
  std::error_code ec;
  // Dot files are the launcher's (Goopie's achievement cache, shortcut icon).
  if (name == kMigrating || name == kRootXexSource || EndsWith(name, ".partial") ||
      name.starts_with("."))
    return true;
  if (name == kRootXex && fs::is_regular_file(root / kRootXexSource, ec))
    return true;
  if (name.starts_with(kRootXex) &&
      std::any_of(kIdaFiles.begin(), kIdaFiles.end(), [&](auto e) { return EndsWith(name, e); }))
    return true;
  return IsVersionFolder(entry);
}

// Moves the root's own tree into `folder`, through a .partial folder that a
// later launch finishes if this one is killed.
bool FinishMove(const fs::path& root, const std::string& folder) {
  const fs::path dest = root / folder;
  const fs::path partial = root / (folder + ".partial");
  std::error_code ec;
  if (!fs::exists(dest, ec)) {
    fs::create_directories(partial, ec);
    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
      if (!StaysInRoot(root, entry.path()))
        entries.push_back(entry.path());
    }
    for (const fs::path& from : entries) {
      fs::rename(from, partial / from.filename(), ec);
      if (ec) {
        REXLOG_ERROR("Could not move {} into {}: {}", from.string(), partial.string(), ec.message());
        return false;
      }
    }
    RenameRetrying(partial, dest, ec);
    if (ec) {
      REXLOG_ERROR("Could not rename {} to {}: {}", partial.string(), dest.string(), ec.message());
      return false;
    }
    REXLOG_INFO("Moved the game files in {} to {}", root.string(), dest.string());
  }
  fs::remove(root / kMigrating, ec);
  return true;
}

std::string MigratingFolder(const fs::path& root) {
  try {
    const toml::table table = toml::parse_file(Utf8(root / kMigrating));
    return table["folder"].value_or(std::string());
  } catch (const toml::parse_error&) {
    return {};
  }
}

void Describe(GameVersion& version, bool write) {
  try {
    const toml::table table = toml::parse_file(Utf8(version.dir / kInfoFile));
    version.id = table["id"].value_or(std::string());
    version.release = table["release"].value_or(std::string());
    version.languages = uint32_t(table["languages"].value_or(int64_t(0)));
  } catch (const toml::parse_error&) {
    const SourceInfo info = IdentifySource(version.dir.string());
    version.id = info.id;
    version.release = info.release;
    version.languages = info.languages;
    if (write && !info.release.empty())
      WriteVersionInfo(version.dir, info.id, info.release, info.languages);
  }
  version.needs_phases = NeedsConversion(version.dir) || NeedsPatching(version.dir);
}

size_t OrderOf(const GameVersion& version) {
  const auto it = std::find(kOrder.begin(), kOrder.end(), version.id);
  return size_t(it - kOrder.begin());
}

}  // namespace

void MigrateLibrary(const fs::path& root) {
  std::error_code ec;
  if (fs::is_regular_file(root / kMigrating, ec)) {
    const std::string folder = MigratingFolder(root);
    if (!folder.empty())
      FinishMove(root, folder);
  }
  fs::create_directories(root, ec);

  if (IsGameDirectory(root)) {
    const SourceInfo info = IdentifySource(root.string());
    const std::string folder = FreeVersionDir(root, info.id).filename().string();
    REXLOG_INFO("Moving the single version install in {} to {}", root.string(), folder);
    WriteText(root / kMigrating, "folder = " + TomlString(folder) + "\n");
    if (FinishMove(root, folder) && !info.release.empty())
      WriteVersionInfo(root / folder, info.id, info.release, info.languages);
  }
}

std::vector<GameVersion> ScanLibrary(const fs::path& root) {
  std::vector<GameVersion> versions;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    const std::string name = entry.path().filename().string();
    if (EndsWith(name, ".partial") || name.find(".old") != std::string::npos)
      continue;
    std::error_code entry_ec;
    if (!entry.is_directory(entry_ec) || !IsGameDirectory(entry.path()))
      continue;
    GameVersion version;
    version.key = name;
    version.dir = entry.path();
    Describe(version, true);
    versions.push_back(std::move(version));
  }
  std::sort(versions.begin(), versions.end(), [](const GameVersion& a, const GameVersion& b) {
    return OrderOf(a) != OrderOf(b) ? OrderOf(a) < OrderOf(b) : a.key < b.key;
  });
  return versions;
}

int FindVersion(std::vector<GameVersion>& versions, std::string_view key) {
  if (key.empty())
    return -1;
  for (size_t i = 0; i < versions.size(); ++i) {
    if (versions[i].key == key)
      return int(i);
  }
  const fs::path path = FromUtf8(key);
  if (!IsGameDirectory(path))
    return -1;
  std::error_code ec;
  GameVersion version;
  version.key = std::string(key);
  version.dir = fs::absolute(path, ec);
  version.temporary = true;
  Describe(version, false);
  versions.push_back(std::move(version));
  return int(versions.size() - 1);
}

fs::path FreeVersionDir(const fs::path& root, std::string id) {
  if (id.empty())
    id = "other";
  std::error_code ec;
  for (int n = 1;; ++n) {
    const std::string name = n == 1 ? id : id + "-" + std::to_string(n);
    if (!fs::exists(root / name, ec) && !fs::exists(root / (name + ".partial"), ec))
      return root / name;
  }
}

void WriteVersionInfo(const fs::path& dir, const std::string& id, const std::string& release,
                      uint32_t languages) {
  std::ostringstream text;
  text << "id = " << TomlString(id) << "\nrelease = " << TomlString(release)
       << "\nlanguages = " << languages << "\n";
  if (!WriteText(dir / kInfoFile, text.str()))
    REXLOG_WARN("Could not write {}", (dir / kInfoFile).string());
}

namespace {

std::string ReadVersionField(const fs::path& dir, const char* key) {
  try {
    const toml::table table = toml::parse_file(Utf8(dir / kInfoFile));
    return table[key].value_or(std::string());
  } catch (const toml::parse_error&) {
    return {};
  }
}

}  // namespace

std::string ReadVersionRelease(const fs::path& dir) {
  return ReadVersionField(dir, "release");
}

std::string ReadVersionId(const fs::path& dir) {
  return ReadVersionField(dir, "id");
}

void RefreshRootExecutable(const fs::path& root, const std::vector<GameVersion>& versions) {
  const fs::path xex = root / kRootXex;
  const fs::path marker = root / kRootXexSource;
  std::error_code ec;
  const bool managed = fs::is_regular_file(marker, ec);
  if (fs::exists(xex, ec) && !managed)
    return;

  // Only World: the build reads this file, and every address it uses is World's.
  const GameVersion* best = nullptr;
  for (const GameVersion& version : versions) {
    if (!version.temporary && version.id == "xbla-world") {
      best = &version;
      break;
    }
  }
  if (!best) {
    if (managed) {
      fs::remove(xex, ec);
      fs::remove(marker, ec);
      REXLOG_INFO("Removed {}: no World version is left", xex.string());
    }
    return;
  }
  if (managed && ReadText(marker) == best->key && fs::is_regular_file(xex, ec))
    return;

  // The shipped file, before any patching.
  fs::path source = best->dir / "default.xex.orig";
  if (!fs::is_regular_file(source, ec))
    source = best->dir / kRootXex;
  if (!fs::is_regular_file(source, ec))
    return;
  fs::remove(xex, ec);
  ec.clear();
  fs::create_hard_link(source, xex, ec);
  if (ec) {
    ec.clear();
    fs::copy_file(source, xex, ec);
  }
  if (ec) {
    REXLOG_WARN("Could not copy {} to {}: {}", source.string(), xex.string(), ec.message());
    return;
  }
  WriteText(marker, best->key);
  REXLOG_INFO("{} now comes from {}", xex.string(), source.string());
}

std::string GameDirectoryOverride() {
  return REXCVAR_GET(game_version);
}

std::string VersionLabel(const GameVersion& version) {
  std::string label = version.release;
  const size_t open = label.rfind('(');
  const size_t close = label.rfind(')');
  if (open != std::string::npos && close > open)
    label = label.substr(open + 1, close - open - 1);
  const std::string folder = version.temporary ? version.dir.filename().string() : version.key;
  if (label.empty())
    return folder;
  // A second copy of a release is told apart by its folder.
  if (!version.temporary && version.key != version.id)
    label += " (" + folder + ")";
  return label;
}

}  // namespace nocturne
