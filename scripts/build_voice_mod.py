#!/usr/bin/env python3
"""Build a complete voice-replacement mod from a JSON description.

Replaces any number of individual voice lines / sound effects across any
number of XACT wave banks (`.xwb`) with slices of arbitrary audio files, and
generates the code mod needed to make the game actually load the result.

Why a code mod is needed
------------------------
The game never asks the filesystem how big a wave bank is. Its overlay
loader (`hd_read_trans`, sub_82252A30) keeps a table of 44-byte records in
static data, one per sound bank, each holding that bank's *baked-in* size:

    record +0x00  .xsb size    +0x04  .xwb size    +0x14 -> name string

and for a `.xwb` it does, in effect:

    size = record[+0x04];              # NOT the file's real size
    read(open("game:\\data\\sound\\sd_<name>.xwb"), wave_bank_buffer, size);

So a modded bank bigger than the shipped one has everything past the
original end-of-file simply never read; the tail is left as stale buffer
contents and plays as static. Raising that one dword per bank is the whole
fix, and it's what the generated mod does.

Two consequences shape this script:

  * Replacements are appended *past the original end of file* and the target
    entry is repointed there. No existing entry ever moves -- relocating an
    entry corrupts its playback for reasons still not understood
    (docs/xma-voice-repack-handoff.md, "build 3").
  * Every `.xwb` load reads into one fixed 1,630,208-byte (0x18E000) buffer
    allocated at startup (sub_824FBE98), always at offset 0. That is the hard
    ceiling for any single bank, and what `--budget` accounting is against.

Audio is written back as raw 16-bit big-endian PCM (the bank is big-endian
throughout, samples included). There is no open-source XMA2 encoder, and
none is needed -- the guest's PCM path is fully implemented.

Usage
-----
    python scripts/build_voice_mod.py voice_mod.json
    python scripts/build_voice_mod.py voice_mod.json --build --install

Config format (see --example for a ready-to-edit one):

    {
      "mod": {
        "id": "die_monster_replacement",
        "name": "Die Monster Replacement",
        "version": "1.0.0",
        "description": "...",
        "author": "you"
      },
      "banks": {
        "sd_dora1": [
          {
            "cue": "011A",
            "input": "C:/Music/song.flac",
            "start": 32.074,
            "end": 43.294
          }
        ]
      }
    }

Each replacement takes `cue` (from `repack_xact_wave.py list`) or `index`,
an `input` audio file (anything ffmpeg reads), and `start` plus either `end`
or `duration` (seconds; omit both to match the original entry's length).
`channels` and `samplerate` are optional -- left out, the best quality that
fits the bank's remaining byte budget is chosen automatically.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_xact_audio import parse_xsb_cue_names, parse_xwb  # noqa: E402
from repack_xact_wave import (  # noqa: E402
    ENTRY_SIZE, ENTRY_TABLE_BASE, _write_entry, encode_miniformat, extract_pcm_slice,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOUND_DIR = REPO_ROOT / "assets" / "DATA" / "sound"
DEFAULT_MODS_SRC = REPO_ROOT.parent / "NocturneRecomp-Mods" / "src"

# sub_824FBE98: dword_83133AE4 = alloc(1630208). Every .xwb load reads into
# this one buffer starting at offset 0, so no bank may exceed it.
WAVE_BANK_BUFFER_SIZE = 0x18E000

# Quality ladder for automatic format selection, best first. Stereo is kept
# ahead of a higher mono rate at the same byte cost: for music the stereo
# image reads as the bigger improvement, and voice lines are usually short
# enough that the top of the ladder fits anyway.
QUALITY_LADDER = [
    (2, 44100), (2, 32000), (2, 24000), (1, 44100), (2, 22050),
    (1, 32000), (1, 24000), (2, 11025), (1, 22050), (1, 11025),
]


def entry_bytes(duration: float, channels: int, samplerate: int, align: int) -> int:
    """Aligned byte cost of `duration` seconds at the given format."""
    raw = int(duration * samplerate) * 2 * channels
    return raw + (-raw) % align


class ConfigError(Exception):
    """A problem with the user's JSON that they need to fix."""


def resolve_target(bank: dict, names: list[str] | None, spec: dict, bank_name: str) -> dict:
    if "cue" in spec:
        if not names:
            raise ConfigError(f"{bank_name}: no cue names resolved from the .xsb; use \"index\"")
        matches = [e for e in bank["entries"] if names[e["index"]] == spec["cue"]]
        if not matches:
            raise ConfigError(f"{bank_name}: no entry with cue {spec['cue']!r}")
        return matches[0]
    if "index" in spec:
        idx = spec["index"]
        if not 0 <= idx < bank["entry_count"]:
            raise ConfigError(f"{bank_name}: index {idx} out of range "
                              f"(bank has {bank['entry_count']} entries)")
        return bank["entries"][idx]
    raise ConfigError(f"{bank_name}: each replacement needs \"cue\" or \"index\"")


def spec_duration(spec: dict, target: dict, bank_name: str) -> float:
    start = float(spec.get("start", 0.0))
    if "duration" in spec and "end" in spec:
        raise ConfigError(f"{bank_name}: give \"duration\" or \"end\", not both")
    if "duration" in spec:
        duration = float(spec["duration"])
    elif "end" in spec:
        duration = float(spec["end"]) - start
        if duration <= 0:
            raise ConfigError(f"{bank_name}: \"end\" ({spec['end']}) must be after "
                              f"\"start\" ({start})")
    else:
        # Match the original entry. Its length in samples is the top 28 bits
        # of dwFlagsAndDuration, which parse_xwb exposes as "duration".
        duration = target["duration"] / (target["samplerate"] or 44100)
    return duration


def choose_format(spec: dict, duration: float, budget: int, align: int,
                  bank_name: str) -> tuple[int, int]:
    """Pick (channels, samplerate), honouring whatever the user pinned."""
    want_ch = spec.get("channels")
    want_sr = spec.get("samplerate")

    if want_ch and want_sr:
        cost = entry_bytes(duration, want_ch, want_sr, align)
        if cost > budget:
            raise ConfigError(
                f"{bank_name}: {duration:.3f}s at {want_ch}ch/{want_sr}Hz needs {cost} bytes "
                f"but only {budget} are left in the bank's budget. Shorten it, lower the "
                f"format, or drop \"channels\"/\"samplerate\" to auto-pick.")
        return want_ch, want_sr

    candidates = [(ch, sr) for ch, sr in QUALITY_LADDER
                  if (want_ch is None or ch == want_ch) and (want_sr is None or sr == want_sr)]
    for ch, sr in candidates:
        if entry_bytes(duration, ch, sr, align) <= budget:
            return ch, sr

    cheapest = candidates[-1] if candidates else QUALITY_LADDER[-1]
    raise ConfigError(
        f"{bank_name}: {duration:.3f}s doesn't fit the remaining {budget} byte budget at any "
        f"supported format (cheapest tried: {cheapest[0]}ch/{cheapest[1]}Hz needs "
        f"{entry_bytes(duration, cheapest[0], cheapest[1], align)}). Shorten the slice.")


def build_bank(bank_name: str, specs: list[dict], sound_dir: Path, out_dir: Path,
               verbose: bool = True) -> dict:
    """Repack one wave bank; returns the info the code generator needs."""
    xwb_path = sound_dir / f"{bank_name}.xwb"
    xsb_path = sound_dir / f"{bank_name}.xsb"
    if not xwb_path.exists():
        raise ConfigError(f"no such wave bank: {xwb_path}")
    if not xsb_path.exists():
        raise ConfigError(f"missing paired sound bank: {xsb_path}")

    bank = parse_xwb(xwb_path)
    names = parse_xsb_cue_names(xsb_path, bank["entry_count"])
    data = bytearray(xwb_path.read_bytes())
    original_size = len(data)
    wavedata_start = bank["wavedata_start"]
    align = struct.unpack_from(">I", data, 0x80)[0] or 1

    wave_segment = bytearray(data[wavedata_start:])
    if len(wave_segment) % align:
        raise ConfigError(f"{bank_name}: wave data doesn't end on a {align}-byte boundary; "
                          f"appending would misalign every replacement")

    if verbose:
        print(f"{bank_name}: {original_size} bytes, {bank['entry_count']} entries, "
              f"budget {WAVE_BANK_BUFFER_SIZE} bytes")

    # Resolve everything up front so budgeting can see the whole picture.
    planned = []
    for spec in specs:
        target = resolve_target(bank, names, spec, bank_name)
        planned.append((spec, target, spec_duration(spec, target, bank_name)))

    auto_remaining = sum(1 for spec, _, _ in planned
                         if spec.get("channels") is None or spec.get("samplerate") is None)

    for spec, target, duration in planned:
        idx = target["index"]
        cue = names[idx] if names else None
        used = wavedata_start + len(wave_segment)
        free = WAVE_BANK_BUFFER_SIZE - used
        is_auto = spec.get("channels") is None or spec.get("samplerate") is None
        # Auto entries split what's left evenly, so one greedy early entry
        # can't starve the rest.
        share = free // auto_remaining if (is_auto and auto_remaining > 1) else free

        channels, samplerate = choose_format(spec, duration, share, align, bank_name)
        if is_auto:
            auto_remaining -= 1

        input_path = Path(spec["input"]).expanduser()
        if not input_path.exists():
            raise ConfigError(f"{bank_name} entry {idx}: no such input file: {input_path}")

        tmp = out_dir / f".__slice_{bank_name}_{idx}.wav"
        pcm = extract_pcm_slice(input_path, float(spec.get("start", 0.0)), duration,
                                samplerate, channels, tmp)
        pad = (-len(pcm)) % align
        cost = len(pcm) + pad
        if cost > free:
            raise ConfigError(f"{bank_name} entry {idx}: needs {cost} bytes, only {free} left")

        new_off = len(wave_segment)
        wave_segment += pcm + b"\x00" * pad
        nsamples = len(pcm) // (2 * channels)
        _write_entry(data, idx, encode_miniformat(0, channels, samplerate, channels * 2, True),
                     nsamples, new_off, cost)

        if verbose:
            print(f"  entry {idx:3d} (cue={cue}) <- {input_path.name} "
                  f"@{spec.get('start', 0.0)}s for {duration:.3f}s, "
                  f"{channels}ch/{samplerate}Hz, {cost} bytes")

    # WAVEBANKSEGMENT[4] describes the wave data region; the engine locates
    # and bounds it with this, so it has to grow with the data.
    struct.pack_into(">II", data, 8 + 4 * 8, wavedata_start, len(wave_segment))
    out_bytes = bytes(data[:wavedata_start]) + bytes(wave_segment)

    if len(out_bytes) > WAVE_BANK_BUFFER_SIZE:
        raise ConfigError(f"{bank_name}: repacked to {len(out_bytes)} bytes, over the "
                          f"{WAVE_BANK_BUFFER_SIZE}-byte wave-bank buffer")

    dest = out_dir / "game" / "DATA" / "sound"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / f"{bank_name}.xwb").write_bytes(out_bytes)
    shutil.copyfile(xsb_path, dest / f"{bank_name}.xsb")

    if verbose:
        pct = 100.0 * len(out_bytes) / WAVE_BANK_BUFFER_SIZE
        print(f"  -> {len(out_bytes)} bytes (was {original_size}), "
              f"{pct:.0f}% of the wave-bank buffer")

    return {
        "name": bank_name,
        "xsb_size": xsb_path.stat().st_size,
        "xwb_size": original_size,
        "new_size": len(out_bytes),
    }


MOD_MAIN_TEMPLATE = '''\
// Generated by scripts/build_voice_mod.py -- do not edit by hand.
//
// Raises the game's baked-in load size for each wave bank this mod ships a
// replacement for. Without this the loader reads only the *original* file's
// byte count and everything past it plays as static; see the script's module
// docstring for the full mechanism.
//
// Records are located by signature rather than by hardcoded address: each
// 44-byte record in the loader's bank table starts with the bank's shipped
// .xsb and .xwb sizes, which are unique per bank across all 55 banks. That
// keeps this working on title-update builds, where static-data addresses
// shift and a baked-in address would silently patch the wrong thing.

#include <rex/system/mod_plugin.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace {{

// Bank table (vanilla: record 0 at 0x82E00CF8, 44-byte stride). The scan
// window is deliberately wider than the table so a shifted build still
// finds it; the two-size signature plus the name-pointer check below is
// what actually identifies a record.
constexpr uint32_t kScanBegin = 0x82E00000u;
constexpr uint32_t kScanEnd = 0x82E02000u;
constexpr uint32_t kImageBegin = 0x82000000u;
constexpr uint32_t kImageEnd = 0x83210000u;

constexpr uint32_t kXwbSizeOffset = 0x04u;
constexpr uint32_t kNamePtrOffset = 0x14u;

// The game's single wave-bank staging buffer (sub_824FBE98).
constexpr uint32_t kWaveBankBufferSize = 0x18E000u;

struct BankPatch {{
  const char* name;
  uint32_t xsb_size;   // shipped .xsb size, signature only
  uint32_t xwb_size;   // shipped .xwb size, signature + what we replace
  uint32_t new_size;   // size of the .xwb this mod ships
}};

constexpr BankPatch kPatches[] = {{
{patch_rows}
}};

{static_asserts}

class GeneratedVoiceMod : public rex::system::IModPlugin {{
 public:
  explicit GeneratedVoiceMod(rex::Runtime* runtime) : runtime_(runtime) {{}}

  // default.xex's static data is in guest memory by the time this fires,
  // and no bank is loaded until gameplay asks for one.
  void OnModuleLaunched() override {{
    if (!runtime_ || !runtime_->memory()) {{
      return;
    }}
    for (const auto& patch : kPatches) {{
      ApplyPatch(patch);
    }}
  }}

 private:
  void ApplyPatch(const BankPatch& patch) {{
    auto* memory = runtime_->memory();
    uint32_t found = 0;
    for (uint32_t addr = kScanBegin; addr + 0x2C <= kScanEnd; addr += 4) {{
      const auto* record = memory->TranslateVirtual<const uint8_t*>(addr);
      if (!record) {{
        continue;
      }}
      if (rex::memory::load_and_swap<uint32_t>(record) != patch.xsb_size ||
          rex::memory::load_and_swap<uint32_t>(record + kXwbSizeOffset) != patch.xwb_size) {{
        continue;
      }}
      // Every real record's +0x14 is a pointer to the bank's name string;
      // requiring it to point into the image rejects stray size-pair
      // lookalikes in unrelated data.
      const uint32_t name_ptr = rex::memory::load_and_swap<uint32_t>(record + kNamePtrOffset);
      if (name_ptr < kImageBegin || name_ptr >= kImageEnd) {{
        continue;
      }}
      if (Write(addr + kXwbSizeOffset, patch.new_size)) {{
        ++found;
      }}
    }}

    if (found == 0) {{
      REXLOG_WARN("[{mod_id}] couldn't find the bank-table record for {{}} "
                  "(.xsb {{:X}} / .xwb {{:X}}) -- it will play as static",
                  patch.name, patch.xsb_size, patch.xwb_size);
    }} else {{
      // More than one record can point at the same bank; all of them need
      // the new size or whichever the game happens to use may be stale.
      REXLOG_INFO("[{mod_id}] {{}}.xwb load size {{:X}} -> {{:X}} bytes ({{}} record(s))",
                  patch.name, patch.xwb_size, patch.new_size, found);
    }}
  }}

  // The table is in read-only static data; the page must be unlocked or the
  // write access-violates.
  bool Write(uint32_t addr, uint32_t value) {{
    auto* memory = runtime_->memory();
    auto* heap = memory->LookupHeap(addr);
    if (!heap) {{
      return false;
    }}
    uint32_t old_protect = 0;
    if (!heap->Protect(addr, sizeof(uint32_t),
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                       &old_protect)) {{
      return false;
    }}
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<void*>(addr), value);
    heap->Protect(addr, sizeof(uint32_t), old_protect, nullptr);
    return true;
  }}

  rex::Runtime* runtime_ = nullptr;
}};

}}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {{
  return rex::system::kModPluginAbiVersion;
}}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {{
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {{
    return nullptr;
  }}
  return new GeneratedVoiceMod(ctx->runtime);
}}
'''

CMAKELISTS_TEMPLATE = '''\
cmake_minimum_required(VERSION 3.25)
project({mod_id} LANGUAGES CXX)

include(${{CMAKE_CURRENT_LIST_DIR}}/../common/mod_cmake/rexmod.cmake)

rexmod_add_plugin({mod_id}
    mod_main.cpp
)
'''


def toml_string(value: object) -> str:
    """Quote a value as a TOML basic string.

    Not optional politeness: mod descriptions routinely contain quotes (a
    line of dialogue, say), and writing one through raw produces a mod.toml
    the loader refuses to parse. It fails *quietly* -- the manifest is
    skipped, so `code = ...` is never read and the plugin never loads, while
    the mod's assets still apply. The result is a bank whose replacement
    audio is present but whose size patch is missing: pure static.
    """
    escaped = (str(value)
               .replace("\\", "\\\\")
               .replace('"', '\\"')
               .replace("\n", "\\n")
               .replace("\r", "\\r")
               .replace("\t", "\\t"))
    return f'"{escaped}"'


def generate_sources(mod: dict, banks: list[dict], src_dir: Path) -> None:
    mod_id = mod["id"]
    rows = "\n".join(
        f'    {{"{b["name"]}", 0x{b["xsb_size"]:X}u, 0x{b["xwb_size"]:X}u, 0x{b["new_size"]:X}u}},'
        for b in banks)
    asserts = "\n".join(
        f'static_assert(0x{b["new_size"]:X}u <= kWaveBankBufferSize,\n'
        f'              "{b["name"]}.xwb would overrun the game\'s wave-bank buffer");'
        for b in banks)

    src_dir.mkdir(parents=True, exist_ok=True)
    (src_dir / "mod_main.cpp").write_text(
        MOD_MAIN_TEMPLATE.format(patch_rows=rows, static_asserts=asserts, mod_id=mod_id),
        encoding="utf-8")
    (src_dir / "CMakeLists.txt").write_text(
        CMAKELISTS_TEMPLATE.format(mod_id=mod_id), encoding="utf-8")

    toml_lines = [
        "manifest_version = 2",
        f"name = {toml_string(mod.get('name', mod_id))}",
        f"version = {toml_string(mod.get('version', '1.0.0'))}",
        f"game_version = {toml_string(mod.get('game_version', '1.3.3'))}",
    ]
    if mod.get("author"):
        toml_lines.append(f"author = {toml_string(mod['author'])}")
    desc = mod.get("description", f"Voice replacements for {', '.join(b['name'] for b in banks)}.")
    toml_lines += [
        f"description = {toml_string(desc)}",
        f"code = {toml_string(mod_id)}",
        'platform = ""',
        "",
    ]
    manifest = "\n".join(toml_lines)

    # Verify rather than trust: a manifest the loader can't parse disables
    # the code plugin silently, and the failure only shows up as static
    # audio much later.
    try:
        import tomllib
        parsed = tomllib.loads(manifest)
        if parsed.get("code") != mod_id:
            raise ValueError(f'"code" round-tripped as {parsed.get("code")!r}')
    except Exception as exc:  # noqa: BLE001 - re-raised as a config error
        raise ConfigError(f"generated mod.toml is invalid ({exc}); check the mod metadata "
                          f"in your config for stray control characters") from exc

    (src_dir / "mod.toml").write_text(manifest, encoding="utf-8")


def build_and_install(mod_id: str, src_dir: Path, build_type: str, install_dir: Path,
                      sdk_dir: Path) -> None:
    """Compile the plugin and assemble the finished mod folder."""
    build_dir = src_dir.parent.parent / "out" / "build" / "voice_mods" / build_type / mod_id
    subprocess.run(
        ["cmake", "-S", str(src_dir), "-B", str(build_dir), "-G", "Ninja",
         f"-DCMAKE_BUILD_TYPE={build_type}", f"-DCMAKE_PREFIX_PATH={sdk_dir}",
         "-DCMAKE_CXX_COMPILER=clang++"],
        check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--parallel"], check=True)

    # RelWithDebInfo game builds load "<stem>rd.dll"; Release builds load
    # "<stem>.dll". Naming it wrong is silently fatal: the loader logs a
    # failure and the size patch never runs, so the audio plays as static.
    suffix = "rd" if build_type == "RelWithDebInfo" else ""
    code_dir = install_dir / "code" / "windows-x64"
    code_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("dll", "pdb"):
        built = build_dir / f"{mod_id}.{ext}"
        if built.exists():
            shutil.copyfile(built, code_dir / f"{mod_id}{suffix}.{ext}")

    game_src = src_dir / "game"
    if game_src.exists():
        game_dst = install_dir / "game"
        if game_dst.exists():
            shutil.rmtree(game_dst)
        shutil.copytree(game_src, game_dst)
    shutil.copyfile(src_dir / "mod.toml", install_dir / "mod.toml")
    print(f"Installed mod to {install_dir}")


EXAMPLE_CONFIG = {
    "mod": {
        "id": "die_monster_replacement",
        "name": "Die Monster Replacement",
        "version": "1.0.0",
        "description": "Replaces Dracula's \"Die monster!\" line.",
    },
    "banks": {
        "sd_dora1": [
            {
                "cue": "011A",
                "input": "C:/Music/song.flac",
                "start": 32.074,
                "end": 43.294,
            }
        ]
    },
}


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", nargs="?", help="Path to the JSON config")
    parser.add_argument("--example", action="store_true",
                        help="Print an example config and exit")
    parser.add_argument("--sound-dir", type=Path, default=DEFAULT_SOUND_DIR,
                        help=f"Original wave banks (default: {DEFAULT_SOUND_DIR})")
    parser.add_argument("--src-root", type=Path, default=DEFAULT_MODS_SRC,
                        help=f"Mod source root; sources go to <root>/<id> "
                             f"(default: {DEFAULT_MODS_SRC})")
    parser.add_argument("--build", action="store_true", help="Also compile the plugin")
    parser.add_argument("--install", action="store_true",
                        help="Assemble the finished mod into this repo's mods/<id> (implies --build)")
    parser.add_argument("--release", action="store_true",
                        help="Build Release (for a `build.py --release` game); default is "
                             "RelWithDebInfo, matching a default build.py game")
    parser.add_argument("--sdk-dir", type=Path, default=REPO_ROOT / "sdk",
                        help="SDK to build the plugin against")
    args = parser.parse_args()

    if args.example:
        print(json.dumps(EXAMPLE_CONFIG, indent=2))
        return
    if not args.config:
        parser.error("a config path is required (or pass --example)")

    config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    mod = config.get("mod") or {}
    if not mod.get("id"):
        raise ConfigError('config needs a "mod" object with an "id"')
    banks_cfg = config.get("banks") or {}
    if not banks_cfg:
        raise ConfigError('config needs a "banks" object with at least one bank')

    src_dir = args.src_root / mod["id"]
    # Stale assets from a previous run would otherwise be shipped alongside
    # the new ones and quietly override banks this config no longer mentions.
    if (src_dir / "game").exists():
        shutil.rmtree(src_dir / "game")
    src_dir.mkdir(parents=True, exist_ok=True)

    banks = []
    for bank_name, specs in banks_cfg.items():
        if isinstance(specs, dict):  # tolerate {"replacements": [...]}
            specs = specs.get("replacements", [])
        if not specs:
            raise ConfigError(f"{bank_name}: no replacements listed")
        banks.append(build_bank(bank_name, specs, args.sound_dir, src_dir))

    generate_sources(mod, banks, src_dir)
    print(f"Wrote mod source to {src_dir}")

    if args.build or args.install:
        build_type = "Release" if args.release else "RelWithDebInfo"
        install_dir = REPO_ROOT / "mods" / mod["id"]
        build_and_install(mod["id"], src_dir, build_type, install_dir, args.sdk_dir)
        print(f"\nEnable it by adding to mods/mods.toml:\n\n"
              f"[[mods]]\nenabled = true\nid = '{mod['id']}'")


if __name__ == "__main__":
    try:
        main()
    except ConfigError as exc:
        sys.exit(f"error: {exc}")
