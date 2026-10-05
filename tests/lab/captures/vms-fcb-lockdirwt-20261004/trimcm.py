#!/usr/bin/env python3
"""trimcm.py <in.pcap> <out.pcap> <cat-hex>[,<cat-hex>..] [op-hex] -- keep only VMS$VAXcluster
message bodies (abs 30 msgtype 0x4b/0x5b/0x7b, SCA ctl type 10) whose category byte (abs 80) is
listed, optionally one opcode (abs 81). Everything else (HELLOs, VC control, MSCP) is dropped."""
import sys, struct
inp, outp = sys.argv[1], sys.argv[2]
cats = {int(c, 16) for c in sys.argv[3].split(',')}
op = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
d = open(inp, 'rb').read(); off = 24; keep = 0
with open(outp, 'wb') as f:
    f.write(d[:24])
    while off + 16 <= len(d):
        h = d[off:off+16]; incl = struct.unpack('<I', h[8:12])[0]; p = d[off+16:off+16+incl]; off += 16 + incl
        if len(p) < 92 or p[12:14] != b'\x60\x07' or p[30] not in (0x4b, 0x5b, 0x7b):
            continue
        if struct.unpack('<H', p[60:62])[0] != 10 or p[80] not in cats:
            continue
        if op is not None and p[81] != op:
            continue
        f.write(h); f.write(p); keep += 1
print(outp, keep)
