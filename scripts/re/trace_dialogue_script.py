#!/usr/bin/env python3
"""Decode a cutscene-dialogue script blob byte-by-byte per the opcode /
param-length table reverse-engineered from sub_823A2F28 in default.xex,
instead of eyeballing a raw hex dump.

This is a *disassembler* for the format, not an interpreter: it does not
evaluate branch/jump opcodes (14/15) or emulate any state, just walks the
stream consuming each opcode's known parameter-byte count and prints what
it finds. Good enough to answer "where are the cue markers / where does the
byte layout diverge between two versions of a blob".

Opcodes that matter for timing (all traced from sub_823A2F28):
  op9  <b0> <b1>  play sound, cue id = (b0 << 4) | b1. Cues 769..1330 are
                  streamed "XA" audio (the voice lines: 0x358..0x35D here);
                  128..132 / 144..148 start a music sequence.
  op10            spin until the sound system is busy (queued cue started).
  op11            spin until the sound system is idle -- i.e. wait for the
                  playing voice line to finish. Polls sub_82230130(), which
                  is `dword_82E7DD00 || word_82E7DCD4`, and word_82E7DCD4 is
                  cleared once the XACT cue actually stops playing.
  op12 <4 bytes>  arm a *separate* frame-counted animation sub-script at that
                  address (run by sub_823A2D20); op18 disarms it. Unrelated
                  to audio.
  op3  <n>        per-character typewriter delay for the text that follows.

Usage:
    python scripts/re/trace_dialogue_script.py assets/default.xex 0xD55910 0xD55B88
    python scripts/re/trace_dialogue_script.py some_blob.bin
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dump_xex_image import decrypt_xex_image, _load_gen_icon  # noqa: E402

# opcode -> number of raw parameter bytes consumed after the opcode byte
# itself. Opcodes not listed here (0, 4, 6, 8, 10, 11, 18) take none; 1
# (newline) also takes none.
PARAM_LENGTHS = {2: 1, 3: 1, 16: 1, 17: 1, 5: 2, 7: 2, 9: 2, 20: 2, 12: 4, 14: 4, 15: 4, 19: 5}


def trace(blob: bytes) -> None:
    i, n = 0, len(blob)
    while i < n:
        op = blob[i]
        start = i
        i += 1
        if op & 0x80:
            ch = blob[i] if i < n else 0
            i += 1
            print(f"{start:4d}  CHAR        {op:02x} {ch:02x}  (glyph code {(op << 8) | ch:#06x})")
            continue
        plen = PARAM_LENGTHS.get(op, 0)
        params = blob[i:i + plen]
        i += plen
        if op == 1:
            print(f"{start:4d}  op1  NEWLINE")
        elif op in PARAM_LENGTHS:
            print(f"{start:4d}  op{op:<3d} params={params.hex(' ')}")
        elif op in (0, 4, 6, 8, 10, 11, 18):
            print(f"{start:4d}  op{op:<3d} (no params)")
        else:
            c = chr(op) if 0x20 <= op < 0x7F else "."
            print(f"{start:4d}  op{op:<3d} UNKNOWN/default -- treated as raw text byte ({c!r})")


def main() -> None:
    args = sys.argv[1:]
    if len(args) == 1:
        blob = Path(args[0]).read_bytes()
    elif len(args) == 3:
        xex_path, start_s, end_s = args
        gi = _load_gen_icon()
        image = decrypt_xex_image(xex_path, gi)
        blob = image[int(start_s, 0):int(end_s, 0)]
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    trace(blob)


if __name__ == "__main__":
    main()
