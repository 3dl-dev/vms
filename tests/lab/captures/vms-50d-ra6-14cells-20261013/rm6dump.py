#!/usr/bin/env python3
# rm6dump.py <pcap> <name> -- the FULL 132-byte cat-0x02 body of every remaster
# frame for one tree, so no cell is read through a summary. rd vms-50d RA6.
import struct, sys, datetime
P, NAME = sys.argv[1], sys.argv[2].encode()
d = open(P, 'rb').read(); o = 24; first = None
while o + 16 <= len(d):
    ts, tu, cl, _ = struct.unpack('<IIII', d[o:o+16]); o += 16
    f = d[o:o+cl]; o += cl
    t = ts + tu / 1e6
    if first is None: first = t
    if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13 or f[55] != 2: continue
    b = f[72:]
    if not ((b[8] == 2 and 0x0e <= b[9] <= 0x14) or
            (b[8] == 0x82 and b[9] in (0x0e, 0x0f, 0x10, 0x11, 0x13, 0x14, 0x15))):
        continue
    if NAME not in b[48:48 + b[47]] and not (b[9] in (0x12, 0x13) and NAME in b): continue
    print('%s %9.4f %s>%s %02x/%02x' % (
        datetime.datetime.fromtimestamp(t, datetime.UTC).strftime('%H:%M:%S.%f'),
        t - first, f[6:12].hex()[-4:], f[0:6].hex()[-4:], b[8], b[9]))
    for i in range(0, 132, 16):
        print('   [%3d] %s' % (i, ' '.join('%02x' % c for c in b[i:i+16])))
