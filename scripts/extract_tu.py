#!/usr/bin/env python3
"""Extract the embedded XEX delta patch (.xexp) from an Xbox 360 LIVE/CON/PIRS
title-update (STFS) package.

The block-addressing math mirrors the ReXGlue SDK's StfsContainerDevice
(see ../rexglue-sdk/src/filesystem/devices/stfs_container_device.cpp) so the
output matches exactly what the runtime loader would read.
"""
import hashlib
import os
import struct
import sys

BLOCK_SIZE = 0x1000
BLOCKS_PER_HASH_LEVEL = (170, 28900, 4913000)
END_OF_CHAIN = 0xFFFFFF

# Absolute offsets inside the StfsHeader (XContentHeader + XContentMetadata),
# both #pragma pack(1). Derived from rex/filesystem/devices/stfs_xbox.h.
OFF_MAGIC = 0x000
OFF_HEADER_SIZE = 0x340          # be u32
OFF_VOLUME_DESCRIPTOR = 0x379    # StfsVolumeDescriptor (0x24 bytes)
OFF_VOLUME_TYPE = 0x3A9          # be u32  (0 = STFS, 1 = SVOD)

MAGICS = {b"CON ", b"LIVE", b"PIRS"}


def u24_le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


def round_up(v, n):
    return (v + n - 1) // n * n


class Stfs:
    def __init__(self, data):
        self.d = data
        magic = data[OFF_MAGIC:OFF_MAGIC + 4]
        if magic not in MAGICS:
            raise ValueError(f"not an STFS package (magic={magic!r})")
        (self.header_size,) = struct.unpack_from(">I", data, OFF_HEADER_SIZE)
        (self.volume_type,) = struct.unpack_from(">I", data, OFF_VOLUME_TYPE)
        if self.volume_type != 0:
            raise ValueError("SVOD packages not supported by this extractor")

        vd = OFF_VOLUME_DESCRIPTOR
        self.flags = data[vd + 2]
        self.read_only_format = self.flags & 0x1
        self.root_active_index = (self.flags >> 1) & 0x1
        (self.file_table_block_count,) = struct.unpack_from("<H", data, vd + 3)
        self.file_table_block_number = u24_le(data[vd + 5:vd + 8])
        (self.total_block_count,) = struct.unpack_from(">I", data, vd + 0x1C)

        self.blocks_per_hash_table = 1 if self.read_only_format else 2
        self.block_step = (
            BLOCKS_PER_HASH_LEVEL[0] + self.blocks_per_hash_table,
            BLOCKS_PER_HASH_LEVEL[1]
            + (BLOCKS_PER_HASH_LEVEL[0] + 1) * self.blocks_per_hash_table,
        )
        self.data_base = round_up(self.header_size, BLOCK_SIZE)
        self._hash_cache = {}

    def block_to_offset(self, block_index):
        base = BLOCKS_PER_HASH_LEVEL[0]
        block = block_index
        for _ in range(3):
            block += ((block_index + base) // base) * self.blocks_per_hash_table
            if block_index < base:
                break
            base *= BLOCKS_PER_HASH_LEVEL[0]
        return self.data_base + (block << 12)

    def _hash_block_number(self, block_index, level):
        if level == 0:
            if block_index < BLOCKS_PER_HASH_LEVEL[0]:
                return 0
            block = (block_index // BLOCKS_PER_HASH_LEVEL[0]) * self.block_step[0]
            block += ((block_index // BLOCKS_PER_HASH_LEVEL[1]) + 1) * self.blocks_per_hash_table
            if block_index < BLOCKS_PER_HASH_LEVEL[1]:
                return block
            return block + self.blocks_per_hash_table
        if level == 1:
            if block_index < BLOCKS_PER_HASH_LEVEL[1]:
                return self.block_step[0]
            block = (block_index // BLOCKS_PER_HASH_LEVEL[1]) * self.block_step[1]
            return block + self.blocks_per_hash_table
        return self.block_step[1]

    def _hash_offset(self, block_index, level):
        return self.data_base + (self._hash_block_number(block_index, level) << 12)

    def _read_hash_table(self, off):
        if off not in self._hash_cache:
            self._hash_cache[off] = self.d[off:off + BLOCK_SIZE]
        return self._hash_cache[off]

    def _level0_next_block(self, block_index):
        # Resolve the active backing block at each hash level (resiliency).
        secondary = BLOCK_SIZE if self.root_active_index else 0
        off0 = self._hash_offset(block_index, 0)
        if self.read_only_format:
            secondary = 0
        else:
            if self.total_block_count > BLOCKS_PER_HASH_LEVEL[0]:
                off1 = self._hash_offset(block_index, 1)
                if self.total_block_count > BLOCKS_PER_HASH_LEVEL[1]:
                    off2 = self._hash_offset(block_index, 2)
                    t2 = self._read_hash_table(off2 + secondary)
                    rec = (block_index // BLOCKS_PER_HASH_LEVEL[1]) % BLOCKS_PER_HASH_LEVEL[0]
                    (info,) = struct.unpack_from(">I", t2, rec * 0x18 + 0x14)
                    secondary = BLOCK_SIZE if (info & 0x40000000) else 0
                t1 = self._read_hash_table(off1 + secondary)
                rec = (block_index // BLOCKS_PER_HASH_LEVEL[0]) % BLOCKS_PER_HASH_LEVEL[0]
                (info,) = struct.unpack_from(">I", t1, rec * 0x18 + 0x14)
                secondary = BLOCK_SIZE if (info & 0x40000000) else 0
        t0 = self._read_hash_table(off0 + secondary)
        rec = block_index % BLOCKS_PER_HASH_LEVEL[0]
        (info,) = struct.unpack_from(">I", t0, rec * 0x18 + 0x14)
        return info & 0xFFFFFF

    def list_files(self):
        entries = []
        table_block = self.file_table_block_number
        for _ in range(self.file_table_block_count):
            off = self.block_to_offset(table_block)
            block = self.d[off:off + BLOCK_SIZE]
            for m in range(BLOCK_SIZE // 0x40):
                e = block[m * 0x40:(m + 1) * 0x40]
                if e[0] == 0:
                    break
                flags = e[0x28]
                name_len = flags & 0x3F
                is_dir = (flags >> 7) & 1
                name = e[:name_len].decode("latin-1")
                allocated = u24_le(e[0x2C:0x2F])
                start_block = u24_le(e[0x2F:0x32])
                (parent,) = struct.unpack_from(">H", e, 0x32)
                (length,) = struct.unpack_from(">I", e, 0x34)
                entries.append(dict(name=name, is_dir=bool(is_dir), length=length,
                                    start_block=start_block, allocated=allocated,
                                    parent=parent))
            table_block = self._level0_next_block(table_block)
            if table_block == END_OF_CHAIN:
                break
        return entries

    def read_file(self, entry):
        out = bytearray()
        block_index = entry["start_block"]
        remaining = entry["length"]
        while remaining and block_index != END_OF_CHAIN:
            n = min(BLOCK_SIZE, remaining)
            off = self.block_to_offset(block_index)
            out += self.d[off:off + n]
            remaining -= n
            block_index = self._level0_next_block(block_index)
        if remaining:
            raise RuntimeError(f"chain ended early, {remaining} bytes short")
        return bytes(out)


# ── XEX delta-patch helpers ─────────────────────────────────────────────────
#
# A title update is an STFS package wrapping an XEX2 *delta patch*. The patch's
# delta descriptor records the SHA-1 of the rsa_signature of the exact base XEX
# it expects (digest_source). Kameo ships three regional variants of the update
# (target versions 2.0.1/2.0.2/2.0.3); each targets a different base dump, so the
# right one to apply is whichever digest_source matches your default.xex.

KEY_DELTA_PATCH = 0x000005FF


def extract_xexp(package_path):
    """Return the embedded XEX delta patch (default.xexp) bytes from a TU package."""
    with open(package_path, "rb") as f:
        stfs = Stfs(f.read())
    files = [e for e in stfs.list_files() if not e["is_dir"]]
    if not files:
        raise RuntimeError(f"no files inside package {package_path}")
    # Prefer the file named *.xexp. The delta patch is not necessarily the largest
    # file in the package (SOTN's TU also ships a replacement audio track that is
    # bigger), so the old "largest file" heuristic picked the wrong file. Fall back
    # to whichever file actually starts with the XEX2 magic.
    named = [e for e in files if e["name"].lower().endswith(".xexp")]
    for entry in named or sorted(files, key=lambda e: e["length"], reverse=True):
        data = stfs.read_file(entry)
        if data[:4] == b"XEX2":
            return data
    raise RuntimeError(f"no XEX delta patch (XEX2) found inside {package_path}")


def extract_update_tree(package_path, out_dir):
    """Extract the TU's data files (the per-language .str string tables),
    preserving their directory layout, into out_dir. This is the content the
    game expects mounted as the update: device. Returns the number of files.

    The XEX delta patch itself is skipped — it is handled by extract_xexp().
    """
    with open(package_path, "rb") as f:
        stfs = Stfs(f.read())
    entries = stfs.list_files()

    def full_path(i):
        parts, p = [entries[i]["name"]], entries[i]["parent"]
        while p != 0xFFFF:
            parts.append(entries[p]["name"])
            p = entries[p]["parent"]
        return parts[::-1]

    count = 0
    for i, e in enumerate(entries):
        if e["is_dir"] or e["name"].endswith(".xexp"):
            continue
        rel = full_path(i)
        dest = os.path.join(out_dir, *rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(stfs.read_file(e))
        count += 1
    return count


def xexp_digest_source(xexp):
    """SHA-1 (hex) of the base XEX rsa_signature this delta patch expects."""
    (count,) = struct.unpack_from(">I", xexp, 0x14)
    for i in range(count):
        key, val = struct.unpack_from(">II", xexp, 0x18 + i * 8)
        if key == KEY_DELTA_PATCH:
            return xexp[val + 0xC:val + 0xC + 0x14].hex()
    raise RuntimeError("no delta patch descriptor in xexp")


def xexp_target_version(xexp):
    """(label, tuple) target version, e.g. ('2.0.1', (0,2,0,1))."""
    (count,) = struct.unpack_from(">I", xexp, 0x14)
    for i in range(count):
        key, val = struct.unpack_from(">II", xexp, 0x18 + i * 8)
        if key == KEY_DELTA_PATCH:
            (v,) = struct.unpack_from(">I", xexp, val + 0x4)
            # xex2_version bitfield: major:4 minor:4 build:16 qfe:8
            t = ((v >> 28) & 0xF, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF)
            return f"{t[0]}.{t[1]}.{t[2]}.{t[3]}", t
    raise RuntimeError("no delta patch descriptor in xexp")


def base_signature(xex_path):
    """SHA-1 (hex) of an XEX's rsa_signature — comparable to a patch's digest_source."""
    with open(xex_path, "rb") as f:
        d = f.read()
    (sec_off,) = struct.unpack_from(">I", d, 0x10)
    return hashlib.sha1(d[sec_off + 8:sec_off + 8 + 0x100]).hexdigest()


def select_matching(packages, base_xex):
    """Pick the package whose embedded patch targets base_xex. Returns (pkg, xexp, version)."""
    want = base_signature(base_xex)
    for pkg in packages:
        xexp = extract_xexp(pkg)
        if xexp_digest_source(xexp) == want:
            return pkg, xexp, xexp_target_version(xexp)[0]
    return None, None, None


def main():
    import argparse
    p = argparse.ArgumentParser(
        description="Extract/select the Kameo title-update XEX delta patch from STFS package(s).")
    p.add_argument("packages", nargs="+", help="TU package file(s) (LIVE/CON/PIRS)")
    p.add_argument("--base", metavar="XEX",
                   help="base default.xex; only the patch matching it is written")
    p.add_argument("-o", "--output", metavar="PATH",
                   help="output path for the selected .xexp (default: <package>.xexp, "
                        "or assets/default.xexp when --base is given)")
    p.add_argument("--update-dir", metavar="DIR", default="update",
                   help="with --base, also extract the TU data files (per-language .str "
                        "tables) here, to be mounted as the update: device (default: update)")
    args = p.parse_args()

    missing = [pkg for pkg in args.packages if not os.path.isfile(pkg)]
    if missing:
        print("error: TU package(s) not found: " + ", ".join(missing), file=sys.stderr)
        print("       pass the actual package file(s), e.g. --base assets/default.xex TU_16L61UI_*",
              file=sys.stderr)
        sys.exit(2)
    if args.base and not os.path.isfile(args.base):
        print(f"error: base XEX not found: {args.base}", file=sys.stderr)
        sys.exit(2)

    if args.base:
        pkg, xexp, ver = select_matching(args.packages, args.base)
        if not pkg:
            print(f"error: none of the given packages match the base XEX "
                  f"({base_signature(args.base)}). These TUs are for other game revisions.",
                  file=sys.stderr)
            sys.exit(1)
        out = args.output or os.path.join(os.path.dirname(args.base) or ".", "default.xexp")
        with open(out, "wb") as f:
            f.write(xexp)
        print(f"matched {os.path.basename(pkg)} -> v{ver} ({len(xexp)} bytes) -> {out}")
        n = extract_update_tree(pkg, args.update_dir)
        print(f"extracted {n} update data file(s) -> {args.update_dir}/ (mount as update:)")
        return

    for pkg in args.packages:
        xexp = extract_xexp(pkg)
        ver = xexp_target_version(xexp)[0]
        out = args.output or os.path.splitext(pkg)[0] + ".xexp"
        with open(out, "wb") as f:
            f.write(xexp)
        print(f"{os.path.basename(pkg)}: v{ver} digest_source={xexp_digest_source(xexp)} "
              f"-> {out} ({len(xexp)} bytes)")


if __name__ == "__main__":
    main()
