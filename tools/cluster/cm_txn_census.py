#!/usr/bin/env python3
"""cm_txn_census.py -- the connection-manager transaction dialogue in a pcap,
one line per frame: envelope, category/opcode, and (for the transaction CLOSE
pair) the body[24:26] cell whose value OVMX has never been able to ground.

CLEAN-ROOM (Rule 8). Every offset is one already grounded and written up in
docs/cluster-protocol-spec.md from observation of our own SIMH OpenVMS VAX
reference cluster plus public OpenVMS documentation:

  the fixed 190-content class is the only one with a grounded Con.ID and SYSAP
  body location (spec 4d); within it the SYSAP body starts at absolute 72 and
  carries send-msg#/ack-msg#/txn/token at body[0:8] and category/opcode at
  body[8:10] (spec 4j), the transition epoch at body[12:16] and role/class at
  body[16:18] (spec 4p).

Nothing here is disassembled, decompiled, or derived from VSI/HPE material.

Usage:
    cm_txn_census.py <pcap> [--cat 06] [--op 00] [--from T] [--to T]
                     [--names MAC=NAME,...] [--closes]

    --closes   only the transaction CLOSE pair (cat 0x06 request / 0x86
               response), printing body[24:26] for each.
"""
import argparse
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"
CM_TOTAL = 190
OFF_SCA_LEN = 14
OFF_CM_BODY = 14 + 58          # absolute 72: the SYSAP body
OFF_CONID_REMOTE = 14 + 50
OFF_CONID_LOCAL = 14 + 54


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


def le16(f, o):
    return struct.unpack("<H", f[o:o + 2])[0]


def le32(f, o):
    return struct.unpack("<I", f[o:o + 4])[0]


def mac(raw):
    return ":".join("%02x" % b for b in raw)


def is_cm(frame):
    return (len(frame) >= OFF_CM_BODY + 60 and frame[12:14] == ETHERTYPE_SCA and
            le16(frame, OFF_SCA_LEN) + 2 == CM_TOTAL)


def decode(idx, ts, frame):
    b = OFF_CM_BODY
    return {
        "idx": idx, "ts": ts,
        "src": mac(frame[6:12]), "dst": mac(frame[0:6]),
        "remote": le32(frame, OFF_CONID_REMOTE),
        "local": le32(frame, OFF_CONID_LOCAL),
        "send": le16(frame, b + 0), "ack": le16(frame, b + 2),
        "txn": le16(frame, b + 4), "token": le16(frame, b + 6),
        "cat": frame[b + 8], "op": frame[b + 9],
        "epoch": le32(frame, b + 12),
        "role": frame[b + 16], "cls": frame[b + 17],
        "close_state": le16(frame, b + 24),
        "body_nonzero": [i for i in range(10, 132)
                         if b + i < len(frame) and frame[b + i] != 0],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--cat", default=None)
    ap.add_argument("--op", default=None)
    ap.add_argument("--closes", action="store_true")
    ap.add_argument("--from", dest="t_from", type=float, default=None)
    ap.add_argument("--to", dest="t_to", type=float, default=None)
    ap.add_argument("--names", default="")
    args = ap.parse_args()

    names = {}
    for pair in filter(None, args.names.split(",")):
        m, _, n = pair.partition("=")
        names[m.lower()] = n

    want_cat = int(args.cat, 16) if args.cat else None
    want_op = int(args.op, 16) if args.op else None

    rows = []
    for idx, ts, frame in read_pcap(args.pcap):
        if not is_cm(frame):
            continue
        d = decode(idx, ts, frame)
        if args.closes and d["cat"] not in (0x06, 0x86):
            continue
        if want_cat is not None and d["cat"] != want_cat:
            continue
        if want_op is not None and d["op"] != want_op:
            continue
        rows.append(d)
    if not rows:
        print("no CM frames matched")
        return
    t0 = rows[0]["ts"]
    if args.t_from is not None:
        t0 = args.t_from
    for d in rows:
        rel = d["ts"] - t0
        if args.t_from is not None and rel < 0:
            continue
        if args.t_to is not None and rel > args.t_to:
            continue
        s = names.get(d["src"], d["src"])
        t = names.get(d["dst"], d["dst"])
        extra = ""
        if d["cat"] in (0x06, 0x86):
            extra = ("  body[24:26]=0x%04x  nonzero-body-offsets=%s" %
                     (d["close_state"], d["body_nonzero"][:12]))
        print("[%6d] t=%9.3f %-8s -> %-8s cat=%02x op=%02x "
              "send=%-4d ack=%-4d txn=%-5d tok=%-5d epoch=%08x role=%02x cls=%02x%s"
              % (d["idx"], rel, s, t, d["cat"], d["op"], d["send"], d["ack"],
                 d["txn"], d["token"], d["epoch"], d["role"], d["cls"], extra))


if __name__ == "__main__":
    main()
