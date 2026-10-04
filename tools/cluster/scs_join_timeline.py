#!/usr/bin/env python3
"""scs_join_timeline.py -- what actually happened to an OVMX join, on one clock.

WHY THIS EXISTS (vms-f3ec). The vms-f3ec report read the join failure off a
count: "OVMX emitted 2 category-0x04 credit returns where the baseline emitted
86". A count cannot tell a flow-control stall from a node that stopped talking,
and here it was the second: OVMX kept its circuits up (HELLOs to the end of the
capture) and simply stopped producing VMS$VAXcluster messages. This script
prints the three quantities that separate those readings, in ONE timebase:

  * every op-5 REJECT_RESPONSE OVMX emitted, and whether it was ADDRESSED
    (SCS$L_DST_CONID != 0, the vms-d7e field);
  * every NISCA circuit FORMATION (the 0x41 START class at abs 30), so a
    port-driver VC close and the reformation after it are visible;
  * the span and count of OVMX's own 190-content VMS$VAXcluster messages.

CLEAN-ROOM. Every offset below is one this repo's own codec TU already owns
and documents (vms_cluster_codec_scs.h: the SCS envelope at abs 56-71, the
GROUNDED format word 0x0004 at abs 58, the verb at abs 60, the Con.ID pair at
abs 64/68; vms_cluster_codec_cm.h: the SYSAP body's category/opcode at abs
80/81). Nothing is guessed and nothing is decompiled -- see CLAUDE.md rule 8.

Usage:
    scs_join_timeline.py <pcap> [<pcap> ...]
    scs_join_timeline.py --selftest
"""
import struct
import sys
import os

ETHERTYPE_SCA = b"\x60\x07"
OVMX_MAC = "52:54:00:00:00:f4"

OFF_SCA_LEN = 14        # SCA length word (content = value + 2)
OFF_NISCA_MT = 30       # NISCA message-type byte; 0x41 is the START class
OFF_SCS_FMT = 58        # GROUNDED format word 0x0004 -> this IS an SCS header
OFF_SCS_OP = 60         # the $SCSDEF verb
OFF_SCS_DST_CONID = 64
OFF_SCS_SRC_CONID = 68
OFF_CM_CATEGORY = 80
OFF_CM_OPCODE = 81

SCS_FMT_WORD = 0x0004
SCS_OP_REJ_RSP = 5
NISCA_MT_START = 0x41
CM_CONTENT = 190        # the fixed VMS$VAXcluster message class
FORMATION_GAP_S = 1.0   # STARTs closer than this belong to one formation


def read_pcap(path):
    """Classic libpcap, either endianness. (ts, frame) pairs."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 24:
        return []
    endian = "<" if data[0:4] in (b"\xd4\xc3\xb2\xa1",
                                  b"\x4d\x3c\xb2\xa1") else ">"
    off, out = 24, []
    while off + 16 <= len(data):
        sec, frac, incl, _orig = struct.unpack(endian + "IIII",
                                               data[off:off + 16])
        off += 16
        out.append((sec + frac / 1e6, data[off:off + incl]))
        off += incl
    return out


def mac(raw):
    return ":".join("%02x" % b for b in raw)


def u16(frame, off):
    return struct.unpack("<H", frame[off:off + 2])[0]


def u32(frame, off):
    return struct.unpack("<I", frame[off:off + 4])[0]


def is_sca(frame):
    return len(frame) > OFF_NISCA_MT and frame[12:14] == ETHERTYPE_SCA


def is_scs(frame):
    # The shortest SCS class (the 58-content connect verbs) ends EXACTLY at
    # abs 72, so the bound is inclusive -- an exclusive one drops every op 5.
    return (len(frame) >= OFF_SCS_SRC_CONID + 4 and
            u16(frame, OFF_SCS_FMT) == SCS_FMT_WORD)


def collapse(times, gap=FORMATION_GAP_S):
    """A burst of STARTs is one circuit formation, not six."""
    out = []
    for t in times:
        if not out or t - out[-1][1] > gap:
            out.append([t, t])
        else:
            out[-1][1] = t
    return [g[0] for g in out]


def scan(path, ovmx=OVMX_MAC):
    frames = read_pcap(path)
    if not frames:
        return None
    t0 = frames[0][0]
    rejects, starts, cm_msgs = [], [], []
    for ts, frame in frames:
        if not is_sca(frame):
            continue
        rel = ts - t0
        if frame[OFF_NISCA_MT] == NISCA_MT_START:
            starts.append(rel)
            continue
        if not is_scs(frame):
            continue
        from_ovmx = mac(frame[6:12]) == ovmx
        if from_ovmx and u16(frame, OFF_SCS_OP) == SCS_OP_REJ_RSP:
            rejects.append((rel, u32(frame, OFF_SCS_DST_CONID),
                            u32(frame, OFF_SCS_SRC_CONID)))
        if from_ovmx and u16(frame, OFF_SCA_LEN) + 2 == CM_CONTENT:
            cm_msgs.append((rel, frame[OFF_CM_CATEGORY], frame[OFF_CM_OPCODE]))
    return {"rejects": rejects, "formations": collapse(starts),
            "cm": cm_msgs}


def report(path, res):
    print("== %s" % os.path.basename(path))
    for rel, dst, src in res["rejects"]:
        print("   %8.3f  OVMX op-5 REJECT_RSP  DST_CONID=%08x %s  "
              "SRC_CONID=%08x" % (rel, dst,
                                  "(ADDRESSED)" if dst else "(UNADDRESSED)",
                                  src))
    if not res["rejects"]:
        print("   (OVMX emitted no op-5 REJECT_RESPONSE)")
    print("   NISCA circuit formations: %s" %
          ", ".join("%.3f" % t for t in res["formations"]))
    cm = res["cm"]
    if cm:
        print("   OVMX VMS$VAXcluster messages: n=%d, %.3f .. %.3f" %
              (len(cm), cm[0][0], cm[-1][0]))
        print("   last five: %s" %
              ", ".join("%.3f cat=%02x op=%02x" % m for m in cm[-5:]))
    else:
        print("   OVMX VMS$VAXcluster messages: NONE")
    print()


def selftest():
    """Run against the in-tree CN=3 baseline, whose numbers are published in
    the vms-d7e commit message (op-5 at 16.315; the port-driver VC close it
    provoked reforms the circuit 1.974 s later, at 18.289)."""
    here = os.path.dirname(os.path.abspath(__file__))
    pcap = os.path.join(here, "..", "..", "tests", "lab", "captures",
                        "cn3-achieved-20260905.pcap")
    pcap = os.path.normpath(pcap)
    res = scan(pcap)
    fails = []

    def check(cond, what):
        print("  %s %s" % ("ok  " if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    print("-- scs_join_timeline selftest against %s" % os.path.basename(pcap))
    check(res is not None and len(res["rejects"]) == 1,
          "the baseline carries exactly one OVMX op-5")
    if res and res["rejects"]:
        rel, dst, _src = res["rejects"][0]
        check(abs(rel - 16.315) < 0.01, "op-5 at 16.315 (vms-d7e's figure)")
        check(dst == 0, "and it is UNADDRESSED, which is the defect d7e fixed")
    check(res is not None and len(res["formations"]) == 2,
          "two circuit formations: the initial one and the one the "
          "unaddressed op-5 provoked")
    if res and len(res["formations"]) == 2:
        check(abs(res["formations"][1] - 18.289) < 0.01,
              "the second at 18.289 (1.974 s after the op-5)")
        cm = res["cm"]
        check(bool(cm) and cm[0][0] > res["formations"][1],
              "every OVMX VMS$VAXcluster message falls AFTER it -- the "
              "admission ran on the REFORMED circuit")
    print("scs_join_timeline selftest: %d failures" % len(fails))
    return 1 if fails else 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == "--selftest":
        return selftest()
    for path in argv[1:]:
        res = scan(path)
        if res is None:
            print("== %s: not a readable pcap" % path)
            continue
        report(path, res)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
