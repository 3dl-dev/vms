#!/usr/bin/env python3
"""mkspec2.py <pcap> <src> <dst> <cat-hex> <op-hex> <step|any> <name> <out.spec> <note...> -- as mkspec.py, keyed
on the category byte too (0x81 responses) and, for op 0x0b, on the step at body[16]."""
import hashlib, struct, sys
pc, src, dst, cat, op, step, name, out = sys.argv[1:9]; note = sys.argv[9:]
cat, op = int(cat, 16), int(op, 16)
d = open(pc, 'rb').read(); off = 24; hits = []; i = 0
while off + 16 <= len(d):
    incl = struct.unpack('<I', d[off+8:off+12])[0]; p = d[off+16:off+16+incl]; off += 16 + incl
    if p[6:12].hex()[-4:] == src and p[0:6].hex()[-4:] == dst and p[80] == cat and p[81] == op \
       and (step == 'any' or p[88] == int(step)):
        hits.append((i, p))
    i += 1
assert hits, name
i, p = hits[0]; b = bytes(p)
with open(out, 'w') as f:
    f.write('%OVMX-CLUSTER-SPECIMEN-1\n')
    f.write('name:      %s\nclass:     scs-msg\norigin:    capture\n' % name)
    f.write('capture:   %s\n' % pc.split('/')[-1])
    f.write('spec:      docs/cluster-protocol-spec.md 4(p) barrier table; rd vms-f297\n')
    f.write('wire-len:  %d\nsha256:    %s\n%%bytes\n' % (len(b), hashlib.sha256(b).hexdigest()))
    f.write('; REAL captured frame %d of tests/lab/captures/vms-f297-open-cells-20261008/%s\n' % (i, pc.split('/')[-1]))
    for n in note: f.write('; %s\n' % n)
    for o in range(0, len(b), 16):
        f.write('@%-4d %s\n' % (o, ' '.join('%02x' % x for x in b[o:o+16])))
print(name, 'frame', i, 'of', len(hits))
