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
2026-08-05. `ctest -R scs_connect_data_figures` does NOT need the captures: it
checks that every figure in EXPECTED still appears verbatim in scs_connect.h
and docs/cluster-protocol-spec.md, so the comment cannot drift away from the
measurement. Only this script, run on a host with the captures, re-derives
EXPECTED itself.

=======================================================================
CIRCULAR-GROUNDING GUARD -- OVMX'S OWN FRAMES ARE NOT EVIDENCE ABOUT VMS
=======================================================================

The lab captures are taken on a shared LAN on which OVMX itself is a talker.
Roughly a quarter of the connect frames in the library were TRANSMITTED BY
OVMX. Counting those as evidence for "this is what a real VAX puts in the
field" is circular: the census could not then distinguish "every VMS node does
this" from "we do this, and so do the VAXes we recorded alongside us".

So this script splits the population by ETHERNET SOURCE MAC, and the split is
a first-class filter, not a footnote:

  * the VAX population is what every GROUNDED figure in scs_connect.h and
    spec sec 4(N) is derived from -- OVMX-sourced frames are EXCLUDED;
  * the OVMX population is reported separately with its own counts. It is
    still legitimate evidence about ONE thing: what OVMX's own encoder emits.
    It is never evidence about what a real VAX puts on the wire.

Identification is sound because OVMX never spoofs its Ethernet source: scsd
takes `our_hw_mac` from SIOCGIFHWADDR (src/vmsscs/scsd.c) and every builder
copies it into abs [6:12]. The OVMX MAC set below is not a guess -- it is the
set of `hwmac=` values scsd itself logged in the lab work directory
(`SCSD-I-HELLOCFG, node='OVMXS8' ... hwmac=b6:16:8a:dc:3a:53`); --logs
re-derives it and the check FAILS if a logged OVMX MAC is missing here.

The blocklist alone would be fragile, so it is backed by a STRUCTURAL rule:
a real lab VAX sources from the DEC NIC OUI 08:00:2b or from a DECnet-assigned
aa:00:04 logical address. Any other source -- in particular any
locally-administered MAC, which is what a Linux tap/veth gets -- is NOT a VAX.
`unclassified_sources` counts every frame this rule cannot place, and the check
requires it to be ZERO. A future OVMX run on a new MAC therefore REDS this
script instead of silently rejoining the VAX population.

THE POPULATION (and why it is exactly this one). Take `sca = frame[14:]` for
every ethertype-0x6007 frame. Keep frames with `len(sca) == 110`, format byte
`sca[17] == 0x13`, and opcode `sca[16]` in the SCS-message family
{0x4b, 0x5b, 0x7b}. Then split on the SCA connection-control message type at
`sca[46:48]` (spec sec 4(h)(1a)): only values 0 (CONNECT_REQ) and 2
(ACCEPT_REQ) are connect frames. That split is not an assumption -- the script
asserts it: in the VAX population the 110-byte class carries message types
{0: 1101, 2: 324, 10: 2889}, and every one of the 1425 type-0/2 frames has an
ASCII SYSAP name at [62:78] while the type-10 frames carry binary there. The
connect-data field is therefore claimed ONLY for message types 0 and 2.

(All 2889 type-10 frames are VAX-sourced -- OVMX emits none, {0: 396, 2: 70}.
That asymmetry is the reason the exclusion has to be applied BEFORE the split
rather than to the totals: the two populations are not scaled copies of each
other, and no figure here may be obtained by scaling one from the other.)

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""

import argparse
import collections
import glob
import os
import re
import struct
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"
DEFAULT_LOGDIR = "/data/training/vax/cluster/work"

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

# --- the circular-grounding guard, see the header note --------------------
# Ethernet source prefixes a real lab VAX transmits from.
VAX_SOURCE_OUIS = ("08:00:2b",   # DEC NIC OUI
                   "aa:00:04")   # DECnet-assigned logical address
# OVMX's own hardware MACs, as logged by scsd itself (`hwmac=` in the lab work
# directory). --logs re-derives this set and the check fails if it has grown.
OVMX_HW_MACS = frozenset((
    "b6:16:8a:dc:3a:53",
    "e6:84:ef:b1:4f:ee",
))

VAX, OVMX = "vax", "ovmx"

EXPECTED = {
    # --- population, VAX-ONLY (OVMX-sourced frames excluded) ---
    "pcaps_scanned": 48,
    "connect_frames": 1425,              # 110-byte 0x?B13, msgtype 0 or 2, VAX-sourced
    "msgtype_histogram": {0: 1101, 2: 324, 10: 2889},
    "unclassified_sources": 0,           # every source MAC placed as VAX or OVMX
    # --- what the guard dropped, so the exclusion is auditable ---
    "ovmx_connect_frames": 466,
    "ovmx_msgtype_histogram": {0: 396, 2: 70},   # OVMX emits NO type-10 frame
    "ovmx_vaxcluster_frames": 55,
    "ovmx_source_macs": ["b6:16:8a:dc:3a:53"],   # the only one that appears in captures
    # --- the VMS$VAXcluster subset and its two invariant spans (VAX-only) ---
    "vaxcluster_frames": 148,
    "vaxcluster_version_quad": 148,      # [94:98] == 01 1b 01 03
    "vaxcluster_tail": 148,              # [105:110] == 08 00 00 06 00
    "vaxcluster_distinct_values": 5,
    "vaxcluster_source_nodes": 4,        # distinct VAX source MACs carrying it
    # The ungrounded middle span [98:105] in full, VAX-sourced, value -> frames.
    # Listed exhaustively because prose that says "two families" has to be
    # checkable: 4 of the 5 fit 01 00 01 00 NN 00 01, and one does NOT.
    "vaxcluster_mid_values": {
        "00 00 00 00 00 00 00": 40,
        "01 00 01 00 02 00 01": 59,
        "01 00 01 00 03 00 01": 37,
        "01 00 01 00 01 00 01": 11,
        "01 00 00 00 02 00 01": 1,
    },
    # --- per-SYSAP census: local SYSAP name -> (frames, distinct values) ---
    "sysap_census": {
        "MSCP$DISK": (809, 1),
        "SCA$TRANSPORT": (32, 2),
        "SCS$DIRECTORY": (201, 1),
        "SCS$DIR_LOOKUP": (134, 1),
        "VMS$DISK_CL_DRVR": (101, 5),
        "VMS$VAXcluster": (148, 5),
    },
    # The spec's independent boundary check: VMS$DISK_CL_DRVR byte [102] takes
    # the LAVC node numbers of the five lab nodes. All 101 frames are VAX-sourced
    # (OVMX runs no such SYSAP), so the guard does not touch this row.
    "disk_cl_drvr_node_bytes": [0x01, 0x02, 0x03, 0x1a, 0x4b],
    # --- the value OVMX adopts, and where it comes from ---
    "ovmx_value": "01 1b 01 03 00 00 00 00 00 00 00 08 00 00 06 00",
    "member_value_in_specimen": "01 1b 01 03 01 00 01 00 02 00 01 08 00 00 06 00",
    # In JOIN_SPECIMEN the joiner emits ONE value for both message types.
    "specimen_joiner_connect_req": 1,
    "specimen_joiner_accept_req": 1,
    "specimen_joiner_distinct_values": 1,
    # Independent attestation of the adopted value: VAX-sourced VMS$VAXcluster
    # frames carrying it OUTSIDE the specimen, and how many distinct VAX nodes
    # emit it. Without this the adopted value would rest on the specimen alone.
    "adopted_value_vax_frames": 40,
    "adopted_value_vax_frames_outside_specimen": 38,
    "adopted_value_vax_nodes": 3,
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


def classify_source(src):
    """VAX, OVMX, or None when the structural rule cannot place the source.

    None is a FAILURE, not a shrug: an unplaceable source must not silently
    land in either population. See the circular-grounding guard above.
    """
    if src in OVMX_HW_MACS:
        return OVMX
    if src.startswith(VAX_SOURCE_OUIS):
        return VAX
    return None


def ovmx_macs_from_logs(logdir):
    """Re-derive OVMX's own MACs from what scsd logged (`hwmac=aa:bb:...`)."""
    found = set()
    pat = re.compile(r"hwmac=([0-9a-fA-F:]{17})")
    for path in sorted(glob.glob(os.path.join(logdir, "**", "*.log"), recursive=True)):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = pat.search(line)
                    if m:
                        found.add(m.group(1).lower())
        except OSError:
            continue
    return found


def _new_pop():
    return {
        "connect_frames": 0,
        "msgtype_histogram": collections.Counter(),
        "vaxcluster_frames": 0,
        "vaxcluster_version_quad": 0,
        "vaxcluster_tail": 0,
        "sysap_values": collections.defaultdict(collections.Counter),
        "name_field_ascii_violations": 0,
        "vaxcluster_sources": collections.Counter(),
        "source_macs": collections.Counter(),
    }


def measure(capdir):
    m = {
        "pcaps_scanned": 0,
        VAX: _new_pop(),
        OVMX: _new_pop(),
        "unclassified_sources": collections.Counter(),
        "specimen": collections.defaultdict(collections.Counter),
        "specimen_source_frames": collections.Counter(),
        # VAX-sourced VMS$VAXcluster frames carrying the adopted value, keyed
        # on (capture basename, source MAC).
        "adopted_value_sightings": collections.Counter(),
    }
    adopted = bytes.fromhex(EXPECTED["ovmx_value"].replace(" ", ""))
    for path in sorted(glob.glob(os.path.join(capdir, "**", "*.pcap"), recursive=True)):
        m["pcaps_scanned"] += 1
        base = os.path.basename(path)
        for pkt in pcap_frames(path):
            if len(pkt) < 16 or pkt[12:14] != b"\x60\x07":
                continue
            src = mac(pkt[6:12])
            if base == JOIN_SPECIMEN:
                m["specimen_source_frames"][src] += 1
            sca = pkt[14:]
            if len(sca) != 110 or sca[17] != 0x13 or sca[16] not in (0x4B, 0x5B, 0x7B):
                continue
            which = classify_source(src)
            if which is None:
                m["unclassified_sources"][src] += 1
                continue
            p = m[which]
            mt = struct.unpack("<H", sca[46:48])[0]
            p["msgtype_histogram"][mt] += 1
            if mt not in (0, 2):
                continue
            p["connect_frames"] += 1
            p["source_macs"][src] += 1
            local = sca[LOCAL_NAME[0]:LOCAL_NAME[1]]
            # The population claim: a connect frame's [62:78] is an ASCII SYSAP
            # name. Anything else means the split above is wrong.
            if not all(32 <= c < 127 for c in local):
                p["name_field_ascii_violations"] += 1
                continue
            name = local.decode("ascii").rstrip()
            cd = bytes(sca[CD_OFF:CD_END])
            p["sysap_values"][name][cd] += 1
            if name == "VMS$VAXcluster":
                p["vaxcluster_frames"] += 1
                p["vaxcluster_sources"][src] += 1
                if cd[0:4] == b"\x01\x1b\x01\x03":
                    p["vaxcluster_version_quad"] += 1
                if cd[11:16] == b"\x08\x00\x00\x06\x00":
                    p["vaxcluster_tail"] += 1
                if which is VAX and cd == adopted:
                    m["adopted_value_sightings"][(base, src)] += 1
            if base == JOIN_SPECIMEN and name == "VMS$VAXcluster":
                m["specimen"][(src, mt)][cd] += 1
    return m


def _report_pop(m, which, label, out):
    p = m[which]
    print("=== %s ===" % label, file=out)
    print("  source MACs                       : %s"
          % ", ".join("%s (n=%d)" % (s, n) for s, n in p["source_macs"].most_common()),
          file=out)
    print("  110-byte 0x?B13 msgtype histogram : %s"
          % dict(sorted(p["msgtype_histogram"].items())), file=out)
    print("  CONNECT_REQ/ACCEPT_REQ frames (0,2): %d" % p["connect_frames"], file=out)
    print("  SYSAP-name non-ASCII residuals    : %d" % p["name_field_ascii_violations"],
          file=out)
    print("  per-local-SYSAP connect-data census (payload [94:110]):", file=out)
    for name in sorted(p["sysap_values"]):
        vals = p["sysap_values"][name]
        print("    %-18s n=%-5d distinct=%d" % (name, sum(vals.values()), len(vals)),
              file=out)
        for cd, n in vals.most_common():
            asc = "".join(chr(c) if 32 <= c < 127 else "." for c in cd)
            print("          %5d  %s  |%s|" % (n, hexs(cd), asc), file=out)
    print("  VMS$VAXcluster invariant spans:", file=out)
    print("    [94:98]   == 01 1b 01 03     : %d/%d"
          % (p["vaxcluster_version_quad"], p["vaxcluster_frames"]), file=out)
    print("    [105:110] == 08 00 00 06 00  : %d/%d"
          % (p["vaxcluster_tail"], p["vaxcluster_frames"]), file=out)
    print("    distinct source nodes         : %d" % len(p["vaxcluster_sources"]), file=out)
    print(file=out)


def report(m, out=sys.stdout):
    print("pcaps scanned                       : %d" % m["pcaps_scanned"], file=out)
    print(file=out)
    print("--- CIRCULAR-GROUNDING GUARD (see the module docstring) ---", file=out)
    print("Every GROUNDED figure below the VAX heading is derived from the VAX", file=out)
    print("population ONLY. The OVMX population is OVMX's own transmissions: it", file=out)
    print("is evidence about OVMX's encoder and about nothing else.", file=out)
    print("  connect frames DROPPED as OVMX-sourced : %d of %d (%.1f%%)"
          % (m[OVMX]["connect_frames"],
             m[VAX]["connect_frames"] + m[OVMX]["connect_frames"],
             100.0 * m[OVMX]["connect_frames"]
             / max(1, m[VAX]["connect_frames"] + m[OVMX]["connect_frames"])),
          file=out)
    print("  VMS$VAXcluster frames DROPPED as OVMX  : %d of %d"
          % (m[OVMX]["vaxcluster_frames"],
             m[VAX]["vaxcluster_frames"] + m[OVMX]["vaxcluster_frames"]), file=out)
    if m["unclassified_sources"]:
        print("  UNCLASSIFIED source MACs (MUST be none): %s"
              % dict(m["unclassified_sources"]), file=out)
    else:
        print("  UNCLASSIFIED source MACs (MUST be none): none", file=out)
    print(file=out)

    _report_pop(m, VAX, "VAX POPULATION -- the evidence base", out)
    _report_pop(m, OVMX, "OVMX POPULATION -- excluded from every GROUNDED figure", out)

    print("=== %s ===" % JOIN_SPECIMEN, file=out)
    print("  0x6007 frames by source: %s" % dict(m["specimen_source_frames"]), file=out)
    print("  (OVMX is present in this capture as a bystander; it sources no", file=out)
    print("   VMS$VAXcluster connect frame in it, so the joiner/member contrast", file=out)
    print("   below is entirely VAX-to-VAX.)", file=out)
    print("  VMS$VAXcluster connect data by source and message type:", file=out)
    for (src, mt) in sorted(m["specimen"]):
        for cd, n in m["specimen"][(src, mt)].most_common():
            print("    %-18s mt=%d (%-11s) x%d  %s"
                  % (src, mt, "CONNECT_REQ" if mt == 0 else "ACCEPT_REQ", n, hexs(cd)),
                  file=out)
    print(file=out)
    print("=== the adopted value, attested outside the specimen (VAX-sourced only) ===",
          file=out)
    for (base, src), n in sorted(m["adopted_value_sightings"].items()):
        print("  %-50s %-18s x%d" % (base, src, n), file=out)


def check(m, logdir=None):
    fails = []
    ok = []
    vax, ovmx = m[VAX], m[OVMX]

    def cmp(label, got, want):
        (ok if got == want else fails).append("%-46s got=%r want=%r" % (label, got, want))

    cmp("pcaps_scanned", m["pcaps_scanned"], EXPECTED["pcaps_scanned"])

    # --- the guard itself, checked before anything it protects --------------
    cmp("unclassified_sources", sum(m["unclassified_sources"].values()),
        EXPECTED["unclassified_sources"])
    cmp("ovmx_connect_frames", ovmx["connect_frames"], EXPECTED["ovmx_connect_frames"])
    cmp("ovmx_msgtype_histogram", dict(ovmx["msgtype_histogram"]),
        EXPECTED["ovmx_msgtype_histogram"])
    cmp("ovmx_vaxcluster_frames", ovmx["vaxcluster_frames"],
        EXPECTED["ovmx_vaxcluster_frames"])
    cmp("ovmx_source_macs seen in captures", sorted(ovmx["source_macs"]),
        sorted(EXPECTED["ovmx_source_macs"]))
    # No OVMX MAC may hide in the VAX population.
    cmp("OVMX MACs inside the VAX population",
        sorted(s for s in vax["source_macs"] if s in OVMX_HW_MACS), [])
    if logdir and os.path.isdir(logdir):
        logged = ovmx_macs_from_logs(logdir)
        cmp("every scsd-logged hwmac is in OVMX_HW_MACS",
            sorted(logged - set(OVMX_HW_MACS)), [])
    else:
        ok.append("%-46s (skipped: no log directory)" % "scsd-logged hwmac cross-check")

    # --- every GROUNDED figure, VAX population ONLY -------------------------
    cmp("connect_frames (VAX)", vax["connect_frames"], EXPECTED["connect_frames"])
    cmp("msgtype_histogram (VAX)", dict(vax["msgtype_histogram"]),
        EXPECTED["msgtype_histogram"])
    cmp("name_field_non_ascii_residuals (VAX)", vax["name_field_ascii_violations"], 0)
    cmp("vaxcluster_frames (VAX)", vax["vaxcluster_frames"], EXPECTED["vaxcluster_frames"])
    cmp("vaxcluster_version_quad (VAX)", vax["vaxcluster_version_quad"],
        EXPECTED["vaxcluster_version_quad"])
    cmp("vaxcluster_tail (VAX)", vax["vaxcluster_tail"], EXPECTED["vaxcluster_tail"])
    cmp("vaxcluster_distinct_values (VAX)", len(vax["sysap_values"]["VMS$VAXcluster"]),
        EXPECTED["vaxcluster_distinct_values"])
    cmp("vaxcluster_source_nodes (VAX)", len(vax["vaxcluster_sources"]),
        EXPECTED["vaxcluster_source_nodes"])
    mid = {}
    for cd, n in vax["sysap_values"]["VMS$VAXcluster"].items():
        mid[hexs(cd[4:11])] = mid.get(hexs(cd[4:11]), 0) + n
    cmp("vaxcluster [98:105] value census (VAX)", mid, EXPECTED["vaxcluster_mid_values"])
    cmp("VMS$DISK_CL_DRVR [102] node bytes (VAX)",
        sorted({cd[8] for cd in vax["sysap_values"]["VMS$DISK_CL_DRVR"]}),
        EXPECTED["disk_cl_drvr_node_bytes"])
    cmp("VMS$DISK_CL_DRVR is OVMX-free",
        sum(ovmx["sysap_values"]["VMS$DISK_CL_DRVR"].values()), 0)
    for name, (n, distinct) in EXPECTED["sysap_census"].items():
        vals = vax["sysap_values"].get(name, {})
        cmp("sysap %s frames (VAX)" % name, sum(vals.values()), n)
        cmp("sysap %s distinct (VAX)" % name, len(vals), distinct)

    # The specimen: the joiner's single value, and the member's contrasting one.
    # Both endpoints are real VAXes; assert that, so the contrast cannot quietly
    # become an OVMX-vs-VAX comparison.
    cmp("specimen joiner is a VAX source", classify_source(VAX3_HW), VAX)
    cmp("specimen member is a VAX source", classify_source(VAX1_LOGICAL), VAX)
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
    # No OVMX-sourced VMS$VAXcluster connect frame may enter the specimen contrast.
    cmp("specimen contrast is OVMX-free",
        sorted({s for (s, _mt) in m["specimen"] if s in OVMX_HW_MACS}), [])

    # The adopted value must be attested by real VAXes beyond the specimen --
    # otherwise it rests on 2 frames and, worse, on frames OVMX shared a wire with.
    total = sum(m["adopted_value_sightings"].values())
    outside = sum(n for (base, _s), n in m["adopted_value_sightings"].items()
                  if base != JOIN_SPECIMEN)
    nodes = len({s for (_b, s) in m["adopted_value_sightings"]})
    cmp("adopted value: VAX frames", total, EXPECTED["adopted_value_vax_frames"])
    cmp("adopted value: VAX frames outside specimen", outside,
        EXPECTED["adopted_value_vax_frames_outside_specimen"])
    cmp("adopted value: distinct VAX nodes", nodes, EXPECTED["adopted_value_vax_nodes"])
    return ok, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--logs", default=DEFAULT_LOGDIR,
                    help="scsd log directory, used to re-derive OVMX's own MACs")
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

    ok, fails = check(m, args.logs)
    print()
    for line in ok:
        print("PASS  %s" % line)
    for line in fails:
        print("FAIL  %s" % line)
    print("\n%d checks, %d failure(s)" % (len(ok) + len(fails), len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
