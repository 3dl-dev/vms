#!/usr/bin/env python3
"""vc_census.py <pcap> <t0> [lo hi] -- the SCA phase-2 circuit dialogue.

CLEAN-ROOM: every offset cited is one already grounded in
docs/cluster-protocol-spec.md from observation of our own SIMH OpenVMS VAX
reference cluster and public OpenVMS documentation --
  abs 30   SCA message type (0x41 START/STACK/ACK, 0xa0 HELLO, 0xb1 last gasp)
  abs 58   config round (0 START, >=1 STACK; the round-2 ACK is 46 bytes)
  abs 60   SCSSYSTEMID of the sender, LE u16
"""
import sys, struct
def frames(path):
    d = open(path, "rb").read()
    endian = "<" if d[:4] in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
    nano = d[:4] in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
    off = 24
    while off + 16 <= len(d):
        ts, tus, caplen, _ = struct.unpack(endian + "IIII", d[off:off+16])
        off += 16
        yield ts + tus / (1e9 if nano else 1e6), d[off:off+caplen]
        off += caplen
p, t0 = sys.argv[1], float(sys.argv[2])
lo, hi = (float(sys.argv[3]), float(sys.argv[4])) if len(sys.argv) > 4 else (-1e18, 1e18)
for ts, f in frames(p):
    t = ts - t0
    if not (lo <= t <= hi) or len(f) < 62 or f[12:14] != b"\x60\x07":
        continue
    c = f[14:]
    mt = c[30 - 14] if len(c) > 16 else 0
    if mt != 0x41:
        continue
    rnd = c[58 - 14] if len(c) > 44 else 0
    sysid = struct.unpack("<H", c[60-14:62-14])[0] if len(c) > 48 else 0
    kind = {0: "START", 1: "STACK"}.get(rnd, "ACK/rnd%d" % rnd)
    if len(c) == 46:
        kind = "ACK"
    print("t=%9.3f  %s -> %s  0x41 %-6s  round=%d sysid=0x%04x clen=%d" % (
        t, ":".join("%02x" % b for b in f[6:12]),
        ":".join("%02x" % b for b in f[0:6]), kind, rnd, sysid, len(c)))
