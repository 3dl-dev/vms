#!/usr/bin/env python3
"""timeline.py <pcap> <t0> <lo> <hi> <macA> <macB> -- every SCA frame between two
stations (plus their multicasts) in a window, one line each.

CLEAN-ROOM: offsets only from docs/cluster-protocol-spec.md --
  abs 30  message type (0x41 START/STACK/ACK, 0x4b/0x5b/0x7b sequenced,
          0x48 credit return, 0xa0 HELLO, 0xb2/0xb3/0xb4 directed discovery)
  abs 32  recv_ack, abs 34 send_seq (sequenced / 0x41)
  abs 36  the sec 4(i).B incarnation echo, on every class (sec 4(h)(4c))
  abs 92  payload [78:80] of a directed discovery frame: the incarnation the
          SENDER advertises for the frame's destination (sec 4(i).B)
"""
import struct
import sys


def frames(path):
    d = open(path, "rb").read()
    nano = d[:4] == b"\x4d\x3c\xb2\xa1"
    off = 24
    while off + 16 <= len(d):
        ts, tu, cl, _ = struct.unpack("<IIII", d[off:off + 16])
        off += 16
        yield ts + tu / (1e9 if nano else 1e6), d[off:off + cl]
        off += cl


def mac(raw):
    return ":".join("%02x" % x for x in raw)


def main():
    p, t0, lo, hi, a, b = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), \
        float(sys.argv[4]), sys.argv[5], sys.argv[6]
    for ts, f in frames(p):
        t = ts - t0
        if not lo <= t <= hi or f[12:14] != b"\x60\x07" or len(f) < 40:
            continue
        s, d = mac(f[6:12]), mac(f[0:6])
        if not ((s, d) in ((a, b), (b, a)) or (s in (a, b) and f[0] & 1)):
            continue
        mt = f[30]
        echo = struct.unpack("<H", f[36:38])[0]
        extra = ""
        if mt in (0xb2, 0xb3, 0xb4, 0xa0) and len(f) >= 94:
            extra = "adv=%d" % struct.unpack("<H", f[92:94])[0]
        if mt in (0x41, 0x48, 0x4b, 0x5b, 0x7b):
            extra = "ack=%d seq=%d" % (struct.unpack("<H", f[32:34])[0],
                                       struct.unpack("<H", f[34:36])[0])
        print("%8.3f %s->%s mt=%02x a36=%d len=%d %s"
              % (t, s[-5:], d[-5:], mt, echo, len(f), extra))


if __name__ == "__main__":
    main()
