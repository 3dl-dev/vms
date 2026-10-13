#!/usr/bin/env python3
"""trimlnm.py <in.pcap> <out.pcap> -- rd vms-94a: keep the clusterwide logical-name traffic: every
VMS$VAXcluster message of category 0x0b/0x8b, and every SCA block-transfer frame (REQUEST DATA 0x08,
DATA 0x0d, SEND DATA 0x09, data-received 0x05) between the same two nodes while a 0x0b request is
outstanding. Frame bytes and timestamps are kept as they were captured."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../tools/cluster'))
import cm_dialogue_audit as A  # noqa: E402

inp, outp = sys.argv[1], sys.argv[2]
d = open(inp, 'rb').read(); off = 24; keep = 0; open_pairs = set()
with open(outp, 'wb') as f:
    f.write(d[:24])
    while off + 16 <= len(d):
        h = d[off:off + 16]; incl = struct.unpack('<I', h[8:12])[0]
        p = d[off + 16:off + 16 + incl]; off += 16 + incl
        if not A.is_sca(p) or len(p) < 56:
            continue
        s, t = p[6:12], p[0:6]
        mt = p[55]
        k = False
        if mt == 0x02 and A.is_cm(p) and (p[80] & 0x7f) == 0x0b:
            k = True
            if p[80] == 0x0b:
                open_pairs.add(frozenset((s, t)))
            else:
                open_pairs.discard(frozenset((s, t)))
        elif mt in (0x05, 0x08, 0x09, 0x0d) and frozenset((s, t)) in open_pairs:
            k = True
        if k:
            f.write(h); f.write(p); keep += 1
print(outp, keep)
