#!/usr/bin/env python3
"""trimops.py <in.pcap> <out.pcap> -- rd vms-f297: keep only the VMS$VAXcluster cat-0x01 messages
this grounding reads (op 0x01 PARAMS, 0x02 request, 0x05 membership record, 0x07/0x08/0x09
transition opens, 0x14 model). Everything else is dropped."""
import sys, struct
KEEP = {0x01, 0x02, 0x05, 0x07, 0x08, 0x09, 0x14}
inp, outp = sys.argv[1], sys.argv[2]
d = open(inp, 'rb').read(); off = 24; keep = 0
with open(outp, 'wb') as f:
    f.write(d[:24])
    while off + 16 <= len(d):
        h = d[off:off+16]; incl = struct.unpack('<I', h[8:12])[0]; p = d[off+16:off+16+incl]; off += 16 + incl
        if len(p) < 92 or p[12:14] != b'\x60\x07' or p[30] not in (0x4b, 0x5b, 0x7b):
            continue
        if struct.unpack('<H', p[60:62])[0] != 10 or p[80] != 0x01 or p[81] not in KEEP:
            continue
        f.write(h); f.write(p); keep += 1
print(outp, keep)
