#!/usr/bin/env python3
"""scs_t45_measure.py -- vms-754: does MTYPE 4/5 mean REJECT_REQ/REJECT_RSP
(docs/cluster-protocol-spec.md sec 4(h)(1a)) or MSCP connect-ACCEPT/CONFIRM
(src/vmsscs/scs_dir.c's SCS_DIR_OP_ACCEPT / SCS_DIR_OP_MSCP_CONFIRM, grounded
by vms-760/vms-e81 on a 336-frame op-5 census)?

Both readings shared one field (content[46:48], scs_env.h's MTYPE) and
disagreed about it. This script is the decisive test between them: an ACCEPT
that binds a working connection must be followed, on that SAME Con.ID pair, by
application traffic (MTYPE 10); a REJECT must not, because there is no
connection left to carry any.

    tools/cluster/scs_t45_measure.py           # re-measure and PASS/FAIL vs EXPECTED
    tools/cluster/scs_t45_measure.py --print   # just print what the captures say

Requires the lab captures, host-only and NOT in git (CLAUDE.md rule 8):
/data/training/vax/cluster/captures/*.pcap (the same 47-pcap library
scs_disc_measure.py and scs_reason_measure.py use). Override with --captures.

`ctest -R scs_t45_figures` re-derives every figure below from the packets when
the captures are present (vms-371 protocol, via rederive()); on a host without
them it still pins every figure in EXPECTED to the prose so the documents
cannot drift apart, and says loudly that the wire itself was not read.

----------------------------------------------------------------------------
WHAT IT MEASURES, AND WHY EACH PART EXISTS
----------------------------------------------------------------------------

(A) THE TERMINAL-DIALOGUE CENSUS -- the decisive test. For every
    envelope-conformant frame whose MTYPE is 4, and separately for every frame
    whose MTYPE is 2 (ACCEPT_REQ, the POSITIVE CONTROL: a message nobody
    disputes is a real accept), track its Con.ID pair and ask whether ANY
    LATER frame in the same pcap, on the pair with the two handles swapped,
    carries MTYPE 10 (application data, spec sec 4(h)(1b) / scs_env.h).

    A real accept should almost always be followed by traffic (a formed
    connection normally does something); a real reject never can be, because
    there is no bound connection left. The two populations come out on
    opposite sides of that line, over the SAME corpus with the SAME method:
    394 ACCEPT_REQ frames, 388 (98%) followed by application traffic; 733
    REJECT_REQ-candidate frames, 0 (0%) ever are. The 6 unfollowed ACCEPT_REQs
    are dialogues accepted near the end of a capture window, not a
    methodology gap -- see the exhibit below.

(B) THE af2 EXHIBIT -- WHERE vms-760's OWN GROUNDING EVIDENCE CAME FROM.
    src/vmsscs/scs_dir.c cited "af2-firsttimer-established-20260728.pcap
    (J->M op=4, rel~143.758)" as the byte-exact template for its op=4 MSCP
    connect-ACCEPT builder. That exact frame, read back with no filtering, is
    frame 2584 of that capture: VAX2 (real HW MAC 08:00:2b:78:56:b9) sending
    to VAX1 (logical LAVC aa:00:04:00:01:04) -- i.e. a REAL VAX ADDRESSING
    ANOTHER REAL VAX, with NO OVMX participant in the capture at all (checked:
    every source MAC in the file is one of those two). VAX1 answers with an
    op=5 at frame 2587. Nothing OVMX-shaped can be "server-first accepting" a
    connection in a two-real-VAX capture.

    Read as a THREAD instead of one frame, the picture sharpens further: this
    same capture contains NINE separate op-4/op-5 exchanges between the same
    two nodes at 10.0s(+-0.2s) intervals with STRICTLY INCREASING Con.IDs (one
    of the nine even carries an explicit op=0x7b RETRANSMIT marker), and then
    a TENTH attempt switches message type entirely -- op=2/op=3 -- and
    succeeds (its Con.ID pair goes on to carry the 94-content MSCP command
    class). That is retry-until-accepted with the connect being REFUSED nine
    times running, not "OVMX accepts the MSCP connect" nine times before a
    real VAX also somehow accepts it a tenth time with a different message
    type. vms-e81's own comment on this census ("Nothing follows it: the
    Con.ID pair never appears again") independently recorded the exact
    signature this script measures in part (A) -- it read the absence of
    follow-up traffic as "a bound connection needs no more", when read against
    the positive control it is the signature of a refusal.

CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8). Everything here reads captured
Ethernet frames off our own 2-node SIMH OpenVMS VAX 7.3 reference cluster; no
VSI/HPE source or binary is read. It imports nothing from OVMX except the
pure-stdlib pcap reader in dissect_sca.py.
"""
import argparse
import collections
import glob
import os
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

# ---------------------------------------------------------------------------
# THE LAB FENCE (vms-096) -- same reasoning and same manifest as
# scs_disc_measure.py / scs_reason_measure.py / scs_t89_measure.py. lab-2
# replicas reuse lab-1's SCSSYSTEMIDs and node MACs by design
# (tests/lab/README.md), so a lab-2 capture dropped into this directory would
# silently move every count below.
# ---------------------------------------------------------------------------


def lab1_only(paths):
    """Return `paths` unchanged, or die naming every non-lab-1 capture in
    them -- checked against tools/cluster/capture_manifest.py's declared
    manifest, never a filename heuristic."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import capture_manifest
    return capture_manifest.check_paths(paths, capture_manifest.LAB1)


def _read_pcap():
    """Lazily imported: the figures gate reads EXPECTED out of this module
    without ever calling measure(), and its mutation harness (if this gate
    grows one) would copy this file alone into a scratch tree."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from dissect_sca import read_pcap
    return read_pcap


SCA_ETHERTYPE = b"\x60\x07"
MSGTYPE_NAMES = {0: "CONNECT_REQ", 1: "CONNECT_RSP", 2: "ACCEPT_REQ",
                 3: "ACCEPT_RSP", 4: "REJECT_REQ", 5: "REJECT_RSP",
                 6: "DISCONNECT_REQ", 7: "DISCONNECT_RSP", 10: "APPLICATION"}
SUBJECT_MTYPES = (2, 4)   # 2 = the positive control, 4 = the disputed subject
FOLLOWUP_MTYPE = 10       # application data, the "this connection is alive" tell

AF2_CAPTURE = "af2-firsttimer-established-20260728.pcap"
# The vms-760/vms-e81 grounding frame, re-identified by its own cited
# timestamp ("rel~143.758"). Frame index is the 0-based raw pcap record
# number, the convention this whole spec family uses.
AF2_GROUNDING_FRAME_IDX = 2584
AF2_GROUNDING_REL_S = 143.758

# ---------------------------------------------------------------------------
# The recorded measurement. Every figure here is quoted in
# src/vmsscs/include/scs_env.h, src/vmsscs/include/scs_dir.h and
# docs/cluster-protocol-spec.md, and test_scs_t45_figures.py asserts it is
# still there. Re-derived 2026-08-06 (vms-754) against the 47-capture library.
# ---------------------------------------------------------------------------
EXPECTED = {
    "n_captures": 47,

    # (A) the terminal-dialogue census, one row per subject MTYPE.
    "terminal_census": {
        2: {"frames": 394, "followed": 388, "terminal": 6},
        4: {"frames": 733, "followed": 0, "terminal": 733},
    },
    # VMS-origin-only split of the same population (08:00:2b / aa:00:04
    # source MACs), cross-checked against scs_reason_measure.py's
    # independently-measured "4 REJECT_REQ: 453 frames, 19 pcaps" over the
    # SAME 47-pcap library and the SAME 62-byte class -- 733 total - 280
    # OVMX-sourced = 453, exact.
    "t4_vms_origin_frames": 453,
    "t4_ovmx_origin_frames": 280,

    # (B.1) THE CITED GROUNDING FRAME ITSELF. scs_dir.c's op-4 MSCP
    # connect-ACCEPT builder cites "af2-firsttimer-established.pcap (J->M
    # op=4, rel~143.758)" as its byte-exact template. Re-identified by that
    # timestamp: frame 2584, VAX2 (real HW MAC) -> VAX1 (logical LAVC), i.e.
    # a REAL VAX addressing ANOTHER REAL VAX with no OVMX participant in the
    # capture at all (checked: every source MAC in the file is one of those
    # two -- see "vms_only_capture" below). No frame anywhere in this file
    # ever again carries this exact Con.ID pair.
    "af2_grounding_frame": {
        "capture": AF2_CAPTURE,
        "idx": AF2_GROUNDING_FRAME_IDX,
        "rel_s": AF2_GROUNDING_REL_S,
        "mtype": 4,
        "dest_conid": 0x3553000A,
        "src_conid": 0x8FD10008,
        "eth_src_is_vax2_hw": True,
        "eth_dst_is_vax1_logical": True,
    },
    "af2_capture_is_two_real_vaxes_only": True,

    # (B.2) A SEPARATE, MORE LEGIBLE THREAD IN THE SAME FILE (a different
    # Con.ID family, NOT the one in B.1): nine op-4/op-5 exchanges at ~10s
    # intervals with a STRICTLY INCREASING dest Con.ID (one carries an
    # explicit op-0x7b RETRANSMIT), followed by a tenth attempt that switches
    # message type to op-2/op-3 and succeeds -- its Con.ID pair goes on to
    # carry the 94-content MSCP command class. Textbook retry-until-accepted
    # with the connect being REFUSED nine times running.
    "af2_retry_thread": {
        "capture": AF2_CAPTURE,
        "frames": [
            (12949, 4, 0x3566000B), (17071, 4, 0x356B000B),
            (17096, 4, 0x356E000B), (17150, 4, 0x3571000B),
            (17175, 4, 0x3576000B), (17204, 4, 0x3579000B),
            (17229, 4, 0x357C000B), (17323, 4, 0x3581000B),
            (17349, 4, 0x3584000B), (17378, 2, 0x3587000B),
        ],
        "rejected_attempts": 9,
        "accepted_attempt_mtype": 2,
    },
}

WIRE_KEYS = ("n_captures", "terminal_census", "t4_vms_origin_frames",
            "t4_ovmx_origin_frames", "af2_grounding_frame",
            "af2_capture_is_two_real_vaxes_only", "af2_retry_thread")
NON_WIRE_KEYS = ()


def is_vms_origin(frame):
    mac = frame[6:12].hex()
    return mac.startswith("08002b") or mac.startswith("aa000400")


def envelope(content):
    """scs_env.h's own conformance test, reimplemented in pure Python so this
    script depends on nothing from the C tree. Returns (mtype, dest, src) or
    None."""
    if len(content) < 58:
        return None
    total = int.from_bytes(content[0:2], "little") + 2
    if total > len(content) or total < 58:
        return None
    if content[44:46] != b"\x04\x00":
        return None
    inner = int.from_bytes(content[42:44], "little")
    if inner != total - 44:
        return None
    mtype = int.from_bytes(content[46:48], "little")
    dest = int.from_bytes(content[50:54], "little")
    src = int.from_bytes(content[54:58], "little")
    return mtype, dest, src


# The two node identities the af2 exhibit checks, spelled out once (spec
# sec 3 decoder ring / dissect_sca.py MAC_NAMES).
VAX2_HW_MAC = bytes.fromhex("08002b7856b9")
VAX1_LOGICAL_MAC = bytes.fromhex("aa0004000104")


def load_events(path, read_pcap):
    """Every envelope-conformant frame in one pcap, in wire order. Also
    records the raw Ethernet src MAC so the af2 exhibit can check who sent
    it, and the frame's own timestamp relative to the FIRST record in the
    file (not the first SCA frame) so rel-seconds match what a dissector
    dumping raw pcap records would print."""
    out = []
    all_macs = set()
    t0 = None
    for idx, (s, us, _o, frame) in enumerate(read_pcap(path)):
        t = s + us / 1e6
        if t0 is None:
            t0 = t
        if len(frame) < 14 or frame[12:14] != SCA_ETHERTYPE:
            continue
        all_macs.add(bytes(frame[6:12]))
        e = envelope(frame[14:])
        if e is None:
            continue
        mtype, dest, src = e
        out.append({"idx": idx, "t": t, "rel": round(t - t0, 3), "mtype": mtype,
                    "dest": dest, "src": src, "vms": is_vms_origin(frame),
                    "eth_src": bytes(frame[6:12]), "eth_dst": bytes(frame[0:6])})
    return out, all_macs


def measure(capdir, files=None, read_pcap=None):
    if read_pcap is None:
        read_pcap = _read_pcap()
    if files is None:
        files = lab1_only(sorted(glob.glob(os.path.join(capdir, "*.pcap"))))

    census = {mt: {"frames": 0, "followed": 0, "terminal": 0} for mt in SUBJECT_MTYPES}
    t4_vms = t4_ovmx = 0
    af2_grounding = None
    af2_only_macs = None
    af2_thread_got = []

    for path in files:
        events, all_macs = load_events(path, read_pcap)
        base = os.path.basename(path)

        for i, ev in enumerate(events):
            mt = ev["mtype"]
            if mt not in SUBJECT_MTYPES:
                continue
            pair = frozenset((ev["dest"], ev["src"]))
            followed = any(
                frozenset((e2["dest"], e2["src"])) == pair and e2["mtype"] == FOLLOWUP_MTYPE
                for e2 in events[i + 1:])
            census[mt]["frames"] += 1
            if followed:
                census[mt]["followed"] += 1
            else:
                census[mt]["terminal"] += 1
            if mt == 4:
                if ev["vms"]:
                    t4_vms += 1
                else:
                    t4_ovmx += 1

        if base == AF2_CAPTURE:
            af2_only_macs = all_macs <= {VAX2_HW_MAC, VAX1_LOGICAL_MAC}
            wanted_idx = {AF2_GROUNDING_FRAME_IDX} | {
                f[0] for f in EXPECTED["af2_retry_thread"]["frames"]}
            for ev in events:
                if ev["idx"] == AF2_GROUNDING_FRAME_IDX:
                    af2_grounding = {
                        "idx": ev["idx"], "rel_s": ev["rel"], "mtype": ev["mtype"],
                        "dest_conid": ev["dest"], "src_conid": ev["src"],
                        "eth_src_is_vax2_hw": ev["eth_src"] == VAX2_HW_MAC,
                        "eth_dst_is_vax1_logical": ev["eth_dst"] == VAX1_LOGICAL_MAC,
                    }
                if ev["idx"] in wanted_idx and ev["idx"] != AF2_GROUNDING_FRAME_IDX:
                    af2_thread_got.append((ev["idx"], ev["mtype"], ev["dest"]))

    af2_thread_got.sort()
    return {
        "n_captures": len(files),
        "terminal_census": census,
        "t4_vms_origin_frames": t4_vms,
        "t4_ovmx_origin_frames": t4_ovmx,
        "af2_grounding_frame": af2_grounding,
        "af2_capture_is_two_real_vaxes_only": af2_only_macs,
        "af2_retry_thread_frames": af2_thread_got,
    }


def rederive(capdir, **_kw):
    """The vms-371 entry point: re-measure and PASS/FAIL every EXPECTED
    figure. Returns ([(ok, label), ...], covered_keys)."""
    read_pcap = _read_pcap()
    got = measure(capdir, read_pcap=read_pcap)
    results = []

    def check(ok, label):
        results.append((ok, label))

    check(got["n_captures"] == EXPECTED["n_captures"],
          f"n_captures {got['n_captures']} != EXPECTED {EXPECTED['n_captures']}")

    for mt, want in EXPECTED["terminal_census"].items():
        g = got["terminal_census"].get(mt, {})
        name = MSGTYPE_NAMES[mt]
        check(g.get("frames") == want["frames"],
              f"MTYPE {mt} ({name}) frames {g.get('frames')} != EXPECTED {want['frames']}")
        check(g.get("followed") == want["followed"],
              f"MTYPE {mt} ({name}) followed-by-app {g.get('followed')} != EXPECTED "
              f"{want['followed']}")
        check(g.get("terminal") == want["terminal"],
              f"MTYPE {mt} ({name}) terminal {g.get('terminal')} != EXPECTED "
              f"{want['terminal']}")

    check(got["t4_vms_origin_frames"] == EXPECTED["t4_vms_origin_frames"],
          f"type-4 VMS-origin frames {got['t4_vms_origin_frames']} != EXPECTED "
          f"{EXPECTED['t4_vms_origin_frames']}")
    check(got["t4_ovmx_origin_frames"] == EXPECTED["t4_ovmx_origin_frames"],
          f"type-4 OVMX-origin frames {got['t4_ovmx_origin_frames']} != EXPECTED "
          f"{EXPECTED['t4_ovmx_origin_frames']}")
    check(got["t4_vms_origin_frames"] + got["t4_ovmx_origin_frames"] ==
          got["terminal_census"][4]["frames"],
          "type-4 VMS + OVMX origin split does not add up to the total census")

    # (B.1) the exact grounding frame vms-760/scs_dir.c cited, re-identified
    # by its own timestamp: which node sent it, and that it never recurs.
    wantg = EXPECTED["af2_grounding_frame"]
    g = got["af2_grounding_frame"]
    check(g is not None, f"af2 grounding frame idx {wantg['idx']} not found in {AF2_CAPTURE}")
    if g is not None:
        for key in ("mtype", "dest_conid", "src_conid",
                    "eth_src_is_vax2_hw", "eth_dst_is_vax1_logical"):
            check(g[key] == wantg[key],
                  f"af2 grounding frame {key}: {g[key]!r} != EXPECTED {wantg[key]!r}")
        check(abs(g["rel_s"] - wantg["rel_s"]) < 0.01,
              f"af2 grounding frame rel-time {g['rel_s']} != EXPECTED {wantg['rel_s']} "
              f"(scs_dir.c's own citation, 'rel~143.758')")

    check(got["af2_capture_is_two_real_vaxes_only"] ==
          EXPECTED["af2_capture_is_two_real_vaxes_only"],
          f"{AF2_CAPTURE}: has a source MAC that is neither VAX1 nor VAX2 -- "
          f"the 'no OVMX in this capture' claim no longer holds")

    # (B.2) the separate nine-reject-then-one-accept thread: exactly the
    # cited (idx, mtype, dest_conid) triples, in order.
    thread_got = got["af2_retry_thread_frames"]
    thread_want = EXPECTED["af2_retry_thread"]["frames"]
    check(thread_got == thread_want,
          f"af2 retry thread frames {thread_got} != EXPECTED {thread_want}")
    if thread_got == thread_want:
        rejected = [e for e in thread_got if e[1] == 4]
        accepted = [e for e in thread_got if e[1] == 2]
        check(len(rejected) == EXPECTED["af2_retry_thread"]["rejected_attempts"],
              f"af2 retry thread: {len(rejected)} op-4 (rejected) events, EXPECTED "
              f"{EXPECTED['af2_retry_thread']['rejected_attempts']}")
        check(len(accepted) == 1 and
              accepted[0][1] == EXPECTED["af2_retry_thread"]["accepted_attempt_mtype"],
              f"af2 retry thread: expected exactly one accepted attempt, got {accepted}")
        check(all(a[2] < b[2] for a, b in zip(thread_got, thread_got[1:])),
              "af2 retry thread: dest Con.IDs are not strictly increasing across "
              "the ten attempts")

    covered = set(EXPECTED) & set(WIRE_KEYS)
    return results, covered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", action="store_true", dest="just_print")
    args = ap.parse_args()

    if args.just_print:
        read_pcap = _read_pcap()
        got = measure(args.captures, read_pcap=read_pcap)
        for mt, row in got["terminal_census"].items():
            print(f"MTYPE {mt} ({MSGTYPE_NAMES[mt]}): {row}")
        print(f"type-4 VMS-origin: {got['t4_vms_origin_frames']}, "
              f"OVMX-origin: {got['t4_ovmx_origin_frames']}")
        print("af2 grounding frame:", got["af2_grounding_frame"])
        print("af2 capture is two real VAXes only:",
              got["af2_capture_is_two_real_vaxes_only"])
        print("af2 retry thread (idx, mtype, dest_conid):")
        for row in got["af2_retry_thread_frames"]:
            print("  ", row)
        return 0

    results, _covered = rederive(args.captures)
    bad = [label for ok, label in results if not ok]
    for label in bad:
        print(f"  FAIL {label}")
    print(f"{'FAIL' if bad else 'PASS'}: {len(results)} checks, {len(bad)} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
