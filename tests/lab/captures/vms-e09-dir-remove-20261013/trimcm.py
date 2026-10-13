#!/usr/bin/env python3
"""trimcm.py <in.pcap> <out.pcap> <macA> <macB> -- rd vms-94a/vms-cd8: keep every VMS$VAXcluster
connection-manager message between two stations, and print each one's send-msg#, ack-msg#,
category and opcode. Frame bytes and timestamps are kept as they were captured."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../tools/cluster'))
import cm_dialogue_audit as A  # noqa: E402

inp, outp = sys.argv[1], sys.argv[2]
pair = {bytes.fromhex(m.replace(':', '')) for m in sys.argv[3:5]}
d = open(inp, 'rb').read(); off = 24; n = 0; keep = 0
with open(outp, 'wb') as f:
    f.write(d[:24])
    while off + 16 <= len(d):
        h = d[off:off + 16]; incl = struct.unpack('<I', h[8:12])[0]
        p = d[off + 16:off + 16 + incl]; off += 16 + incl; n += 1
        if not A.is_sca(p) or len(p) < 82 or p[55] != 0x02 or not A.is_cm(p):
            continue
        if {p[6:12], p[0:6]} != pair:
            continue
        f.write(h); f.write(p); keep += 1
        send, ack = struct.unpack('<HH', p[72:76])
        print('%5d %s -> %s send=%-3d ack=%-3d cat=%02x op=%02x' % (
            n, A.mac(p[6:12]), A.mac(p[0:6]), send, ack, p[80], p[81]))
print(outp, keep, file=sys.stderr)
