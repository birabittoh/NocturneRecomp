#!/usr/bin/env python3
"""Replace one voice line / sound effect inside an XACT wave bank (.xwb).

For swapping an *entire* asset file (e.g. a .wma BGM track) you don't need
this script at all -- just drop the replacement at the matching path under
a mod's game/ folder, per docs/making-mods.md. This script is only for
replacing a single entry *inside* a .xwb wave bank (one voice line/sfx cue
among many packed in the same file), which requires re-encoding the
replacement audio and re-packing the container around it.

We don't have an XMA2 encoder available (ffmpeg can decode XMA2 but not
encode it), so replacement entries are written back as raw 16-bit PCM
(WAVEBANKMINIWAVEFORMAT tag 0) instead of XMA. The rest of the bank
(everything except the replaced entry's bytes/length/format field and the
byte offsets of entries that come after it) is left untouched.

Usage:
    # List entries/cues in a bank
    python scripts/repack_xact_wave.py list assets/DATA/sound/sd_dora1.xwb

    # Replace entry by cue name, from a slice of another audio file
    python scripts/repack_xact_wave.py replace assets/DATA/sound/sd_dora1.xwb \\
        --cue 011A --input "some_song.flac" --start 32.074 \\
        --output extracted/audio/sd_dora1_patched.xwb

    # Replace by index instead of cue name, explicit duration override
    python scripts/repack_xact_wave.py replace assets/DATA/sound/sd_dora1.xwb \\
        --index 12 --input line.wav --duration 3.9 --output out.xwb
"""

from __future__ import annotations

import argparse
import array
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_xact_audio import (  # noqa: E402
    CODEC_NAMES, build_xma2_wav, ffmpeg_decode, ffprobe_duration,
    parse_xsb_cue_names, parse_xwb,
)

ENTRY_TABLE_BASE = 0x90
ENTRY_SIZE = 24


def decode_entry_to_wav(entry: dict, out_path: Path) -> tuple[bool, str]:
    """Decode a bank entry (any codec tag we understand) to a PCM WAV."""
    if entry["tag"] == 1:  # XMA
        return ffmpeg_decode(build_xma2_wav(entry), out_path)
    if entry["tag"] == 0:  # PCM already
        with wave.open(str(out_path), "wb") as w:
            w.setnchannels(entry["channels"] or 1)
            w.setsampwidth(2)
            w.setframerate(entry["samplerate"] or 44100)
            w.writeframes(_byteswap16(entry["data"]))  # bank is PCM16BE
        return True, ""
    return False, f"unsupported codec tag {entry['tag']} ({CODEC_NAMES.get(entry['tag'], '?')})"


def cmd_list(args: argparse.Namespace) -> None:
    xwb_path = Path(args.xwb)
    bank = parse_xwb(xwb_path)
    names = parse_xsb_cue_names(xwb_path.with_suffix(".xsb"), bank["entry_count"])
    for e in bank["entries"]:
        idx = e["index"]
        cue = names[idx] if names else None
        codec = CODEC_NAMES.get(e["tag"], "?")
        # A replacement can't exceed the entry's own byte budget, so show what
        # that buys as 16-bit mono seconds at a few sample rates.
        budget = " ".join(f"{sr // 1000}k:{e['play_len'] / (2 * sr):.1f}s"
                          for sr in (11025, 22050, 44100))
        print(f"{idx:3d}  cue={str(cue):10s} codec={codec:4s} ch={e['channels']} "
              f"sr={e['samplerate']:6d} bytes={e['play_len']:7d}  budget[mono16] {budget}")


def encode_miniformat(tag: int, channels: int, samplerate: int, blockalign: int, is16bit: bool) -> int:
    v = tag & 0x3
    v |= (channels & 0x7) << 2
    v |= (samplerate & 0x3FFFF) << 5
    v |= (blockalign & 0xFF) << 23
    v |= (1 if is16bit else 0) << 31
    return v


def extract_pcm_slice(input_path: Path, start: float, duration: float,
                       samplerate: int, channels: int, tmp_path: Path) -> bytes:
    r = subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-ss", str(start), "-t", str(duration), "-i", str(input_path),
         "-ac", str(channels), "-ar", str(samplerate), "-sample_fmt", "s16",
         "-f", "wav", str(tmp_path)],
        capture_output=True, text=True,
    )
    if r.returncode != 0 or not tmp_path.exists():
        raise RuntimeError(f"ffmpeg failed to extract slice: {r.stderr}")
    with wave.open(str(tmp_path), "rb") as w:
        data = w.readframes(w.getnframes())
    tmp_path.unlink(missing_ok=True)
    # The whole wave bank is big-endian, samples included: Xbox 360 XACT PCM
    # entries are PCM16BE. ffmpeg hands us little-endian, which plays as static.
    return _byteswap16(data)


def _byteswap16(data: bytes) -> bytes:
    a = array.array("h")
    a.frombytes(data[: len(data) & ~1])
    a.byteswap()
    return a.tobytes() + data[len(data) & ~1:]


def cmd_replace(args: argparse.Namespace) -> None:
    xwb_path = Path(args.xwb)
    xsb_path = xwb_path.with_suffix(".xsb")
    bank = parse_xwb(xwb_path)
    names = parse_xsb_cue_names(xsb_path, bank["entry_count"])

    if args.cue is not None:
        if not names:
            sys.exit("no cue names resolved for this bank; use --index instead")
        matches = [e for e in bank["entries"] if names[e["index"]] == args.cue]
        if not matches:
            sys.exit(f"no entry with cue {args.cue!r}")
        target = matches[0]
    elif args.index is not None:
        target = bank["entries"][args.index]
    else:
        sys.exit("must pass --cue or --index")

    idx = target["index"]
    cue = names[idx] if names else None
    print(f"Replacing entry {idx} (cue={cue}, codec={CODEC_NAMES.get(target['tag'])}, "
          f"ch={target['channels']}, sr={target['samplerate']}, bytes={target['play_len']})")

    # Determine target duration.
    if args.duration is not None:
        duration = args.duration
    else:
        tmp_wav = xwb_path.parent / f".__orig_{idx}.wav"
        ok, err = decode_entry_to_wav(target, tmp_wav)
        if not ok:
            sys.exit(f"couldn't decode original entry to measure its duration: {err}")
        duration = ffprobe_duration(tmp_wav)
        tmp_wav.unlink(missing_ok=True)
        if duration is None:
            sys.exit("couldn't determine original entry duration; pass --duration explicitly")
    print(f"Target duration: {duration:.3f}s")

    channels = args.channels or target["channels"] or 2
    samplerate = args.samplerate or target["samplerate"] or 37800

    tmp_slice = xwb_path.parent / f".__slice_{idx}.wav"
    new_data = extract_pcm_slice(Path(args.input), args.start, duration, samplerate, channels, tmp_slice)
    new_fmt = encode_miniformat(0, channels, samplerate, channels * 2, True)  # PCM, 16-bit

    # The replacement is written *in place*, over the target entry's existing
    # bytes, and the file keeps its original size exactly. Both constraints are
    # load-bearing, established by testing against the real game:
    #   - Nothing past the original end of file is ever loaded. Data appended
    #     there plays as pure static, no matter how little is appended.
    #   - Relocating an entry corrupts it even when its bytes and metadata are
    #     provably correct, so no entry may move.
    # Together that caps a replacement at the target entry's own play_len; the
    # `list` command prints that budget per entry.
    data = bytearray(xwb_path.read_bytes())
    wavedata_start = bank["wavedata_start"]

    # WAVEBANKDATA.dwAlignment (file offset 0x80): every entry's PlayRegion
    # offset AND length is a multiple of it in every shipped bank, and the
    # engine aligns its streaming reads down to it, so a replacement whose
    # size isn't a multiple knocks itself and every later entry off-alignment.
    align = struct.unpack_from(">I", xwb_path.read_bytes(), 0x80)[0] or 1
    pad = (-len(new_data)) % align
    if pad:
        print(f"Padding replacement with {pad} bytes of silence for {align}-byte alignment")

    budget = target["play_len"]
    if args.allow_grow:
        # Investigation escape hatch (see docs/xma-voice-repack-handoff.md, "build 4"):
        # append the replacement past the original EOF and repoint the target
        # entry at it, leaving every other entry exactly where it is. This is
        # known to play as static -- it exists so the SDK's read trace can show
        # whether the guest ever asks for those bytes at all.
        new_wave_segment = bytearray(data[wavedata_start:])
        grow_off = len(new_wave_segment)
        assert grow_off % align == 0, f"wave segment end {grow_off} is not {align}-aligned"
        new_wave_segment += new_data + b"\x00" * pad
        nsamples = len(new_data) // (2 * channels)
        _write_entry(data, idx, new_fmt, nsamples, grow_off, len(new_data) + pad)
        struct.pack_into(">II", data, 8 + 4 * 8, wavedata_start, len(new_wave_segment))
        out_bytes = bytes(data[:wavedata_start]) + bytes(new_wave_segment)
        _emit(args, xwb_path, xsb_path, out_bytes, len(data), idx)
        print(f"NOTE: --allow-grow build; entry {idx} now lives past the original EOF "
              f"at wave-data offset 0x{grow_off:X}. Expected to play as static.")
        return

    if len(new_data) + pad > budget:
        over = len(new_data) + pad - budget
        sys.exit(
            f"replacement is {len(new_data) + pad} bytes but entry {idx} only has "
            f"{budget}; growing the bank or moving entries breaks playback.\n"
            f"Trim {over / (2 * channels * samplerate):.2f}s with --duration, or lower "
            f"--samplerate/--channels.")

    # Existing wave data stays byte-for-byte apart from the target entry's own
    # bytes; any slack left in its slot is zeroed so no stale audio survives.
    new_wave_segment = bytearray(data[wavedata_start:])
    nsamples = len(new_data) // (2 * channels)
    new_off = target["play_off"]
    new_len = len(new_data) + pad
    new_wave_segment[new_off:new_off + budget] = (new_data + b"\x00" * (budget - len(new_data)))

    _write_entry(data, idx, new_fmt, nsamples, new_off, new_len)

    # Keep the WAVEBANKSEGMENT entry for the wave data (seg[4], at file offset
    # 8 + 4*8) in sync -- the engine locates and bounds the wave data with it,
    # and a stale length means every entry past the replaced one reads out of
    # bounds (silence, then a hang waiting on an end-of-stream that never comes).
    struct.pack_into(">II", data, 8 + 4 * 8, wavedata_start, len(new_wave_segment))

    out_bytes = bytes(data[:wavedata_start]) + bytes(new_wave_segment)
    _emit(args, xwb_path, xsb_path, out_bytes, len(data), idx)


def _write_entry(data: bytearray, idx: int, new_fmt: int, nsamples: int,
                 new_off: int, new_len: int) -> None:
    """Rewrite one 24-byte WAVEBANKENTRY row in place."""
    o = ENTRY_TABLE_BASE + idx * ENTRY_SIZE
    flags_dur, fmt, poff, plen, loop_start, loop_total = struct.unpack_from(">IIIIII", data, o)
    # The engine derives stream length / end-of-stream from dwFlagsAndDuration
    # and LoopRegion, not just PlayRegion.dwLength -- leaving them stale makes
    # playback hang waiting for samples that never arrive. Duration/loop
    # describe the real audio, not the alignment padding.
    flags_dur = (flags_dur & 0xF) | (nsamples << 4)
    struct.pack_into(">IIIIII", data, o, flags_dur, new_fmt, new_off, new_len, 0, nsamples)


def _emit(args: argparse.Namespace, xwb_path: Path, xsb_path: Path,
          out_bytes: bytes, orig_size: int, idx: int) -> None:
    out_path = Path(args.output) if args.output else xwb_path
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out_bytes)
    print(f"Wrote {out_path} ({len(out_bytes)} bytes, was {orig_size})")

    if args.output and str(out_path.parent) != str(xwb_path.parent):
        out_xsb = out_path.with_suffix(".xsb")
        out_xsb.write_bytes(xsb_path.read_bytes())
        print(f"Copied paired {out_xsb}")

    # Validate: re-parse and decode the new entry + its neighbors.
    verify_bank = parse_xwb(out_path)
    check_indices = [i for i in (idx - 1, idx, idx + 1) if 0 <= i < verify_bank["entry_count"]]
    for i in check_indices:
        e = verify_bank["entries"][i]
        tmp = out_path.parent / f".__verify_{i}.wav"
        ok, err = decode_entry_to_wav(e, tmp)
        dur = ffprobe_duration(tmp) if ok else None
        status = f"OK dur={dur:.3f}s" if ok else f"FAILED: {err}"
        marker = " <-- replaced" if i == idx else ""
        print(f"  verify entry {i}: {status}{marker}")
        tmp.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="List entries/cues in a wave bank")
    p_list.add_argument("xwb", help="Path to the .xwb file (.xsb must be alongside it)")
    p_list.set_defaults(func=cmd_list)

    p_replace = sub.add_parser("replace", help="Replace one entry with a slice of another audio file")
    p_replace.add_argument("xwb", help="Path to the source .xwb file (.xsb must be alongside it)")
    p_replace.add_argument("--cue", help="Cue name of the entry to replace (see 'list')")
    p_replace.add_argument("--index", type=int, help="Entry index to replace, instead of --cue")
    p_replace.add_argument("--input", required=True, help="Replacement audio file (any format ffmpeg reads)")
    p_replace.add_argument("--start", type=float, default=0.0, help="Start offset (seconds) into --input")
    p_replace.add_argument("--duration", type=float, help="Override duration (seconds); default: match original entry")
    p_replace.add_argument("--channels", type=int, help="Override output channel count; default: match original entry")
    p_replace.add_argument("--samplerate", type=int, help="Override output sample rate; default: match original entry")
    p_replace.add_argument("--output", help="Output .xwb path; default: overwrite the input in place")
    p_replace.add_argument("--allow-grow", action="store_true",
                           help="Investigation only: append past the original EOF instead of "
                                "fitting the entry's own slot. Known to play as static.")
    p_replace.set_defaults(func=cmd_replace)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
