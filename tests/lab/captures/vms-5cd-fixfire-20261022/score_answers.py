#!/usr/bin/env python3
"""score_5cd.py <pcap> <ovmx-mac-tail> -- the rd vms-5cd fire's own scoring read.

Tabulates EVERY cat-0x82 op-0x01 answer the OVMX node ORIGINATED, with the
outcome cell body[34] and the resource name, so "zero double grants" is a COUNT
and not an absence of looking. Uses the shipping census walker
(tools/cluster/remaster_census.py) for the frame/body split -- no private
offsets.
"""
import sys
import os

sys.path.insert(0, os.path.join(sys.argv[3], "tools", "cluster"))
import remaster_census as rc   # noqa: E402

OUTCOME = {0xfa: "GRANTED", 0xfb: "QUEUED", 0xf8: "REDIRECT",
           0xf9: "NO-ENTRY/MASTERED", 0xfe: "DECLINED"}


def name_of(body):
    n = body[48 + 0:48 + 32]
    ln = body[44 + 3] if len(body) > 47 else 0
    txt = bytes(body[48:48 + 32]).split(b"\x00")[0]
    return txt.decode("latin1", "replace").strip(), ln, n


def main():
    pcap, mac_tail, _root = sys.argv[1], sys.argv[2].lower(), sys.argv[3]
    n_ans = 0
    for idx, src, dst, body in rc.sca_bodies_with_src(pcap):
        if len(body) < 36:
            continue
        cat, op = body[8], body[9]
        if cat != 0x82 or op != 0x01:
            continue
        if src.hex().lower()[-len(mac_tail):] != mac_tail:
            continue
        n_ans += 1
        nm, _ln, _raw = name_of(body)
        print("f%-6s %s -> %s  body[34]=0x%02x %-16s res=%r"
              % (idx, src.hex(), dst.hex(), body[34],
                 OUTCOME.get(body[34], "?"), nm))
    print("--- %d cat-0x82 op-0x01 answers ORIGINATED by %s ---" % (n_ans, mac_tail))


if __name__ == "__main__":
    main()
