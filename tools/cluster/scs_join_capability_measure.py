#!/usr/bin/env python3
"""scs_join_capability_measure.py - vms-70e2: re-derive, from the raw lab-2
captures, WHICH SCA traffic an OVMX daemon puts on the wire when it joins a
VMScluster and which it does not.

WHY THIS EXISTS
---------------
`vms-70e2` set out to prove a same-identity REJOIN end to end. It could not get
that far, because the POSITIVE CONTROL failed: an SCSD built from
`work/vms-187-closure` cannot complete a FIRST join either. A matched bracket on
one lab-2 pod, three runs inside twelve minutes with only the BINARY varying,
established that -- and this script is what turns the bracket into a number
anyone can re-derive from the captures instead of from a log or a diff.

THE BRACKET (lab-2 `vaxlab-4`, a virgin pod, 2026-08-05)

    A1   SCSD from work/vms-187-closure         OVMXA1 / 1410   NOT JOINED
    A0   SCSD from worktree-760-active-directory OVMXA0 / 1412  JOINED, t+13 s
    A3   SCSD from work/vms-187-closure         OVMXA3 / 1413   NOT JOINED

Guardrail 18: every identity is proven on the wire, not from a log -- the
`identity` figure below is the set of `OVMX??` strings found in the capture
itself. Guardrail 20: the closing control (A3) ran AFTER the joining one, so
"the lab stopped admitting anyone" is excluded.

WHAT IT MEASURES, PER CAPTURE
-----------------------------
(A) `cm_190_tx` / `cm_190_rx` -- SCA frames of the 190-byte fixed class (spec
    sec 4(d): the class the connection-manager membership dialogue rides,
    sec 4(g)/4(j)) between the OVMX MAC and any peer, counted per direction.
    This is the join dialogue itself, measured without trusting any opcode
    offset: only the length class and the two MACs.

(B) `ctl_tx` / `ctl_rx` -- the SCA connection-control message type at payload
    [46:48] (spec sec 4(h)(1a)), per direction, over the connection-control
    length classes. This is what names the ONE frame the failing binary owes
    and never sends.

(C) `identity` -- the `OVMX??` node names present in the capture bytes.

THE RESULT, IN ONE LINE: the joining binary transmits 514 CM frames and receives
583; the failing binary transmits 3, receives 0, and never sends the ACCEPT_RSP
(message type 3) that its own state machine logs as required-but-unemitted.

THREE BRACKETS LIVE HERE, in the order they were measured. Each has its own
EXPECTED/CAPTURES pair and its own pod, and they are NEVER folded together --
each one's gate asserts a SHAPE that a merged dict could not state.

    EXPECTED      vms-70e2  vaxlab-4  does the closure branch join at all?  NO
    EXPECTED_578  vms-578   vaxlab-4  does the INTEGRATED tree join?        YES
    EXPECTED_449  vms-449   vaxlab-6  can a returning identity REJOIN?      NO

    tools/cluster/scs_join_capability_measure.py            # PASS/FAIL vs EXPECTED
    tools/cluster/scs_join_capability_measure.py --print    # just print

Requires the lab captures, host-only and NOT in git (CLAUDE.md rule 8):
/data/training/vax/cluster/captures-lab2/vms{70e2,578}-*.pcap. Override with
--captures.

THIS IS THE ONE MEASUREMENT TOOL THAT READS LAB-2, and the reason the captures
it reads live in a SEPARATE directory (vms-096). Every other measurement tool
here is grounded in LAB-1 -- the hand-run SIMH cluster under
/data/training/vax/cluster/captures -- and globs that directory. lab-2 replicas
reuse lab-1's SCSSYSTEMIDs and node MACs by design (tests/lab/README.md), so
while these six captures sat in the lab-1 library they silently moved every
lab-1 census: scs_disc_measure (23/34 checks red), scs_reason_measure (13/27),
scs_connect_data_measure (18/67) and scs_credit_measure (11/30). Those four now
carry a `lab1_only()` fence that dies loudly if a `-lab2-` capture reappears
there. Do not move these files back.

`ctest -R scs_join_capability_figures` does not REQUIRE them: it exercises this
file's decoder against synthesized frames and asserts every figure in EXPECTED
still appears in docs/cluster-protocol-spec.md sec 4(O). When the captures ARE
present it also calls rederive() below and re-derives BOTH brackets -- vms-70e2's
and the vms-578 acceptance bracket, which until vms-371 was pinned by no gate at
all -- reddening on any figure the packets no longer support (vms-371).

CLEAN-ROOM (rule 8): every byte read here is from OUR OWN captures off OUR OWN
lab. No VSI/HPE source or binary was consulted. The [46:48] message-type
location is spec sec 4(h)(1a), grounded in vms-dd5.
"""
import argparse
import os
import re
import struct
import sys

# The SCA length classes that carry a connection-control message type. sec
# 4(h)(1a) grounds {62, 66, 110} for the four FORMATION messages and vms-591
# added 58 for the two RESPONSE messages, which are shorter than all three.
#
# 94 and 106 are DELIBERATELY NOT HERE. Both classes have a live [46:48] and
# reading it as a message type produces figures that look plausible and are
# meaningless: the 106-byte class is the START frame, whose [46:48] is the
# sender's SCSSYSTEMID (it decodes as 1410/1412/1413 -- the three identities of
# this bracket), and the 94-byte class is not connection control either. This
# is the vms-591 sampling error in the other direction, and it was caught by
# running the measurement rather than by reasoning about it.
CTL_CLASSES = (58, 62, 66, 110)

# The fixed SCS message class the CM membership dialogue rides (spec sec 4(d)).
CM_CLASS = 190

# Payload offset of the SCA connection-control message type (spec sec 4(h)(1a)).
MSGTYPE_OFF = 46

MSGTYPE_NAMES = {
    0: "CONNECT_REQ", 1: "CONNECT_RSP", 2: "ACCEPT_REQ", 3: "ACCEPT_RSP",
    4: "REJECT_REQ", 5: "REJECT_RSP", 6: "DISCONNECT_REQ", 7: "DISCONNECT_RSP",
}

# ---------------------------------------------------------------------------
# THE CHECKED-IN MEASUREMENT
# ---------------------------------------------------------------------------
# Every figure quoted in docs/cluster-protocol-spec.md sec 4(O) comes from here.
# `ctl_tx` lists only the message types OVMX itself emitted, because the whole
# finding is about what OVMX does and does not build.
EXPECTED = {
    "pod": "vaxlab-4",
    "lab": "lab-2",
    "date": "2026-08-05",
    "runs": {
        "A1": {
            "branch": "work/vms-187-closure",
            "identity": ["OVMXA1"],
            "joined": False,
            "cm_190_tx": 3,
            "cm_190_rx": 0,
            "ctl_tx": {0: 1, 1: 1, 2: 1, 6: 2},
            "accept_rsp_tx": 0,
        },
        "A0": {
            "branch": "worktree-760-active-directory",
            "identity": ["OVMXA0"],
            "joined": True,
            "cm_190_tx": 514,
            "cm_190_rx": 583,
            "ctl_tx": {0: 2, 1: 6, 2: 2, 3: 2, 4: 4, 6: 2, 7: 2, 9: 2},
            "accept_rsp_tx": 2,
        },
        "A3": {
            "branch": "work/vms-187-closure",
            "identity": ["OVMXA3"],
            "joined": False,
            "cm_190_tx": 3,
            "cm_190_rx": 0,
            "ctl_tx": {0: 1, 1: 1, 2: 1, 6: 2},
            "accept_rsp_tx": 0,
        },
    },
    # The OVMX tap MAC. Every replica of the lab-2 StatefulSet reuses it, which
    # is why the three runs share one address and are told apart by SCSNODE.
    "ovmx_mac": "4e:83:cd:c4:fe:54",
}

CAPTURES = {
    "A1": "vms70e2-A1-lab2-vaxlab4-20260805.pcap",
    "A0": "vms70e2-A0-lab2-vaxlab4-20260805.pcap",
    "A3": "vms70e2-A3-lab2-vaxlab4-20260805.pcap",
}

# The LAB-2 capture library -- a SIBLING of the lab-1 grounding library, not a
# subdirectory of it (scs_connect_data_measure.py globs `**/*.pcap`
# recursively, so a subdirectory would not fence it out). See the module
# docstring.
DEFAULT_CAPTURE_DIR = "/data/training/vax/cluster/captures-lab2"

ETHERTYPE_SCA = b"\x60\x07"


# ===========================================================================
# vms-578: THE SECOND BRACKET -- does the INTEGRATED tree join?
# ===========================================================================
# vms-70e2's bracket above measured that work/vms-187-closure CANNOT complete a
# first join and worktree-760-active-directory can. vms-578 merged the two and
# this is the acceptance measurement for that merge, taken the same way on the
# SAME pod (vaxlab-4) minutes apart, with the control BETWEEN the two runs of
# the binary under test rather than after them:
#
#     B1  integrated tree, default env
#     B3  worktree-760-active-directory, default env   <- the control
#     B2  integrated tree, default env
#
# All three JOINED (CLUSTER_NODES=3, XITDONE=1). Every figure below is
# re-derived from the captures by the same decoder as the bracket above; the
# identity of each run is proven ON THE WIRE, not from a log.
#
# HELD SEPARATE FROM EXPECTED["runs"] ON PURPOSE. The figures gate asserts the
# SHAPE of the vms-70e2 bracket -- exactly two runs of the failing binary around
# one of the joining one -- and folding three more runs into that dict would
# make that assertion unstatable. This dict is checked by main() below and
# leaves the gate's surface untouched.
EXPECTED_578 = {
    "pod": "vaxlab-4",
    "lab": "lab-2",
    "date": "2026-08-05",
    "runs": {
        "B1": {
            "branch": "work/vms-578 (integrated)",
            # OVMXA0 is a RESIDUE, not this run's identity: VAX1 still held the
            # CSB of the vms-70e2 A0 run on this pod and names it on the wire.
            # B2, run after another cycle, is clean. Recorded rather than
            # filtered -- a decoder that only ever sees one name is a decoder
            # nobody has tested against a busy cluster.
            "identity": ["OVMXA0", "OVMXB1"],
            "joined": True,
            "cm_190_tx": 509,
            "cm_190_rx": 575,
            "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
            "accept_rsp_tx": 2,
        },
        "B3": {
            "branch": "worktree-760-active-directory",
            "identity": ["OVMXB3"],
            "joined": True,
            "cm_190_tx": 513,
            "cm_190_rx": 579,
            "ctl_tx": {0: 2, 1: 8, 2: 2, 3: 2, 4: 6, 6: 2, 7: 2, 9: 2},
            "accept_rsp_tx": 2,
        },
        "B2": {
            "branch": "work/vms-578 (integrated)",
            "identity": ["OVMXB2"],
            "joined": True,
            "cm_190_tx": 508,
            "cm_190_rx": 571,
            "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
            "accept_rsp_tx": 2,
        },
    },
}

CAPTURES_578 = {
    "B1": "vms578-B1-lab2-vaxlab4-20260805.pcap",
    "B3": "vms578-B3-lab2-vaxlab4-20260805.pcap",
    "B2": "vms578-B2-lab2-vaxlab4-20260805.pcap",
}


def _lab2_named(capdir, names):
    """Verify `names` (a fixed, named capture list -- this script does not
    glob) are declared lab-2 in tools/cluster/capture_manifest.py, and audit
    `capdir` for any OTHER capture that disagrees with the manifest (vms-beb).
    This is the one tool that legitimately reads lab-2; the check still
    matters here because it catches a lab-1 file wandering INTO
    captures-lab2, or a capture this script names that the manifest has never
    heard of. Imports the manifest LAZILY, from this file's own directory.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import capture_manifest
    return capture_manifest.check_named(capdir, names, capture_manifest.LAB2)

# ===========================================================================
# vms-449: THE THIRD BRACKET -- THE REJOIN QUESTION, ASKED AND ANSWERED
# ===========================================================================
# vms-2f3's question -- can a returning OVMX identity rejoin a cluster it was
# removed from? -- had never actually been ASKED on a tree that can complete a
# FIRST join. vms-70e2's positive control failed (EXPECTED above); vms-578 fixed
# that (EXPECTED_578) and nobody re-ran the triple. This is the triple.
#
# THE ANSWER IS NO. Four consecutive same-identity rejoins were refused on a
# VIRGIN pod that had never seen an OVMX node, each one bracketed by a fresh
# identity that joined in 13 s on the same pod with the same binary.
#
# THE BRACKET (lab-2 `vaxlab-6`, a virgin pod, 2026-08-05, one binary
# throughout: this worktree's build of main at f874b04)
#
#     A1  OVMXJ0 / 1500  FIRST JOIN   JOINED   CLUSTER_NODES=3  XITDONE=1
#     B1  OVMXJ0 / 1500  rejoin #1    REFUSED                   XITDONE=0
#     C1  OVMXK1 / 1501  fresh        JOINED   <- control BETWEEN the tests
#     B2  OVMXJ0 / 1500  rejoin #2    REFUSED                   XITDONE=0
#     C2  OVMXK2 / 1502  fresh        JOINED   <- control BETWEEN the tests
#     B3  OVMXJ0 / 1500  rejoin #3    REFUSED                   XITDONE=0
#     C3  OVMXK3 / 1503  fresh        JOINED   <- closing control
#     B4  OVMXJ0 / 1500  rejoin #4    REFUSED  } matched pair under csbwatch.sh,
#     C4  OVMXK4 / 1504  fresh        JOINED   } which reads the peer's CSB LIVE
#
# Guardrail 18: every identity below is the set of `OVMX??` strings in the
# capture itself, never SCSD's log. Guardrail 20: a control sits BETWEEN each
# pair of test runs, not merely before and after them. Guardrail 14: four
# positive controls, on the same lab, with the same binary, minutes apart.
#
# ⚠ THE OVMX MAC IS PER-POD, NOT PER-LAB. `vaxlab-6`'s OVMX tap is
# 26:8b:49:99:95:3c and `vaxlab-4`'s is 4e:83:cd:c4:fe:54 -- the lab-2 README's
# "every replica reuses the same node MACs by design" is true of the VAX nodes
# (aa:00:04:00:01:04) and NOT of the OVMX tap, which the pod's netns mints per
# instance. Reusing EXPECTED["ovmx_mac"] here would have measured every figure
# as zero and looked like a total wire failure. Each bracket carries its own.
EXPECTED_449 = {
    "pod": "vaxlab-6",
    "lab": "lab-2",
    "date": "2026-08-05",
    "ovmx_mac": "26:8b:49:99:95:3c",
    # The rejoin subject and the four fresh controls, so a reader can tell which
    # rows are the experiment without decoding the tag scheme.
    "subject": "OVMXJ0",
    "runs": {
        "A1": {"role": "first-join", "identity": ["OVMXJ0"], "joined": True,
               "cm_190_tx": 510, "cm_190_rx": 579,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 9, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
        "B1": {"role": "rejoin", "identity": ["OVMXJ0"], "joined": False,
               "cm_190_tx": 14, "cm_190_rx": 11,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 2, 9: 4},
               "ctl_rx": {0: 2, 1: 2, 2: 2, 3: 2, 8: 4},
               "accept_rsp_tx": 2},
        "C1": {"role": "control", "identity": ["OVMXK1"], "joined": True,
               "cm_190_tx": 510, "cm_190_rx": 583,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 9, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
        "B2": {"role": "rejoin", "identity": ["OVMXJ0"], "joined": False,
               "cm_190_tx": 14, "cm_190_rx": 11,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 2, 9: 4},
               "ctl_rx": {0: 2, 1: 2, 2: 2, 3: 2, 8: 4},
               "accept_rsp_tx": 2},
        "C2": {"role": "control", "identity": ["OVMXK2"], "joined": True,
               "cm_190_tx": 510, "cm_190_rx": 578,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 9, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
        "B3": {"role": "rejoin", "identity": ["OVMXJ0"], "joined": False,
               "cm_190_tx": 14, "cm_190_rx": 11,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 2, 9: 4},
               "ctl_rx": {0: 2, 1: 2, 2: 2, 3: 2, 8: 4},
               "accept_rsp_tx": 2},
        "C3": {"role": "control", "identity": ["OVMXK3"], "joined": True,
               "cm_190_tx": 510, "cm_190_rx": 574,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 9, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
        "B4": {"role": "rejoin", "identity": ["OVMXJ0"], "joined": False,
               "cm_190_tx": 14, "cm_190_rx": 11,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 2, 9: 4},
               "ctl_rx": {0: 2, 1: 2, 2: 2, 3: 2, 8: 4},
               "accept_rsp_tx": 2},
        "C4": {"role": "control", "identity": ["OVMXK4"], "joined": True,
               "cm_190_tx": 513, "cm_190_rx": 583,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 9, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
    },
}

# The run ORDER, which is the bracket. Held separately from the dict because
# what makes this a bracket rather than nine runs is that no two rejoins are
# adjacent -- see check_449_bracket_shape().
ORDER_449 = ["A1", "B1", "C1", "B2", "C2", "B3", "C3", "B4", "C4"]

CAPTURES_449 = {
    t: "vms449-%s-lab2-vaxlab6-20260805.pcap" % t for t in ORDER_449
}


# ===========================================================================
# vms-449R: THE REPLICATION -- IS THE REFUSAL A PROPERTY OF OVMX, OR OF A POD?
# ===========================================================================
# EXPECTED_449 answered the rejoin question on ONE pod. A single pod cannot
# distinguish "OVMX identities are refused readmission" from "vaxlab-6 was
# sick". This is the same experiment on `vaxlab-7`, scaled up fresh, verified
# CLUSTER_NODES=2 before the first run, with the SAME binary (build-449,
# worktree HEAD = main at f874b04) and default environment. SCSSYSTEMIDs
# 1520-1523.
#
#     A1  OVMXM0 / 1520  FIRST JOIN   JOINED    XITDONE=1
#     B1  OVMXM0 / 1520  rejoin #1    REFUSED   XITDONE=0
#     C1  OVMXN1 / 1521  fresh        JOINED    <- closing control
#
# ⚠ THIS BRACKET IS SHORTER THAN EXPECTED_449 ON PURPOSE, AND THE REASON IS
# RECORDED RATHER THAN HIDDEN. Four further runs (B2/C2/B3/C3) were started and
# are VOID: the lab pod terminated at 2026-08-05T18:58:50Z (exit 255,
# RESTARTS=1) and came back at 18:59:01Z, so both VAXes rebooted *during* B2.
# A1/B1/C1 all completed before 18:56:58 and are unaffected. The void runs are
# discarded on HARNESS grounds -- the restart, not their figures -- which is
# guardrail 19 applied in the only order that is honest: a run whose lab
# rebooted under it does not get to vote either way. Their captures are NOT
# archived and no figure from them appears anywhere.
#
# WHAT THIS BRACKET THEREFORE DOES AND DOES NOT ESTABLISH. It shows the refusal
# and both of EXPECTED_449's wire discriminators reproducing on a second,
# independent, virgin pod -- so the finding is not an artefact of vaxlab-6. It
# contains ONE rejoin, so it does NOT independently establish the
# "three consecutive rejoins, so a single refusal is not a fluke" property.
# That property rests on EXPECTED_449's four. check_449r_bracket_shape() states
# exactly this weaker shape and must never be conflated with the stronger one.
#
# ⚠ The OVMX tap MAC is per-POD: vaxlab-7 mints a THIRD distinct value, which
# is independent confirmation of the trap recorded in spec sec 4(O.2).
EXPECTED_449R = {
    "pod": "vaxlab-7",
    "lab": "lab-2",
    "date": "2026-08-05",
    "ovmx_mac": "3a:ad:35:5d:23:80",
    "subject": "OVMXM0",
    "runs": {
        "A1": {"role": "first-join", "identity": ["OVMXM0"], "joined": True,
               "cm_190_tx": 504, "cm_190_rx": 568,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 6, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
        "B1": {"role": "rejoin", "identity": ["OVMXM0"], "joined": False,
               "cm_190_tx": 14, "cm_190_rx": 11,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 2, 9: 4},
               "ctl_rx": {0: 2, 1: 2, 2: 2, 3: 2, 8: 4},
               "accept_rsp_tx": 2},
        "C1": {"role": "control", "identity": ["OVMXN1"], "joined": True,
               "cm_190_tx": 502, "cm_190_rx": 568,
               "ctl_tx": {0: 2, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 9: 2},
               "ctl_rx": {0: 6, 1: 2, 2: 2, 3: 2, 6: 3, 7: 3, 8: 2},
               "accept_rsp_tx": 2},
    },
}

ORDER_449R = ["A1", "B1", "C1"]

CAPTURES_449R = {
    t: "vms449r-%s-lab2-vaxlab7-20260805.pcap" % t for t in ORDER_449R
}


def _read_pcap(path):
    """The pcap reader, imported LAZILY from dissect_sca.py so that the ctest
    figures gate -- which copies this file alone into a scratch tree for its
    mutation harness -- never needs the dissector on sys.path."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from dissect_sca import read_pcap
    return read_pcap(path)


def mac_str(b):
    return ":".join("%02x" % c for c in b)


def sca_frames(records):
    """Yield (src_mac, dst_mac, sca_payload) for every 0x6007 frame.

    The SCA payload starts immediately after the 14-byte Ethernet header, which
    is the `abs = payload + 14` convention docs/cluster-protocol-spec.md states
    under 'Byte-offset convention'.
    """
    for rec in records:
        frame = rec[3]
        if len(frame) < 16 or frame[12:14] != ETHERTYPE_SCA:
            continue
        yield mac_str(frame[6:12]), mac_str(frame[0:6]), frame[14:]


def measure_frames(frames, ovmx_mac):
    """Measure one run from an iterable of (src, dst, sca) tuples.

    Kept separate from the pcap reader so the ctest gate can drive it with
    synthesized frames and prove the decode, not just the prose.
    """
    ovmx_mac = ovmx_mac.lower()
    out = {
        "cm_190_tx": 0,
        "cm_190_rx": 0,
        "ctl_tx": {},
        "ctl_rx": {},
        "identity": set(),
    }
    for src, dst, sca in frames:
        src, dst = src.lower(), dst.lower()
        tx = src == ovmx_mac
        rx = dst == ovmx_mac
        if not (tx or rx):
            # Peer-to-peer traffic. Deliberately not counted: the finding is
            # about what OVMX exchanges, and the two VAXes talk to each other
            # at the same rate whatever OVMX does.
            continue
        if len(sca) == CM_CLASS:
            out["cm_190_tx" if tx else "cm_190_rx"] += 1
        if len(sca) in CTL_CLASSES and len(sca) >= MSGTYPE_OFF + 2:
            t = struct.unpack("<H", sca[MSGTYPE_OFF:MSGTYPE_OFF + 2])[0]
            bucket = out["ctl_tx" if tx else "ctl_rx"]
            bucket[t] = bucket.get(t, 0) + 1
        for name in re.findall(rb"OVMX[A-Z0-9]{2}", sca):
            out["identity"].add(name.decode("ascii"))
    out["identity"] = sorted(out["identity"])
    out["accept_rsp_tx"] = out["ctl_tx"].get(3, 0)
    return out


def measure_capture(path, ovmx_mac):
    return measure_frames(sca_frames(_read_pcap(path)), ovmx_mac)


# EXPECTED keys re-derived from packets, and keys no capture can produce.
# `pod`, `lab` and `date` are provenance labels; `ovmx_mac` is an INPUT to the
# measurement (which MAC is OVMX's tap) rather than an output of it, and is
# checked structurally by the ctest gate instead. tests/vmsscs/scs_wire.py reds
# if EXPECTED grows a figure in neither list.
WIRE_KEYS = ("runs",)
NON_WIRE_KEYS = ("pod", "lab", "date", "ovmx_mac")

# Every capture BOTH brackets need. test_scs_join_capability_figures.py passes
# this to scs_wire.capture_dir(), so a directory holding only one bracket's
# files counts as ABSENT and gets the loud banner rather than a half-run that
# silently skips the vms-578 acceptance figures.
BRACKET_CAPTURES = tuple(CAPTURES.values()) + tuple(CAPTURES_578.values())


def rederive(capdir, **_kw):
    """THE ctest GATE'S ENTRY POINT (vms-371) -- BOTH brackets.

    vms-70e2's bracket (EXPECTED) proved work/vms-187-closure cannot complete a
    FIRST join and the active-directory tree can. vms-578's acceptance bracket
    (EXPECTED_578) is the three runs that JOIN -- the evidence the whole SCA
    layer rests on -- and until vms-371 NO gate pinned it to the packets at all.
    Both are re-derived here, from the captures, by the same decoder.

    Returns (results, covered_keys) with `results` as [(ok, label), ...].
    """
    results = []

    def ck(cond, msg):
        results.append((bool(cond), msg))

    got = {}
    for bracket, caps, exp in (("70e2", CAPTURES, EXPECTED),
                               ("578", CAPTURES_578, EXPECTED_578)):
        got[bracket] = {}
        for tag, fn in caps.items():
            path = os.path.join(capdir, fn)
            if not os.path.exists(path):
                ck(False, "%s %s: capture %s is not under %s"
                   % (bracket, tag, fn, capdir))
                continue
            m = measure_capture(path, EXPECTED["ovmx_mac"])
            got[bracket][tag] = m
            e = exp["runs"][tag]
            for field in ("identity", "cm_190_tx", "cm_190_rx", "ctl_tx",
                          "accept_rsp_tx"):
                ck(m[field] == e[field],
                   "%s %s %s %r != %r" % (bracket, tag, field, m[field], e[field]))

    # The vms-70e2 finding as a RELATION, not only as three tables: the joining
    # binary is the only one that emits an ACCEPT_RSP and the only one the peer
    # answers at the CM layer.
    a = got["70e2"]
    if len(a) == len(CAPTURES):
        ck(a["A0"]["accept_rsp_tx"] > 0 and a["A1"]["accept_rsp_tx"] == 0
           and a["A3"]["accept_rsp_tx"] == 0,
           "the ACCEPT_RSP discriminator did not hold")
        ck(a["A0"]["cm_190_rx"] > 0 and a["A1"]["cm_190_rx"] == 0
           and a["A3"]["cm_190_rx"] == 0,
           "the inbound-CM discriminator did not hold")

    # The vms-578 acceptance finding as a RELATION: EVERY arm reached the
    # connection-manager layer, which is the thing work/vms-187-closure never
    # did. Measured off the packets, not read off EXPECTED_578.
    b = got["578"]
    if len(b) == len(CAPTURES_578):
        ck(all(b[t]["cm_190_rx"] > 0 for t in CAPTURES_578),
           "an arm of the vms-578 acceptance bracket received no CM frames")
        ck(all(b[t]["accept_rsp_tx"] > 0 for t in CAPTURES_578),
           "an arm of the vms-578 acceptance bracket emitted no ACCEPT_RSP")
        # ...and the control sits BETWEEN the two runs under test, which is what
        # makes it a bracket rather than a before/after.
        ck(list(CAPTURES_578) == ["B1", "B3", "B2"]
           and EXPECTED_578["runs"]["B3"]["branch"] == "worktree-760-active-directory",
           "the vms-578 control is not bracketed by the two integrated runs")

    return results, {"runs"}
def check_449_bracket_shape():
    """The vms-449 bracket's SHAPE, asserted from the checked-in table alone.

    This runs on every host, captures or not. It exists because the figures are
    only worth reading if the experiment they came from was bracketed: a run of
    rejoins with the controls piled up at one end would produce the same numbers
    and mean nothing (guardrail 20). Returns a list of failure strings.
    """
    f = []
    runs = EXPECTED_449["runs"]
    if sorted(ORDER_449) != sorted(runs):
        f.append("ORDER_449 %s does not cover EXPECTED_449 %s"
                 % (sorted(ORDER_449), sorted(runs)))
        return f
    roles = [runs[t]["role"] for t in ORDER_449]
    rejoins = [t for t in ORDER_449 if runs[t]["role"] == "rejoin"]
    controls = [t for t in ORDER_449 if runs[t]["role"] == "control"]
    firsts = [t for t in ORDER_449 if runs[t]["role"] == "first-join"]

    if len(firsts) != 1 or ORDER_449[0] != firsts[0]:
        f.append("the bracket must OPEN with exactly one first-join run")
    if len(rejoins) < 3:
        f.append("fewer than three rejoins were attempted, so a single refusal "
                 "could be mistaken for a rule (%d)" % len(rejoins))
    # Guardrail 20, stated mechanically: no two test runs are adjacent, so a
    # control always sits BETWEEN them.
    for i in range(len(roles) - 1):
        if roles[i] == "rejoin" and roles[i + 1] == "rejoin":
            f.append("runs %s and %s are adjacent rejoins -- no control between "
                     "them" % (ORDER_449[i], ORDER_449[i + 1]))
    if roles[-1] != "control":
        f.append("the bracket does not CLOSE with a control, so 'the lab "
                 "stopped admitting anyone' is not excluded")
    # The subject is one identity across every rejoin, and no control shares it.
    subj = EXPECTED_449["subject"]
    for t in rejoins + firsts:
        if runs[t]["identity"] != [subj]:
            f.append("%s is a test run but its wire identity is %s, not [%r]"
                     % (t, runs[t]["identity"], subj))
    for t in controls:
        if subj in runs[t]["identity"]:
            f.append("control %s carries the subject identity %s -- it is not a "
                     "control" % (t, subj))
        if runs[t]["identity"] in ([runs[c]["identity"] for c in controls
                                    if c != t]):
            f.append("control %s reuses another control's identity %s -- a "
                     "reused control identity is itself a rejoin"
                     % (t, runs[t]["identity"]))
    # The verdicts, which are the answer.
    if not all(runs[t]["joined"] for t in controls + firsts):
        f.append("a control or the first join did not join -- suspect the "
                 "harness, not the theory (guardrail 19)")
    if any(runs[t]["joined"] for t in rejoins):
        f.append("EXPECTED_449 records a rejoin as JOINED; the recorded answer "
                 "is that every rejoin was refused")
    # The wire discriminator, as a RELATION over the table and not one row.
    for t in rejoins:
        if runs[t]["ctl_rx"].get(6, 0) or runs[t]["ctl_rx"].get(7, 0):
            f.append("%s: a refused rejoin is recorded as receiving a "
                     "DISCONNECT_REQ/RSP from the peer" % t)
    for t in controls + firsts:
        if not (runs[t]["ctl_rx"].get(6, 0) and runs[t]["ctl_rx"].get(7, 0)):
            f.append("%s: a joining run is recorded as receiving no peer "
                     "DISCONNECT pair" % t)
    # And the vms-70e2 precondition: this tree emits ACCEPT_RSP on EVERY arm,
    # which is what makes it 'the tree that joins' and the question askable.
    for t in ORDER_449:
        if runs[t]["accept_rsp_tx"] <= 0:
            f.append("%s emitted no ACCEPT_RSP -- that is the vms-70e2 failing "
                     "binary's signature and this bracket must not contain it" % t)
    return f


def check_449r_bracket_shape():
    """The REPLICATION bracket's shape -- deliberately a WEAKER claim.

    check_449_bracket_shape() demands three-or-more rejoins with a control
    between each pair, because EXPECTED_449 has to carry "not a fluke" on its
    own. EXPECTED_449R has ONE rejoin and must not pretend otherwise, so this
    asserts only what a replication needs to be worth reading:

      * it opens with a first join that JOINED (the positive control -- without
        it a refusal says nothing, cf. vms-70e2);
      * it contains at least one rejoin of a single subject identity;
      * it CLOSES with a control that joined, so "the pod stopped admitting
        anyone" is excluded;
      * no control shares the subject's identity (a control that does IS a
        rejoin);
      * and it is measured against a DIFFERENT pod and a different OVMX tap MAC
        from EXPECTED_449 -- a "replication" on the same pod replicates nothing.

    Returns a list of failure strings.
    """
    f = []
    runs = EXPECTED_449R["runs"]
    if sorted(ORDER_449R) != sorted(runs):
        f.append("ORDER_449R %s does not cover EXPECTED_449R %s"
                 % (sorted(ORDER_449R), sorted(runs)))
        return f
    roles = [runs[t]["role"] for t in ORDER_449R]
    rejoins = [t for t in ORDER_449R if runs[t]["role"] == "rejoin"]
    controls = [t for t in ORDER_449R if runs[t]["role"] == "control"]
    firsts = [t for t in ORDER_449R if runs[t]["role"] == "first-join"]
    subj = EXPECTED_449R["subject"]

    if len(firsts) != 1 or ORDER_449R[0] != firsts[0]:
        f.append("the replication must OPEN with exactly one first-join run")
    if not rejoins:
        f.append("the replication contains no rejoin -- it replicates nothing")
    if roles[-1] != "control":
        f.append("the replication does not CLOSE with a control, so 'the pod "
                 "stopped admitting anyone' is not excluded")
    for t in rejoins + firsts:
        if runs[t]["identity"] != [subj]:
            f.append("%s is a test run but its wire identity is %s, not [%r]"
                     % (t, runs[t]["identity"], subj))
    for t in controls:
        if subj in runs[t]["identity"]:
            f.append("control %s carries the subject identity %s -- it is not "
                     "a control" % (t, subj))
    if not all(runs[t]["joined"] for t in controls + firsts):
        f.append("a control or the first join did not join -- suspect the "
                 "harness, not the theory (guardrail 19)")
    if any(runs[t]["joined"] for t in rejoins):
        f.append("EXPECTED_449R records a rejoin as JOINED; the recorded "
                 "replication is that the rejoin was refused")
    # The discriminators, required to reproduce -- that IS the replication.
    for t in rejoins:
        if runs[t]["ctl_rx"].get(6, 0) or runs[t]["ctl_rx"].get(7, 0):
            f.append("%s: a refused rejoin is recorded as receiving a peer "
                     "DISCONNECT_REQ/RSP -- the sec 4(O.2) discriminator did "
                     "not reproduce" % t)
    for t in controls + firsts:
        if not (runs[t]["ctl_rx"].get(6, 0) and runs[t]["ctl_rx"].get(7, 0)):
            f.append("%s: a joining run is recorded as receiving no peer "
                     "DISCONNECT pair" % t)
    for t in ORDER_449R:
        if runs[t]["accept_rsp_tx"] <= 0:
            f.append("%s emitted no ACCEPT_RSP -- the vms-70e2 failing "
                     "signature must not appear in this bracket" % t)
    # A replication on the same pod is not a replication.
    if EXPECTED_449R["pod"] == EXPECTED_449["pod"]:
        f.append("EXPECTED_449R names the same pod as EXPECTED_449 (%s) -- "
                 "that replicates nothing" % EXPECTED_449R["pod"])
    if EXPECTED_449R["ovmx_mac"] == EXPECTED_449["ovmx_mac"]:
        f.append("EXPECTED_449R reuses EXPECTED_449's OVMX tap MAC; the tap is "
                 "per-pod, so this would be the same pod or a mismeasurement")
    if EXPECTED_449R["subject"] == EXPECTED_449["subject"]:
        f.append("EXPECTED_449R reuses EXPECTED_449's subject identity -- a "
                 "replication must carry an identity of its own")
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", default=DEFAULT_CAPTURE_DIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

    # vms-beb: refuse rather than silently read if any named capture is
    # unknown to the manifest, mislabeled, or if args.captures holds a
    # capture the manifest declares for a different lab.
    _lab2_named(args.captures, list(CAPTURES.values()) + list(CAPTURES_578.values()))

    got = {}
    for tag, fn in CAPTURES.items():
        path = os.path.join(args.captures, fn)
        if not os.path.exists(path):
            print("MISSING capture: %s" % path, file=sys.stderr)
            print("These captures are host-only (rule 8). Run this on a lab host.",
                  file=sys.stderr)
            return 2
        got[tag] = measure_capture(path, EXPECTED["ovmx_mac"])

    for tag in ("A1", "A0", "A3"):
        m = got[tag]
        e = EXPECTED["runs"][tag]
        print("== %s  (%s)  joined=%s" % (tag, e["branch"], e["joined"]))
        print("   identity on the wire : %s" % ", ".join(m["identity"]))
        print("   CM 190-byte frames   : tx=%d rx=%d" % (m["cm_190_tx"], m["cm_190_rx"]))
        print("   OVMX-sent ctl types  : %s" % ", ".join(
            "%d=%s x%d" % (t, MSGTYPE_NAMES.get(t, "type%d" % t), n)
            for t, n in sorted(m["ctl_tx"].items())))
        print("   ACCEPT_RSP (type 3)  : %d" % m["accept_rsp_tx"])

    # --- vms-578's bracket, reported the same way ---
    got578 = {}
    for tag, fn in CAPTURES_578.items():
        path = os.path.join(args.captures, fn)
        if os.path.exists(path):
            got578[tag] = measure_capture(path, EXPECTED["ovmx_mac"])
    for tag in ("B1", "B3", "B2"):
        if tag not in got578:
            continue
        m = got578[tag]
        e = EXPECTED_578["runs"][tag]
        print("== %s  (%s)  joined=%s" % (tag, e["branch"], e["joined"]))
        print("   identity on the wire : %s" % ", ".join(m["identity"]))
        print("   CM 190-byte frames   : tx=%d rx=%d" % (m["cm_190_tx"], m["cm_190_rx"]))
        print("   OVMX-sent ctl types  : %s" % ", ".join(
            "%d=%s x%d" % (t, MSGTYPE_NAMES.get(t, "type%d" % t), n)
            for t, n in sorted(m["ctl_tx"].items())))
        print("   ACCEPT_RSP (type 3)  : %d" % m["accept_rsp_tx"])

    # --- vms-449's bracket: the rejoin triple, reported the same way ---
    got449 = {}
    for tag, fn in CAPTURES_449.items():
        path = os.path.join(args.captures, fn)
        if os.path.exists(path):
            got449[tag] = measure_capture(path, EXPECTED_449["ovmx_mac"])
    for tag in ORDER_449:
        if tag not in got449:
            continue
        m = got449[tag]
        e = EXPECTED_449["runs"][tag]
        print("== %s  (%s)  joined=%s" % (tag, e["role"], e["joined"]))
        print("   identity on the wire : %s" % ", ".join(m["identity"]))
        print("   CM 190-byte frames   : tx=%d rx=%d" % (m["cm_190_tx"], m["cm_190_rx"]))
        print("   OVMX-sent ctl types  : %s" % ", ".join(
            "%d=%s x%d" % (t, MSGTYPE_NAMES.get(t, "type%d" % t), n)
            for t, n in sorted(m["ctl_tx"].items())))
        print("   peer-sent ctl types  : %s" % ", ".join(
            "%d=%s x%d" % (t, MSGTYPE_NAMES.get(t, "type%d" % t), n)
            for t, n in sorted(m["ctl_rx"].items())))

    # --- vms-449R's replication bracket, on the OTHER pod ---
    got449r = {}
    for tag, fn in CAPTURES_449R.items():
        path = os.path.join(args.captures, fn)
        if os.path.exists(path):
            got449r[tag] = measure_capture(path, EXPECTED_449R["ovmx_mac"])
    for tag in ORDER_449R:
        if tag not in got449r:
            continue
        m = got449r[tag]
        e = EXPECTED_449R["runs"][tag]
        print("== 449R %s  (%s)  joined=%s  [%s]"
              % (tag, e["role"], e["joined"], EXPECTED_449R["pod"]))
        print("   identity on the wire : %s" % ", ".join(m["identity"]))
        print("   CM 190-byte frames   : tx=%d rx=%d" % (m["cm_190_tx"], m["cm_190_rx"]))
        print("   peer-sent ctl types  : %s" % ", ".join(
            "%d=%s x%d" % (t, MSGTYPE_NAMES.get(t, "type%d" % t), n)
            for t, n in sorted(m["ctl_rx"].items())))

    if args.just_print:
        return 0

    results, _covered = rederive(args.captures)
    fails = [label for ok, label in results if not ok]

    # vms-992: `ck` below is main()'s OWN failure recorder, appending
    # straight to `fails` -- it is NOT rederive()'s `ck` closure (that one
    # is scoped inside rederive() and appends to its own local `results`,
    # and goes out of scope the moment rederive() returns). Before this fix
    # every `ck(...)` call past this point raised NameError -- main() had
    # never actually run to completion on either side of vms-beb's merge.
    def ck(cond, msg):
        if not cond:
            fails.append(msg)

    # --- vms-449: THE REJOIN BRACKET, checked figure by figure ---
    # The shape check runs on EVERY host; only the figure re-derivation needs
    # the captures.
    fails.extend(check_449_bracket_shape())
    if len(got449) == len(CAPTURES_449):
        for tag in ORDER_449:
            m, e = got449[tag], EXPECTED_449["runs"][tag]
            for field in ("identity", "cm_190_tx", "cm_190_rx", "ctl_tx",
                          "ctl_rx", "accept_rsp_tx"):
                ck(m[field] == e[field],
                   "449 %s %s %r != %r" % (tag, field, m[field], e[field]))
        rejoins = [t for t in ORDER_449
                   if EXPECTED_449["runs"][t]["role"] == "rejoin"]
        joiners = [t for t in ORDER_449
                   if EXPECTED_449["runs"][t]["role"] != "rejoin"]
        # THE ANSWER, as a relation over the measured captures rather than as
        # nine tables. Two independent discriminators, both 4/4 vs 5/5:
        #
        #  (1) the CM membership dialogue collapses by a factor of ~36 on a
        #      rejoin -- it is not that OVMX goes silent (it transmits 14) but
        #      that the exchange stops after the opening burst; and
        #  (2) the peer never sends the SCA DISCONNECT pair (types 6 and 7)
        #      that, on every joining run, tears down its own SCS$DIRECTORY
        #      connection before readmission. That is sec 4k.5's divergence
        #      point, measured as a message-type census instead of by hand.
        ck(max(got449[t]["cm_190_rx"] for t in rejoins)
           < min(got449[t]["cm_190_rx"] for t in joiners) / 10,
           "the CM-collapse discriminator did not hold across the 449 bracket")
        ck(all(got449[t]["ctl_rx"].get(6, 0) == 0
               and got449[t]["ctl_rx"].get(7, 0) == 0 for t in rejoins),
           "a refused rejoin received a peer DISCONNECT pair")
        ck(all(got449[t]["ctl_rx"].get(6, 0) > 0
               and got449[t]["ctl_rx"].get(7, 0) > 0 for t in joiners),
           "a joining run received no peer DISCONNECT pair")
        # ...and every arm, refused ones included, emits the ACCEPT_RSP whose
        # absence WAS the vms-70e2 failure. The question was actually asked.
        ck(all(got449[t]["accept_rsp_tx"] > 0 for t in ORDER_449),
           "an arm of the 449 bracket emitted no ACCEPT_RSP -- on this tree "
           "every arm must, or the rejoin question was not the thing measured")
    else:
        print("[vms-449 bracket captures absent -- its figures were not "
              "re-derived; the bracket SHAPE was still checked]")

    # --- vms-449R: THE REPLICATION, checked the same way ------------------
    fails.extend(check_449r_bracket_shape())
    if len(got449r) == len(CAPTURES_449R):
        for tag in ORDER_449R:
            m, e = got449r[tag], EXPECTED_449R["runs"][tag]
            for field in ("identity", "cm_190_tx", "cm_190_rx", "ctl_tx",
                          "ctl_rx", "accept_rsp_tx"):
                ck(m[field] == e[field],
                   "449R %s %s %r != %r" % (tag, field, m[field], e[field]))
        rj = [t for t in ORDER_449R
              if EXPECTED_449R["runs"][t]["role"] == "rejoin"]
        jn = [t for t in ORDER_449R
              if EXPECTED_449R["runs"][t]["role"] != "rejoin"]
        # THE REPLICATION, as the same two relations that carried sec 4(O.2) --
        # measured on a different pod, against a different tap MAC.
        ck(max(got449r[t]["cm_190_rx"] for t in rj)
           < min(got449r[t]["cm_190_rx"] for t in jn) / 10,
           "the CM-collapse discriminator did not reproduce on "
           + EXPECTED_449R["pod"])
        ck(all(got449r[t]["ctl_rx"].get(6, 0) == 0
               and got449r[t]["ctl_rx"].get(7, 0) == 0 for t in rj),
           "a refused rejoin received a peer DISCONNECT pair on "
           + EXPECTED_449R["pod"])
        ck(all(got449r[t]["ctl_rx"].get(6, 0) > 0
               and got449r[t]["ctl_rx"].get(7, 0) > 0 for t in jn),
           "a joining run received no peer DISCONNECT pair on "
           + EXPECTED_449R["pod"])
        ck(all(got449r[t]["accept_rsp_tx"] > 0 for t in ORDER_449R),
           "an arm of the 449R replication emitted no ACCEPT_RSP")
        # And the rejoin's collapsed census must be the SAME on both pods --
        # that identity is the strongest single statement the pair makes.
        if len(got449) == len(CAPTURES_449):
            b6 = [got449[t] for t in ORDER_449
                  if EXPECTED_449["runs"][t]["role"] == "rejoin"]
            for t in rj:
                ck(all(got449r[t]["cm_190_tx"] == o["cm_190_tx"]
                       and got449r[t]["cm_190_rx"] == o["cm_190_rx"]
                       and got449r[t]["ctl_tx"] == o["ctl_tx"]
                       and got449r[t]["ctl_rx"] == o["ctl_rx"] for o in b6),
                   "the refused-rejoin census differs between %s and %s; spec "
                   "sec 4(O.3) claims the two pods produce the identical census"
                   % (EXPECTED_449["pod"], EXPECTED_449R["pod"]))
    else:
        print("[vms-449R replication captures absent -- its figures were not "
              "re-derived; the bracket SHAPE was still checked]")

    if fails:
        for f in fails:
            print("FAIL: %s" % f)
        return 1
    print("\nPASS -- every figure re-derived from the captures matches EXPECTED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
