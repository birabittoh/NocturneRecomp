#!/usr/bin/env python3
"""Build the guest shader pack: assets/MEDIA shader files -> HLSL -> DXIL/SPIR-V.

Nothing here runs at game time. The game loads its shaders from a handful of
loose files in assets/MEDIA (`.xvu` vertex, `.xpu` pixel), each holding exactly
one container, so its shader inventory is a closed set and translating it is a
finite offline problem:

    shader_extract.Shader   parse the container
    xenos_hlsl              microcode -> HLSL
    dxc                     HLSL -> DXIL and SPIR-V

Output is `guest_shaders.bin` plus a `.S` sidecar that `.incbin`s it into the
executable's read-only data, read back by src/gpu/guest_shaders.cpp.

Unlike Eternal Sonata, the guest has no shader table to index, so a bound shader
object is resolved to its pack entry by hashing its microcode (the pack's hash
section, see krypton::GuestShaderPack::FindSlot). Pixel shaders hash as they
are. Vertex shaders do not: the guest's D3D rewrites every vfetch instruction
in place from the bound vertex declaration, so their key is the hash with those
instructions zeroed (see vertex_key). The runtime computes the same thing from
the container header the shader object carries.

Not every shader the game binds comes from a file. The executable statically
links the XDK's HLSL compiler and builds the sample framework's font shaders
(ATG::Font, which draws all of the game's text) from source embedded at
0x820567D8 at startup, so their microcode exists only in guest memory. They are
ported by hand below (FONT_VS_BODY, FONT_PS_BODY) on top of the preamble the
translator emits, and keyed by the hashes the runtime computes for the guest's
compiled copies. The compiler and its source are both fixed in the executable,
so those keys are stable.

Left out of the pack, and logged at runtime if the game ever binds them:

  * EnhancedLayer and Particle, an older container revision (0x102A0E0x) whose
    header the translator does not read yet.
  * TesselationVS, a hardware tessellation shader: it fetches the four control
    points of a patch by index, so the same elements appear four times in its
    fetch table and there is no flat input layout to build for it.

Usage:
    python scripts/gen-guest-shaders.py --dxc <path/to/dxc> --out <dir>
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "ext", "KryptonRecomp"))
import shader_extract as X  # noqa: E402
from shader_build import compile_all, write_if_different, write_pack_asm  # noqa: E402
import xenos_hlsl as H  # noqa: E402
import xenos_ucode as U  # noqa: E402

PACK_MAGIC = b"NRGS"
PACK_VERSION = 1

SLOTS = X.MAX_INDEX + 1

FLAG_POINT_SIZE = 1 << 0
FLAG_HAS_CUBE = 1 << 1

# A literal pool is 4 or 8 float4s (xenos_ucode.literal_constants); a block
# holds the larger.
LITERAL_BLOCK = 128

# The container revision the translator understands.
CURRENT_MAGIC = 0x102A1100

# ATG::Font's pair, as the guest's compiler builds it from:
#
#   uniform float4 Color    : register(c1);
#   uniform float2 TexScale : register(c2);
#   sampler FontTexture     : register(s0);
#
#   Out.Position = float4(In.Pos.xy - 0.5, 0, 1);
#   Out.Diffuse = Color;
#   Out.TexCoord0 = In.Tex * TexScale;
#   Out.ChannelSelector = In.ChannelSelector;
#
# and a pixel shader that either modulates the texel by Diffuse, or, when a
# channel is selected, decodes that channel of the packed 4:4:4:4 glyph texture:
# above 0.5 is an opaque white pixel whose low bits are luminance, below it a
# black pixel whose low bits are alpha.
#
# The interpolators reuse the title's keys (80 for the texture coordinate, 81
# for the channel selector, 160 for the colour) so the pair links like any
# other. The vertex declaration is POSITION float2, TEXCOORD0 float2 and
# TEXCOORD1 8:8:8:8.
FONT_VS_KEY = 0x81D114ABD62842E8
FONT_PS_KEY = 0x5CC647F80C1324BD

FONT_VS_BODY = """\
struct VSInput {
    float4 in_position0 : POSITION0;
    float4 in_texcoord0 : TEXCOORD0;
    float4 in_texcoord1 : TEXCOORD1;
};

struct VSOutput {
    float4 out_position : SV_Position;
    float4 out_p80 : TEXCOORD80;
    float4 out_p81 : TEXCOORD81;
    float4 out_p160 : TEXCOORD160;
    float4 out_param_gen : TEXCOORD254;
    float out_point_size : TEXCOORD255;
};

VSOutput main(VSInput input) {
    VSOutput output = (VSOutput)0;
    output.out_position = float4(input.in_position0.xy - 0.5f, 0.0f, 1.0f);
    output.out_position.xy = output.out_position.xy * xe_screen_transform.xy
                             + xe_screen_transform.zw;
    output.out_p80 = float4(input.in_texcoord0.xy * c[2].xy, 0.0f, 0.0f);
    output.out_p81 = input.in_texcoord1;
    output.out_p160 = c[1];
    output.out_point_size = -1.0f;
    return output;
}
"""

FONT_PS_BODY = """\
Texture2D xe_texture0 : register(t0, space0);
SamplerState xe_sampler0 : register(s0, space1);

struct PSInput {
    float4 in_position : SV_Position;
    float4 in_p80 : TEXCOORD80;
    float4 in_p81 : TEXCOORD81;
    float4 in_p160 : TEXCOORD160;
    float4 in_param_gen : TEXCOORD254;
    float in_point_size : TEXCOORD255;
};

float4 main(PSInput input) : SV_Target0 {
    float4 texel = xe_texture_fetch_post(
        xe_texture0.Sample(xe_sampler0, input.in_p80.xy), 0u);
    float4 out_color;
    if (dot(input.in_p81, 1.0f.xxxx) != 0.0f) {
        float value = dot(texel, input.in_p81);
        float4 decoded;
        decoded.rgb = (value > 0.5f ? 2.0f * value - 1.0f : 0.0f).xxx;
        decoded.a = 2.0f * (value > 0.5f ? 1.0f : value);
        out_color = decoded * input.in_p160;
    } else {
        out_color = texel * input.in_p160;
    }
    xe_alpha_test(out_color.a);
    return out_color;
}
"""


# The guest's viewport transform, applied by every vertex shader in the pack.
#
# The hardware maps a position to the render target through PA_CL_VTE_CNTL and
# the PA_CL_VPORT_* scale and offset, and this title turns those off for its 2D
# draws (ATG::Font sets D3DRS_VIEWPORTENABLE to FALSE, and the sprite path
# writes pixel positions with zero vertex constants), so its positions are
# render target pixels rather than clip space. The host rasteriser always
# applies a viewport to clip space, so the guest's transform and the pixel to
# clip mapping are folded into one multiply-add per draw:
#
#     clip.xy = position.xy * scale + position.w * offset
#
# which is right whether the guest transform is enabled or not. The runtime
# builds it from the register shadows (ScreenTransform in
# src/gpu/native_renderer_draw.cpp). b5 has to agree with the pipeline layout.
SCREEN_TRANSFORM_CB = """\
cbuffer XeScreenTransform : register(b5, space2) { float4 xe_screen_transform; };

"""
SCREEN_TRANSFORM = """\
    output.out_position.xy = out_position.xy * xe_screen_transform.xy
                             + out_position.w * xe_screen_transform.zw;
"""


def add_screen_transform(text):
    """Declare the transform and apply it to a translated vertex shader's
    position export, after everything else the shader does to it."""
    marker = "struct VSInput {"
    store = "    output.out_position = out_position;\n"
    if text.count(marker) != 1 or text.count(store) != 1:
        raise SystemExit("vertex shader layout changed; cannot place the "
                         "screen transform")
    text = text.replace(marker, SCREEN_TRANSFORM_CB + marker)
    return text.replace(store, store + SCREEN_TRANSFORM)


def vertex_fetch_dwords(header):
    """Dword offsets into the microcode of every instruction the guest patches.

    Container header dword 6 is the byte offset of the program sub header S.
    S[0] is the byte size of the literal constant prefix in front of the
    instructions, and the vertex fetch table at S[9 + S[6]] (S[7] entries)
    numbers instructions from the end of that prefix, 3 dwords each. Mirrored by
    ShaderKey in src/gpu/guest_shaders.cpp, which reads the same header from
    the shader object.
    """
    dwords = struct.unpack(">%dI" % (len(header) // 4), header)
    sub = dwords[dwords[6] // 4:]
    prefix = sub[0] // 4
    start = 9 + sub[6]
    return [prefix + 3 * (entry & 0xFFF) + i
            for entry in sub[start:start + sub[7]] for i in range(3)]


def vertex_key(shader):
    """FNV-1a 64 over the microcode with the patched vfetch dwords zeroed."""
    words = bytearray(shader.microcode)
    for dword in vertex_fetch_dwords(shader.header):
        words[dword * 4:dword * 4 + 4] = b"\0\0\0\0"
    return X.microcode_hash(bytes(words))


class Translated:
    """One guest shader, translated but not yet compiled."""

    @classmethod
    def ported(cls, kind, slot, name, key, inputs, keys, texture_mask):
        """A shader written by hand rather than translated."""
        entry = cls.__new__(cls)
        entry.kind = kind
        entry.slot = slot
        entry.name = name
        entry.source_file = "ported"
        entry.hash = key
        entry.inputs = inputs
        entry.keys = keys
        entry.literals = None
        entry.texture_mask = texture_mask
        entry.flags = 0
        entry.blobs = {}
        entry.source = None
        return entry

    def __init__(self, shader, emitted):
        self.kind = shader.kind
        self.slot = shader.index
        self.name = shader.name
        self.source_file = shader.source
        self.hash = vertex_key(shader) if shader.kind == "vs" else shader.hash
        self.inputs = [(e["usage"], e["usage_index"]) for e in emitted.inputs] \
            if shader.kind == "vs" else []
        self.keys = sorted(e["key"] for e in emitted.interpolators)
        self.literals = emitted.literals
        self.texture_mask = 0
        self.flags = 0
        if shader.kind == "ps":
            for texture_slot, kind_name in emitted.textures.items():
                self.texture_mask |= 1 << texture_slot
                if kind_name == "TextureCube":
                    self.flags |= FLAG_HAS_CUBE
            # SQ_PROGRAM_CNTL param_gen and SQ_CONTEXT_MISC param_gen_pos, from
            # the header words the guest ORs into its registers at draw time.
            words = struct.unpack_from(
                ">4I", shader.container,
                struct.unpack_from(">I", shader.container, 24)[0])
            if (words[2] >> 18) & 1:
                # The entry's tail byte holds the literal count in this pack,
                # so there is nowhere to put param_gen_pos. No NR shader sets
                # it; fail loudly if one ever does.
                raise SystemExit("%s uses param_gen, which this pack cannot "
                                 "carry" % shader.name)
        elif H.EXPORT_POINT_SIZE in emitted.exports:
            self.flags |= FLAG_POINT_SIZE
        self.blobs = {}
        self.source = None


def load_shaders(media):
    """Every current-revision container in `media`, numbered per kind in
    file name order, plus the names of the files left out."""
    sources, skipped = [], []
    for name in sorted(os.listdir(media)):
        if not name.lower().endswith((".xvu", ".xpu")):
            continue
        with open(os.path.join(media, name), "rb") as handle:
            data = handle.read()
        magic, = struct.unpack_from(">I", data)
        if magic & X.CONTAINER_MAGIC_MASK != CURRENT_MAGIC:
            skipped.append("%s (old container)" % os.path.splitext(name)[0])
            continue
        sources.append((os.path.splitext(name)[0], [(0, data)]))
    return X.collect_unique(sources), skipped


def translate(shaders, hlsl_dir, skipped):
    os.makedirs(hlsl_dir, exist_ok=True)

    # One title-wide interpolator layout, so any (VS, PS) pair the game forms
    # links: see Shader.interpolator_layout in xenos_hlsl.
    decoded = []
    keys = set()
    for shader in shaders:
        kind = H.PixelShader if shader.kind == "ps" else H.VertexShader
        emitted = kind(shader.name, shader.microcode, shader.header)
        if emitted.unsupported:
            raise SystemExit("%s (%s) does not translate: %s"
                             % (shader.name, shader.source,
                                "; ".join(emitted.unsupported)))
        if shader.kind == "vs":
            signature = [(e["usage"], e["usage_index"]) for e in emitted.inputs]
            if len(set(signature)) != len(signature):
                skipped.append("%s (tessellation)" % shader.source)
                continue
            # The fetch table this key masks must be exactly the vfetch
            # instructions the decoder finds, or the runtime key would miss.
            prefix = U.decode(shader.microcode)["prefix"] // 4
            expected = sorted(prefix + 3 * e["instruction"] + i
                              for e in emitted.inputs for i in range(3))
            if sorted(vertex_fetch_dwords(shader.header)) != expected:
                raise SystemExit("%s: vertex fetch table does not match its "
                                 "vfetch instructions" % shader.name)
        keys.update(entry["key"] for entry in emitted.interpolators)
        decoded.append((shader, emitted))

    layout = sorted(keys)
    out = []
    for shader, emitted in decoded:
        emitted.interpolator_layout = layout
        text = emitted.emit(shader.name)
        if shader.kind == "vs":
            text = add_screen_transform(text)
        text = text.encode("utf-8")
        entry = Translated(shader, emitted)
        entry.source = os.path.join(hlsl_dir, shader.name + ".hlsl")
        write_if_different(entry.source, text)
        entry.text = text.decode("utf-8")
        out.append(entry)

    out.append(port_font(out, layout, hlsl_dir, "vs", FONT_VS_KEY,
                         [(0, 0), (5, 0), (5, 1)], 0, FONT_VS_BODY,
                         "struct VSInput {"))
    out.append(port_font(out, layout, hlsl_dir, "ps", FONT_PS_KEY, [], 1,
                         FONT_PS_BODY, "struct PSInput {"))
    return out


def port_font(entries, layout, hlsl_dir, kind, key, inputs, texture_mask,
              body, marker):
    """One of the font pair, on the preamble of a translated shader of the same
    kind: everything before its first structure is the constant banks, the
    draw state and the helpers, and the bindings have to match exactly."""
    if not {80, 81, 160} <= set(layout):
        raise SystemExit("font shaders: the title's interpolator keys changed")
    base = next(e for e in entries
                if e.kind == kind and (kind == "vs" or e.texture_mask))
    preamble = base.text[:base.text.index(marker)]
    # A translated pixel shader declares its textures just above PSInput; the
    # body declares its own.
    preamble = preamble[:preamble.find("Texture2D ")] \
        if "Texture2D " in preamble else preamble
    # A translated vertex shader's preamble already ends in the screen
    # transform's cbuffer (add_screen_transform), which the body uses.
    slot = 1 + max(e.slot for e in entries if e.kind == kind)
    name = "%s_%03d" % (kind, slot)
    entry = Translated.ported(kind, slot, name, key, inputs, [80, 81, 160],
                              texture_mask)
    entry.source_file = "ATG::Font"
    entry.source = os.path.join(hlsl_dir, name + ".hlsl")
    write_if_different(entry.source,
                       (preamble.replace(base.name, name) + body).encode("utf-8"))
    return entry


def pack(entries):
    """Header, entry table, hash section, signature tables, then the blobs.

    The layout krypton::GuestShaderPack reads with has_hash_section set and no
    renderer-owned extras.
    """
    blob = bytearray()
    inputs = bytearray()
    keys = bytearray()
    literals = bytearray(LITERAL_BLOCK)
    literal_index = {}

    # Right aligned in a fixed block, so the pool's registers end at 255 like
    # the bank they overlay; the entry's tail byte counts them.
    def place_literals(values):
        if values is None:
            return 0
        data = bytes(LITERAL_BLOCK - 4 * len(values)) + \
            struct.pack("<%df" % len(values), *values)
        if data not in literal_index:
            literal_index[data] = len(literals) // LITERAL_BLOCK
            literals.extend(data)
        return literal_index[data]

    seen = {}

    def place(data):
        if not data:
            return (0, 0)
        if data not in seen:
            while len(blob) % 4:
                blob.append(0)
            seen[data] = (len(blob), len(data))
            blob.extend(data)
        return seen[data]

    empty = struct.pack("<IIIIIHBBHBBH", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    table = [empty] * (SLOTS * 2)
    hashes = [0] * (SLOTS * 2)
    for entry in entries:
        index = (0 if entry.kind == "vs" else SLOTS) + entry.slot
        dxil = place(entry.blobs.get("dxil", b""))
        spirv = place(entry.blobs.get("spirv", b""))
        input_offset = len(inputs) // 2
        for usage, usage_index in entry.inputs:
            inputs.extend((usage, usage_index))
        key_offset = len(keys)
        keys.extend(entry.keys)
        table[index] = struct.pack(
            "<IIIIIHBBHBBH",
            dxil[0], dxil[1], spirv[0], spirv[1], entry.texture_mask,
            input_offset, len(entry.inputs), entry.flags,
            key_offset, len(entry.keys),
            0 if entry.literals is None else len(entry.literals) // 4,
            place_literals(entry.literals))
        hashes[index] = entry.hash

    header = struct.pack("<4sIIIIII", PACK_MAGIC, PACK_VERSION, SLOTS,
                         len(inputs) // 2, len(keys), len(literals) // LITERAL_BLOCK,
                         len(blob))
    return (header + b"".join(table) + struct.pack("<%dQ" % len(hashes), *hashes)
            + bytes(inputs) + bytes(keys) + bytes(literals) + bytes(blob))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--media", default=os.path.join(root, "assets/MEDIA"))
    parser.add_argument("--out", default=os.path.join(root, "out/guest_shaders"),
                        help="working directory for the .hlsl and compiled blobs")
    parser.add_argument("--pack", help="pack file to write "
                                       "(default <out>/guest_shaders.bin)")
    parser.add_argument("--pack-asm", help=".S file to write, .incbin-ing "
                                           "--pack (default <pack>.S)")
    parser.add_argument("--dxc", required=True, help="dxc executable")
    parser.add_argument("--dxc-lib-dir",
                        help="directory holding libdxcompiler, if it is not "
                             "next to the executable (Linux and macOS)")
    parser.add_argument("--formats", default="dxil,spirv",
                        help="comma separated: dxil, spirv")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    formats = [f.strip() for f in args.formats.split(",") if f.strip()]
    for fmt in formats:
        if fmt not in ("dxil", "spirv"):
            parser.error("unknown format %r" % fmt)

    # DXC ships as a thin executable next to a shared library it dlopens by
    # plain name, so on Linux and macOS the loader has to be pointed at it.
    if args.dxc_lib_dir:
        variable = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" \
            else "LD_LIBRARY_PATH"
        os.environ[variable] = args.dxc_lib_dir + os.pathsep + \
            os.environ.get(variable, "")
    dxc_command = [os.path.normpath(os.path.abspath(args.dxc))]

    shaders, skipped = load_shaders(args.media)
    entries = translate(shaders, os.path.join(args.out, "hlsl"), skipped)
    compile_all(dxc_command, entries, formats, args.out, args.jobs)
    data = pack(entries)

    pack_path = args.pack or os.path.join(args.out, "guest_shaders.bin")
    os.makedirs(os.path.dirname(os.path.abspath(pack_path)), exist_ok=True)
    write_if_different(pack_path, data)
    write_pack_asm(args.pack_asm or (pack_path + ".S"), pack_path,
                   "kGuestShaderPackData")

    for entry in entries:
        print("  %s %s key %016X" % (entry.name, entry.source_file, entry.hash))
    if skipped:
        print("  not in the pack: %s" % ", ".join(skipped))
    print("guest shaders: %d vertex, %d pixel, formats %s, pack %d bytes -> %s"
          % (sum(e.kind == "vs" for e in entries),
             sum(e.kind == "ps" for e in entries),
             "+".join(formats), len(data), pack_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
