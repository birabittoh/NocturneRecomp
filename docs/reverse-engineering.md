# Extracted game data reference

Facts about how this game's text/data is packaged, encoded, and laid out
in memory, gathered while extracting enemy/item names and investigating
sprite compression. For the general modding mechanism (not specific to
this data), see `docs/making-mods.md`'s "Patching static game text/data"
section.

## `default.xex` packaging

`assets/default.xex` is a standard retail **XEX2** container: AES-128
encrypted (`encryption_type=1`/NORMAL) and "basic" compressed
(`compression_type=1`/BASIC — a simple data/zero-run block scheme, not
LZX). None of this is project-specific obfuscation; it's documented
Xbox 360 packaging.

- Base header (`data[0:24]`, big-endian): magic `XEX2`, module_flags,
  `header_size` (@0x8 — offset where the PE/exe body starts),
  `security_offset` (@0x10), `header_count` (@0x14), then `header_count`
  × 8-byte `(key, value)` optional-header entries.
- Optional header `0x000003FF` (`XEX_HEADER_FILE_FORMAT_INFO`) points at
  `{ info_size:u32, encryption_type:u16, compression_type:u16 }`.
- `xex2_security_info.aes_key` is at file offset `security_offset +
  0x150`: the title's AES key, itself encrypted with the Xbox 360 retail
  common key (`20B185A59D28FDC340583FBB0896BF91` — public across the
  Xbox 360 homebrew/emulation ecosystem; also embedded in this project's
  own `rexglue-sdk/src/system/xex_module.cpp` as `xe_xex2_retail_key`,
  since the SDK performs this same decryption at runtime). One AES-ECB
  block decrypt of that 16-byte key with the retail key gives the
  session key.
- BASIC compression is a list of `(data_size, zero_size)` blocks
  immediately after the 8-byte file-format-info header
  (`(info_size-8)/8` of them): AES-128-CBC-decrypt (zero IV) the
  concatenation of all `data_size` regions as one continuous stream,
  then re-inflate by inserting `zero_size` zero bytes after each chunk.
  This title has 2 blocks: `(15007744, 3702784)` and `(196608, 0)`.
- No signature/hash check runs on this load path (RSA signature
  verification only happens for `.xexp` *patch* application, not the
  base image load) — only a PE MZ/NT header sanity check.
- `XexModule::ReadImage` also supports `encryption_type=0`
  (`XEX_ENCRYPTION_NONE`) + `compression_type=0`
  (`XEX_COMPRESSION_NONE`), which just `memcpy`s the file body directly
  — a fully valid, unencrypted/uncompressed XEX2 file loads with no
  further changes needed.
- The game additionally applies `assets/default.xexp` (a title-update
  delta patch) over the base image on every launch (startup log:
  `XEX patch applied successfully: base version: 0.0.1.4, new version:
  0.0.2.4`). This shifts guest addresses in the string-table region
  (below) by a **constant `+0x30` (48 bytes)** relative to what an
  offline decrypt of the unpatched base file would compute — confirmed
  for two known-good addresses, not proven constant everywhere. **Always
  verify a computed address against a live, running process
  (`scripts/re/scan_guest_memory.py`) rather than trusting offline
  file-offset arithmetic alone.**

Scripts: `scripts/re/dump_xex_image.py` (decrypt+decompress to plain
bytes), `scripts/re/rebuild_xex_unencrypted.py` (decrypt, optionally
patch strings in place, re-emit as an unencrypted/uncompressed XEX2
file).

## String/name table encoding

Item and enemy name fields use a **"big first letter" font encoding**:
the first character of each name is stored as `ASCII_uppercase - 0x20`
(e.g. `'D'` `0x44` → `0x24`, `'M'` `0x4D` → `0x2D`, `'P'` `0x50` → `0x30`);
remaining characters are **plain, uppercase-only ASCII** (no lowercase, no
digits observed to render correctly — the font's glyph set for this
context appears restricted to A-Z). Description/flavor-text fields are
plain ASCII throughout, no special first-letter encoding.

Records in the item tables are `[DESCRIPTION][NAME]`, description first:
proven by the English table's two starter items, where
`"Resembles family sword"` immediately precedes `ALUCART SWORD` and
`"Resembles family shield"` immediately precedes `ALUCART SHIELD`.

Multi-word names are stored as separate consecutive fields, each with its
own big-first-letter encoding (e.g. `AXE` + `KNIGHT`, `AXE` re-encoded as
`!XE`).

Field termination is **not consistent** and must be measured per field,
not assumed:
- Most fields end in a single `0x00` before the next field/entry starts.
- Some fields (seen on both name and description fields) are followed
  directly by `0xFF` with no `0x00` at all.
- Some are followed by more than one `0x00`.
- `0xFF` marks a full entry's end in some records but is not present
  between every field — it is not a reliable universal delimiter on its
  own.
- Two control-byte sequences decode to punctuation in description text:
  `0x81` alone → `'`; `0x81 0x69` / `0x81 0x6a` → `(` / `)`. Other
  non-ASCII control bytes (used for accented characters — à, è, ì, ò, ù,
  ¿, ¡, ü, ß, etc.) are not decoded; extracted description text with
  those characters will show gaps/artifacts.

## Enemy name table

- English enemy names: guest `0x292205df8`–`0x292206534` (host addresses
  from a live scan; subtract the run's `virtual_membase`, commonly but
  not guaranteed to be `0x100000000`, for the true guest address). Flat
  stream of big-font-encoded word tokens, one or more words per enemy
  name, no explicit array/length prefix.
- Enemy bestiary flavor text (separate table, not positionally linked to
  the name table — they are ~57 KB apart with the entire item name table
  between them): guest `0x292214d28`–`0x29221612c`. Plain ASCII.
- Files: `enemy_names_raw.txt` (token stream with addresses),
  `enemy_names_combined.txt` (tokens grouped into full names — some
  multi-word groupings marked `(?)`, unconfirmed), `enemy_descriptions.txt`
  (flavor text with addresses).
- Cross-referenced against `sotn.dev/debug.html` and
  `drrak.github.io/sotn/` (community PS1 reverse-engineering references);
  names matched in both.

## Item name table

Five per-language blocks, back to back, each `~0x1b00`–`0x1c00` bytes:
German, Italian, Spanish, French, English, in that order. A Japanese
(Shift-JIS) block precedes German; not decoded (would need its own
decoder for the multi-byte encoding combined with the control-byte
scheme above).

Address ranges (guest, host-scan-derived as above):
- DE: `0x292208958`–`0x29220a4fc`
- IT: `0x29220a4fc`–`0x29220c044`
- ES: `0x29220c044`–`0x29220dcfc`
- FR: `0x29220dcfc`–`0x29220fad0`
- EN: `0x29220fad0`–`0x292211572` (last entry: Empty Hand)

Files: `item_names_en.txt`, `item_names_de.txt`, `item_names_it.txt`,
`item_names_es.txt`, `item_names_fr.txt` — one paired `NAME`/`DESC` per
line, in memory order, with the description's guest address.

Known decoding gaps:
- Some entries absorb more than one sentence into `DESC` where an item's
  description spans multiple `0x00`-terminated sub-strings with no
  intervening name token — the pairing algorithm attributes all of them
  to the next name found, which is not always the intended grouping.
- Accented characters are not decoded (see encoding section above).

## Sprite graphics compression (PS1 format)

This port's "Original" graphics mode uses the same custom
sprite-compression bitstream as the original PS1 build (distinct from the
"Enhanced" HD textures in `assets/MEDIA/sprites.xpr`, a plain XPR2
resource of pre-converted `TX2D` GPU textures — not this format).
Confirmed by an exact byte-for-byte match of a decoder error-path string,
`"over:%08x(%04x)"`, between the community's independent PS1
reverse-engineering project (`https://github.com/Xeeynamo/sotn-decomp`)
and this build's own decrypted `default.xex` (at the guest-relative
offset matching `0x292206534` in the enemy-name-table addressing above).

Algorithm: a custom nibble-oriented (4-bit-unit) RLE scheme. Each
compressed block starts with an 8-byte cache of recently-seen values,
then the stream is read one nibble at a time as a 4-bit opcode selecting
one of: a zero-run, 1-3 literal nibbles, a repeated nibble N times, a
replay of one of the 8 cached values, or a stop code. Output is always
exactly 8 KB (`0x2000` bytes) per block.

**Location of the compressed data in this build is not confirmed.**
sotn-decomp's own extraction tool expects a PS1-address-space ("stage
header") pointer structure (literal `0x8xxxxxxx` PS1 RAM pointers) that
has no reason to survive in this Xbox 360 port's independently re-linked
binary (`0x82xxxxxx` address space), even though the bitstream format
itself carried over unchanged. A heuristic entropy-based search (looking
for contiguous high-entropy regions, then trying the decoder against
candidate offsets within them) did not find it: every clean decode
produced was confirmed (by rendering under several pixel-format
guesses) to be non-image noise, not real sprite data. "Decodes cleanly"
is not a strong enough signal on its own — this opcode set is permissive
enough that high-entropy non-sprite data can decode to completion
without erroring. The compressed sprite data's real location (if present
in this build at all, as opposed to the "Original" mode being a
rendering style over the same HD texture data) remains unlocated.
Resolving this would need the actual pointer/table structure this port
uses in place of the PS1 stage header, not heuristic scanning.

## Where the PS1 picture is drawn

There is no separate PS1 texture: the game renders the PS1 side's picture
directly into its one 1280x720 render target, within
`graphics.stretch_rect` (`0x82882C68`, LTRB in render-target pixels; the
full-screen bounds are at `0x82882C98`). That rect is what the "Change
Screen Size" screen and the stretch presets resize (see
`src/screens/graphics_settings.cpp`). So anything meant to apply to the PS1
picture alone, like the `screen_filter` cvar's scanlines, has to work on that
region of the frame, not on a texture. The frame is drawn by
`XenonFramework::DrawLayers` (`sub_82582AC0`): the PS1 side's layers first,
then, at `0x82583174`, the UI manager's draw (the Xbox pause menu and the
screens opened from it), then the swap. The screen filter is applied at that
point, so the UI lands on top of it.

## Modding these tables

Two mechanisms, detailed in `docs/making-mods.md`'s "Patching static game
text/data" section:

1. Ship a replacement `default.xex` via `mods/<name>/game/default.xex`
   (`scripts/re/rebuild_xex_unencrypted.py` builds one). Whole-file
   replacement, no merge — only one such mod can be active at a time.
2. Patch guest memory from a code mod at `OnModuleLaunched()`, via the
   shared helper in `mods_src/common/include/rexmod/text_patch.h`
   (`ApplyTextPatch` for description fields, `ApplyNameFieldPatch` for
   name fields — the latter applies the big-first-letter encoding
   automatically). Any number of mods can coexist as long as they target
   different addresses. Requires: the guest address confirmed against a
   live process (not offline file-offset math, per the `+0x30`
   title-update-patch note above); the target field's read-only page
   unlocked before writing (`BaseHeap::Protect`); the field length
   measured from what actually follows the string in memory, not assumed
   from `len(text) + 1`.

Example mods: `mods_src/xex_patch_potion` (patches the English "Potion"
item description), `mods_src/xex_patch_red_rust` (patches the English
"Red Rust" item name, both word-fields).

## Files in this directory

- `enemy_names_raw.txt`, `enemy_names_combined.txt`, `enemy_descriptions.txt`
- `item_names_en.txt`, `item_names_de.txt`, `item_names_it.txt`,
  `item_names_es.txt`, `item_names_fr.txt`

## Scripts (in `../scripts/re/`)

- `scan_guest_memory.py` — scan/read a running process's guest memory.
- `dump_xex_image.py` — decrypt + decompress `default.xex` to plain bytes.
- `rebuild_xex_unencrypted.py` — decrypt, optionally patch strings, and
  re-emit `default.xex` as a plain XEX2 file the SDK loads natively.
