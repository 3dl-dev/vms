#!/usr/bin/env python3
"""vc_fields.py <pcap> <t0> <lo> <hi> -- the identity fields of every 0x41
START/STACK/ACK in a window.

CLEAN-ROOM: offsets from docs/cluster-protocol-spec.md sec 4(g) phase 2 --
abs 30 msgtype, 32 recv_ack, 34 send_seq, 36 incarnation echo (4(i).B),
58 config round, 60 SCSSYSTEMID, 80 this system incarnation (u64),
104 node name.
"""
import sys, struct
def frames(path):
    d = open(path, "rb").read()
    e = "<"
    nano = d[:4] in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
    off = 24
    while off + 16 <= len(d):
        ts, tus, caplen, _ = struct.unpack(e + "IIII", d[off:off+16]); off += 16
        yield ts + tus / (1e9 if nano else 1e6), d[off:off+caplen]; off += caplen
p, t0, lo, hi = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
u16 = lambda c, a: struct.unpack("<H", c[a-14:a-12])[0]
for ts, f in frames(p):
    t = ts - t0
    if not (lo <= t <= hi) or len(f) < 60 or f[12:14] != b"\x60\x07": continue
    c = f[14:]
    if len(c) < 46 or c[30-14] != 0x41: continue
    rnd = u16(c, 58) if len(c) >= 60 else -1
    kind = "ACK" if len(c) == 46 else ("START" if rnd == 0 else "STACK")
    inc = struct.unpack("<Q", c[80-14:88-14])[0] if len(c) >= 88 else 0
    nm = bytes(c[104-14:112-14]).decode("latin1").strip() if len(c) >= 112 else ""
    print("t=%9.3f %-5s %-6s sys=0x%04x recv_ack=%5d send_seq=%5d echo=%5d rnd=%d inc=0x%016x %s"
          % (t, ":".join("%02x" % b for b in f[9:12]), kind,
             u16(c, 60) if len(c) >= 62 else 0, u16(c, 32), u16(c, 34), u16(c, 36),
             rnd, inc, nm))
