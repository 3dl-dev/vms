#!/usr/bin/env python3
# mk08.py <outdir> <fixturedir> <tag>=<pcap>[:<context>] ... : for each source capture, write a
# trimmed pcap holding only its cat-0x01 op-0x08 frames (first copy of each), and one codec
# specimen (.spec) per frame, byte-exact, with its provenance and context in the comments.
import sys, struct, gzip, hashlib, os
out, fxdir = sys.argv[1], sys.argv[2]
def frames(path):
    op = gzip.open if path.endswith('.gz') else open
    f = op(path, 'rb'); gh = f.read(24)
    E = '<' if gh[:4] in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1') else '>'
    while True:
        h = f.read(16)
        if len(h) < 16: break
        s, us, cl, ol = struct.unpack(E+'IIII', h); d = f.read(cl)
        yield gh, h, s, us, d
def sid(b): return b[4] | (b[5] << 8)
B = 72
for arg in sys.argv[3:]:
    tag, rest = arg.split('=', 1)
    path, ctx = (rest.split(':', 1) + [''])[:2]
    seen = set(); keep = []; gh0 = None
    for gh, h, s, us, p in frames(path):
        gh0 = gh
        if len(p) < B+60 or p[12:14] != b'\x60\x07' or p[30] not in (0x4b, 0x5b, 0x7b): continue
        if struct.unpack('<H', p[60:62])[0] != 10 or p[B+8] != 1 or p[B+9] != 8: continue
        k = (sid(p[24:30]), sid(p[16:22]), p[B] | p[B+1] << 8)
        if k in seen: continue
        seen.add(k); keep.append((h, p, k))
    cap = 'af4-op08-%s.pcap' % tag
    with open(os.path.join(out, cap), 'wb') as o:
        o.write(gh0)
        for h, p, k in keep: o.write(h); o.write(p)
    for i, (h, p, k) in enumerate(keep):
        name = 'cm-open-remove-%s-%d' % (tag.lower(), i + 1)
        with open(os.path.join(fxdir, name + '.spec'), 'w') as fx:
            fx.write('%%OVMX-CLUSTER-SPECIMEN-1\nname:      %s\nclass:     scs-msg\norigin:    capture\n' % name)
            fx.write('capture:   %s\nspec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)\n' % cap)
            fx.write('wire-len:  %d\nsha256:    %s\n%%bytes\n' % (len(p), hashlib.sha256(p).hexdigest()))
            fx.write('; REAL captured frame %d of tests/lab/captures/vms-af4-op08-remove-20261001/%s\n' % (i + 1, cap))
            fx.write('; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID %d (coordinator) -> %d.\n' % (k[0], k[1]))
            fx.write('; %s\n' % ctx)
            for j in range(0, len(p), 16):
                fx.write('@%-4d %s\n' % (j, ' '.join('%02x' % x for x in p[j:j+16])))
        print(name, k, 'body[55]=%02x' % p[B+55])
