#!/usr/bin/env python3
"""Trim the vms-3ac crash capture BY RULE: keep every SCS-MSG frame the OVMX
node was party to (its MAC as source or destination), plus every frame naming
RA7T whoever sent it. Payloads unaltered. usage: trimrule.py <in> <out> <mac>"""
import struct, sys
MAC = bytes.fromhex(sys.argv[3].replace(':', ''))
data = open(sys.argv[1], 'rb').read()
def frames(d):
    off = 24
    while off + 16 <= len(d):
        h = d[off:off+16]; _t,_u,cl,_o = struct.unpack('<IIII', h)
        f = d[off+16:off+16+cl]; off += 16 + cl
        yield h, f
def scs(f):
    return len(f) == 204 and f[12:14] == b'\x60\x07' and f[31] == 0x13 and f[55] == 0x02
out = bytearray(data[:24]); kept = total = 0
for h, f in frames(data):
    total += 1
    if not scs(f):
        continue
    if f[0:6] == MAC or f[6:12] == MAC or bytes(f[72+48:72+52]) == b'RA7T':
        out += h + f; kept += 1
open(sys.argv[2], 'wb').write(bytes(out))
print("kept %d of %d frames" % (kept, total))
