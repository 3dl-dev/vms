#!/usr/bin/env python3
"""cm_crash_window.py -- what was on the wire immediately before a peer
announced its departure.

A real OpenVMS node puts a LAST-GASP datagram on the cluster multicast both on
a clean leave and on its way out of a cluster bugcheck (spec sec 4(O.30): SCA
msgtype at absolute offset 30 == 0xb1). When a peer bugchecks next to this
implementation's traffic, the only honest way to attribute it is to read the
frames that preceded the gasp -- a console line cannot say which frame did it.

This prints, for every 0xb1 in a capture, the N SCA frames before it, decoded
as far as docs/cluster-protocol-spec.md grounds them, and names the LAST frame
each other station sent before it. Nothing here concludes anything: it puts the
window in front of a human.

CLEAN-ROOM (Rule 8): every offset cited is one already grounded in
docs/cluster-protocol-spec.md from observation of our own SIMH OpenVMS VAX
reference cluster and public OpenVMS documentation.

Usage:
    cm_crash_window.py <pcap> [--before N] [--names MAC=NAME,...]
"""
import argparse
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"
OFF_MSGTYPE = 30          # spec sec 4(O.30)
MT_LAST_GASP = 0xB1
CM_TOTAL = 190
OFF_CM_BODY = 14 + 58

CTRL = {0: "CONNECT_REQ", 1: "CONNECT_RSP", 2: "ACCEPT_REQ", 3: "ACCEPT_RSP",
        4: "REJECT_REQ", 5: "REJECT_RSP", 6: "DISCONNECT_REQ",
        7: "DISCONNECT_RSP", 10: "APPDATA"}


def read_pcap(path):
    with open(path, "rb") as fh:
        gh = fh.read(24)
        if len(gh) < 24:
            return
        magic = gh[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            endian, nano = "<", False
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian, nano = ">", False
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian, nano = "<", True
        elif magic == b"\xa1\xb2\x3c\x4d":
            endian, nano = ">", True
        else:
            raise SystemExit(f"{path}: not a classic pcap")
        idx = 0
        while True:
            rh = fh.read(16)
            if len(rh) < 16:
                return
            ts_sec, ts_frac, caplen, _ = struct.unpack(endian + "IIII", rh)
            data = fh.read(caplen)
            if len(data) < caplen:
                return
            yield idx, ts_sec + ts_frac / (1e9 if nano else 1e6), data
            idx += 1


def mac(raw):
    return ":".join("%02x" % b for b in raw)


def is_sca(frame):
    return len(frame) > OFF_MSGTYPE and frame[12:14] == ETHERTYPE_SCA


def describe(frame, names):
    """One line of whatever the spec grounds for this frame's class."""
    pl = frame[14:]
    total = struct.unpack("<H", pl[0:2])[0] + 2
    out = "content=%-4d mtype30=0x%02x" % (total, frame[OFF_MSGTYPE])
    if len(pl) >= 58:
        ctype = struct.unpack("<H", pl[46:48])[0]
        rem = struct.unpack("<I", pl[50:54])[0]
        loc = struct.unpack("<I", pl[54:58])[0]
        if total == CM_TOTAL:
            b = OFF_CM_BODY
            out += ("  CM cat=%02x op=%02x send=%d ack=%d txn=%d tok=%d"
                    % (frame[b + 8], frame[b + 9],
                       struct.unpack("<H", frame[b + 0:b + 2])[0],
                       struct.unpack("<H", frame[b + 2:b + 4])[0],
                       struct.unpack("<H", frame[b + 4:b + 6])[0],
                       struct.unpack("<H", frame[b + 6:b + 8])[0]))
        elif ctype in CTRL and total in (110, 94, 66, 62, 58):
            out += "  %-14s rem=%08x loc=%08x" % (CTRL[ctype], rem, loc)
            if total in (110, 94) and len(pl) >= 94:
                out += "  n1=%r n2=%r" % (
                    pl[62:78].decode("latin-1").rstrip("\x00 "),
                    pl[78:94].decode("latin-1").rstrip("\x00 "))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--before", type=int, default=25)
    ap.add_argument("--names", default="")
    args = ap.parse_args()

    names = {}
    for pair in filter(None, args.names.split(",")):
        m, _, n = pair.partition("=")
        names[m.lower()] = n

    def nm(m):
        return names.get(m, m)

    frames = [(i, t, f) for i, t, f in read_pcap(args.pcap) if is_sca(f)]
    gasps = [k for k, (_, _, f) in enumerate(frames)
             if f[OFF_MSGTYPE] == MT_LAST_GASP]
    if not gasps:
        print("no last-gasp (abs30 == 0xb1) frame in this capture")
        return

    for k in gasps:
        gi, gt, gf = frames[k]
        print("\n=== last gasp: frame %d, %s announced departure ===" %
              (gi, nm(mac(gf[6:12]))))
        lo = max(0, k - args.before)
        last_by_src = {}
        for j in range(lo, k):
            i, t, f = frames[j]
            src = nm(mac(f[6:12]))
            last_by_src[src] = (i, gt - t, describe(f, names))
            print("  [%6d] -%7.3f s  %-8s -> %-8s %s" %
                  (i, gt - t, src, nm(mac(f[0:6])), describe(f, names)))
        print("  -- last frame each station sent before the gasp --")
        for src, (i, dt, d) in sorted(last_by_src.items(),
                                      key=lambda kv: kv[1][1]):
            print("     %-8s  -%7.3f s  frame %d  %s" % (src, dt, i, d))


if __name__ == "__main__":
    main()
