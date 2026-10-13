#!/usr/bin/env python3
# trimpcap.py <in> <out> <rel_start> <rel_end> [<rel_start> <rel_end> ...]
# Keep only the packets inside the named windows, relative to the first packet.
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
wins = [(float(sys.argv[i]), float(sys.argv[i+1])) for i in range(3, len(sys.argv), 2)]
d = open(src, 'rb').read()
out = bytearray(d[:24]); o = 24; first = None; n = 0
while o + 16 <= len(d):
    hdr = d[o:o+16]; ts, tu, cl, _ = struct.unpack('<IIII', hdr); o += 16
    rec = d[o:o+cl]; o += cl
    t = ts + tu / 1e6
    if first is None: first = t
    r = t - first
    if any(a <= r <= b for a, b in wins):
        out += hdr + rec; n += 1
open(dst, 'wb').write(bytes(out))
print('kept', n, 'packets ->', dst)
