#!/usr/bin/env python3
"""List every CTERM Start Read (host -> terminal) in a filter-dump pcap: flags,
max length, end-of-data, timeout, prompt text. usage: cterm_reads.py <pcap>   (env MASK=pw1,pw2: replaced by XX)
Also lists Read Data (3), Out-of-Band (4), Characteristics (11), VMS 15."""
import os, struct, sys
MASK = [m.encode() for m in os.environ.get("MASK", "").split(",") if m]

def mask(b):
    for m in MASK:
        b = b.replace(m, b"XX")
    return b
d = open(sys.argv[1], 'rb').read(); off = 24; t0 = None
while off + 16 <= len(d):
    ts, us, incl, _ = struct.unpack('<IIII', d[off:off + 16]); f = d[off + 16:off + 16 + incl]; off += 16 + incl
    if f[12:14] != b'\x60\x03': continue
    t = ts + us / 1e6; t0 = t0 or t
    dl = f[14] | f[15] << 8; p = f[16:16 + dl]
    if p and p[0] & 0x80: p = p[p[0] & 0x7f:]
    if not p or p[0] & 7 != 6: continue
    nsp = p[21:]
    if not nsp or nsp[0] not in (0x00, 0x20, 0x40, 0x60): continue
    q = nsp[5:]
    if len(q) >= 2 and q[1] & 0x80: q = q[2:]          # ackdat
    if len(q) >= 2 and q[1] & 0x80: q = q[2:]          # ackoth
    q = q[2:]                                          # segnum
    if q[:2] != b'\x09\x00': continue
    j = 2
    while j + 2 <= len(q):
        L = q[j] | q[j + 1] << 8; m = mask(q[j + 2:j + 2 + L]); j += 2 + L
        if not m: continue
        if m[0] == 2 and len(m) >= 17:
            fl = m[1] | m[2] << 8 | m[3] << 16
            mx, eod, to, eop, sod, lw = struct.unpack('<6H', m[4:16])
            print(f'{t - t0:9.3f} START-READ flags {fl:#08x} max {mx} eod {eod} timeout {to} eop {eop} sod {sod} lw {lw} data {m[16:]!r}')
        elif m[0] in (3, 4, 8, 11, 12, 13, 14, 15):
            print(f'{t - t0:9.3f} type {m[0]:2d} {m.hex(" ")[:90]}')
