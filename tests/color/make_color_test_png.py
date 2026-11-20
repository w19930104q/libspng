#!/usr/bin/env python3
"""Generate deterministic PNG files exercising sRGB / gAMA / iCCP coexistence.

Stdlib only; run with: python3 -I make_color_test_png.py <icc_profile> <out_dir>

The generated files cover the PNG color-space chunk precedence rules
(sRGB takes precedence over gAMA; iCCP does not supply a gamma value for the
SPNG_DECODE_GAMMA approximation) across 8-bit and 16-bit sources, including
rendering-intent distinctions, a non-conformant gAMA paired with sRGB and the
no-profile fallback cases.
"""
import struct
import sys
import zlib

SRGB_GAMA = 45455          # PNG encoding of 1/0.45455, mandated alongside sRGB
NONSTD_GAMA = 55000       # deliberately not the sRGB-implied value
PNG_SIG = b"\x89PNG\r\n\x1a\n"


def chunk(ctype, body):
    return (struct.pack(">I", len(body)) + ctype + body +
            struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))


def ihdr(width, height, bit_depth, color_type=2):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth,
                                      color_type, 0, 0, 0))


def gama(value):
    return chunk(b"gAMA", struct.pack(">I", value))


def srgb(rendering_intent):
    return chunk(b"sRGB", bytes([rendering_intent]))


def iccp(profile, name=b"ICC profile"):
    body = name + b"\x00\x00" + zlib.compress(profile, 9)
    return chunk(b"iCCP", body)


def idat_rgb(width, height, bit_depth):
    channels = 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        for x in range(width):
            for c in range(channels):
                if bit_depth == 8:
                    raw.append((x * 7 + y * 13 + c * 41) & 0xFF)
                else:
                    sample = ((x * 1031 + y * 4099 + c * 17029) & 0xFFFF)
                    raw += struct.pack(">H", sample)
    return chunk(b"IDAT", zlib.compress(bytes(raw), 9))


def write_png(path, width, height, bit_depth, color_chunks, profile=None):
    out = bytearray(PNG_SIG)
    out += ihdr(width, height, bit_depth)
    # Color-space chunks must precede PLTE/IDAT; keep a fixed order so the
    # precedence decision is exercised regardless of chunk layout.
    for name, arg in color_chunks:
        if name == "gAMA":
            out += gama(arg)
        elif name == "sRGB":
            out += srgb(arg)
        elif name == "iCCP":
            out += iccp(profile)
    out += idat_rgb(width, height, bit_depth)
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(bytes(out))


# (filename, bit_depth, chunks, description used only by the reader test)
CASES_8 = [
    # required: all three chunks present, conformant gAMA
    ("color_srgb_gama_iccp.png", 8,
     [("sRGB", 0), ("gAMA", SRGB_GAMA), ("iCCP", None)]),
    # required: all three chunks present, non-conformant gAMA so the sRGB
    # precedence decision is observable in the decoded pixels
    ("color_srgb_gama_bad_iccp.png", 8,
     [("sRGB", 0), ("gAMA", NONSTD_GAMA), ("iCCP", None)]),
    # sRGB + iCCP without gAMA
    ("color_srgb_iccp.png", 8,
     [("sRGB", 0), ("iCCP", None)]),
    # sRGB + gAMA with a *non-conformant* gAMA: sRGB must win
    ("color_srgb_gama_bad.png", 8,
     [("sRGB", 0), ("gAMA", NONSTD_GAMA)]),
    # gAMA + iCCP without sRGB: gAMA drives the correction
    ("color_gama_iccp.png", 8,
     [("gAMA", SRGB_GAMA), ("iCCP", None)]),
    # iCCP only: no gamma source, SPNG_DECODE_GAMMA must be a no-op fallback
    ("color_iccp_only.png", 8,
     [("iCCP", None)]),
    # no color profile at all: same no-op fallback
    ("color_none.png", 8, []),
    # rendering intent distinctions (perceptual 0 vs. saturation 1)
    ("color_srgb_intent0.png", 8, [("sRGB", 0)]),
    ("color_srgb_intent1.png", 8, [("sRGB", 1)]),
    # plain gAMA fallback, non-conformant so the correction is observable
    ("color_gama_only.png", 8, [("gAMA", NONSTD_GAMA)]),
]

CASES_16 = [
    # 16-bit source with all three chunks, conformant gAMA
    ("color16_srgb_gama_iccp.png", 16,
     [("sRGB", 0), ("gAMA", SRGB_GAMA), ("iCCP", None)]),
    # 16-bit source with all three chunks, non-conformant gAMA
    ("color16_srgb_gama_bad_iccp.png", 16,
     [("sRGB", 0), ("gAMA", NONSTD_GAMA), ("iCCP", None)]),
    # 16-bit source, sRGB only
    ("color16_srgb.png", 16, [("sRGB", 0)]),
    # 16-bit source, iCCP-only fallback
    ("color16_iccp_only.png", 16, [("iCCP", None)]),
    # 16-bit source, no profile fallback
    ("color16_none.png", 16, []),
]


def main():
    if len(sys.argv) != 3:
        print("usage: python3 -I make_color_test_png.py <icc_profile> <out_dir>")
        return 1
    with open(sys.argv[1], "rb") as f:
        profile = f.read()
    out_dir = sys.argv[2].rstrip("/")
    for filename, depth, chunks in CASES_8 + CASES_16:
        write_png(out_dir + "/" + filename, 32, 32, depth, chunks, profile)
        print("wrote", filename)
    return 0


if __name__ == "__main__":
    sys.exit(main())
