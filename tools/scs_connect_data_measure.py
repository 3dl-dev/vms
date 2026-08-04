#!/usr/bin/env python3
"""
scs_connect_data_measure.py -- re-derive every figure in the CONNECT DATA
verdict of src/vmsscs/include/scs_connect.h and docs/cluster-protocol-spec.md
section 4(n) from the lab captures (vms-fdd).

WHY THIS EXISTS. The verdict is a comment, and a comment is not evidence.
The 16 bytes OVMX puts at the end of its VMS$VAXcluster CONNECT_REQ are a
VERSION CLAIM (VAXcluster Principles p. 2-25: the two connection managers use
the connect data "to effectively identify to each other which version of VMS
each is associated with", and either end may refuse the other on it). A wrong
value is worse than none, so the value must stay re-derivable from the raw
captures rather than resting on a comment nobody can re-run.

    tools/scs_connect_data_measure.py            # re-measure, PASS/FAIL vs EXPECTED
    tools/scs_connect_data_measure.py --print    # just print what the captures say

Requires the lab captures, which are host-only and NOT in git:

    /data/training/vax/cluster/captures/*.pcap        (lab-1, see CLAUDE.md r.8)

Override with --captures DIR. A full run reads every .pcap and takes a couple
of minutes.

EXPECTED below is the checked-in record of what the captures measured on
2026-08-04. `ctest -R scs_connect_data_figures` does NOT need the captures: it
checks that every figure in EXPECTED still appears verbatim in scs_connect.h
and docs/cluster-protocol-spec.md, so the comment cannot drift away from the
measurement. Only this script, run on a host with the captures, re-derives
EXPECTED itself.

THE POPULATION (and why it is exactly this one). Take `sca = frame[14:]` for
every ethertype-0x6007 frame. Keep frames with `len(sca) == 110`, format byte
`sca[17] == 0x13`, and opcode `sca[16]` in the SCS-message family
{0x4b, 0x5b, 0x7b}. Then split on the SCA connection-control message type at
`sca[46:48]` (spec sec 4(h)(1a)): only values 0 (CONNECT_REQ) and 2
(ACCEPT_REQ) are connect frames. That split is not an assumption -- the script
asserts it: over the whole library the 110-byte class carries message types
{0: 1497, 2: 394, 10: 2889}, and every one of the 1891 type-0/2 frames has an
ASCII SYSAP name at [62:78] while the type-10 frames carry binary there. The
connect-data field is therefore claimed ONLY for message types 0 and 2.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""

import argparse
import collections
import glob
import os
import struct
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

# The SCA payload-relative span of the connect-data field, and the two 16-byte
# SYSAP name fields that precede it (spec sec 4h(2) grounds the names).
CD_OFF, CD_END = 94, 110
LOCAL_NAME, REMOTE_NAME = (62, 78), (78, 94)

# The authoritative established-join specimen (spec sec 1: the ONLY capture of a
# real node being admitted to an already-running cluster, which is the operation
# OVMX performs).
JOIN_SPECIMEN = "vax3-2to3-established-join-20260730.pcap"
VAX3_HW = "08:00:2b:11:22:33"   # the joiner in that capture
VAX1_LOGICAL = "aa:00:04:00:01:04"  # an established member in that capture

EXPECTED = {
    # --- population ---
    "pcaps_scanned": 48,
    "connect_frames": 1891,              # 110-byte 0x?B13, msgtype 0 or 2
    "msgtype_histogram": {0: 1497, 2: 394, 10: 2889},
    # --- the VMS$VAXcluster subset and its two invariant spans ---
    "vaxcluster_frames": 203,
    "vaxcluster_version_quad": 203,      # [94:98] == 01 1b 01 03
    "vaxcluster_tail": 203,              # [105:110] == 08 00 00 06 00
    "vaxcluster_distinct_values": 5,
    # --- per-SYSAP census: local SYSAP name -> (frames, distinct values) ---
    "sysap_census": {
        "MSCP$DISK": (1052, 1),
        "SCA$TRANSPORT": (32, 2),
        "SCS$DIRECTORY": (314, 1),
        "SCS$DIR_LOOKUP": (189, 1),
        "VMS$DISK_CL_DRVR": (101, 5),
        "VMS$VAXcluster": (203, 5),
    },
    # --- the value OVMX adopts, and where it comes from ---
    "ovmx_value": "01 1b 01 03 00 00 00 00 00 00 00 08 00 00 06 00",
    "member_value_in_specimen": "01 1b 01 03 01 00 01 00 02 00 01 08 00 00 06 00",
    # In JOIN_SPECIMEN the joiner emits ONE value for both message types.
    "specimen_joiner_connect_req": 1,
    "specimen_joiner_accept_req": 1,
    "specimen_joiner_distinct_values": 1,
}


def pcap_frames(path):
    """Yield raw Ethernet frames from a classic pcap file."""
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic = gh[:4]
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            end = "<"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            end = ">"
        else:
            return
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                break
            _ts, _tu, incl, _orig = struct.unpack(end + "IIII", ph)
            data = f.read(incl)
            if len(data) < incl:
                break
            yield data


def mac(b):
    return ":".join("%02x" % c for c in b)


def hexs(b):
    return " ".join("%02x" % c for c in b)


def measure(capdir):
    m = {
        "pcaps_scanned": 0,
        "connect_frames": 0,
        "msgtype_histogram": collections.Counter(),
        "vaxcluster_frames": 0,
        "vaxcluster_version_quad": 0,
        "vaxcluster_tail": 0,
        "sysap_values": collections.defaultdict(collections.Counter),
        "name_field_ascii_violations": 0,
        "specimen": collections.defaultdict(collections.Counter),
    }
    for path in sorted(glob.glob(os.path.join(capdir, "**", "*.pcap"), recursive=True)):
        m["pcaps_scanned"] += 1
        base = os.path.basename(path)
        for pkt in pcap_frames(path):
            if len(pkt) < 16 or pkt[12:14] != b"\x60\x07":
                continue
            sca = pkt[14:]
            if len(sca) != 110 or sca[17] != 0x13 or sca[16] not in (0x4B, 0x5B, 0x7B):
                continue
            mt = struct.unpack("<H", sca[46:48])[0]
            m["msgtype_histogram"][mt] += 1
            if mt not in (0, 2):
                continue
            m["connect_frames"] += 1
            local = sca[LOCAL_NAME[0]:LOCAL_NAME[1]]
            # The population claim: a connect frame's [62:78] is an ASCII SYSAP
            # name. Anything else means the split above is wrong.
            if not all(32 <= c < 127 for c in local):
                m["name_field_ascii_violations"] += 1
                continue
            name = local.decode("ascii").rstrip()
            cd = bytes(sca[CD_OFF:CD_END])
            m["sysap_values"][name][cd] += 1
            if name == "VMS$VAXcluster":
                m["vaxcluster_frames"] += 1
                if cd[0:4] == b"\x01\x1b\x01\x03":
                    m["vaxcluster_version_quad"] += 1
                if cd[11:16] == b"\x08\x00\x00\x06\x00":
                    m["vaxcluster_tail"] += 1
            if base == JOIN_SPECIMEN and name == "VMS$VAXcluster":
                m["specimen"][(mac(pkt[6:12]), mt)][cd] += 1
    return m


def report(m, out=sys.stdout):
    print("pcaps scanned                       : %d" % m["pcaps_scanned"], file=out)
    print("110-byte 0x?B13 msgtype histogram   : %s"
          % dict(sorted(m["msgtype_histogram"].items())), file=out)
    print("CONNECT_REQ/ACCEPT_REQ frames (0,2) : %d" % m["connect_frames"], file=out)
    print("SYSAP-name-field non-ASCII residuals: %d" % m["name_field_ascii_violations"], file=out)
    print(file=out)
    print("per-local-SYSAP connect-data census (payload [94:110]):", file=out)
    for name in sorted(m["sysap_values"]):
        vals = m["sysap_values"][name]
        print("  %-18s n=%-5d distinct=%d" % (name, sum(vals.values()), len(vals)), file=out)
        for cd, n in vals.most_common():
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in cd)
            print("        %5d  %s  |%s|" % (n, hexs(cd), asc), file=out)
    print(file=out)
    print("VMS$VAXcluster invariant spans:", file=out)
    print("  [94:98]  == 01 1b 01 03        : %d/%d"
          % (m["vaxcluster_version_quad"], m["vaxcluster_frames"]), file=out)
    print("  [105:110] == 08 00 00 06 00    : %d/%d"
          % (m["vaxcluster_tail"], m["vaxcluster_frames"]), file=out)
    print(file=out)
    print("%s -- VMS$VAXcluster connect data by source and message type:"
          % JOIN_SPECIMEN, file=out)
    for (src, mt) in sorted(m["specimen"]):
        for cd, n in m["specimen"][(src, mt)].most_common():
            print("  %-18s mt=%d (%-11s) x%d  %s"
                  % (src, mt, "CONNECT_REQ" if mt == 0 else "ACCEPT_REQ", n, hexs(cd)),
                  file=out)


def check(m):
    fails = []
    ok = []

    def cmp(label, got, want):
        (ok if got == want else fails).append("%-42s got=%r want=%r" % (label, got, want))

    cmp("pcaps_scanned", m["pcaps_scanned"], EXPECTED["pcaps_scanned"])
    cmp("connect_frames", m["connect_frames"], EXPECTED["connect_frames"])
    cmp("msgtype_histogram", dict(m["msgtype_histogram"]), EXPECTED["msgtype_histogram"])
    cmp("name_field_non_ascii_residuals", m["name_field_ascii_violations"], 0)
    cmp("vaxcluster_frames", m["vaxcluster_frames"], EXPECTED["vaxcluster_frames"])
    cmp("vaxcluster_version_quad", m["vaxcluster_version_quad"],
        EXPECTED["vaxcluster_version_quad"])
    cmp("vaxcluster_tail", m["vaxcluster_tail"], EXPECTED["vaxcluster_tail"])
    cmp("vaxcluster_distinct_values", len(m["sysap_values"]["VMS$VAXcluster"]),
        EXPECTED["vaxcluster_distinct_values"])
    for name, (n, distinct) in EXPECTED["sysap_census"].items():
        vals = m["sysap_values"].get(name, {})
        cmp("sysap %s frames" % name, sum(vals.values()), n)
        cmp("sysap %s distinct" % name, len(vals), distinct)

    # The specimen: the joiner's single value, and the member's contrasting one.
    joiner = collections.Counter()
    for (src, mt), vals in m["specimen"].items():
        if src == VAX3_HW:
            joiner.update(vals)
    cmp("specimen joiner distinct values", len(joiner),
        EXPECTED["specimen_joiner_distinct_values"])
    cmp("specimen joiner CONNECT_REQ frames",
        sum(m["specimen"].get((VAX3_HW, 0), {}).values()),
        EXPECTED["specimen_joiner_connect_req"])
    cmp("specimen joiner ACCEPT_REQ frames",
        sum(m["specimen"].get((VAX3_HW, 2), {}).values()),
        EXPECTED["specimen_joiner_accept_req"])
    if joiner:
        cmp("specimen joiner value == OVMX value",
            hexs(joiner.most_common(1)[0][0]), EXPECTED["ovmx_value"])
    else:
        fails.append("specimen joiner value: no frames found")
    member = collections.Counter()
    for (src, mt), vals in m["specimen"].items():
        if src == VAX1_LOGICAL:
            member.update(vals)
    if member:
        cmp("specimen member value (contrast)",
            hexs(member.most_common(1)[0][0]), EXPECTED["member_value_in_specimen"])
    else:
        fails.append("specimen member value: no frames found")
    return ok, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="just_print", action="store_true",
                    help="print the measurement, do not check it against EXPECTED")
    args = ap.parse_args()

    if not os.path.isdir(args.captures):
        print("capture directory not found: %s" % args.captures, file=sys.stderr)
        print("These captures are host-only (CLAUDE.md rule 8, lab-1).", file=sys.stderr)
        return 2

    m = measure(args.captures)
    report(m)
    if args.just_print:
        return 0

    ok, fails = check(m)
    print()
    for line in ok:
        print("PASS  %s" % line)
    for line in fails:
        print("FAIL  %s" % line)
    print("\n%d checks, %d failure(s)" % (len(ok) + len(fails), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
