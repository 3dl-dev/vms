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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", default=DEFAULT_CAPTURE_DIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

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

    if args.just_print:
        return 0

    results, _covered = rederive(args.captures)
    fails = [label for ok, label in results if not ok]

    if fails:
        for f in fails:
            print("FAIL: %s" % f)
        return 1
    print("\nPASS -- every figure re-derived from the captures matches EXPECTED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
