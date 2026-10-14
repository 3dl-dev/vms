#!/usr/bin/env python3
# cmparams.py <pcap>... -- every cat-0x01 op-0x01 PARAMS from a MEMBER
# (body[18:20] != 0), split by the sender's version string at body[88:96]
# (a real VAX writes "V7.3", OVMX writes its own "...VMX V0.7...").
import struct, sys, collections
vax, ovmx = [], []
for path in sys.argv[1:]:
    try: d = open(path, 'rb').read()
    except Exception: continue
    o = 24
    while o + 16 <= len(d):
        ts, tu, cl, ol = struct.unpack('<IIII', d[o:o+16]); o += 16
        f = d[o:o+cl]; o += cl
        if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13: continue
        b = bytes(f[72:])
        if len(b) < 132 or b[8] != 1 or b[9] != 1: continue
        if (b[18] | (b[19] << 8)) == 0: continue
        ver = b[88:96]
        (ovmx if b'MX' in ver or b'V0.' in ver else vax).append((path, b))
def census(name, fr):
    print('=== %s: %d member PARAMS' % (name, len(fr)))
    if not fr: return
    for i in range(0, 132):
        vals = collections.Counter(x[1][i] for x in fr)
        if len(vals) == 1 and list(vals)[0] == 0: continue
        print('  [%3d] %s' % (i, ' '.join('%02x:%d' % (v, c) for v, c in vals.most_common(5))))
census('REAL VAX', vax)
print()
census('OVMX', ovmx)
if ovmx:
    print('\n-- one OVMX member PARAMS, in full (%s):' % ovmx[0][0])
    b = ovmx[0][1]
    for i in range(0, 132, 16):
        print('   [%3d] %s' % (i, ' '.join('%02x' % c for c in b[i:i+16])))
if vax:
    print('\n-- one REAL VAX member PARAMS, in full (%s):' % vax[0][0])
    b = vax[0][1]
    for i in range(0, 132, 16):
        print('   [%3d] %s' % (i, ' '.join('%02x' % c for c in b[i:i+16])))
