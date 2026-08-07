#!/usr/bin/env python3
"""Decode all XACT wave banks under assets/DATA/sound to playable WAV files.

The game's voice/sfx banks are Xbox 360 XACT wave banks (.xwb, paired with a
.xsb sound bank for cue names) rather than a RIFF container, and store audio
as raw XMA2 bitstreams. This parses the .xwb/.xsb structures directly and
shells out to ffmpeg (which understands XMA2) to decode each entry to WAV.

Requires ffmpeg/ffprobe on PATH.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOUND_DIR = REPO_ROOT / "assets" / "DATA" / "sound"
OUT_DIR = REPO_ROOT / "extracted" / "audio"

CODEC_NAMES = {0: "PCM", 1: "XMA", 2: "ADPCM", 3: "WMA"}


def decode_miniformat(v: int) -> tuple[int, int, int, int, int]:
    tag = v & 0x3
    channels = (v >> 2) & 0x7
    samplerate = (v >> 5) & 0x3FFFF
    blockalign = (v >> 23) & 0x7F
    bits = (v >> 30) & 0x3
    return tag, channels, samplerate, blockalign, bits


def parse_xwb(path: Path) -> dict:
    data = path.read_bytes()
    assert data[:4] == b"DNBW", f"bad magic in {path}"
    ver, hver = struct.unpack_from(">II", data, 4)
    entry_count = struct.unpack_from(">I", data, 0x34)[0]
    name = data[0x38:0x38 + 16].split(b"\x00")[0].decode("ascii", "replace")

    # Entry metadata table starts at 0x90 (confirmed against the guest XACT
    # engine in default.xex: sub_8256A080 memcpy's 24 bytes of the entry into
    # its runtime wave struct, and the getters read MiniWaveFormat at entry+4,
    # PlayRegion.dwLength at entry+12). Standard XACT3 WAVEBANKENTRY:
    #   +0  dwFlagsAndDuration  (Flags:4 | DurationInSamples:28)
    #   +4  MiniWaveFormat
    #   +8  PlayRegion.dwOffset (relative to wave data start)
    #   +12 PlayRegion.dwLength (bytes)
    #   +16 LoopRegion.dwStartSample
    #   +20 LoopRegion.dwTotalSamples
    base = 0x90
    entries = []
    for i in range(entry_count):
        o = base + i * 24
        flags_dur, fmt, poff, plen, loop_start, loop_total = struct.unpack_from(">IIIIII", data, o)
        tag, ch, sr, ba, bits = decode_miniformat(fmt)
        entries.append(dict(index=i, tag=tag, channels=ch, samplerate=sr,
                             blockalign=ba, bits=bits, play_off=poff, play_len=plen,
                             flags=flags_dur & 0xF, duration=flags_dur >> 4,
                             loop_start=loop_start, loop_total=loop_total))

    # WAVEBANKSEGMENT table: 5 x {dwOffset, dwLength} at file offset 8.
    # seg[1] is the entry metadata table (offset 0x90, length 24*count) and
    # seg[4] is the wave data; both match exactly on every shipped bank. The
    # guest engine reads these to locate both regions (sub_82562980), so a
    # repack MUST keep seg[4].dwLength in sync with the real wave data size.
    segments = [struct.unpack_from(">II", data, 8 + i * 8) for i in range(5)]
    wavedata_start = segments[4][0]
    for e in entries:
        e["abs_off"] = wavedata_start + e["play_off"]
        e["data"] = data[e["abs_off"]: e["abs_off"] + e["play_len"]]

    return dict(name=name, version=ver, entry_count=entry_count, entries=entries,
                segments=segments, wavedata_start=wavedata_start, filelen=len(data))


def parse_xsb_cue_names(path: Path, entry_count: int) -> list[str] | None:
    """Best-effort: for simple single-wavebank sound banks, cue names appear
    as a null-terminated ASCII string table in the same order as wave-bank
    entry index."""
    if not path.exists():
        return None
    data = path.read_bytes()
    if data[:4] != b"KBDS":
        return None
    candidates = []
    i = 0
    while i < len(data):
        b = data[i]
        if 0x30 <= b <= 0x39 or 0x41 <= b <= 0x5A:
            j = i
            while j < len(data) and (0x30 <= data[j] <= 0x39 or 0x41 <= data[j] <= 0x5A):
                j += 1
            if j < len(data) and data[j] == 0 and (j - i) >= 2:
                candidates.append(data[i:j].decode("ascii"))
                i = j + 1
                continue
        i += 1
    if len(candidates) >= entry_count:
        return candidates[-entry_count:]
    return None


def build_xma2_wav(entry: dict) -> bytes:
    """Wrap a raw XMA2 bitstream in a minimal RIFF/WAVE container with an
    XMA2WAVEFORMATEX fmt chunk so ffmpeg's wav demuxer + xma2 decoder read it."""
    ch = entry["channels"] or 1
    sr = entry["samplerate"] or 22050
    data = entry["data"]

    wFormatTag = 0x166
    nBlockAlign = ch * 2
    fmt_chunk = struct.pack(
        "<HHIIHHH", wFormatTag, ch, sr, sr * ch * 2, nBlockAlign, 16, 34,
    )
    approx_samples = max(1, (len(data) * 8) // max(1, nBlockAlign))
    fmt_chunk += struct.pack(
        "<HIIIIIIIBBH",
        1,                 # NumStreams
        0,                 # ChannelMask
        approx_samples,    # SamplesEncoded
        len(data),         # BytesPerBlock
        0,                 # PlayBegin
        approx_samples,    # PlayLength
        0, 0, 0,           # LoopBegin, LoopLength, LoopCount
        4,                 # EncoderVersion
        1,                 # BlockCount
    )

    riff = b"WAVE"
    riff += b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk
    riff += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(riff)) + riff


def ffmpeg_decode(wav_bytes: bytes, out_path: Path) -> tuple[bool, str]:
    tmp_in = out_path.with_suffix(out_path.suffix + ".xma.wav")
    tmp_in.write_bytes(wav_bytes)
    r = subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(tmp_in), str(out_path)],
        capture_output=True, text=True,
    )
    tmp_in.unlink(missing_ok=True)
    return r.returncode == 0 and out_path.exists(), r.stderr


def ffprobe_duration(path: Path) -> float | None:
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", str(path)],
        capture_output=True, text=True,
    )
    try:
        return float(r.stdout.strip())
    except ValueError:
        return None


def process_bank(base_name: str) -> tuple[dict, list[dict]]:
    bank = parse_xwb(SOUND_DIR / f"{base_name}.xwb")
    names = parse_xsb_cue_names(SOUND_DIR / f"{base_name}.xsb", bank["entry_count"])
    results = []
    for e in bank["entries"]:
        idx = e["index"]
        cue = names[idx] if names else None
        codec = CODEC_NAMES.get(e["tag"], "?")
        wav_path = OUT_DIR / f"{base_name}_{idx:02d}_{cue or 'unk'}.wav"
        ok, err = False, ""
        if e["tag"] == 1:  # XMA
            ok, err = ffmpeg_decode(build_xma2_wav(e), wav_path)
        results.append(dict(bank=base_name, index=idx, cue=cue, codec=codec,
                             channels=e["channels"], samplerate=e["samplerate"],
                             raw_len=e["play_len"], wav_path=wav_path if ok else None,
                             decode_ok=ok, err=err[:200] if not ok else ""))
    return bank, results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("banks", nargs="*",
                         help="Bank base names (e.g. sd_dora) to decode; default: all sd_*.xwb")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    if args.banks:
        bank_names = args.banks
    else:
        bank_names = sorted(p.stem for p in SOUND_DIR.glob("sd_*.xwb"))

    all_results = []
    for base_name in bank_names:
        bank, results = process_bank(base_name)
        n_ok = sum(1 for r in results if r["decode_ok"])
        print(f"== {base_name}: entries={bank['entry_count']} decoded={n_ok}")
        all_results.extend(results)

    n_fail = sum(1 for r in all_results if not r["decode_ok"])
    print(f"\nDone: {len(all_results)} entries, {n_fail} failed. Output in {OUT_DIR}")
    if n_fail:
        for r in all_results:
            if not r["decode_ok"]:
                print(f"  FAILED {r['bank']} idx={r['index']}: {r['err']}")


if __name__ == "__main__":
    main()
