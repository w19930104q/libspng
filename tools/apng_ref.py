#!/usr/bin/env python3
"""Independent APNG reference renderer (pure python, zlib only).

Decodes every frame's IDAT/fdAT datastream and composites onto the canvas
following the APNG 1.0 spec (dispose_op / blend_op). Used to cross-check
the C implementation in spng.c without relying on another decoder.
"""
import struct, zlib, sys, os

PAETH_BPP = None

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c

def defilter(raw, width, height, bpp, rowbytes):
    """raw: concatenated filtered scanlines (filter byte + rowbytes data).
    Each filtered scanline is rowbytes+1 bytes."""
    stride = rowbytes + 1
    out = bytearray(rowbytes * height)
    prev = bytearray(rowbytes)
    pos = 0
    for y in range(height):
        f = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + rowbytes])
        pos += stride
        if f == 0:
            pass
        elif f == 1:  # sub
            for i in range(bpp, rowbytes):
                line[i] = (line[i] + line[i - bpp]) & 0xff
        elif f == 2:  # up
            for i in range(rowbytes):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:  # average
            for i in range(rowbytes):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + (a + prev[i]) // 2) & 0xff
        elif f == 4:  # paeth
            for i in range(rowbytes):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 0xff
        else:
            raise ValueError(f"bad filter {f}")
        out[y * rowbytes:(y + 1) * rowbytes] = line
        prev = line
    return bytes(out)

def read_be16(b, o):
    return (b[o] << 8) | b[o + 1]

def iter_samples(data, bit_depth, n):
    """Unpack n packed samples of bit_depth (<8) from one row's bytes."""
    res = []
    mask = (1 << bit_depth) - 1
    shift = 8 - bit_depth
    for i in range(n):
        res.append((data[i * bit_depth // 8] >> (shift - (i * bit_depth % 8))) & mask)
    return res

class Frame:
    pass

def parse_apng(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError("not png")
    off = 8
    chunks = []
    while off < len(d):
        ln = struct.unpack('>I', d[off:off + 4])[0]
        typ = d[off + 4:off + 8]
        body = d[off + 8:off + 8 + ln]
        crc = d[off + 8 + ln:off + 12 + ln]
        chunks.append((typ, body))
        off += 12 + ln
    ihdr = chunks[0][1]
    w, h, bd, ct, comp, filt, inter = struct.unpack('>IIBBBBB', ihdr)
    assert comp == 0 and filt == 0
    plte = None
    trns = None
    actl = None
    frames = []
    cur = None
    default_idat = bytearray()
    for typ, body in chunks[1:]:
        if typ == b'PLTE':
            plte = [(body[i * 3], body[i * 3 + 1], body[i * 3 + 2])
                    for i in range(len(body) // 3)]
        elif typ == b'tRNS':
            trns = body
        elif typ == b'acTL':
            actl = struct.unpack('>II', body)
        elif typ == b'fcTL':
            seq, fw, fh, fx, fy, dnum, dden, disp, blend = struct.unpack('>IIIIIHHBB', body)
            cur = Frame()
            cur.seq, cur.w, cur.h, cur.x, cur.y = seq, fw, fh, fx, fy
            cur.delay = (dnum, dden)
            cur.disp, cur.blend = disp, blend
            cur.data = bytearray()
            cur.kind = None
            frames.append(cur)
        elif typ == b'IDAT':
            default_idat += body
            if cur is not None and cur.kind is None:
                cur.kind = 'IDAT'
                cur.data += body
        elif typ == b'fdAT':
            if cur is None:
                continue
            cur.kind = 'fdAT'
            cur.data += body[4:]  # strip sequence number
    return dict(w=w, h=h, bd=bd, ct=ct, inter=inter, plte=plte, trns=trns,
                actl=actl, frames=frames, default_idat=bytes(default_idat))

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}

def decode_frame_pixels(info, frame):
    """Decode one frame to raw PNG-format rows (host order for 16-bit)."""
    bd, ct = info['bd'], info['ct']
    w, h = frame.w, frame.h
    nch = CHANNELS[ct]
    if bd < 8:
        rowbytes = (w * bd + 7) // 8
    else:
        rowbytes = w * nch * (bd // 8)
    bpp = max(1, nch * bd // 8)  # filter bpp, rounded up
    stride = rowbytes + 1
    comp = frame.data
    raw = zlib.decompress(comp)
    if len(raw) != stride * h:
        raise ValueError(f"decompressed size {len(raw)} != {stride * h}")
    flat = defilter(raw, w, h, bpp, rowbytes)
    rows = [flat[y * rowbytes:(y + 1) * rowbytes] for y in range(h)]
    return rows

# ---- conversion to target formats ----

def to_rgba8(info, frame, rows):
    bd, ct, plte, trns = info['bd'], info['ct'], info['plte'], info['trns']
    w = frame.w
    out = bytearray()
    for row in rows:
        if ct == 6:  # RGBA
            if bd == 8:
                out += row
            else:
                for i in range(w):
                    out += bytes(row[i * 8 + k * 2] for k in range(4))
        elif ct == 2:  # RGB
            if bd == 8:
                for i in range(w):
                    r, g, b = row[i * 3:i * 3 + 3]
                    a = 0 if (trns is not None and
                              r == trns[0] and g == trns[1] and b == trns[2]) else 255
                    out += bytes((r, g, b, a))
            else:
                for i in range(w):
                    r16 = read_be16(row, i * 6)
                    g16 = read_be16(row, i * 6 + 2)
                    b16 = read_be16(row, i * 6 + 4)
                    r, g, b = r16 >> 8, g16 >> 8, b16 >> 8
                    t = struct.unpack('>HHH', trns) if trns else None
                    a = 0 if (t and (r16, g16, b16) == t) else 255
                    out += bytes((r, g, b, a))
        elif ct == 4:  # GA
            if bd == 8:
                for i in range(w):
                    g, a = row[i * 2:i * 2 + 2]
                    out += bytes((g, g, g, a))
            else:
                for i in range(w):
                    g16 = read_be16(row, i * 4)
                    a16 = read_be16(row, i * 4 + 2)
                    g = g16 >> 8
                    out += bytes((g, g, g, a16 >> 8))
        elif ct == 0:  # gray
            if bd == 16:
                for i in range(w):
                    g16 = read_be16(row, i * 2)
                    g = g16 >> 8
                    t16 = struct.unpack('>H', trns)[0] if trns else None
                    a = 0 if (t16 is not None and g16 == t16) else 255
                    out += bytes((g, g, g, a))
            elif bd == 8:
                for g in row:
                    a = 0 if (trns and g == trns[0]) else 255
                    out += bytes((g, g, g, a))
            else:
                samples = iter_samples(row, bd, w)
                for s in samples:
                    g = (s * 255) // ((1 << bd) - 1)
                    a = 0 if (trns and s == trns[0]) else 255
                    out += bytes((g, g, g, a))
        elif ct == 3:  # palette
            if bd == 8:
                idxs = list(row[:w])
            else:
                idxs = iter_samples(row, bd, w)
            for idx in idxs:
                r, g, b = plte[idx]
                a = trns[idx] if (trns and idx < len(trns)) else 255
                out += bytes((r, g, b, a))
    return bytes(out)

def to_rgba16(info, frame, rows):
    bd, ct, plte, trns = info['bd'], info['ct'], info['plte'], info['trns']
    w = frame.w
    out = bytearray()
    def put(r, g, b, a):
        out.extend(struct.pack('>HHHH', r, g, b, a))
    for row in rows:
        if ct == 6 and bd == 16:
            for i in range(w):
                out += row[i * 8:i * 8 + 8]
        elif ct == 2 and bd == 16:
            t = struct.unpack('>HHH', trns) if trns else None
            for i in range(w):
                r, g, b = (read_be16(row, i * 6), read_be16(row, i * 6 + 2),
                           read_be16(row, i * 6 + 4))
                a = 0 if (t and (r, g, b) == t) else 65535
                put(r, g, b, a)
        elif ct == 4 and bd == 16:
            for i in range(w):
                g = read_be16(row, i * 4)
                a = read_be16(row, i * 4 + 2)
                put(g, g, g, a)
        elif ct == 0 and bd == 16:
            t16 = struct.unpack('>H', trns)[0] if trns else None
            for i in range(w):
                g = read_be16(row, i * 2)
                a = 0 if (t16 is not None and g == t16) else 65535
                put(g, g, g, a)
        elif ct == 3:
            idxs = list(row[:w]) if bd == 8 else iter_samples(row, bd, w)
            for idx in idxs:
                r, g, b = plte[idx]
                a8 = trns[idx] if (trns and idx < len(trns)) else 255
                put(r * 257, g * 257, b * 257, a8 * 257)
        elif ct == 6 and bd == 8:
            for i in range(w):
                r, g, b, a = row[i * 4:i * 4 + 4]
                put(r * 257, g * 257, b * 257, a * 257)
        elif ct == 2 and bd == 8:
            for i in range(w):
                r, g, b = row[i * 3:i * 3 + 3]
                a = 0 if (trns is not None and
                          r == trns[0] and g == trns[1] and b == trns[2]) else 255
                put(r * 257, g * 257, b * 257, a * 257)
        elif ct == 4 and bd == 8:
            for i in range(w):
                g, a = row[i * 2:i * 2 + 2]
                put(g * 257, g * 257, g * 257, a * 257)
        elif ct == 0 and bd == 8:
            for g in row:
                a = 0 if (trns and g == trns[0]) else 255
                g16 = g * 257
                put(g16, g16, g16, a * 257)
        elif ct == 0 and bd < 8:
            for s in iter_samples(row, bd, w):
                a = 0 if (trns and s == trns[0]) else 255
                g8 = (s * 255) // ((1 << bd) - 1)
                g16 = g8 * 257
                put(g16, g16, g16, a * 257)
        else:
            raise ValueError(f"rgba16 from ct{ct} bd{bd} not expected in tests")
    return bytes(out)

def blend_chan(s, d, sa, mx):
    return (s * sa + d * (mx - sa) + mx // 2) // mx

def render(path, fmt='RGBA8'):
    info = parse_apng(path)
    W, H = info['w'], info['h']
    ps = 8 if fmt == 'RGBA16' else 4
    mx = 65535 if fmt == 'RGBA16' else 255
    conv = to_rgba16 if fmt == 'RGBA16' else to_rgba8
    canvas = bytearray(W * H * ps)
    prev_region = None
    results = []
    for frame in info['frames']:
        # dispose previous
        if results:
            pf = results[-1][1]
            if pf.disp == 1:  # background
                for y in range(pf.h):
                    base = ((pf.y + y) * W + pf.x) * ps
                    canvas[base:base + pf.w * ps] = b'\x00' * (pf.w * ps)
            elif pf.disp == 2:  # previous
                pr, ppf = prev_region
                for y in range(ppf.h):
                    dst = ((ppf.y + y) * W + ppf.x) * ps
                    src = y * ppf.w * ps
                    canvas[dst:dst + ppf.w * ps] = pr[src:src + ppf.w * ps]
        if frame.disp == 2:
            pr = bytearray(frame.w * frame.h * ps)
            for y in range(frame.h):
                dst = ((frame.y + y) * W + frame.x) * ps
                pr[y * frame.w * ps:(y + 1) * frame.w * ps] = canvas[dst:dst + frame.w * ps]
            prev_region = (pr, frame)
        rows = decode_frame_pixels(info, frame)
        region = conv(info, frame, rows)
        # APNG: blend_op only applies to color types with an alpha channel:
        # native alpha (4, 6) or tRNS-transparency (0, 2, 3). Without alpha
        # the source always replaces the destination (SOURCE and OVER match).
        have_alpha = info['ct'] in (4, 6) or (info['trns'] is not None)
        for y in range(frame.h):
            for x in range(frame.w):
                di = ((frame.y + y) * W + (frame.x + x)) * ps
                si = (y * frame.w + x) * ps
                if not have_alpha or frame.blend == 0:
                    canvas[di:di + ps] = region[si:si + ps]
                else:
                    if ps == 4:
                        sa = region[si + 3]
                        if sa == 0:
                            continue
                        da = canvas[di + 3]
                        if sa == 255:
                            canvas[di:di + 4] = region[si:si + 4]
                        else:
                            # source-over, including the output alpha channel:
                            # a_out = sa + da*(1-sa)
                            out_a = sa + ((da * (mx - sa) + mx // 2) // mx)
                            for c in range(3):
                                canvas[di + c] = blend_chan(
                                    region[si + c], canvas[di + c], sa, mx)
                            canvas[di + 3] = out_a
                    else:
                        sa = read_be16(region, si + 6)
                        if sa == 0:
                            continue
                        da = read_be16(canvas, di + 6)
                        if info['bd'] == 8:
                            # 8-bit source scaled to 16-bit: blend at 8-bit
                            # precision (APNG composites at source depth),
                            # then scale results back up by 257.
                            sa8, da8 = sa >> 8, da >> 8
                            if sa8 == 255:
                                canvas[di:di + 8] = region[si:si + 8]
                                continue
                            out_a8 = sa8 + (
                                (da8 * (255 - sa8) + 127) // 255)
                            for c in range(3):
                                s = read_be16(region, si + c * 2) >> 8
                                dv = read_be16(canvas, di + c * 2) >> 8
                                nv = blend_chan(s, dv, sa8, 255) * 257
                                canvas[di + c * 2] = nv >> 8
                                canvas[di + c * 2 + 1] = nv & 0xff
                            out_a = out_a8 * 257
                            canvas[di + 6] = out_a >> 8
                            canvas[di + 7] = out_a & 0xff
                            continue
                        if sa == 65535:
                            canvas[di:di + 8] = region[si:si + 8]
                        else:
                            out_a = sa + ((da * (mx - sa) + mx // 2) // mx)
                            for c in range(3):
                                s = read_be16(region, si + c * 2)
                                dv = read_be16(canvas, di + c * 2)
                                nv = blend_chan(s, dv, sa, mx)
                                canvas[di + c * 2] = nv >> 8
                                canvas[di + c * 2 + 1] = nv & 0xff
                            canvas[di + 6] = out_a >> 8
                            canvas[di + 7] = out_a & 0xff
        results.append((bytes(canvas), frame))
    return info, results

if __name__ == '__main__':
    for p in sys.argv[1:]:
        info, frames = render(p)
        print(os.path.basename(p), info['ct'], info['bd'],
              f"frames={len(frames)} actl={info['actl']}")
