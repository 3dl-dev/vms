#!/usr/bin/env python3
"""mkspec.py <pcap> <src-mac-tail> <dst-mac-tail> <op-hex> <epoch|any> <name> <out.spec> <note...>
rd vms-f297: emit a manifest-hashed specimen for ONE real cat-0x01 frame of a trimmed capture,
selected by sender/recipient MAC tail (last two bytes, hex), opcode and (for opens) epoch."""
import hashlib, struct, sys
pc, src, dst, op, ep, name, out = sys.argv[1:8]
note = sys.argv[8:]
op = int(op, 16)
d = open(pc, 'rb').read(); off = 24; hits = []; i = 0
while off + 16 <= len(d):
    incl = struct.unpack('<I', d[off+8:off+12])[0]; p = d[off+16:off+16+incl]; off += 16 + incl
    if p[6:12].hex()[-4:] == src and p[0:6].hex()[-4:] == dst and p[80] == 0x01 and p[81] == op \
       and (ep == 'any' or p[84] == int(ep)):
        hits.append((i, p))
    i += 1
assert len(hits) >= 1, (name, len(hits))
i, p = hits[0]
b = bytes(p)
with open(out, 'w') as f:
    f.write('%OVMX-CLUSTER-SPECIMEN-1\n')
    f.write('name:      %s\nclass:     scs-msg\norigin:    capture\n' % name)
    f.write('capture:   %s\n' % pc.split('/')[-1])
    f.write('spec:      Davis p. 7-40 Phase 1 contents; rd vms-f297 one-variable oracle\n')
    f.write('wire-len:  %d\nsha256:    %s\n%%bytes\n' % (len(b), hashlib.sha256(b).hexdigest()))
    f.write('; REAL captured frame %d of tests/lab/captures/vms-f297-open-cells-20261008/%s\n' % (i, pc.split('/')[-1]))
    for n in note:
        f.write('; %s\n' % n)
    for o in range(0, len(b), 16):
        f.write('@%-4d %s\n' % (o, ' '.join('%02x' % x for x in b[o:o+16])))
print(name, 'frame', i, 'of', len(hits))
