#!/usr/bin/env python3
"""Apply an Xbox 360 XEX delta patch (.xexp) to its base .xex and write an
uncompressed, unencrypted patched .xex that can be opened in Ghidra
(e.g. with the xexloaderwv extension).

The logic mirrors XexModule::ApplyPatch / ReadImageCompressed in the ReXGlue
SDK (../rexglue-sdk/src/system/xex_module.cpp and lzx.cpp), including the
LZX decoder (a port of the subset of libmspack's lzxd used there).

Usage: apply_xexp.py [base.xex patch.xexp [out.xex]]
Defaults: assets/default.xex assets/default.xexp -> assets/default_patched.xex
"""
import hashlib
import os
import struct
import sys

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    sys.exit("This script needs the 'cryptography' package (pip install cryptography)")

RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])
DEVKIT_KEY = bytes(16)

XEX_HEADER_FILE_FORMAT_INFO = 0x3FF
XEX_HEADER_DELTA_PATCH_DESCRIPTOR = 0x5FF
XEX_HEADER_IMAGE_BASE_ADDRESS = 0x10201

ENC_NONE, ENC_NORMAL = 0, 1
COMP_NONE, COMP_BASIC, COMP_NORMAL = 0, 1, 2

# Offsets inside xex2_security_info
SEC_IMAGE_SIZE = 0x4
SEC_RSA_SIGNATURE = 0x8
SEC_AES_KEY = 0x150


def aes_cbc_decrypt(key, data):
    data = bytes(data)
    data = data[:len(data) & ~15]
    if not data:
        return b""
    d = Cipher(algorithms.AES(key), modes.CBC(bytes(16))).decryptor()
    return d.update(data) + d.finalize()


def u32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def get_opt_header(hdr, key):
    """Return (kind, value_or_offset) following XexModule::GetOptHeader."""
    for i in range(u32(hdr, 20)):
        k, v = struct.unpack_from(">II", hdr, 24 + 8 * i)
        if k == key:
            return (k & 0xFF), v, 24 + 8 * i
    return None


def opt_data_offset(hdr, key):
    r = get_opt_header(hdr, key)
    if r is None:
        return None
    kind, v, _ = r
    if kind == 0x01:
        raise ValueError("inline header has no data offset")
    return v


# --------------------------------------------------------------------------
# LZX decoder (libmspack lzxd, non-delta stream, optional reference window)
# --------------------------------------------------------------------------

LZX_FRAME_SIZE = 0x8000
LZX_NUM_CHARS = 256
LZX_MIN_MATCH = 2
LZX_NUM_PRIMARY_LENGTHS = 7
LZX_NUM_SECONDARY_LENGTHS = 249
POSITION_SLOTS = [30, 32, 34, 36, 38, 42, 50, 66, 98, 162, 290]
EXTRA_BITS = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
              9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16]
POSITION_BASE = [0]
for _i in range(1, 290):
    POSITION_BASE.append(
        POSITION_BASE[-1] + (1 << (EXTRA_BITS[_i - 1] if _i - 1 < 36 else 17)))


class LzxError(Exception):
    pass


def build_table(lens):
    """Canonical MSB-first Huffman lookup over 16 peeked bits.
    Each entry is (symbol << 5) | length, or 0 for an unassigned code."""
    table = [0] * 65536
    code = 0
    for l in range(1, 17):
        for sym, sl in enumerate(lens):
            if sl != l:
                continue
            start = code << (16 - l)
            cnt = 1 << (16 - l)
            if start + cnt > 65536:
                raise LzxError("over-subscribed huffman tree")
            table[start:start + cnt] = [(sym << 5) | l] * cnt
            code += 1
        code <<= 1
    return table


class LzxStream:
    def __init__(self, data, window_size, out_len, ref_data=None):
        if window_size & (window_size - 1) or window_size < 0x8000:
            raise LzxError("bad window size")
        wbits = window_size.bit_length() - 1
        if wbits > 21:
            raise LzxError("window too large for non-delta LZX")
        self.src = bytes(data) + b"\0" * 8
        self.ip = 0
        self.buf = 0
        self.nbits = 0
        self.window_size = window_size
        self.window = bytearray(window_size)
        self.ref_data_size = 0
        if ref_data:
            pad = window_size - len(ref_data)
            self.window[pad:] = ref_data
            self.ref_data_size = window_size
        self.num_offsets = POSITION_SLOTS[wbits - 15] << 3
        self.out_len = out_len
        self.main_len = [0] * (LZX_NUM_CHARS + self.num_offsets)
        self.length_len = [0] * LZX_NUM_SECONDARY_LENGTHS

    # -- bit reader (16-bit little-endian words, MSB first) --
    def ensure(self, n):
        while self.nbits < n:
            w = self.src[self.ip] | (self.src[self.ip + 1] << 8)
            if self.ip >= len(self.src) - 8:
                raise LzxError("read past end of input")
            self.ip += 2
            self.buf = (self.buf << 16) | w
            self.nbits += 16

    def bits(self, n):
        if n == 0:
            return 0
        self.ensure(n)
        self.nbits -= n
        v = (self.buf >> self.nbits) & ((1 << n) - 1)
        self.buf &= (1 << self.nbits) - 1
        return v

    def sym(self, table):
        if self.nbits < 16:
            # the stream may legitimately end inside the final peek
            while self.nbits < 16:
                if self.ip >= len(self.src) - 8:
                    self.buf <<= 16
                    self.nbits += 16
                else:
                    self.ensure(self.nbits + 1)
        e = table[(self.buf >> (self.nbits - 16)) & 0xFFFF]
        if not e:
            raise LzxError("bad huffman code")
        l = e & 31
        self.nbits -= l
        self.buf &= (1 << self.nbits) - 1
        return e >> 5

    def read_lens(self, lens, first, last):
        pre = [self.bits(4) for _ in range(20)]
        pt = build_table(pre)
        x = first
        while x < last:
            z = self.sym(pt)
            if z == 17:
                y = self.bits(4) + 4
                lens[x:x + y] = [0] * y
                x += y
            elif z == 18:
                y = self.bits(5) + 20
                lens[x:x + y] = [0] * y
                x += y
            elif z == 19:
                y = self.bits(1) + 4
                z = self.sym(pt)
                z = lens[x] - z
                if z < 0:
                    z += 17
                lens[x:x + y] = [z] * y
                x += y
            else:
                z = lens[x] - z
                if z < 0:
                    z += 17
                lens[x] = z
                x += 1

    def decode(self):
        out = bytearray()
        win = self.window
        wsize = self.window_size
        window_posn = 0
        frame_posn = 0
        frame = 0
        R0 = R1 = R2 = 1
        block_type = 0
        block_len = 0
        block_remaining = 0
        header_read = False
        intel_filesize = 0
        aligned_table = main_table = length_table = None
        length_empty = False

        end_frame = self.out_len // LZX_FRAME_SIZE + 1
        while frame < end_frame:
            if not header_read:
                intel_filesize = 0
                if self.bits(1):
                    hi = self.bits(16)
                    lo = self.bits(16)
                    intel_filesize = (hi << 16) | lo
                header_read = True

            frame_size = LZX_FRAME_SIZE
            if self.out_len - len(out) < frame_size:
                frame_size = self.out_len - len(out)
            bytes_todo = frame_posn + frame_size - window_posn

            while bytes_todo > 0:
                if block_remaining == 0:
                    if block_type == 3 and (block_len & 1):
                        self.ip += 1  # realign after odd-sized uncompressed block
                    block_type = self.bits(3)
                    hi = self.bits(16)
                    lo = self.bits(8)
                    block_len = block_remaining = (hi << 8) | lo
                    if block_type in (1, 2):
                        if block_type == 2:
                            al = [self.bits(3) for _ in range(8)]
                            aligned_table = build_table(al)
                        self.read_lens(self.main_len, 0, 256)
                        self.read_lens(self.main_len, 256,
                                       LZX_NUM_CHARS + self.num_offsets)
                        main_table = build_table(self.main_len)
                        self.read_lens(self.length_len, 0,
                                       LZX_NUM_SECONDARY_LENGTHS)
                        length_empty = not any(self.length_len)
                        length_table = None if length_empty else build_table(self.length_len)
                    elif block_type == 3:
                        consumed = self.ip * 8 - self.nbits
                        if consumed % 16 == 0:
                            self.ip += 2
                        else:
                            self.ip = (consumed + 15) // 16 * 2
                        self.buf = 0
                        self.nbits = 0
                        R0, R1, R2 = struct.unpack_from("<III", self.src, self.ip)
                        self.ip += 12
                    else:
                        raise LzxError("bad block type")

                this_run = min(block_remaining, bytes_todo)
                bytes_todo -= this_run
                block_remaining -= this_run

                if block_type in (1, 2):
                    aligned = block_type == 2
                    while this_run > 0:
                        main_element = self.sym(main_table)
                        if main_element < LZX_NUM_CHARS:
                            win[window_posn] = main_element
                            window_posn += 1
                            this_run -= 1
                            continue
                        main_element -= LZX_NUM_CHARS
                        match_length = main_element & LZX_NUM_PRIMARY_LENGTHS
                        if match_length == LZX_NUM_PRIMARY_LENGTHS:
                            if length_empty:
                                raise LzxError("length tree empty")
                            match_length += self.sym(length_table)
                        match_length += LZX_MIN_MATCH

                        slot = main_element >> 3
                        if slot == 0:
                            match_offset = R0
                        elif slot == 1:
                            match_offset = R1
                            R1 = R0
                            R0 = match_offset
                        elif slot == 2:
                            match_offset = R2
                            R2 = R0
                            R0 = match_offset
                        else:
                            extra = 17 if slot >= 36 else EXTRA_BITS[slot]
                            match_offset = POSITION_BASE[slot] - 2
                            if extra >= 3 and aligned:
                                if extra > 3:
                                    match_offset += self.bits(extra - 3) << 3
                                match_offset += self.sym(aligned_table)
                            elif extra:
                                match_offset += self.bits(extra)
                            R2 = R1
                            R1 = R0
                            R0 = match_offset

                        if window_posn + match_length > wsize:
                            raise LzxError("match ran over window wrap")

                        if match_offset > window_posn:
                            if (match_offset > len(out) and
                                    match_offset - window_posn > self.ref_data_size):
                                raise LzxError("match offset beyond LZX stream")
                            j = match_offset - window_posn
                            if j > wsize:
                                raise LzxError("match offset beyond window")
                            src = wsize - j
                            n = match_length
                            if j < n:
                                win[window_posn:window_posn + j] = win[src:src + j]
                                win[window_posn + j:window_posn + n] = win[0:n - j]
                            else:
                                win[window_posn:window_posn + n] = win[src:src + n]
                        else:
                            src = window_posn - match_offset
                            n = match_length
                            if match_offset >= n:
                                win[window_posn:window_posn + n] = win[src:src + n]
                            else:
                                pat = bytes(win[src:window_posn])
                                rep = (pat * (n // match_offset + 1))[:n]
                                win[window_posn:window_posn + n] = rep
                        this_run -= match_length
                        window_posn += match_length
                else:
                    win[window_posn:window_posn + this_run] = \
                        self.src[self.ip:self.ip + this_run]
                    self.ip += this_run
                    window_posn += this_run
                    this_run = 0

                if this_run < 0:
                    if -this_run > block_remaining:
                        raise LzxError("overrun went past end of block")
                    block_remaining -= -this_run

            if window_posn - frame_posn != frame_size:
                raise LzxError("decode beyond output frame limits")

            # re-align input bitstream to a 16-bit boundary
            if self.nbits & 15:
                self.nbits -= self.nbits & 15
                self.buf &= (1 << self.nbits) - 1

            data = win[frame_posn:frame_posn + frame_size]
            if (intel_filesize and frame < 32768 and frame_size > 10):
                curpos = len(out)
                pos = 0
                end = frame_size - 10
                while pos < end:
                    if data[pos] != 0xE8:
                        pos += 1
                        curpos += 1
                        continue
                    pos += 1
                    abs_off = struct.unpack_from("<i", data, pos)[0]
                    if -curpos <= abs_off < intel_filesize:
                        rel = abs_off - curpos if abs_off >= 0 else abs_off + intel_filesize
                        struct.pack_into("<i", data, pos, rel)
                    pos += 4
                    curpos += 5
            out += data

            frame_posn += frame_size
            frame += 1
            if window_posn == wsize:
                window_posn = 0
            if frame_posn == wsize:
                frame_posn = 0

        return bytes(out)


def lzx_decompress(data, window_size, out_len, ref_data=None):
    return LzxStream(data, window_size, out_len, ref_data).decode()


def lzxdelta_apply_patch(patch, window_size, dest):
    """Port of lzxdelta_apply_patch; `patch` is bytes, `dest` a bytearray."""
    pos = 0
    n = len(patch)
    while pos < n:
        if pos + 12 > n:
            break
        old_addr, new_addr, ulen, clen = struct.unpack_from(">IIHH", patch, pos)
        if clen == 0 and ulen == 0 and new_addr == 0 and old_addr == 0:
            break
        pos += 12
        if clen == 0:
            dest[new_addr:new_addr + ulen] = bytes(ulen)
            skip = 0
        elif clen == 1:
            dest[new_addr:new_addr + ulen] = bytes(dest[old_addr:old_addr + ulen])
            skip = 0
        else:
            ref = bytes(dest[old_addr:old_addr + ulen])
            dest[new_addr:new_addr + ulen] = lzx_decompress(
                patch[pos:pos + clen], window_size, ulen, ref)
            skip = clen
        pos += skip
    return 0


# --------------------------------------------------------------------------
# XEX handling
# --------------------------------------------------------------------------

def decode_base_image(xex, session_key):
    """Decrypt/decompress the base XEX image (ReadImage*)."""
    hdr_size = u32(xex, 8)
    ffi = opt_data_offset(xex, XEX_HEADER_FILE_FORMAT_INFO)
    info_size, enc, comp = struct.unpack_from(">IHH", xex, ffi)
    sec = u32(xex, 0x10)
    image_size = u32(xex, sec + SEC_IMAGE_SIZE)
    body = xex[hdr_size:]

    if comp == COMP_NONE:
        img = aes_cbc_decrypt(session_key, body) if enc == ENC_NORMAL else bytes(body)
        return bytearray(img)

    if comp == COMP_BASIC:
        nblocks = (info_size - 8) // 8
        out = bytearray()
        p = 0
        for i in range(nblocks):
            ds, zs = struct.unpack_from(">II", xex, ffi + 8 + 8 * i)
            chunk = body[p:p + ds]
            p += ds
            out += aes_cbc_decrypt(session_key, chunk) if enc == ENC_NORMAL else chunk
            out += bytes(zs)
        return out

    if comp != COMP_NORMAL:
        raise ValueError(f"unsupported compression type {comp}")

    window_size = u32(xex, ffi + 8)
    blk_size = u32(xex, ffi + 12)
    blk_hash = bytes(xex[ffi + 16:ffi + 36])
    inp = aes_cbc_decrypt(session_key, body) if enc == ENC_NORMAL else bytes(body)

    comp_stream = bytearray()
    p = 0
    while blk_size:
        blk = inp[p:p + blk_size]
        if hashlib.sha1(blk).digest() != blk_hash:
            raise ValueError("base XEX block hash mismatch (wrong key?)")
        nxt_size, = struct.unpack_from(">I", blk, 0)
        nxt_hash = bytes(blk[4:24])
        q = 24
        while True:
            chunk = (blk[q] << 8) | blk[q + 1]
            q += 2
            if not chunk:
                break
            comp_stream += blk[q:q + chunk]
            q += chunk
        p += blk_size
        blk_size, blk_hash = nxt_size, nxt_hash

    return bytearray(lzx_decompress(comp_stream, window_size, image_size))


def apply_patch(base, xexp):
    if base[:4] != b"XEX2" or xexp[:4] != b"XEX2":
        raise ValueError("not a XEX2 file")
    pflags = u32(xexp, 4)
    if not pflags & 0x70:
        raise ValueError("second file is not a XEX patch")

    pd = opt_data_offset(xexp, XEX_HEADER_DELTA_PATCH_DESCRIPTOR)
    (psize, target_ver, source_ver) = struct.unpack_from(">III", xexp, pd)
    digest_source = bytes(xexp[pd + 0xC:pd + 0x20])
    image_key_source = bytes(xexp[pd + 0x20:pd + 0x30])
    (size_of_target_headers, dh_src_off, dh_src_size, dh_tgt_off,
     di_src_off, di_src_size, di_tgt_off) = struct.unpack_from(">7I", xexp, pd + 0x30)
    delta_info_off = pd + 0x4C

    ffi = opt_data_offset(xexp, XEX_HEADER_FILE_FORMAT_INFO)
    _, p_enc, p_comp = struct.unpack_from(">IHH", xexp, ffi)
    p_window = u32(xexp, ffi + 8)
    p_first_size = u32(xexp, ffi + 12)
    p_first_hash = bytes(xexp[ffi + 16:ffi + 36])

    base_sec = u32(base, 0x10)
    if hashlib.sha1(bytes(base[base_sec + SEC_RSA_SIGNATURE:base_sec + SEC_RSA_SIGNATURE + 0x100])).digest() != digest_source:
        print("warning: patch source digest doesn't match base XEX signature",
              file=sys.stderr)

    # Base image (decoded with the original key, retail key first).
    base_hdr_size = u32(base, 8)
    orig_image_size = u32(base, base_sec + SEC_IMAGE_SIZE)
    image = None
    for key in (RETAIL_KEY, DEVKIT_KEY):
        orig_session_key = aes_cbc_decrypt(key, base[base_sec + SEC_AES_KEY:base_sec + SEC_AES_KEY + 16])
        print(f"decoding base image ({'retail' if key is RETAIL_KEY else 'devkit'} key)...")
        try:
            image = decode_base_image(base, orig_session_key)
        except (ValueError, LzxError) as e:
            print("  failed:", e, file=sys.stderr)
            continue
        if image[:2] == b"MZ":
            break
        image = None
    if image is None:
        raise ValueError("couldn't decode the base image")
    used_key = key

    # ---- header patch ----
    if dh_src_off > base_hdr_size:
        raise ValueError("header patch source outside base header")
    if dh_src_size > base_hdr_size - dh_src_off:
        raise ValueError("header patch source too large")
    if dh_tgt_off > size_of_target_headers:
        raise ValueError("header patch target outside header")

    header_target_size = size_of_target_headers or (dh_tgt_off + dh_src_size)
    hdr = bytearray(base[:base_hdr_size])
    if header_target_size > len(hdr):
        hdr += bytes(header_target_size - len(hdr))
    if dh_src_off:
        hdr[dh_tgt_off:dh_tgt_off + dh_src_size] = bytes(hdr[dh_src_off:dh_src_off + dh_src_size])
    if header_target_size < len(hdr):
        hdr[header_target_size:] = bytes(len(hdr) - header_target_size)

    # The patch's own delta stream covers the header (compressed_len + 0xC).
    clen = struct.unpack_from(">H", xexp, delta_info_off + 10)[0]
    lzxdelta_apply_patch(bytes(xexp[delta_info_off:delta_info_off + clen + 0xC]), p_window, hdr)
    del hdr[header_target_size:]

    new_sec = u32(hdr, 0x10)
    new_image_size = u32(hdr, new_sec + SEC_IMAGE_SIZE)

    # ---- keys ----
    new_base_key = aes_cbc_decrypt(used_key, hdr[new_sec + SEC_AES_KEY:new_sec + SEC_AES_KEY + 16])
    patch_sec = u32(xexp, 0x10)
    patch_key = aes_cbc_decrypt(new_base_key, xexp[patch_sec + SEC_AES_KEY:patch_sec + SEC_AES_KEY + 16])
    if aes_cbc_decrypt(new_base_key, image_key_source) != orig_session_key:
        raise ValueError("patch image key doesn't match original XEX")

    # ---- image patch ----
    patch_body = xexp[u32(xexp, 8):]
    if p_enc == ENC_NORMAL:
        patch_body = aes_cbc_decrypt(patch_key, patch_body)
    elif p_enc != ENC_NONE:
        raise ValueError("unsupported patch encryption")

    if new_image_size > len(image):
        image += bytes(new_image_size - len(image))
    if di_src_off:
        image[di_tgt_off:di_tgt_off + di_src_size] = bytes(image[di_src_off:di_src_off + di_src_size])
    image_target_size = di_tgt_off + di_src_size
    if image_target_size < orig_image_size:
        image[image_target_size:orig_image_size] = bytes(orig_image_size - image_target_size)

    blk_size, blk_hash = p_first_size, p_first_hash
    p = 0
    nblocks = 0
    while blk_size:
        blk = patch_body[p:p + blk_size]
        if hashlib.sha1(blk).digest() != blk_hash:
            raise ValueError("patch block hash mismatch")
        nxt_size, = struct.unpack_from(">I", blk, 0)
        nxt_hash = bytes(blk[4:24])
        lzxdelta_apply_patch(bytes(blk[24:]), p_window, image)
        p += blk_size
        blk_size, blk_hash = nxt_size, nxt_hash
        nblocks += 1
    print(f"applied {nblocks} patch blocks")

    del image[new_image_size:]
    return hdr, image, source_ver, target_ver


def fmt_ver(v):
    return f"{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    args = sys.argv[1:]
    base_path = args[0] if len(args) > 0 else os.path.join(root, "assets", "default.xex")
    xexp_path = args[1] if len(args) > 1 else os.path.join(root, "assets", "default.xexp")
    out_path = args[2] if len(args) > 2 else os.path.join(root, "assets", "default_patched.xex")

    with open(base_path, "rb") as f:
        base = f.read()
    with open(xexp_path, "rb") as f:
        xexp = f.read()

    hdr, image, sv, tv = apply_patch(base, xexp)
    print(f"patch: {fmt_ver(sv)} -> {fmt_ver(tv)}")

    # Make the header describe a plain image: no compression, no encryption.
    ffi = opt_data_offset(hdr, XEX_HEADER_FILE_FORMAT_INFO)
    struct.pack_into(">IHH", hdr, ffi, 8, ENC_NONE, COMP_NONE)
    if len(hdr) != u32(hdr, 8):
        raise ValueError('patched header size mismatch')
    if image[:2] != b"MZ":
        raise ValueError("patched image has no MZ header")

    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(image)
    print(f"wrote {out_path} ({len(hdr) + len(image)} bytes)")


if __name__ == "__main__":
    main()
