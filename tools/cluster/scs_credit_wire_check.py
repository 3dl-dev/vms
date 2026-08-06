#!/usr/bin/env python3
"""
scs_credit_wire_check.py <pcap> [<pcap> ...]  --  vms-aa1

READ THE CREDIT FIELD OFF THE WIRE, per node, per SCS message type.

WHY IT EXISTS. vms-aa1 makes OVMX stamp a live p. 2-44 Pending Receive Credit
into the GROUNDED credit field at SCA [48:50] (grounded by scs_credit.h's WIRE
VERDICT and tools/scs_credit_measure.py -- this script re-derives NOTHING about
where the field is, it only reads it). The claim that has to be checked on a lab
capture is narrow and behavioural: OVMX's own MTYPE-10 frames must carry a
VARYING value there, where before this item they carried whatever their captured
template held. This reads that value out of the packet bytes.

WHOSE FRAMES ARE WHOSE is decided from the CAPTURE, never from SCSD's own log
(guardrail 18), and by the SAME rule the MTYPE census in scs_rx.h uses: a
REAL-VAX source is an Ethernet source MAC with the DEC OUI `08:00:2b` or the
DECnet-logical prefix `aa:00:04`. Anything else on this wire is OVMX (the pod's
`br0`, a locally-administered address).

That rule is applied to the SOURCE MAC alone. It is NOT the presence of an
`OVMX??` string in the frame: the reference node ECHOES the joiner's SCSNODE
back in its own membership frames, so a name match tags both ends and reports
"OVMX emitted everything". This script printed exactly that on its first run.
The names found are still printed, and the MACs they came from listed, so a run
whose identity never reached the wire stays visible.

Usage:
    scs_credit_wire_check.py d94-TAGA.pcap d94-TAGB.pcap

Prints, per capture and per source class, the MTYPE x credit histogram. Exit
status is 0 always: this is an instrument, not a gate. The gate that must go red
is tests/vmsscs/test_scsd_wire.c; this answers "and what happened on a real
wire".
"""
import re
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"
OFF_INNER_LEN = 42
OFF_FORMAT = 44
OFF_MTYPE = 46
OFF_CREDIT = 48   # GROUNDED: scs_credit.h WIRE VERDICT. Not re-derived here.
OFF_DEST = 50
OFF_SRC = 54
NAME_RE = re.compile(rb"OVMX[A-Z0-9]{2}")


def frames(path):
    data = open(path, "rb").read()
    if len(data) < 24:
        return
    magic = struct.unpack("<I", data[:4])[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    off = 24
    while off + 16 <= len(data):
        _ts, _tu, cl, _ol = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        pkt = data[off:off + cl]
        off += cl
        if len(pkt) >= 14:
            yield pkt


def sca(pkt):
    """The envelope-conformant SCA view of a frame, or None."""
    if len(pkt) < 72 or pkt[12:14] != ETHERTYPE_SCA:
        return None
    c = pkt[14:]
    total = int.from_bytes(c[0:2], "little") + 2
    if total > len(c) or total < 58:
        return None
    if int.from_bytes(c[OFF_FORMAT:OFF_FORMAT + 2], "little") != 4:
        return None
    if int.from_bytes(c[OFF_INNER_LEN:OFF_INNER_LEN + 2], "little") != total - 44:
        return None
    return {
        "total": total,
        "mtype": int.from_bytes(c[OFF_MTYPE:OFF_MTYPE + 2], "little"),
        "credit": int.from_bytes(c[OFF_CREDIT:OFF_CREDIT + 2], "little"),
        "dest": int.from_bytes(c[OFF_DEST:OFF_DEST + 4], "little"),
        "src": int.from_bytes(c[OFF_SRC:OFF_SRC + 4], "little"),
    }


def is_real_vax(mac):
    """The scs_rx.h census rule: DEC OUI or the DECnet-logical prefix."""
    return mac[:3] == b"\x08\x00\x2b" or mac[:4] == b"\xaa\x00\x04\x00"


def report(path):
    names = set()
    pkts = list(frames(path))
    src_macs = {}
    for pkt in pkts:
        src_macs.setdefault(pkt[6:12], 0)
        src_macs[pkt[6:12]] += 1
        for m in NAME_RE.findall(pkt):
            names.add(m.decode())
    print("== %s" % path)
    print("   frames=%d  OVMX node names anywhere in the capture: %s"
          % (len(pkts), " ".join(sorted(names)) or "(none)"))
    for mac in sorted(src_macs):
        print("   src %s  n=%-6d %s"
              % (":".join("%02x" % b for b in mac), src_macs[mac],
                 "real-VAX" if is_real_vax(mac) else "OVMX"))
    for label, want_ovmx in (("OVMX-emitted", True), ("peer-emitted", False)):
        hist = {}
        for pkt in pkts:
            v = sca(pkt)
            if v is None:
                continue
            if is_real_vax(pkt[6:12]) == want_ovmx:
                continue
            hist.setdefault(v["mtype"], {}).setdefault(v["credit"], 0)
            hist[v["mtype"]][v["credit"]] += 1
        print("   -- %s" % label)
        if not hist:
            print("      (no envelope-conformant SCS frames)")
            continue
        for mt in sorted(hist):
            vals = hist[mt]
            n = sum(vals.values())
            distinct = len(vals)
            shown = " ".join("%d:%d" % (c, vals[c]) for c in sorted(vals))
            print("      MTYPE %-3d n=%-6d distinct-credits=%d   %s"
                  % (mt, n, distinct, shown))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 0
    for p in argv[1:]:
        report(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
