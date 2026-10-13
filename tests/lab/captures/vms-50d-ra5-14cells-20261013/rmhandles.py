#!/usr/bin/env python3
# rmhandles.py <pcap> <name> -- every remaster frame (02/0e..14 and the 82/15 answers) for
# the named tree: sender->recipient, body[12:20], [20:24], [24:28], [28:40], and the distinct
# 32-bit handles at [20:24]/[24:28], to look up with SDA SHOW LOCK on each node.
import struct, sys
P, NAME = sys.argv[1], sys.argv[2].encode()
d = open(P, 'rb').read(); o = 24; first = None; ids = []
while o + 16 <= len(d):
    ts, tu, cl, _ = struct.unpack('<IIII', d[o:o+16]); o += 16
    f = d[o:o+cl]; o += cl
    t = ts + tu / 1e6
    if first is None: first = t
    if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13 or f[55] != 2: continue
    b = f[72:]
    if not ((b[8] == 2 and 0x0e <= b[9] <= 0x14) or (b[8] == 0x82 and b[9] in (0x0e, 0x0f, 0x10, 0x11, 0x13, 0x14, 0x15))):
        continue
    if NAME not in b[48:48 + b[47]] and not (b[9] in (0x12, 0x13) and NAME in b): continue
    h1 = struct.unpack('<I', b[20:24])[0]; h2 = struct.unpack('<I', b[24:28])[0]
    print('%9.4f %s>%s %02x/%02x [12:20]%s [20:24]%08X [24:28]%08X [28:40]%s name=%r'
          % (t - first, f[6:12].hex()[-4:], f[0:6].hex()[-4:], b[8], b[9], b[12:20].hex(),
             h1, h2, b[28:40].hex(), b[48:48 + b[47]]))
    for h in (h1, h2):
        if h and h not in ids: ids.append(h)
print('IDS', ' '.join('%08X' % h for h in ids))
