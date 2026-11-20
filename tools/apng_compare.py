#!/usr/bin/env python3
"""Byte-for-byte comparison of C frame dumps against the pure-python
APNG reference renderer.

usage: apng_compare.py <region|compose> <rgba8|rgba16> <dumpdir> file.png ...

C dump naming: <dumpdir>/<basename>_<reg|comp>_f<N>.bin, canvas-sized.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apng_ref


def host_swap16(buf):
    """C emits host-endian (little-endian here) 16-bit samples; the python
    reference works in big-endian PNG byte order. Swap every 16-bit word."""
    out = bytearray(len(buf))
    for i in range(0, len(buf), 2):
        out[i] = buf[i + 1]
        out[i + 1] = buf[i]
    return bytes(out)


def region_frames(info, fmt):
    """Zeroed canvas-sized buffer per frame with only the frame rectangle
    written, matching the C library's region (non-compose) output."""
    W, H = info['w'], info['h']
    ps = 8 if fmt == 'RGBA16' else 4
    conv = apng_ref.to_rgba16 if fmt == 'RGBA16' else apng_ref.to_rgba8
    out = []
    for frame in info['frames']:
        rows = apng_ref.decode_frame_pixels(info, frame)
        region = conv(info, frame, rows)
        canvas = bytearray(W * H * ps)
        for y in range(frame.h):
            dst = ((frame.y + y) * W + frame.x) * ps
            src = y * frame.w * ps
            canvas[dst:dst + frame.w * ps] = region[src:src + frame.w * ps]
        out.append(bytes(canvas))
    return out


def main():
    mode, fmt_name, dumpdir = sys.argv[1], sys.argv[2], sys.argv[3]
    files = sys.argv[4:]
    fmt = 'RGBA16' if fmt_name == 'rgba16' else 'RGBA8'
    tag = 'comp' if mode == 'compose' else 'reg'

    nframes_checked = 0
    rc = 0

    for path in sorted(files):
        base = os.path.basename(path)
        info = apng_ref.parse_apng(path)
        if info['actl'] is None:
            # plain PNG: single full-canvas frame from the IDAT stream
            f = type('Fr', (), dict(w=info['w'], h=info['h'],
                                    data=info['default_idat']))()
            rows = apng_ref.decode_frame_pixels(info, f)
            conv = apng_ref.to_rgba16 if fmt == 'RGBA16' else apng_ref.to_rgba8
            expected = [conv(info, f, rows)]
            n_exp = 1
        else:
            if mode == 'compose':
                _, rendered = apng_ref.render(path, fmt)
                expected = [canvas for canvas, _ in rendered]
            else:
                expected = region_frames(info, fmt)
            n_exp = len(info['frames'])

        for i, exp in enumerate(expected):
            dump_path = os.path.join(dumpdir, f"{base}_{tag}_f{i}.bin")
            if not os.path.exists(dump_path):
                print(f"FAIL {base} f{i}: missing dump {dump_path}")
                rc = 1
                continue
            got = open(dump_path, 'rb').read()
            if fmt == 'RGBA16':
                got = host_swap16(got)
            if len(got) != len(exp):
                print(f"FAIL {base} f{i}: size {len(got)} != {len(exp)}")
                rc = 1
                continue
            if got != exp:
                diffs = [j for j in range(len(exp)) if got[j] != exp[j]]
                first = diffs[0]
                print(f"FAIL {base} f{i}: {len(diffs)} byte diffs, "
                      f"first at {first} (got {got[first]:#x} exp {exp[first]:#x})")
                rc = 1
                continue
            nframes_checked += 1

        # make sure the C side did not emit extra frames
        extra = i = n_exp
        while os.path.exists(os.path.join(dumpdir, f"{base}_{tag}_f{i}.bin")):
            print(f"FAIL {base}: extra dump f{i}")
            rc = 1
            i += 1

    if rc == 0:
        print(f"OK: {nframes_checked} frames match across {len(files)} files "
              f"({mode}, {fmt_name})")
    return rc


if __name__ == '__main__':
    sys.exit(main())
