#!/usr/bin/env python3
# mkspec.py <pcap> <src-sysid> <cat> <op> <smsg> <loc-conid-hex|any> <name> <out.spec> <note...>
# Emits a manifest-hashed specimen for ONE real frame of a trimmed capture.
import hashlib, struct, sys
sys.path.insert(0, '/tmp/eb3/docs/clean-room/tools')
from pcap import frames
pc, src, cat, op, smsg, loc, name, out = sys.argv[1:9]
note = sys.argv[9:]
src, cat, op, smsg = int(src), int(cat, 16), int(op, 16), int(smsg)
hits = []
for i, (t, p) in enumerate(frames(pc)):
    if len(p) < 82 or p[12:14] != b'\x60\x07' or p[30] not in (0x4b, 0x5b):
        continue
    if struct.unpack('<H', p[60:62])[0] != 10 or (p[28] | p[29] << 8) != src:
        continue
    if p[80] != cat or p[81] != op or (p[72] | p[73] << 8) != smsg:
        continue
    if loc != 'any' and struct.unpack('<I', p[68:72])[0] != int(loc, 16):
        continue
    hits.append((i, p))
assert len(hits) == 1, (name, len(hits))
i, p = hits[0]
b = bytes(p)
with open(out, 'w') as f:
    f.write('%OVMX-CLUSTER-SPECIMEN-1\n')
    f.write('name:      %s\nclass:     scs-msg\norigin:    capture\n' % name)
    f.write('capture:   %s\n' % pc.split('/')[-1])
    f.write('spec:      docs/cluster-protocol-spec.md 4(j)/4(p) + rd vms-eb3 oracle F5/F6\n')
    f.write('wire-len:  %d\nsha256:    %s\n%%bytes\n' % (len(b), hashlib.sha256(b).hexdigest()))
    f.write('; REAL captured frame %d of tests/lab/captures/vms-eb3-joiner-freeze-20260930/%s\n' % (i, pc.split('/')[-1]))
    for n in note:
        f.write('; %s\n' % n)
    for o in range(0, len(b), 16):
        f.write('@%-4d %s\n' % (o, ' '.join('%02x' % x for x in b[o:o+16])))
print(name, 'frame', i)
