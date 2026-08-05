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

=======================================================================
A MAC IS NOT A NODE -- IDENTITY COMES FROM THE FRAME, NOT FROM THE SOURCE MAC
=======================================================================

The guard above splits the POPULATION on the Ethernet source MAC, and that is
the right axis for "is this frame ours". It is the WRONG axis for "how many
independent nodes agree", and this script previously conflated the two. Both
directions of the error are real in this capture library, and both were
measured:

  * ONE NODE, TWO MACs. `08:00:2b:4a:b7:15` (the DEC NIC hardware address) and
    `aa:00:04:00:01:04` (the DECnet-assigned logical address that replaces it
    once DECnet starts) are the SAME machine: both name themselves `VAX1` with
    SCSSYSTEMID 1025 in their own START frames.
  * ONE MAC, THREE NODES. `08:00:2b:78:56:b9` was reconfigured across reboots
    and appears as `VAX2` (1026), `VX3` (1050) and `ZK` (1099).

So a source-MAC count can be too low AND too high at once, and "N distinct
sources agree" cannot be read off it. Two finer counts are derived instead,
both from the frames themselves:

  1. NODE IDENTITY, per frame. Every connect frame carries the sender's own
     LAVC logical address at payload [10:16] in the form aa:00:04:00:NN:04,
     where NN is the LAVC node number = SCSSYSTEMID & 1023 (spec sec 4g). This
     is checked, not assumed: whenever the ETHERNET source is itself an
     aa:00:04 address, [10:16] equals it (0 mismatches), and no VAX-sourced
     connect frame carries any other form (0 residuals). NN is then resolved to
     the ASCII node NAME through the 106-byte START frames, which carry the
     name at payload [90:98] and the SCSSYSTEMID at [46:48] (spec sec 4g); the
     script requires that map to be a bijection and to cover every node number
     that appears in the connect census.
  2. HARDWARE SOURCE. Node identities that share a MAC, and MACs that share a
     node identity, are the same lab machine. The connected components of the
     MAC<->node-identity graph are the distinct machines. This is the
     CONSERVATIVE count and it is the one an "independent sources agree" claim
     must use: VX3 and ZK are separate cluster members but the same reconfigured
     box, so they are not independent observations of VMS behaviour.

A third, independent confirmation of the MAC split falls out of this: the node
numbers the VAX population emits and the ones the OVMX population emits must be
DISJOINT sets. A misclassified source would show up as a node number in the
wrong population.

=======================================================================
WHAT THAT COUNT IS WORTH -- THE LAB'S ACTUAL CONFIGURATION
=======================================================================

The hardware-source count is a real derived quantity, but it must NOT be read
as "3 independent VMS systems", and this script previously called them exactly
that ("3 independent hardware sources", "distinct lab machines"). That was the
second false claim the veracity review threw out, and it was the more dangerous
one because it was the conservative figure the first correction retreated TO.
The lab's real configuration, from /data/training/vax/cluster/README-lab.md and
the shared cluster/vax.ini:

  * 3 EMULATOR INSTANCES -- vax1, vax2, vax3 -- every one of them the same
    emulated model (MicroVAX 3900 / KA655) under the same SIMH binary on one
    Linux host. There is NO physical hardware diversity in this lab at all;
    "hardware source" here means EMULATOR INSTANCE and nothing stronger.
  * 3 SYSTEM ROOTS -- [SYS0] (VAX1), [SYS1] (VAX2, and VX3/1050 and ZK/1099,
    which are that same root re-identified with MC SYSGEN between reboots), and
    [SYS11] (VAX3, a diskless satellite whose root is MSCP-served).
  * 1 SYSTEM DISK IMAGE -- all three roots live in the single file
    data/d0.dsk. vax1 and vax2 attach it at the same time (dual-ported:
    `attach rq0 ../data/d0.dsk` in the shared vax.ini) and vax3 reads its root
    out of it over MSCP.
  * 1 VMS INSTALLATION -- one OpenVMS VAX V7.3 install, whose SYS$COMMON
    executive images all three roots share. This half is MEASURED, not merely
    read off the lab notes: every one of the 668 VAX-sourced 106-byte START
    frames in the library reports version [58:66] = "VMS V7.3" on hardware
    [74:78] = "VAX " -- ONE distinct version string across all 5 node
    identities (spec sec 4g grounds both fields).

SO THE HONEST ATTESTATION BEHIND EVERY GROUNDED FIGURE HERE IS: ONE VMS BUILD,
UNDER THREE SYSTEM ROOTS, ON ONE SYSTEM DISK IMAGE, ACROSS THREE EMULATOR
INSTANCES. Three roots of one installation agreeing about a connect-data byte
is much closer to ONE OBSERVATION REPEATED than to three independent
confirmations, and nothing here may be presented as the latter.

What the census DOES establish is that the value is stable across node
identity, node number, system root, boot, incarnation and role (joiner vs
member) -- worth having, and it is the whole of it. What it CANNOT establish is
anything at all about another VMS version, another build, or a second
installation of the same version: the sample contains exactly one of each.
Spec sec 5 carries that as a standing limit on sec 4(N).

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

# THE LAB FENCE (vms-096). DEFAULT_CAPDIR is the LAB-1 grounding library and
# every figure in EXPECTED is a lab-1 measurement. lab-2 replicas reuse lab-1's
# SCSSYSTEMIDs and node MACs by design (tests/lab/README.md), so a lab-2
# capture deposited here silently moves every census -- six 2026-08-05
# vaxlab-4 captures did exactly that and put 18 of this script's 67 checks red,
# including inventing a fifth "VAX" hardware source MAC. They now live in the
# /data/training/vax/cluster/captures-lab2 SIBLING; a sibling and not a
# subdirectory precisely because the sweep below globs `**/*.pcap` RECURSIVELY.
# Full rationale in tools/cluster/scs_disc_measure.py.
#
# vms-beb: checked against the declared manifest in
# tools/cluster/capture_manifest.py now, not this filename marker -- an
# unknown or mislabeled capture reds too, not just one carrying "-lab2-".
LAB2_MARKER = "-lab2-"   # kept for readers who still grep for it; unused below


def lab1_only(paths):
    """Return `paths` unchanged, or die naming every non-lab-1 capture in
    them -- checked against tools/cluster/capture_manifest.py's declared
    manifest. Imports it LAZILY from the sibling tools/cluster/ directory
    (this script lives one level up, in tools/).
    """
    cluster_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cluster")
    if cluster_dir not in sys.path:
        sys.path.insert(0, cluster_dir)
    import capture_manifest
    return capture_manifest.check_paths(paths, capture_manifest.LAB1)
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

# --- node identity, see "A MAC IS NOT A NODE" above -----------------------
# The sender's own LAVC logical address inside the connect frame, and the two
# START-frame fields the node number is resolved to a name through.
SRC_LAVC = (10, 16)          # payload-relative, form aa:00:04:00:NN:04
LAVC_PREFIX = b"\xaa\x00\x04\x00"
START_LEN, START_OPCODE = 106, 0x41
START_NAME = (90, 98)        # 8-byte blank-padded ASCII node name (spec 4g)
START_SCSSYSTEMID = (46, 48)  # LE u16 (spec 4g); node number == id & 1023
START_VERSION = (58, 66)     # 8-byte ASCII VMS version, e.g. "VMS V7.3" (4g)
START_HARDWARE = (74, 78)    # 4-byte ASCII hardware family, e.g. "VAX " (4g)
LAVC_NODE_MASK = 1023

# --- the lab configuration behind the counts, see the docstring note ------
# NOT capture-derived and never presented as such: a pcap cannot show which
# system root a node booted or which file its disk is. Sourced from
# /data/training/vax/cluster/README-lab.md and cluster/vax.ini, declared here
# so the honesty statement in scs_connect.h and spec sec 4(N) is checkable
# against something, and cross-checked below against the measured census.
LAB_SYSTEM_ROOT = {"VAX1": "SYS0", "VAX2": "SYS1", "VX3": "SYS1",
                   "ZK": "SYS1", "VAX3": "SYS11"}
LAB_SYSTEM_DISK_IMAGES = ("data/d0.dsk",)
LAB_EMULATOR_INSTANCES = ("vax1", "vax2", "vax3")
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
    # The per-SYSAP counts the guard dropped -- the spec sec 4(N) table's last
    # column. Pinned so that column re-derives like every other figure.
    "ovmx_sysap_census": {
        "MSCP$DISK": 243,
        "SCS$DIRECTORY": 113,
        "SCS$DIR_LOOKUP": 55,
        "VMS$VAXcluster": 55,
    },
    "ovmx_source_macs": ["b6:16:8a:dc:3a:53"],   # the only one that appears in captures
    # --- the VMS$VAXcluster subset and its two invariant spans (VAX-only) ---
    "vaxcluster_frames": 148,
    "vaxcluster_version_quad": 148,      # [94:98] == 01 1b 01 03
    "vaxcluster_tail": 148,              # [105:110] == 08 00 00 06 00
    "vaxcluster_distinct_values": 5,
    # --- identity, NOT MACs (see "A MAC IS NOT A NODE" above) ---------------
    # Kept only so the discrepancy stays visible: 4 is the number of distinct
    # VAX source MACs carrying VMS$VAXcluster connect data. It is NOT a node
    # count and no prose figure may be derived from it -- it is simultaneously
    # too high (VAX1 sources from two MACs) and too low (one MAC carries three
    # node identities). Use vaxcluster_node_census / vaxcluster_hardware_sources.
    "vaxcluster_source_macs": 4,
    # The census by NODE IDENTITY, resolved from each frame's own LAVC node
    # number and named through the START frames. Sums to vaxcluster_frames.
    "vaxcluster_node_census": {
        "VAX1": 74, "VAX2": 32, "VAX3": 36, "VX3": 3, "ZK": 3,
    },
    "vaxcluster_node_identities": 5,
    # Connected components of the MAC <-> node-identity graph. This is the
    # CONSERVATIVE count -- it is NOT an independence count. See "WHAT THAT
    # COUNT IS WORTH" above: these are three SIMH instances of one emulated
    # model on one host, booting three roots of ONE VMS installation off ONE
    # disk image, so they are not independent observations of VMS behaviour.
    "vaxcluster_hardware_sources": 3,
    # --- what the attestation actually rests on (the configuration note) ---
    # MEASURED from the 106-byte START frames, VAX-sourced only:
    "vax_start_frames": 668,
    "vax_vms_versions": ["VMS V7.3"],    # [58:66], 1 distinct over 5 identities
    "vax_hardware_strings": ["VAX "],    # [74:78], 1 distinct
    # DECLARED lab configuration (LAB_* above), cross-checked against the
    # census. Not measured; a capture cannot show a system root.
    "lab_vms_installations": 1,
    "lab_system_roots": 3,
    "lab_system_disk_images": 1,
    "lab_emulator_instances": 3,
    # LAVC node number -> ASCII node name, over the whole VAX population.
    "vax_node_names": {1: "VAX1", 2: "VAX2", 3: "VAX3", 26: "VX3", 75: "ZK"},
    # The three lab machines, as (source MACs, node identities). Derived, not
    # asserted: this is what the MAC<->identity graph decomposes into.
    "hardware_source_groups": [
        (["08:00:2b:11:22:33"], ["VAX3"]),
        (["08:00:2b:4a:b7:15", "aa:00:04:00:01:04"], ["VAX1"]),
        (["08:00:2b:78:56:b9"], ["VAX2", "VX3", "ZK"]),
    ],
    # Identity plumbing that must hold for the two counts above to mean anything.
    "vax_srclavc_residuals": 0,        # VAX connect frames NOT in aa:00:04:00:NN:04 form
    "vax_srclavc_mismatches": 0,       # aa:00:04-sourced frames whose [10:16] != their MAC
    "unnamed_vax_node_numbers": [],    # every node number in the census has a START name
    "node_numbers_in_both_populations": [],   # VAX and OVMX node numbers are disjoint
    # Per-SYSAP identity counts, so no row can quietly go back to citing MACs.
    "sysap_node_identities": {
        "MSCP$DISK": 5, "SCA$TRANSPORT": 5, "SCS$DIRECTORY": 5,
        "SCS$DIR_LOOKUP": 5, "VMS$DISK_CL_DRVR": 5, "VMS$VAXcluster": 5,
    },
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
    # Node identities, and the conservative hardware-source count. The old
    # figure here was 3 DISTINCT SOURCE MACS, which is not a node count.
    "adopted_value_vax_node_identities": 5,
    "adopted_value_vax_hardware_sources": 3,
    "adopted_value_vax_captures": 18,
    # This script's own check total, quoted by both documents as "Last full
    # run: N checks, 0 failures". Self-referential on purpose -- see the
    # measure_check_count cmp() at the end of check().
    "measure_check_count": 67,
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


def src_lavc_node(sca):
    """The sender's own LAVC node number, read out of the frame itself.

    Returns None when payload [10:16] is not in the aa:00:04:00:NN:04 form --
    which is a residual to be counted, never a frame to be quietly attributed.
    """
    a = sca[SRC_LAVC[0]:SRC_LAVC[1]]
    if len(a) != 6 or a[:4] != LAVC_PREFIX or a[5] != 0x04:
        return None
    return a[4]


def hardware_components(mac_nodes):
    """Connected components of the MAC <-> node-identity graph.

    Two MACs that ever present the same node identity, and two node identities
    that ever share a MAC, are the same lab machine. Returns a list of
    (sorted MACs, sorted node numbers).
    """
    node_macs = collections.defaultdict(set)
    for src, nodes in mac_nodes.items():
        for n in nodes:
            node_macs[n].add(src)
    seen, comps = set(), []
    for start in sorted(mac_nodes):
        if start in seen:
            continue
        macs, nodes, stack = set(), set(), [("m", start)]
        while stack:
            kind, v = stack.pop()
            if kind == "m":
                if v in macs:
                    continue
                macs.add(v)
                stack.extend(("n", n) for n in mac_nodes[v])
            else:
                if v in nodes:
                    continue
                nodes.add(v)
                stack.extend(("m", s) for s in node_macs[v])
        seen |= macs
        comps.append((sorted(macs), sorted(nodes)))
    return comps


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
        # identity, per frame (see "A MAC IS NOT A NODE")
        "vaxcluster_nodes": collections.Counter(),   # node number -> frames
        "mac_nodes": collections.defaultdict(set),   # MAC -> node numbers
        "sysap_nodes": collections.defaultdict(set),  # SYSAP -> node numbers
        "srclavc_residuals": 0,
        "srclavc_mismatches": 0,
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
        # on (capture basename, node identity).
        "adopted_value_sightings": collections.Counter(),
        # LAVC node number -> ASCII node name, from the 106-byte START frames.
        "node_names": collections.defaultdict(collections.Counter),
        # VAX-sourced START frames only: how many VMS versions and hardware
        # families the whole VAX population actually reports. This is the
        # measured half of "one VMS build" (see the configuration note).
        "vax_start_frames": 0,
        "vax_start_versions": collections.Counter(),
        "vax_start_hardware": collections.Counter(),
    }
    adopted = bytes.fromhex(EXPECTED["ovmx_value"].replace(" ", ""))
    for path in lab1_only(sorted(glob.glob(os.path.join(capdir, "**", "*.pcap"),
                                           recursive=True))):
        m["pcaps_scanned"] += 1
        base = os.path.basename(path)
        for pkt in pcap_frames(path):
            if len(pkt) < 16 or pkt[12:14] != b"\x60\x07":
                continue
            src = mac(pkt[6:12])
            if base == JOIN_SPECIMEN:
                m["specimen_source_frames"][src] += 1
            sca = pkt[14:]
            # START frames name the nodes: node number (SCSSYSTEMID & 1023) ->
            # ASCII node name. This is how a connect frame's LAVC node number
            # becomes an identity a human can check against the lab.
            if (len(sca) == START_LEN and sca[16] == START_OPCODE
                    and sca[17] == 0x13):
                nm = sca[START_NAME[0]:START_NAME[1]]
                sid = struct.unpack("<H", sca[START_SCSSYSTEMID[0]:
                                              START_SCSSYSTEMID[1]])[0]
                if all(32 <= c < 127 for c in nm) and nm.strip():
                    m["node_names"][sid & LAVC_NODE_MASK][nm.decode().strip()] += 1
                    # VAX-only: OVMX's own START frames say "VMX V0.1" (and,
                    # where it replays a captured body, "VMS V7.3"), which is
                    # evidence about OVMX and must not enter this count.
                    if classify_source(src) == VAX:
                        m["vax_start_frames"] += 1
                        m["vax_start_versions"][
                            sca[START_VERSION[0]:START_VERSION[1]]
                            .decode("latin-1")] += 1
                        m["vax_start_hardware"][
                            sca[START_HARDWARE[0]:START_HARDWARE[1]]
                            .decode("latin-1")] += 1
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
            # Identity, read out of THIS frame -- not inferred from the MAC.
            node = src_lavc_node(sca)
            if node is None:
                p["srclavc_residuals"] += 1
            else:
                p["mac_nodes"][src].add(node)
                if src.startswith("aa:00:04") and mac(
                        sca[SRC_LAVC[0]:SRC_LAVC[1]]) != src:
                    p["srclavc_mismatches"] += 1
            local = sca[LOCAL_NAME[0]:LOCAL_NAME[1]]
            # The population claim: a connect frame's [62:78] is an ASCII SYSAP
            # name. Anything else means the split above is wrong.
            if not all(32 <= c < 127 for c in local):
                p["name_field_ascii_violations"] += 1
                continue
            name = local.decode("ascii").rstrip()
            cd = bytes(sca[CD_OFF:CD_END])
            p["sysap_values"][name][cd] += 1
            if node is not None:
                p["sysap_nodes"][name].add(node)
            if name == "VMS$VAXcluster":
                p["vaxcluster_frames"] += 1
                p["vaxcluster_sources"][src] += 1
                if node is not None:
                    p["vaxcluster_nodes"][node] += 1
                if cd[0:4] == b"\x01\x1b\x01\x03":
                    p["vaxcluster_version_quad"] += 1
                if cd[11:16] == b"\x08\x00\x00\x06\x00":
                    p["vaxcluster_tail"] += 1
                if which is VAX and cd == adopted:
                    m["adopted_value_sightings"][(base, node)] += 1
            if base == JOIN_SPECIMEN and name == "VMS$VAXcluster":
                m["specimen"][(src, mt)][cd] += 1
    return m


def node_name(m, node):
    """The ASCII node name for a LAVC node number, or None if unnamed."""
    c = m["node_names"].get(node)
    return c.most_common(1)[0][0] if c else None


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
    print("    distinct source MACs          : %d  (NOT a node count)"
          % len(p["vaxcluster_sources"]), file=out)
    print("  identity (from each frame's own LAVC node number, named via START):",
          file=out)
    print("    by node identity              : %s"
          % ", ".join("%s(%d)=%d" % (node_name(m, n) or "UNNAMED", n, c)
                      for n, c in sorted(p["vaxcluster_nodes"].items())), file=out)
    print("    distinct node identities      : %d" % len(p["vaxcluster_nodes"]),
          file=out)
    comps = hardware_components(p["mac_nodes"])
    print("    distinct hardware sources     : %d  (emulator instances -- NOT"
          " an independence count, see the configuration note)" % len(comps),
          file=out)
    for macs, nodes in comps:
        print("        %-42s %s"
              % (",".join(macs), [node_name(m, n) or "node %d" % n for n in nodes]),
              file=out)
    print("    src-LAVC residuals / mismatches: %d / %d"
          % (p["srclavc_residuals"], p["srclavc_mismatches"]), file=out)
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

    print("--- WHAT THE ATTESTATION RESTS ON (see the module docstring) ---", file=out)
    print("The hardware-source count below is EMULATOR INSTANCES, not independent", file=out)
    print("VMS systems. The honest attestation is:", file=out)
    print("  VMS installations (declared)           : %d  (roots share SYS$COMMON)"
          % EXPECTED["lab_vms_installations"], file=out)
    print("  system roots (declared)                : %d  %s"
          % (len(set(LAB_SYSTEM_ROOT.values())),
             sorted(set(LAB_SYSTEM_ROOT.values()))), file=out)
    print("  system disk images (declared)          : %d  %s"
          % (len(LAB_SYSTEM_DISK_IMAGES), list(LAB_SYSTEM_DISK_IMAGES)), file=out)
    print("  emulator instances (declared)          : %d  %s"
          % (len(LAB_EMULATOR_INSTANCES), list(LAB_EMULATOR_INSTANCES)), file=out)
    print("  VAX START frames (MEASURED)            : %d" % m["vax_start_frames"],
          file=out)
    print("  VMS version strings [58:66] (MEASURED) : %s"
          % dict(m["vax_start_versions"]), file=out)
    print("  hardware strings [74:78] (MEASURED)    : %s"
          % dict(m["vax_start_hardware"]), file=out)
    print("  identity -> system root (declared)     : %s"
          % dict(sorted(LAB_SYSTEM_ROOT.items())), file=out)
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
    for (base, nd), n in sorted(m["adopted_value_sightings"].items(),
                                key=lambda kv: (kv[0][0], kv[0][1] or 0)):
        print("  %-50s %-10s x%d"
              % (base, node_name(m, nd) or "node %s" % nd, n), file=out)


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
    cmp("ovmx_sysap_census (what the guard dropped, per SYSAP)",
        {k: sum(v.values()) for k, v in ovmx["sysap_values"].items()},
        EXPECTED["ovmx_sysap_census"])
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
    # --- identity: a MAC is not a node ---------------------------------------
    # The MAC count is checked only so its divergence from the identity counts
    # stays on the record; no prose figure may be taken from it.
    cmp("vaxcluster_source_MACS (NOT a node count)", len(vax["vaxcluster_sources"]),
        EXPECTED["vaxcluster_source_macs"])
    cmp("vax src-LAVC residuals", vax["srclavc_residuals"],
        EXPECTED["vax_srclavc_residuals"])
    cmp("vax src-LAVC vs Ethernet-source mismatches", vax["srclavc_mismatches"],
        EXPECTED["vax_srclavc_mismatches"])
    # Every node number in the VAX connect census must resolve to exactly one
    # ASCII node name, and each name to exactly one number.
    vax_nodes = set(vax["vaxcluster_nodes"])
    for _n, vals in vax["sysap_nodes"].items():
        vax_nodes |= vals
    cmp("unnamed VAX node numbers",
        sorted(n for n in vax_nodes if node_name(m, n) is None),
        EXPECTED["unnamed_vax_node_numbers"])
    cmp("node number -> name is 1:1 over the VAX population",
        {n: node_name(m, n) for n in sorted(vax_nodes)},
        EXPECTED["vax_node_names"])
    cmp("ambiguous node numbers (more than one name)",
        sorted(n for n in vax_nodes if len(m["node_names"].get(n, {})) > 1), [])
    # A misclassified source would surface here as a node number in both.
    ovmx_nodes = set(ovmx["vaxcluster_nodes"])
    for _n, vals in ovmx["sysap_nodes"].items():
        ovmx_nodes |= vals
    cmp("node numbers appearing in BOTH populations",
        sorted(vax_nodes & ovmx_nodes), EXPECTED["node_numbers_in_both_populations"])
    # The census by identity, and the two counts prose may quote.
    cmp("vaxcluster census by node identity (VAX)",
        {node_name(m, n): c for n, c in vax["vaxcluster_nodes"].items()},
        EXPECTED["vaxcluster_node_census"])
    cmp("vaxcluster census by identity sums to vaxcluster_frames",
        sum(vax["vaxcluster_nodes"].values()), EXPECTED["vaxcluster_frames"])
    cmp("vaxcluster_node_identities (VAX)", len(vax["vaxcluster_nodes"]),
        EXPECTED["vaxcluster_node_identities"])
    comps = hardware_components(vax["mac_nodes"])
    cmp("vaxcluster_hardware_sources (VAX)", len(comps),
        EXPECTED["vaxcluster_hardware_sources"])
    cmp("hardware source groups (MACs, node names)",
        sorted((macs, [node_name(m, n) for n in nodes]) for macs, nodes in comps),
        sorted((list(macs), list(names))
               for macs, names in EXPECTED["hardware_source_groups"]))
    cmp("per-SYSAP distinct node identities (VAX)",
        {k: len(v) for k, v in vax["sysap_nodes"].items()},
        EXPECTED["sysap_node_identities"])
    # --- what the attestation rests on (see "WHAT THAT COUNT IS WORTH") ------
    # MEASURED: the whole VAX population reports ONE VMS version on ONE
    # hardware family. If a second VMS build ever reaches this wire, this reds
    # and the "one VMS build" prose has to be re-derived rather than kept.
    cmp("VAX START frames (MEASURED)", m["vax_start_frames"],
        EXPECTED["vax_start_frames"])
    cmp("VMS version strings over the VAX population (MEASURED)",
        sorted(m["vax_start_versions"]), EXPECTED["vax_vms_versions"])
    cmp("hardware strings over the VAX population (MEASURED)",
        sorted(m["vax_start_hardware"]), EXPECTED["vax_hardware_strings"])
    # DECLARED configuration, cross-checked against the MEASURED census: the
    # root map must cover exactly the identities the census found, so a new
    # node identity cannot appear on the wire without the honesty statement
    # being re-derived for it.
    cmp("declared system roots cover exactly the census identities",
        sorted(LAB_SYSTEM_ROOT), sorted(EXPECTED["vaxcluster_node_census"]))
    cmp("distinct system roots (declared)", len(set(LAB_SYSTEM_ROOT.values())),
        EXPECTED["lab_system_roots"])
    cmp("distinct system disk images (declared)", len(LAB_SYSTEM_DISK_IMAGES),
        EXPECTED["lab_system_disk_images"])
    cmp("distinct emulator instances (declared)", len(LAB_EMULATOR_INSTANCES),
        EXPECTED["lab_emulator_instances"])
    # One installation, because all the roots are on one disk image sharing one
    # SYS$COMMON -- so this may never exceed the disk-image count.
    cmp("VMS installations (declared) <= system disk images",
        EXPECTED["lab_vms_installations"] <= EXPECTED["lab_system_disk_images"],
        True)
    cmp("VMS installations (declared) matches the measured version count",
        EXPECTED["lab_vms_installations"], len(m["vax_start_versions"]))
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
    ident = {n for (_b, n) in m["adopted_value_sightings"]}
    caps = {b for (b, _n) in m["adopted_value_sightings"]}
    cmp("adopted value: VAX frames", total, EXPECTED["adopted_value_vax_frames"])
    cmp("adopted value: VAX frames outside specimen", outside,
        EXPECTED["adopted_value_vax_frames_outside_specimen"])
    cmp("adopted value: distinct VAX node identities", len(ident),
        EXPECTED["adopted_value_vax_node_identities"])
    cmp("adopted value: distinct hardware sources",
        len([c for c in comps if set(c[1]) & ident]),
        EXPECTED["adopted_value_vax_hardware_sources"])
    cmp("adopted value: distinct captures", len(caps),
        EXPECTED["adopted_value_vax_captures"])
    # How many checks this script runs is itself a figure the two documents
    # quote ("Last full run: N checks, 0 failures"), and it drifted: the
    # documents said 57 after the script had grown to 66. Pinning it here means
    # adding a check without re-running and re-recording reds, and the ctest
    # gate can hold the documents to the same number without the captures.
    cmp("measure_check_count (this script's own total)",
        len(ok) + len(fails) + 1, EXPECTED["measure_check_count"])
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
