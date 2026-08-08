#!/usr/bin/env python3
"""Build a mod from a single JSON description: swap dialogue audio, dialogue
text, or both together, in one step.

    python scripts/build_mod.py my_mod.json
    python scripts/build_mod.py my_mod.json --build --install
    python scripts/build_mod.py --example

Config format (see --example for a ready-to-edit one):

    {
      "mod": {
        "id": "die_monster_reimagined",
        "name": "Die Monster, Reimagined",
        "version": "1.0.0",
        "description": "..."
      },
      "banks": { ... optional, see "Replacing voice audio" below ... },
      "dialogue": { ... optional, see "Replacing dialogue text" below ... }
    }

A config needs at least one of "banks" or "dialogue"; give both to ship a
single mod that replaces a line's audio and its on-screen text together, or
just one to touch only that side. Either way this produces exactly one mod
folder (one `mod.toml`, one DLL) -- a player enables one `[[mods]]` entry in
`mods/mods.toml`, not two.

Replacing voice audio
----------------------
"banks" maps a `.xwb` wave-bank name to a list of replacements:

    "banks": {
      "sd_dora1": [
        {"cue": "011A", "input": "C:/Music/song.flac", "start": 32.074, "end": 43.294}
      ]
    }

Each replacement needs `cue` (from `repack_xact_wave.py list`) or `index`,
an `input` audio file (anything ffmpeg reads), and `start` plus either `end`
or `duration` (seconds; omit both to match the original entry's length).
`channels`/`samplerate` are optional -- left out, the best quality that fits
the bank's remaining byte budget is chosen automatically.

Why a code mod is needed for this: the game never asks the filesystem how
big a wave bank is. Its overlay loader (`hd_read_trans`, sub_82252A30) keeps
a table of 44-byte records in static data, one per sound bank, each holding
that bank's *baked-in* size (`+0x00` .xsb size, `+0x04` .xwb size, `+0x14`
-> name string), and reads exactly that many bytes -- so a modded bank
bigger than the shipped one has everything past the original end-of-file
simply never read; the tail is stale buffer contents that plays as static.
Raising that one dword per bank is the whole fix, and it's what the
generated mod does at startup, locating each record live by its two-size
signature (not a hardcoded address) so it keeps working across vanilla and
title-update builds.

Replacements are appended *past the original end of file* and the target
entry is repointed there -- no existing entry ever moves, since relocating
one corrupts its playback for reasons still not understood. Every `.xwb`
load reads into one fixed 1,630,208-byte (0x18E000) buffer allocated at
startup (sub_824FBE98); that's the hard ceiling `--budget` accounting is
against, and audio is written back as raw 16-bit big-endian PCM (the bank
is big-endian throughout) since there's no open-source XMA2 encoder and the
guest's PCM path is fully implemented already.

Replacing dialogue text
-------------------------
"dialogue" maps a known cutscene-script name to a list of find/replace
edits:

    "dialogue": {
      "sd_dora1_intro_en": [
        {"find": "Die monster.", "replace": "Placeholder text of any length"},
        {"find": "You don't belong", "replace": "works for a shorter"},
        {"find": "of", "occurrence": 1, "replace": "disambiguated by position"}
      ]
    }

Each `find` must match exactly one place in the script's original English
text (byte-exact, case-sensitive) -- ambiguous or missing matches are a
config error, not a silent no-op. `replace` can be any length. If `find`
genuinely repeats (a short word used in two different lines), add
`"occurrence": N` (1-based, in order of appearance) instead of dropping
that word from the config -- omitting it doesn't remove the original text,
it just leaves the ambiguity for the tool to reject.

Why this needs different tooling than item/enemy text: those live in
fixed-length, null-padded fields (see `rexmod/text_patch.h`), so a
same-length-or-shorter in-place patch is safe. Cutscene dialogue is a
compact opcode/control-code script -- opcode bytes interleaved directly with
literal ASCII text, no length prefix, no padding before the next
language's script, which starts at the very next byte. So this doesn't
patch in place at all: it allocates a fresh guest buffer, writes a full
replacement copy of the script there (any length), and repoints the single
4-byte pointer that referenced the original -- the same append-elsewhere,
repoint-a-pointer trick "banks" uses for wave-bank audio. Both the original
script and the pointer to it are located live by content/value signature,
never a hardcoded offline address.

Currently only "sd_dora1_intro_en" (the English opening exchange) has a
known byte range; add more to KNOWN_SCRIPTS as they're located the same way
(decompile the per-language script-pointer table's consumer, sub_823A2F28,
and read the gap between one language slot's pointer and the next's).

By default, editing "dialogue" also makes each speaker's box hold on screen
until that speaker's voice line has actually finished playing (see
`insert_voice_holds()`) -- vanilla doesn't need this because the shipped
lines take long enough to type out that the voice is over before the box
would close anyway, but a shortened or lengthened replacement line breaks
that assumption. Opt out per script with `"hold_for_voice": false`:

    "dialogue": {
      "sd_dora1_intro_en": {"hold_for_voice": false, "edits": [ ... ]}
    }

See scripts/re/trace_dialogue_script.py for how the timing opcodes
(op9/op10/op11) were traced, and extract_xact_audio.py / repack_xact_wave.py
for the lower-level tools this script's "banks" side is built on (useful on
their own for listing a bank's cue names or replacing a single entry from
the CLI without building a full mod).
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
sys.path.insert(0, str(Path(__file__).resolve().parent / "re"))
from extract_xact_audio import parse_xsb_cue_names, parse_xwb  # noqa: E402
from repack_xact_wave import (  # noqa: E402
    _write_entry, encode_miniformat, extract_pcm_slice,
)
from dump_xex_image import decrypt_xex_image, _load_gen_icon  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOUND_DIR = REPO_ROOT / "assets" / "DATA" / "sound"
DEFAULT_XEX = REPO_ROOT / "assets" / "default.xex"
DEFAULT_MODS_SRC = REPO_ROOT.parent / "NocturneRecomp-Mods" / "src"

IMAGE_BASE = 0x82000000


class ConfigError(Exception):
    """A problem with the user's JSON that they need to fix."""


# --------------------------------------------------------------------------
# Voice audio ("banks")
# --------------------------------------------------------------------------

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


# --------------------------------------------------------------------------
# Dialogue text ("dialogue")
# --------------------------------------------------------------------------

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


# --------------------------------------------------------------------------
# Code generation (shared by both, one mod either way)
# --------------------------------------------------------------------------

MOD_MAIN_TEMPLATE = '''\
// Generated by scripts/build_mod.py -- do not edit by hand.
//
// {kind_comment}

#include <rex/system/mod_plugin.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <cstring>

namespace {{

{bank_decls}
{script_decls}

class GeneratedMod : public rex::system::IModPlugin {{
 public:
  explicit GeneratedMod(rex::Runtime* runtime) : runtime_(runtime) {{}}

  // Both default.xex's static data and any loaded wave bank are only
  // meaningful in guest memory by the time this fires (module about to
  // launch); no bank is loaded until gameplay first asks for one.
  void OnModuleLaunched() override {{
    if (!runtime_ || !runtime_->memory()) {{
      return;
    }}
{bank_apply_calls}
{script_apply_calls}
  }}

 private:
{bank_methods}
{script_methods}
  // The target is in read-only static data; the page must be unlocked or
  // the write access-violates.
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
  return new GeneratedMod(ctx->runtime);
}}
'''

BANK_DECLS_TEMPLATE = '''\
// Voice-bank load-size table (vanilla: record 0 at 0x82E00CF8, 44-byte
// stride). Located by signature, not a hardcoded address: each 44-byte
// record in the loader's bank table starts with the bank's shipped .xsb and
// .xwb sizes, which are unique per bank across all 55 banks. See
// scripts/build_mod.py's module docstring for why a load-size patch is
// needed at all.
constexpr uint32_t kBankScanBegin = 0x82E00000u;
constexpr uint32_t kBankScanEnd = 0x82E02000u;
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

constexpr BankPatch kBankPatches[] = {{
{patch_rows}
}};

{static_asserts}\
'''

SCRIPT_DECLS_TEMPLATE = '''\
// Dialogue-script relocation table. See scripts/build_mod.py's module
// docstring for why this relocates the whole script rather than patching in
// place.
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

constexpr ScriptPatch kScriptPatches[] = {{
{patch_rows}
}};

constexpr uint32_t kScriptScanMargin = 0x{scan_margin:X}u;\
'''

BANK_METHODS = '''\
  void ApplyBankPatch(const BankPatch& patch) {
    auto* memory = runtime_->memory();
    uint32_t found = 0;
    for (uint32_t addr = kBankScanBegin; addr + 0x2C <= kBankScanEnd; addr += 4) {
      const auto* record = memory->TranslateVirtual<const uint8_t*>(addr);
      if (!record) {
        continue;
      }
      if (rex::memory::load_and_swap<uint32_t>(record) != patch.xsb_size ||
          rex::memory::load_and_swap<uint32_t>(record + kXwbSizeOffset) != patch.xwb_size) {
        continue;
      }
      // Every real record's +0x14 is a pointer to the bank's name string;
      // requiring it to point into the image rejects stray size-pair
      // lookalikes in unrelated data.
      const uint32_t name_ptr = rex::memory::load_and_swap<uint32_t>(record + kNamePtrOffset);
      if (name_ptr < kImageBegin || name_ptr >= kImageEnd) {
        continue;
      }
      if (Write(addr + kXwbSizeOffset, patch.new_size)) {
        ++found;
      }
    }

    if (found == 0) {
      REXLOG_WARN("[{mod_id}] couldn't find the bank-table record for {} "
                  "(.xsb {:X} / .xwb {:X}) -- it will play as static",
                  patch.name, patch.xsb_size, patch.xwb_size);
    } else {
      // More than one record can point at the same bank; all of them need
      // the new size or whichever the game happens to use may be stale.
      REXLOG_INFO("[{mod_id}] {}.xwb load size {:X} -> {:X} bytes ({} record(s))",
                  patch.name, patch.xwb_size, patch.new_size, found);
    }
  }

'''

SCRIPT_METHODS = '''\
  void ApplyScriptPatch(const ScriptPatch& patch) {
    auto* memory = runtime_->memory();

    // 1. Find the live address of the original script by content -- a
    // hardcoded offline address can't be trusted; see docs/making-mods.md.
    const uint32_t sig_addr = FindSignature(memory, patch);
    if (!sig_addr) {
      REXLOG_WARN("[{mod_id}] couldn't find the original \\"{}\\" script in memory -- "
                  "leaving dialogue unchanged", patch.name);
      return;
    }
    const uint32_t script_addr = sig_addr - patch.signature_offset;

    // 2. Find the 4-byte pointer that references it (the table slot for
    // this language), by scanning for its *value* rather than assuming a
    // fixed table layout.
    const uint32_t table_slot_addr = FindPointerTo(memory, script_addr, patch.vanilla_addr);
    if (!table_slot_addr) {
      REXLOG_WARN("[{mod_id}] found \\"{}\\" at {:X} but not the table pointer to it -- "
                  "leaving dialogue unchanged", patch.name, script_addr);
      return;
    }

    // 3. Allocate a fresh guest buffer (any length) and write the
    // replacement script into it.
    const uint32_t new_addr = memory->SystemHeapAlloc(patch.new_blob_len);
    if (!new_addr) {
      REXLOG_WARN("[{mod_id}] guest allocation failed for \\"{}\\" ({} bytes)",
                  patch.name, patch.new_blob_len);
      return;
    }
    uint8_t* dest = memory->TranslateVirtual<uint8_t*>(new_addr);
    if (!dest) {
      REXLOG_WARN("[{mod_id}] couldn't translate freshly allocated address {:X}", new_addr);
      return;
    }
    std::memcpy(dest, patch.new_blob, patch.new_blob_len);

    // 4. Repoint the table slot at the new buffer. The slot itself is
    // read-only static data, same as the wave-bank table.
    if (Write(table_slot_addr, new_addr)) {
      REXLOG_INFO("[{mod_id}] \\"{}\\" script {:X} -> {:X} ({} bytes, table slot {:X})",
                  patch.name, script_addr, new_addr, patch.new_blob_len, table_slot_addr);
    } else {
      REXLOG_WARN("[{mod_id}] failed to repoint table slot {:X} for \\"{}\\"",
                  table_slot_addr, patch.name);
    }
  }

  uint32_t FindSignature(rex::memory::Memory* memory, const ScriptPatch& patch) {
    const uint32_t lo = patch.vanilla_addr > kScriptScanMargin + kImageBegin
                            ? patch.vanilla_addr - kScriptScanMargin
                            : kImageBegin;
    const uint32_t hi = patch.vanilla_addr + kScriptScanMargin < kImageEnd
                            ? patch.vanilla_addr + kScriptScanMargin
                            : kImageEnd;
    for (uint32_t addr = lo; addr + patch.signature_len <= hi; ++addr) {
      const auto* bytes = memory->TranslateVirtual<const uint8_t*>(addr);
      if (bytes && std::memcmp(bytes, patch.signature, patch.signature_len) == 0) {
        return addr;
      }
    }
    return 0;
  }

  uint32_t FindPointerTo(rex::memory::Memory* memory, uint32_t target, uint32_t vanilla_script_addr) {
    // The table itself sits near the scripts it points into; scanning the
    // same generous window around the vanilla script address covers it.
    const uint32_t lo = vanilla_script_addr > kScriptScanMargin + kImageBegin
                            ? vanilla_script_addr - kScriptScanMargin
                            : kImageBegin;
    const uint32_t hi = vanilla_script_addr + kScriptScanMargin < kImageEnd
                            ? vanilla_script_addr + kScriptScanMargin
                            : kImageEnd;
    for (uint32_t addr = lo; addr + 4 <= hi; addr += 4) {
      const auto* word = memory->TranslateVirtual<const uint8_t*>(addr);
      if (word && rex::memory::load_and_swap<uint32_t>(word) == target) {
        return addr;
      }
    }
    return 0;
  }

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
    """Quote a value as a TOML basic string.

    Not optional politeness: mod descriptions routinely contain quotes (a
    line of dialogue, say), and writing one through raw produces a mod.toml
    the loader refuses to parse. It fails *quietly* -- the manifest is
    skipped, so `code = ...` is never read and the plugin never loads, while
    the mod's assets still apply. For a "banks" mod the result is a bank
    whose replacement audio is present but whose size patch is missing:
    pure static.
    """
    escaped = (str(value)
               .replace("\\", "\\\\")
               .replace('"', '\\"')
               .replace("\n", "\\n")
               .replace("\r", "\\r")
               .replace("\t", "\\t"))
    return f'"{escaped}"'


def generate_sources(mod: dict, banks: list[dict], scripts: list[dict], src_dir: Path) -> None:
    mod_id = mod["id"]

    bank_decls = script_decls = ""
    bank_apply_calls = script_apply_calls = ""
    bank_methods = script_methods = ""

    if banks:
        rows = "\n".join(
            f'    {{"{b["name"]}", 0x{b["xsb_size"]:X}u, 0x{b["xwb_size"]:X}u, 0x{b["new_size"]:X}u}},'
            for b in banks)
        asserts = "\n".join(
            f'static_assert(0x{b["new_size"]:X}u <= kWaveBankBufferSize,\n'
            f'              "{b["name"]}.xwb would overrun the game\'s wave-bank buffer");'
            for b in banks)
        bank_decls = BANK_DECLS_TEMPLATE.format(patch_rows=rows, static_asserts=asserts)
        bank_apply_calls = ("    for (const auto& patch : kBankPatches) {\n"
                            "      ApplyBankPatch(patch);\n"
                            "    }\n")
        bank_methods = BANK_METHODS.replace("{mod_id}", mod_id)

    if scripts:
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
        script_decls = SCRIPT_DECLS_TEMPLATE.format(
            signature_arrays="\n".join(arrays), patch_rows="\n".join(rows),
            scan_margin=SCAN_MARGIN)
        # kImageBegin/kImageEnd are shared with the bank side; declare them
        # here too if this mod has no "banks" section.
        if not banks:
            script_decls = (
                "constexpr uint32_t kImageBegin = 0x82000000u;\n"
                "constexpr uint32_t kImageEnd = 0x83210000u;\n\n" + script_decls)
        script_apply_calls = ("    for (const auto& patch : kScriptPatches) {\n"
                              "      ApplyScriptPatch(patch);\n"
                              "    }\n")
        script_methods = SCRIPT_METHODS.replace("{mod_id}", mod_id)

    if banks and scripts:
        kind_comment = ("Combined voice-audio and dialogue-text replacement mod. See\n"
                        "// scripts/build_mod.py's module docstring for the full mechanism\n"
                        "// behind each half.")
    elif banks:
        kind_comment = "Voice-audio replacement mod. See scripts/build_mod.py's module docstring."
    else:
        kind_comment = "Dialogue-text replacement mod. See scripts/build_mod.py's module docstring."

    src_dir.mkdir(parents=True, exist_ok=True)
    (src_dir / "mod_main.cpp").write_text(
        MOD_MAIN_TEMPLATE.format(
            kind_comment=kind_comment,
            bank_decls=bank_decls, script_decls=script_decls,
            bank_apply_calls=bank_apply_calls, script_apply_calls=script_apply_calls,
            bank_methods=bank_methods, script_methods=script_methods),
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
    default_desc_parts = [b["name"] for b in banks] + [s["name"] for s in scripts]
    desc = mod.get("description", f"Replacements for {', '.join(default_desc_parts)}.")
    toml_lines += [
        f"description = {toml_string(desc)}",
        f"code = {toml_string(mod_id)}",
        'platform = ""',
        "",
    ]
    manifest = "\n".join(toml_lines)

    # Verify rather than trust: a manifest the loader can't parse disables
    # the code plugin silently, and the failure only shows up much later as
    # unreplaced audio/text.
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
    build_dir = src_dir.parent.parent / "out" / "build" / "mods" / build_type / mod_id
    subprocess.run(
        ["cmake", "-S", str(src_dir), "-B", str(build_dir), "-G", "Ninja",
         f"-DCMAKE_BUILD_TYPE={build_type}", f"-DCMAKE_PREFIX_PATH={sdk_dir}",
         "-DCMAKE_CXX_COMPILER=clang++"],
        check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--parallel"], check=True)

    # RelWithDebInfo game builds load "<stem>rd.dll"; Release builds load
    # "<stem>.dll". Naming it wrong is silently fatal: the loader logs a
    # failure and none of this mod's patches ever run.
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

    # docs/making-mods.md: "platform" is written by build tooling, not read
    # by it -- a fresh manifest leaves it empty, and after a successful build
    # it's (re)set to whichever platform(s) install_dir/code/ actually ships
    # a binary for. Here that's always just windows-x64, since this script
    # only ever invokes a local (host) compiler.
    set_platform(install_dir / "mod.toml", "windows-x64")
    print(f"Installed mod to {install_dir}")


def set_platform(mod_toml: Path, platform: str) -> None:
    lines = mod_toml.read_text(encoding="utf-8").splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.startswith("platform "):
            lines[i] = f"platform = {toml_string(platform)}\n"
            break
    mod_toml.write_text("".join(lines), encoding="utf-8")


EXAMPLE_CONFIG = {
    "mod": {
        "id": "die_monster_reimagined",
        "name": "Die Monster, Reimagined",
        "version": "1.0.0",
        "description": "Replaces Dracula's opening line, audio and text together.",
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
    parser.add_argument("--example", action="store_true",
                        help="Print an example config and exit")
    parser.add_argument("--sound-dir", type=Path, default=DEFAULT_SOUND_DIR,
                        help=f"Original wave banks, for \"banks\" (default: {DEFAULT_SOUND_DIR})")
    parser.add_argument("--xex", type=Path, default=DEFAULT_XEX,
                        help=f"default.xex to read original scripts from, for \"dialogue\" "
                             f"(default: {DEFAULT_XEX})")
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
    dialogue_cfg = config.get("dialogue") or {}
    if not banks_cfg and not dialogue_cfg:
        raise ConfigError('config needs a "banks" and/or "dialogue" object with at least '
                          'one replacement')

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

    scripts = []
    if dialogue_cfg:
        image = load_image(args.xex)
        for name, entry in dialogue_cfg.items():
            # A script maps either straight to its list of edits, or to an
            # object carrying per-script options alongside them.
            if isinstance(entry, dict):
                edits = entry.get("edits")
                if edits is None:
                    raise ConfigError(f'{name}: object form needs an "edits" list')
                hold = bool(entry.get("hold_for_voice", True))
            else:
                edits, hold = entry, True
            scripts.append(build_script(name, edits, image, hold_for_voice=hold))

    generate_sources(mod, banks, scripts, src_dir)
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
