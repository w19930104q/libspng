#!/usr/bin/env python3
"""Inspect APNG structure: IHDR, acTL, fcTL per frame, chunk layout."""
import struct, sys, os

def parse(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    off = 8
    chunks = []
    while off < len(d):
        ln = struct.unpack('>I', d[off:off+4])[0]
        typ = d[off+4:off+8]
        body = d[off+8:off+8+ln]
        chunks.append((typ, body))
        off += 12 + ln
    ihdr = chunks[0][1]
    w, h, bd, ct, comp, filt, inter = struct.unpack('>IIBBBBB', ihdr)
    actl = None
    frames = []
    seq_order = []
    cur = None
    dat_kind = None
    for typ, body in chunks[1:]:
        if typ == b'acTL':
            actl = struct.unpack('>II', body)
        elif typ == b'fcTL':
            seq, fw, fh, fx, fy, dnum, dden, disp, blend = struct.unpack('>IIIIIHHBB', body)
            seq_order.append(('fcTL', seq))
            cur = dict(seq=seq, w=fw, h=fh, x=fx, y=fy, dnum=dnum, dden=dden,
                       disp=disp, blend=blend, dat=[])
            frames.append(cur)
        elif typ == b'IDAT':
            if cur is not None:
                cur['dat'].append('IDAT')
        elif typ == b'fdAT':
            seq = struct.unpack('>I', body[:4])[0]
            seq_order.append(('fdAT', seq))
            if cur is not None:
                cur['dat'].append('fdAT')
    return dict(path=path, w=w, h=h, bd=bd, ct=ct, inter=inter, actl=actl,
                frames=frames, seq_order=seq_order,
                chunk_types=[t for t, _ in chunks])

CT = {0:'gray', 2:'rgb', 3:'pal', 4:'ga', 6:'rgba'}
DISP = {0:'NONE', 1:'BGND', 2:'PREV'}
BLEND = {0:'SRC', 1:'OVER'}

if __name__ == '__main__':
    for p in sorted(sys.argv[1:]):
        info = parse(p)
        a = info['actl']
        print(f"{os.path.basename(p)} {info['w']}x{info['h']} {CT[info['ct']]}{info['bd']} "
              f"inter={info['inter']} actl={a} nframes={len(info['frames'])}")
        for i, f in enumerate(info['frames']):
            print(f"   f{i}: {f['w']}x{f['h']}+{f['x']}+{f['y']} "
                  f"delay={f['dnum']}/{f['dden']} {DISP[f['disp']]:4s} {BLEND[f['blend']]:4s} "
                  f"dat={f['dat'][0] if f['dat'] else '?'}")
