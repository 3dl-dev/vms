#!/usr/bin/env python3
"""trimcm.py <in.pcap> <out.pcap> [cats] -- rd vms-52d: keep only VMS$VAXcluster connection-manager
messages (tools/cluster/cm_dialogue_audit.is_cm), optionally only the given body[8] categories
(comma-separated hex, the 0x80 response bit ignored). Frame bytes and timestamps are kept as-is."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../tools/cluster'))
import cm_dialogue_audit as A  # noqa: E402

inp, outp = sys.argv[1], sys.argv[2]
cats = {int(c, 16) for c in sys.argv[3].split(',')} if len(sys.argv) > 3 else None
d = open(inp, 'rb').read(); off = 24; keep = 0
with open(outp, 'wb') as f:
    f.write(d[:24])
    while off + 16 <= len(d):
        h = d[off:off + 16]; incl = struct.unpack('<I', h[8:12])[0]
        p = d[off + 16:off + 16 + incl]; off += 16 + incl
        if not A.is_cm(p):
            continue
        if cats is not None and (p[A.OFF_CM_BODY + 8] & 0x7f) not in cats:
            continue
        f.write(h); f.write(p); keep += 1
print(outp, keep)
