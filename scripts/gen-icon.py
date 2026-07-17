#!/usr/bin/env python3
"""Embeds the game's title icon as a byte array into src/icon.generated.h.

Run automatically by CMake (see CMakeLists.txt) before compiling. The icon is
extracted directly from assets/default.xex (the XEX resource table holds an
embedded XDBF blob keyed by title ID, which in turn holds the title icon PNG)
so no extra checked-in or separately-fetched asset is needed - default.xex is
already a required codegen input.
"""
import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
XEX_PATH = os.path.join(ROOT, "assets", "default.xex")
OUT_PATH = os.path.join(ROOT, "src", "icon.generated.h")
OUT_ICO_PATH = os.path.join(ROOT, "src", "icon.generated.ico")

XEX_HEADER_RESOURCE_INFO = 0x000002FF
XEX_HEADER_FILE_FORMAT_INFO = 0x000003FF
XEX_HEADER_EXECUTION_INFO = 0x00040006

XEX_ENCRYPTION_NONE = 0
XEX_ENCRYPTION_NORMAL = 1
XEX_COMPRESSION_NONE = 0
XEX_COMPRESSION_BASIC = 1

# Well-known retail XEX2 title key, used across the Xbox 360 homebrew/research
# tooling ecosystem (xenia, free60, etc.) to unwrap the per-title AES key.
RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")

XDBF_MAGIC = 0x58444246  # 'XDBF'
XDBF_SECTION_IMAGE = 0x0002
XDBF_ID_TITLE_ICON = 0x8000


# --- Minimal pure-Python AES-128 (decrypt-only, CBC with zero IV) ----------

_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)


def _xtime(a):
    a <<= 1
    if a & 0x100:
        a ^= 0x11B
    return a & 0xFF


def _gmul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        a = _xtime(a)
        b >>= 1
    return p & 0xFF


def _gf_mul_inverse(a):
    if a == 0:
        return 0
    for b in range(1, 256):
        if _gmul(a, b) == 1:
            return b
    raise AssertionError("unreachable")


def _build_sbox():
    sbox = bytearray(256)
    for a in range(256):
        inv = _gf_mul_inverse(a)
        # Affine transform: b = inv ^ rotl(inv,1) ^ rotl(inv,2) ^ rotl(inv,3) ^ rotl(inv,4) ^ 0x63
        b = inv
        rot = inv
        for _ in range(4):
            rot = ((rot << 1) | (rot >> 7)) & 0xFF
            b ^= rot
        b ^= 0x63
        sbox[a] = b
    return bytes(sbox)


_SBOX = _build_sbox()


def _build_inv_sbox():
    inv = bytearray(256)
    for i, v in enumerate(_SBOX):
        inv[v] = i
    return bytes(inv)


_INV_SBOX = _build_inv_sbox()


def _round_key_bytes(w, round_index):
    cols = w[4 * round_index : 4 * round_index + 4]
    return bytes(cols[c][r] for c in range(4) for r in range(4))


def _add_round_key(state, rk):
    return bytes(s ^ k for s, k in zip(state, rk))


def _inv_sub_bytes(state):
    return bytes(_INV_SBOX[b] for b in state)


def _inv_shift_rows(state):
    # state is column-major: state[r + 4*c]
    s = list(state)
    out = [0] * 16
    for c in range(4):
        for r in range(4):
            out[r + 4 * ((c + r) % 4)] = s[r + 4 * c]
    return bytes(out)


def _inv_mix_columns(state):
    out = bytearray(16)
    for c in range(4):
        col = state[4 * c : 4 * c + 4]
        out[4 * c + 0] = _gmul(col[0], 14) ^ _gmul(col[1], 11) ^ _gmul(col[2], 13) ^ _gmul(col[3], 9)
        out[4 * c + 1] = _gmul(col[0], 9) ^ _gmul(col[1], 14) ^ _gmul(col[2], 11) ^ _gmul(col[3], 13)
        out[4 * c + 2] = _gmul(col[0], 13) ^ _gmul(col[1], 9) ^ _gmul(col[2], 14) ^ _gmul(col[3], 11)
        out[4 * c + 3] = _gmul(col[0], 11) ^ _gmul(col[1], 13) ^ _gmul(col[2], 9) ^ _gmul(col[3], 14)
    return bytes(out)


def _aes128_decrypt_block(key, block):
    nk, nr = 4, 10
    w = [list(key[4 * i : 4 * i + 4]) for i in range(nk)]
    for i in range(nk, 4 * (nr + 1)):
        temp = list(w[i - 1])
        if i % nk == 0:
            temp = temp[1:] + temp[:1]
            temp = [_SBOX[b] for b in temp]
            temp[0] ^= _RCON[i // nk - 1]
        w.append([w[i - nk][j] ^ temp[j] for j in range(4)])

    state = _add_round_key(block, _round_key_bytes(w, nr))
    for rnd in range(nr - 1, 0, -1):
        state = _inv_shift_rows(state)
        state = _inv_sub_bytes(state)
        state = _add_round_key(state, _round_key_bytes(w, rnd))
        state = _inv_mix_columns(state)
    state = _inv_shift_rows(state)
    state = _inv_sub_bytes(state)
    state = _add_round_key(state, _round_key_bytes(w, 0))
    return state


def aes128_cbc_decrypt(key, ciphertext, iv=bytes(16)):
    assert len(ciphertext) % 16 == 0
    plaintext = bytearray()
    prev = iv
    for i in range(0, len(ciphertext), 16):
        block = ciphertext[i : i + 16]
        decrypted = _aes128_decrypt_block(key, block)
        plaintext += bytes(a ^ b for a, b in zip(decrypted, prev))
        prev = block
    return bytes(plaintext)


# --- XEX2 / XDBF parsing -----------------------------------------------------


def _read_opt_headers(data):
    header_count = struct.unpack_from(">I", data, 0x14)[0]
    headers = {}
    off = 0x18
    for _ in range(header_count):
        key, value = struct.unpack_from(">II", data, off)
        headers[key] = value
        off += 8
    return headers


def _read_basic_blocks(data, ffi_off, info_size):
    block_count = (info_size - 8) // 8
    blocks = []
    p = ffi_off + 8
    for _ in range(block_count):
        data_size, zero_size = struct.unpack_from(">II", data, p)
        blocks.append((data_size, zero_size))
        p += 8
    return blocks


def _map_image_range_to_body(blocks, image_off, size):
    """Map a byte range in the decompressed image to the corresponding range in
    the (still encrypted/compressed) file body. Basic compression is just
    alternating (data, zero-fill) runs, and zero-fill bytes aren't stored in the
    file at all, so the requested range must fall entirely within one data run.
    """
    if blocks is None:
        return image_off  # uncompressed: image and body line up 1:1.

    out_pos = 0
    in_pos = 0
    for data_size, zero_size in blocks:
        if image_off >= out_pos and image_off + size <= out_pos + data_size:
            return in_pos + (image_off - out_pos)
        out_pos += data_size + zero_size
        in_pos += data_size
    raise RuntimeError("resource range is not contained in a single compression data block")


def _decrypt_body_range(body, session_key, body_off, size):
    """Decrypt only the AES-CBC blocks covering [body_off, body_off + size).

    CBC decryption of a block only depends on that ciphertext block and the
    immediately preceding one, so a targeted range can be decrypted directly
    without processing the whole (multi-megabyte) file body.
    """
    block_start = (body_off // 16) * 16
    block_end = ((body_off + size + 15) // 16) * 16
    iv = body[block_start - 16 : block_start] if block_start > 0 else bytes(16)
    plaintext = aes128_cbc_decrypt(session_key, body[block_start:block_end], iv)
    rel = body_off - block_start
    return plaintext[rel : rel + size]


def _find_title_resource(data, opt_headers, header_size, security_offset):
    title_id = struct.unpack_from(">I", data, opt_headers[XEX_HEADER_EXECUTION_INFO] + 0xC)[0]
    resource_name = f"{title_id:08X}".encode("ascii")

    res_off = opt_headers[XEX_HEADER_RESOURCE_INFO]
    res_size = struct.unpack_from(">I", data, res_off)[0]
    count = (res_size - 4) // 16
    load_address = struct.unpack_from(">I", data, security_offset + 0x110)[0]

    address = size = None
    p = res_off + 4
    for _ in range(count):
        name = data[p : p + 8].rstrip(b"\x00")
        cand_address, cand_size = struct.unpack_from(">II", data, p + 8)
        if name == resource_name:
            address, size = cand_address, cand_size
            break
        p += 16
    if address is None:
        raise RuntimeError(f"no XEX resource named {resource_name.decode()} (title ID)")
    image_off = address - load_address

    ffi_off = opt_headers[XEX_HEADER_FILE_FORMAT_INFO]
    info_size, encryption_type, compression_type = struct.unpack_from(">IHH", data, ffi_off)
    if compression_type not in (XEX_COMPRESSION_NONE, XEX_COMPRESSION_BASIC):
        raise RuntimeError(f"unsupported XEX compression type {compression_type}")
    if encryption_type not in (XEX_ENCRYPTION_NONE, XEX_ENCRYPTION_NORMAL):
        raise RuntimeError(f"unsupported XEX encryption type {encryption_type}")

    blocks = _read_basic_blocks(data, ffi_off, info_size) if compression_type == XEX_COMPRESSION_BASIC else None
    body_off = _map_image_range_to_body(blocks, image_off, size)

    body = data[header_size:]
    if encryption_type == XEX_ENCRYPTION_NORMAL:
        encrypted_key = data[security_offset + 0x150 : security_offset + 0x160]
        session_key = _aes128_decrypt_block(RETAIL_KEY, encrypted_key)
        return _decrypt_body_range(body, session_key, body_off, size)
    return body[body_off : body_off + size]


def _extract_xdbf_icon(xdbf):
    magic, _version, _entry_count, entry_used, free_count, _free_used = struct.unpack_from(
        ">IIIIII", xdbf, 0
    )
    if magic != XDBF_MAGIC:
        raise RuntimeError("title resource is not a valid XDBF blob")

    entries_off = 24
    entry_size = 18
    content_off = entries_off + entry_size * struct.unpack_from(">I", xdbf, 8)[0] + 8 * free_count

    for i in range(entry_used):
        e_off = entries_off + i * entry_size
        section = struct.unpack_from(">H", xdbf, e_off)[0]
        entry_id = struct.unpack_from(">Q", xdbf, e_off + 2)[0]
        offset, size = struct.unpack_from(">II", xdbf, e_off + 10)
        if section == XDBF_SECTION_IMAGE and entry_id == XDBF_ID_TITLE_ICON:
            return xdbf[content_off + offset : content_off + offset + size]
    raise RuntimeError("no title icon entry found in XDBF resource")


def extract_icon_png(xex_path):
    with open(xex_path, "rb") as f:
        data = f.read()

    if data[0:4] != b"XEX2":
        raise RuntimeError(f"{xex_path} is not an XEX2 file")

    header_size = struct.unpack_from(">I", data, 0x8)[0]
    security_offset = struct.unpack_from(">I", data, 0x10)[0]

    opt_headers = _read_opt_headers(data)
    xdbf = _find_title_resource(data, opt_headers, header_size, security_offset)
    return _extract_xdbf_icon(xdbf)


def write_ico(png_data, out_path):
    """Wraps the PNG in a minimal single-image .ico container so it can be
    embedded as the Windows executable's icon via a resource script. Windows
    Vista+ accepts a raw PNG payload inside an ICONDIRENTRY (no BMP/DIB
    re-encoding needed) as long as the entry's width/height match the image.
    """
    width, height = struct.unpack_from(">II", png_data, 16)

    def dim_byte(v):
        return 0 if v >= 256 else v

    icondir = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack(
        "<BBBBHHII",
        dim_byte(width),
        dim_byte(height),
        0,  # color count (0 = not a palette image)
        0,  # reserved
        1,  # color planes
        32,  # bits per pixel
        len(png_data),
        len(icondir) + 16,  # image data offset (right after the one entry)
    )
    with open(out_path, "wb") as f:
        f.write(icondir)
        f.write(entry)
        f.write(png_data)


def main():
    icon_data = extract_icon_png(XEX_PATH)
    write_ico(icon_data, OUT_ICO_PATH)

    lines = [
        "#pragma once",
        "",
        "// Generated by scripts/gen-icon.py from assets/default.xex. Do not edit by",
        "// hand or commit this file directly - it is rebuilt on every CMake configure.",
        "",
        "namespace nocturne {",
        "",
        "inline constexpr unsigned char kIconPNG[] = {",
    ]
    with open(OUT_PATH, "w", newline="\n") as out:
        out.write("\n".join(lines) + "\n")
        for i in range(0, len(icon_data), 20):
            chunk = icon_data[i : i + 20]
            out.write("    " + ", ".join(str(b) for b in chunk) + ",\n")
        out.write("};\n")
        out.write("inline constexpr unsigned int kIconPNGSize = sizeof(kIconPNG);\n\n")
        out.write("}  // namespace nocturne\n")

    print(f"+ wrote {OUT_PATH} ({len(icon_data)} byte icon)")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
