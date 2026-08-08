#!/usr/bin/env python3
"""Build a mod that replaces lines of *scripted cutscene dialogue* text --
e.g. the opening "Die monster! You don't belong in this world!" exchange --
with text of any length, not just a same-length swap.

Why this needs a different trick than item/enemy text
-------------------------------------------------------
Item/enemy names and descriptions (see rexmod/text_patch.h, ApplyTextPatch)
live in fixed-length, null-padded fields: a same-length-or-shorter
replacement can be written in place safely.

Cutscene dialogue does not work that way. It's stored as a compact
opcode/control-code *script* -- opcode bytes (line breaks, per-character
reveal delays, portrait/box sizing) interleaved directly with the literal
ASCII text, with no length prefix anywhere and no padding between fields.
For the English "Die monster..." exchange this was confirmed by decompiling
the interpreter (sub_823A2F28 in default.xex, driven by a per-language
script-pointer table at off_82D55DEC, indexed by the current dialogue-event
id in dword_8313BC20): the whole conversation for one language is one
self-contained 632-byte blob (file offset 0xD55910-0xD55B88 in the decrypted
image), immediately followed by the next language's blob with no gap.
Overwriting bytes in place -- even zero-padded to the original length --
would either truncate a longer replacement or leave the next language's
data corrupted by a shorter one, since nothing marks where this blob ends
except "the next language's blob begins".

The fix: don't patch in place at all. Allocate a *new* guest buffer (via
Memory::SystemHeapAlloc, the same guest heap the game itself allocates
from), write a full replacement copy of the script there with only the
requested lines swapped -- any length, since it's not constrained by
anything the original occupied -- and repoint the *single 4-byte pointer*
in off_82D55DEC that referenced the original blob to the new one instead.
Nothing about the original bytes (this language's or any other's) is ever
touched, so there's no adjacent-data risk at all. This mirrors how
build_voice_mod.py replaces wave-bank audio: append new data elsewhere,
repoint a pointer, leave everything else untouched.

Both addresses used at runtime -- the original script blob (to find and
copy) and the table slot that points to it (to overwrite) -- are located by
scanning live guest memory for their content/value, the same signature-based
approach build_voice_mod.py uses for the wave-bank table, so this keeps
working across the vanilla/title-update address split. See
docs/making-mods.md's "Patching static game text/data" for why a hardcoded
offline file offset can't be trusted as a guest address on its own.

Currently only "sd_dora1_intro_en" (the English opening exchange) has a
known blob range; add more to KNOWN_SCRIPTS as they're located the same way
(decompile the table's consumer, find the language's byte range between two
consecutive table-slot pointers).

Usage
-----
    python scripts/build_dialogue_mod.py dialogue_mod.json
    python scripts/build_dialogue_mod.py dialogue_mod.json --build --install

Config format (see --example):

    {
      "mod": {
        "id": "die_monster_text",
        "name": "Die Monster Text",
        "version": "1.0.0",
        "description": "..."
      },
      "dialogue": {
        "sd_dora1_intro_en": [
          {"find": "Die monster.", "replace": "Placeholder text of any length"},
          {"find": "You don't belong", "replace": "works for a shorter"},
          {"find": "in this world!", "replace": "or a much longer replacement line alike"}
        ]
      }
    }

Each `find` must match exactly one place in the script's original English
text (byte-exact, case-sensitive) -- ambiguous or missing matches are a
config error, not a silent no-op. `replace` can be any length. If `find`
genuinely repeats (a short word like "of" used in two different lines), add
`"occurrence": N` (1-based, in order of appearance) to pick one instead of
leaving it ambiguous -- don't just drop that word from the config, since the
original text for it is *not* removed by omission and will still print
alongside the rest of your replacement lines.

Line timing
-----------
By default every speaker's box is made to hold on screen until that
speaker's voice line has actually finished, by inserting the interpreter's
"wait for audio idle" opcode ahead of each box teardown -- see
insert_voice_holds() for why the vanilla script doesn't need this but any
edited one does. Turn it off for a script with the object form:

      "dialogue": {
        "sd_dora1_intro_en": {
          "hold_for_voice": false,
          "edits": [ ... ]
        }
      }
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "re"))
from dump_xex_image import decrypt_xex_image, _load_gen_icon  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_XEX = REPO_ROOT / "assets" / "default.xex"
DEFAULT_MODS_SRC = REPO_ROOT.parent / "NocturneRecomp-Mods" / "src"

# (start, end) file offsets into the *decrypted* default.xex image, found by
# decompiling the table at off_82D55DEC and reading the gap between one
# language slot's pointer and the next's -- see module docstring.
KNOWN_SCRIPTS = {
    "sd_dora1_intro_en": (0xD55910, 0xD55B88),
}

# Longest lines in the original script are ~20 chars ("Mankind ill needs a",
# "again given flesh."); this is a conservative default for the box width a
# replacement line has to fit inside before it needs to wrap. Override per
# script or per edit with "wrap_width" if a line is still clipping or has
# room to spare -- there's no box-width field decoded yet, so this is a
# starting point to tune against what you see in-game, not a measured value.
DEFAULT_WRAP_WIDTH = 22

IMAGE_BASE = 0x82000000

# How far past the vanilla address a TU build's copy of this data might have
# shifted. Generous on purpose: this is a one-time startup scan, not a
# per-frame cost.
SCAN_MARGIN = 0x40000


# Opcode -> parameter bytes consumed after the opcode byte, per sub_823A2F28.
# Opcodes not listed take none. Needed to walk a blob and tell a real opcode
# byte from a text byte that happens to have the same value.
PARAM_LENGTHS = {2: 1, 3: 1, 16: 1, 17: 1, 5: 2, 7: 2, 9: 2, 20: 2, 12: 4, 14: 4, 15: 4, 19: 5}

# Opcodes that tear down the speaker's box: 6 closes it (portrait swap
# follows), 0 ends the script.
OP_END_BOX = 6
OP_END_SCRIPT = 0
# Opcode 11: "spin until the sound system is idle". sub_823A2F28 case 11 calls
# sub_82230130() -- `dword_82E7DD00 || word_82E7DCD4` -- and rewinds the script
# cursor by one byte while it's true, so the script stalls until the currently
# playing voice line has finished.
OP_WAIT_AUDIO_IDLE = 11


class ConfigError(Exception):
    """A problem with the user's JSON that they need to fix."""


def load_image(xex_path: Path) -> bytes:
    if not xex_path.exists():
        raise ConfigError(f"no such file: {xex_path}")
    gi = _load_gen_icon()
    return decrypt_xex_image(str(xex_path), gi)


def word_wrap(text: str, width: int) -> list[str]:
    """Greedy word-wrap; long single "words" (no spaces) are left as-is
    rather than broken mid-word, since we have no hyphenation rule."""
    words = text.split(" ")
    lines: list[str] = []
    current = ""
    for word in words:
        candidate = f"{current} {word}" if current else word
        if len(candidate) > width and current:
            lines.append(current)
            current = word
        else:
            current = candidate
    if current:
        lines.append(current)
    return lines or [""]


def walk_opcodes(blob: bytes):
    """Yield (offset, opcode) for every real opcode byte in `blob`.

    Mirrors sub_823A2F28's cursor: a byte with the high bit set is a 2-byte
    glyph, otherwise the byte is an opcode and PARAM_LENGTHS says how many
    parameter bytes follow. Literal ASCII text bytes fall through the
    interpreter's `default:` and are yielded too -- callers must filter to the
    opcode values they care about (all of which are < 0x20, so they can't be
    confused with printable text; parameter bytes, which *can* be anything,
    are correctly skipped by this walk).
    """
    i, n = 0, len(blob)
    while i < n:
        op = blob[i]
        start = i
        i += 1
        if op & 0x80:
            i += 1  # second half of a glyph pair
            continue
        i += PARAM_LENGTHS.get(op, 0)
        yield start, op


def insert_voice_holds(blob: bytes) -> tuple[bytes, int]:
    """Make each speaker's box stay up until their voice line has finished.

    A turn in the vanilla script is laid out as:

        ...text...  op6 (close box)  op19  op5 (open next box)
        op11 (wait for audio idle)  op9 (queue next voice)  op10 (wait busy)

    so the *only* thing that waits for a voice line to finish is the op11 at
    the head of the **next** turn -- and it runs after op6/op5 have already
    torn the current box down and swapped in the next speaker's portrait. In
    vanilla that's invisible: the original lines take long enough to type out
    that the voice is essentially over by the time op6 is reached, so the
    stall at op11 lands on a box that is about to be replaced anyway.

    Shorten (or blank) the text and that assumption collapses -- the script
    reaches op6 while the voice is still playing, so the portrait switches and
    the text goes blank early, then op11 stalls on the *next* speaker's empty
    box. Same thing happens if the audio is replaced with something longer
    than the line it's under.

    Moving the wait to just *before* the teardown fixes it for any text length
    and any audio length: hold the finished line on screen until its own voice
    is done, and only then close the box. op11 takes no parameters, so this is
    a one-byte insertion that leaves every other byte and every jump-free
    control sequence intact.
    """
    # Collected first, then applied back-to-front, so earlier insertions don't
    # shift the offsets of later ones.
    sites = []
    prev_op = None
    for offset, op in walk_opcodes(blob):
        if op in (14, 15):
            # Absolute/relative jumps. Nothing in the known scripts uses them,
            # and inserting a byte would silently invalidate every jump target
            # (they're absolute guest addresses into the blob, not offsets we
            # can fix up from here).
            raise ConfigError(
                f"script contains a jump opcode ({op}) at byte {offset}; inserting voice "
                f"holds would break its target. Set \"hold_for_voice\": false for this script.")
        if op == OP_END_SCRIPT:
            # First op0 ends the script; anything after it is padding (the
            # vanilla blob has seven trailing zero bytes) and never executes.
            break
        if op == OP_END_BOX and prev_op != OP_WAIT_AUDIO_IDLE:
            # `prev_op` check: the last turn of the vanilla script already
            # spells this wait out by hand (op11 immediately before its op6),
            # which is what confirms the intent -- don't double it up.
            sites.append(offset)
        prev_op = op

    out = bytearray(blob)
    for offset in reversed(sites):
        out.insert(offset, OP_WAIT_AUDIO_IDLE)
    return bytes(out), len(sites)


def build_script(name: str, edits: list[dict], image: bytes,
                 hold_for_voice: bool = True) -> dict:
    if name not in KNOWN_SCRIPTS:
        raise ConfigError(f'"{name}" is not a known dialogue script (have: '
                          f'{", ".join(KNOWN_SCRIPTS)}). See the module docstring for how to '
                          f'locate a new one.')
    if not edits:
        raise ConfigError(f"{name}: no replacements listed")

    start, end = KNOWN_SCRIPTS[name]
    original = image[start:end]
    if not original:
        raise ConfigError(f"{name}: file offsets {start:#x}-{end:#x} read no data -- is "
                          f"assets/default.xex the expected build?")

    # Every "find" is matched against the pristine *original* bytes, never
    # against a blob already mutated by earlier edits in this loop -- a
    # replacement can legitimately contain another edit's find text (e.g.
    # a rewritten line that happens to include the word "souls" ahead of
    # the original "souls" line), and matching against mutated output would
    # make that a false "ambiguous" collision, or worse, replace text that
    # was itself just inserted by a previous edit.
    spans = []  # (start, end, replacement_bytes), sorted by start
    for i, edit in enumerate(edits):
        if "find" not in edit or "replace" not in edit:
            raise ConfigError(f'{name}[{i}]: needs "find" and "replace"')
        find = edit["find"].encode("ascii")
        wrap_width = int(edit.get("wrap_width", DEFAULT_WRAP_WIDTH))
        if wrap_width > 0:
            # Opcode 0x01 is the interpreter's plain "advance to next visual
            # line" (case 1 in sub_823A2F28) -- no parameters needed, unlike
            # the \x03<delay>\x02<box> sequence the original script uses
            # between *spoken* lines (which also switches portrait/timing
            # state and would be wrong to reuse here for an in-box wrap).
            replace = b"\x01".join(line.encode("ascii", "replace")
                                    for line in word_wrap(edit["replace"], wrap_width))
        else:
            replace = edit["replace"].encode("ascii", "replace")
        count = original.count(find)
        if count == 0:
            raise ConfigError(f'{name}[{i}]: "{edit["find"]}" not found in the original script')
        occurrence = int(edit.get("occurrence", 1))
        if occurrence < 1 or occurrence > count:
            raise ConfigError(f'{name}[{i}]: "{edit["find"]}" appears {count} time(s) in the '
                              f'script, but "occurrence" is {occurrence}')
        if count > 1 and "occurrence" not in edit:
            raise ConfigError(f'{name}[{i}]: "{edit["find"]}" appears {count} times in the '
                              f'script -- ambiguous, add "occurrence" (1-based) to pick one')
        match_start = original.find(find)
        for _ in range(occurrence - 1):
            match_start = original.find(find, match_start + 1)
        spans.append((match_start, match_start + len(find), replace))

    spans.sort(key=lambda s: s[0])
    for (_, prev_end, _), (next_start, _, _) in zip(spans, spans[1:]):
        if next_start < prev_end:
            raise ConfigError(f"{name}: two \"find\" matches overlap around byte {next_start} -- "
                              f"one is a substring of the other's match region")

    new_blob = bytearray()
    cursor = 0
    for match_start, match_end, replace in spans:
        new_blob += original[cursor:match_start]
        new_blob += replace
        cursor = match_end
    new_blob += original[cursor:]

    holds = 0
    if hold_for_voice:
        try:
            new_blob, holds = insert_voice_holds(bytes(new_blob))
        except ConfigError as exc:
            raise ConfigError(f"{name}: {exc}") from None

    # The signature used to find this script live: a slice of the *original*
    # bytes starting at the first edit's match, long enough to be unique
    # (the shared per-event header before it repeats in every language, so
    # the signature must start inside the language-specific text).
    first_find = edits[0]["find"].encode("ascii")
    sig_offset = original.find(first_find)
    if sig_offset < 0:
        raise ConfigError(f'{name}: internal error locating signature for "{edits[0]["find"]}"')
    signature = original[sig_offset:sig_offset + 64] or original[sig_offset:]

    return {
        "name": name,
        "vanilla_addr": IMAGE_BASE + start,
        "signature": bytes(signature),
        "signature_offset": sig_offset,  # signature_addr - script_addr
        "new_blob": bytes(new_blob),
        "voice_holds": holds,
    }


MOD_MAIN_TEMPLATE = '''\
// Generated by scripts/build_dialogue_mod.py -- do not edit by hand.
//
// Replaces cutscene-dialogue script text by relocation, not in-place
// overwrite: the original script has no length prefix or padding (see the
// script's module docstring), so a same-length-or-shorter in-place patch
// either truncates a longer replacement or, worse, corrupts the very next
// language's script, which starts at the very next byte with no gap.
//
// Instead this allocates a fresh guest buffer, writes a full replacement
// copy of the script there (any length), and repoints the single 4-byte
// slot in the game's per-language script table that referenced the
// original -- found live by content/value signature, not a hardcoded
// address, so this keeps working across vanilla and title-update builds.
// The original bytes (this language's or any other's) are never touched.

#include <rex/system/mod_plugin.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <cstring>

namespace {{

constexpr uint32_t kImageBegin = 0x82000000u;
constexpr uint32_t kImageEnd = 0x83210000u;

struct ScriptPatch {{
  const char* name;
  uint32_t vanilla_addr;      // scan window center for the original script
  const uint8_t* signature;   // unique slice of the *original* script bytes
  uint32_t signature_len;
  uint32_t signature_offset;  // signature_addr - script_addr
  const uint8_t* new_blob;
  uint32_t new_blob_len;
}};

{signature_arrays}

constexpr ScriptPatch kPatches[] = {{
{patch_rows}
}};

constexpr uint32_t kScanMargin = 0x{scan_margin:X}u;

class GeneratedDialogueMod : public rex::system::IModPlugin {{
 public:
  explicit GeneratedDialogueMod(rex::Runtime* runtime) : runtime_(runtime) {{}}

  // default.xex's static data (the original scripts and the table that
  // points into them) is in guest memory by the time this fires.
  void OnModuleLaunched() override {{
    if (!runtime_ || !runtime_->memory()) {{
      return;
    }}
    for (const auto& patch : kPatches) {{
      ApplyPatch(patch);
    }}
  }}

 private:
  void ApplyPatch(const ScriptPatch& patch) {{
    auto* memory = runtime_->memory();

    // 1. Find the live address of the original script by content -- a
    // hardcoded offline address can't be trusted; see docs/making-mods.md.
    const uint32_t sig_addr = FindSignature(memory, patch);
    if (!sig_addr) {{
      REXLOG_WARN("[{mod_id}] couldn't find the original \\"{{}}\\" script in memory -- "
                  "leaving dialogue unchanged", patch.name);
      return;
    }}
    const uint32_t script_addr = sig_addr - patch.signature_offset;

    // 2. Find the 4-byte pointer that references it (the table slot for
    // this language), by scanning for its *value* rather than assuming a
    // fixed table layout.
    const uint32_t table_slot_addr = FindPointerTo(memory, script_addr, patch.vanilla_addr);
    if (!table_slot_addr) {{
      REXLOG_WARN("[{mod_id}] found \\"{{}}\\" at {{:X}} but not the table pointer to it -- "
                  "leaving dialogue unchanged", patch.name, script_addr);
      return;
    }}

    // 3. Allocate a fresh guest buffer (any length) and write the
    // replacement script into it.
    const uint32_t new_addr = memory->SystemHeapAlloc(patch.new_blob_len);
    if (!new_addr) {{
      REXLOG_WARN("[{mod_id}] guest allocation failed for \\"{{}}\\" ({{}} bytes)",
                  patch.name, patch.new_blob_len);
      return;
    }}
    uint8_t* dest = memory->TranslateVirtual<uint8_t*>(new_addr);
    if (!dest) {{
      REXLOG_WARN("[{mod_id}] couldn't translate freshly allocated address {{:X}}", new_addr);
      return;
    }}
    std::memcpy(dest, patch.new_blob, patch.new_blob_len);

    // 4. Repoint the table slot at the new buffer. The slot itself is
    // read-only static data, same as the wave-bank table.
    if (Write(table_slot_addr, new_addr)) {{
      REXLOG_INFO("[{mod_id}] \\"{{}}\\" script {{:X}} -> {{:X}} ({{}} bytes, table slot {{:X}})",
                  patch.name, script_addr, new_addr, patch.new_blob_len, table_slot_addr);
    }} else {{
      REXLOG_WARN("[{mod_id}] failed to repoint table slot {{:X}} for \\"{{}}\\"",
                  table_slot_addr, patch.name);
    }}
  }}

  uint32_t FindSignature(rex::memory::Memory* memory, const ScriptPatch& patch) {{
    const uint32_t lo = patch.vanilla_addr > kScanMargin + kImageBegin
                            ? patch.vanilla_addr - kScanMargin
                            : kImageBegin;
    const uint32_t hi = patch.vanilla_addr + kScanMargin < kImageEnd
                            ? patch.vanilla_addr + kScanMargin
                            : kImageEnd;
    for (uint32_t addr = lo; addr + patch.signature_len <= hi; ++addr) {{
      const auto* bytes = memory->TranslateVirtual<const uint8_t*>(addr);
      if (bytes && std::memcmp(bytes, patch.signature, patch.signature_len) == 0) {{
        return addr;
      }}
    }}
    return 0;
  }}

  uint32_t FindPointerTo(rex::memory::Memory* memory, uint32_t target, uint32_t vanilla_script_addr) {{
    // The table itself sits near the scripts it points into; scanning the
    // same generous window around the vanilla script address covers it.
    const uint32_t lo = vanilla_script_addr > kScanMargin + kImageBegin
                            ? vanilla_script_addr - kScanMargin
                            : kImageBegin;
    const uint32_t hi = vanilla_script_addr + kScanMargin < kImageEnd
                            ? vanilla_script_addr + kScanMargin
                            : kImageEnd;
    for (uint32_t addr = lo; addr + 4 <= hi; addr += 4) {{
      const auto* word = memory->TranslateVirtual<const uint8_t*>(addr);
      if (word && rex::memory::load_and_swap<uint32_t>(word) == target) {{
        return addr;
      }}
    }}
    return 0;
  }}

  // The table slot is in read-only static data; unlock before writing.
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
  return new GeneratedDialogueMod(ctx->runtime);
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


def cpp_byte_array(name: str, data: bytes) -> str:
    rows = ", ".join(f"0x{b:02X}u" for b in data)
    return f"constexpr uint8_t {name}[] = {{{rows}}};"


def toml_string(value: object) -> str:
    escaped = (str(value)
               .replace("\\", "\\\\")
               .replace('"', '\\"')
               .replace("\n", "\\n")
               .replace("\r", "\\r")
               .replace("\t", "\\t"))
    return f'"{escaped}"'


def generate_sources(mod: dict, scripts: list[dict], src_dir: Path) -> None:
    mod_id = mod["id"]

    arrays = []
    rows = []
    for i, s in enumerate(scripts):
        sig_name = f"kSig{i}"
        blob_name = f"kBlob{i}"
        arrays.append(cpp_byte_array(sig_name, s["signature"]))
        arrays.append(cpp_byte_array(blob_name, s["new_blob"]))
        rows.append(
            f'    {{"{s["name"]}", 0x{s["vanilla_addr"]:X}u, {sig_name}, '
            f'{len(s["signature"])}u, {s["signature_offset"]}u, {blob_name}, '
            f'{len(s["new_blob"])}u}},')

    src_dir.mkdir(parents=True, exist_ok=True)
    (src_dir / "mod_main.cpp").write_text(
        MOD_MAIN_TEMPLATE.format(signature_arrays="\n".join(arrays), patch_rows="\n".join(rows),
                                 scan_margin=SCAN_MARGIN, mod_id=mod_id),
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
    desc = mod.get("description", f"Dialogue text replacements for {', '.join(s['name'] for s in scripts)}.")
    toml_lines += [
        f"description = {toml_string(desc)}",
        f"code = {toml_string(mod_id)}",
        'platform = ""',
        "",
    ]
    manifest = "\n".join(toml_lines)

    try:
        import tomllib
        parsed = tomllib.loads(manifest)
        if parsed.get("code") != mod_id:
            raise ValueError(f'"code" round-tripped as {parsed.get("code")!r}')
    except Exception as exc:  # noqa: BLE001
        raise ConfigError(f"generated mod.toml is invalid ({exc})") from exc

    (src_dir / "mod.toml").write_text(manifest, encoding="utf-8")


def build_and_install(mod_id: str, src_dir: Path, build_type: str, install_dir: Path,
                      sdk_dir: Path) -> None:
    build_dir = src_dir.parent.parent / "out" / "build" / "dialogue_mods" / build_type / mod_id
    subprocess.run(
        ["cmake", "-S", str(src_dir), "-B", str(build_dir), "-G", "Ninja",
         f"-DCMAKE_BUILD_TYPE={build_type}", f"-DCMAKE_PREFIX_PATH={sdk_dir}",
         "-DCMAKE_CXX_COMPILER=clang++"],
        check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--parallel"], check=True)

    suffix = "rd" if build_type == "RelWithDebInfo" else ""
    code_dir = install_dir / "code" / "windows-x64"
    code_dir.mkdir(parents=True, exist_ok=True)
    for ext in ("dll", "pdb"):
        built = build_dir / f"{mod_id}.{ext}"
        if built.exists():
            shutil.copyfile(built, code_dir / f"{mod_id}{suffix}.{ext}")

    shutil.copyfile(src_dir / "mod.toml", install_dir / "mod.toml")
    print(f"Installed mod to {install_dir}")


EXAMPLE_CONFIG = {
    "mod": {
        "id": "die_monster_text",
        "name": "Die Monster Text",
        "version": "1.0.0",
        "description": "Replaces the opening dialogue text.",
    },
    "dialogue": {
        "sd_dora1_intro_en": [
            {"find": "Die monster.", "replace": "PLACEHOLDER: any length works"},
            {"find": "You don't belong", "replace": "PLACEHOLDER: this line too"},
            {"find": "in this world!", "replace": "PLACEHOLDER: no length limit at all"},
        ]
    },
}


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", nargs="?", help="Path to the JSON config")
    parser.add_argument("--example", action="store_true", help="Print an example config and exit")
    parser.add_argument("--xex", type=Path, default=DEFAULT_XEX,
                        help=f"default.xex to read original scripts from (default: {DEFAULT_XEX})")
    parser.add_argument("--src-root", type=Path, default=DEFAULT_MODS_SRC,
                        help=f"Mod source root; sources go to <root>/<id> "
                             f"(default: {DEFAULT_MODS_SRC})")
    parser.add_argument("--build", action="store_true", help="Also compile the plugin")
    parser.add_argument("--install", action="store_true",
                        help="Assemble the finished mod into this repo's mods/<id> (implies --build)")
    parser.add_argument("--release", action="store_true",
                        help="Build Release; default is RelWithDebInfo")
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
    dialogue_cfg = config.get("dialogue") or {}
    if not dialogue_cfg:
        raise ConfigError('config needs a "dialogue" object with at least one script')

    image = load_image(args.xex)
    scripts = []
    for name, entry in dialogue_cfg.items():
        # A script maps either straight to its list of edits, or to an object
        # carrying per-script options alongside them.
        if isinstance(entry, dict):
            edits = entry.get("edits")
            if edits is None:
                raise ConfigError(f'{name}: object form needs an "edits" list')
            hold = bool(entry.get("hold_for_voice", True))
        else:
            edits, hold = entry, True
        scripts.append(build_script(name, edits, image, hold_for_voice=hold))

    src_dir = args.src_root / mod["id"]
    src_dir.mkdir(parents=True, exist_ok=True)
    generate_sources(mod, scripts, src_dir)
    print(f"Wrote mod source to {src_dir}")
    for s in scripts:
        print(f"  {s['name']}: {len(s['new_blob'])} bytes (signature @ +0x{s['signature_offset']:X}, "
              f"{s['voice_holds']} voice hold(s) inserted)")

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
