#!/usr/bin/env python3
"""scs_t89_measure.py -- vms-a58: what SCS message types 8 and 9 ARE, measured.

    tools/cluster/scs_t89_measure.py           # re-measure and PASS/FAIL vs EXPECTED
    tools/cluster/scs_t89_measure.py --print   # just print what the captures say

Requires the lab-1 grounding library, host-only and NOT in git (CLAUDE.md rule
8): /data/training/vax/cluster/captures/*.pcap. Override with --captures.
Every figure quoted in docs/cluster-protocol-spec.md sec 4(h)(1f) and sec 5
comes out of this script, and tests/vmsscs/test_scs_t89_figures.py holds the
prose to it (and, on a lab host, re-derives it from the packets via rederive()).

============================================================================
THE QUESTION
============================================================================
docs/cluster-protocol-spec.md sec 4(h)(1b) turned up two message types nobody
had asked for: `8` and `9`, in the 58-content class, paired, sitting between
the ACCEPT_RSP and the DISCONNECT_REQ. It called them "a ninth and tenth"
connection-control message where *VAXcluster Principles* ch. 2 draws eight,
and said "nothing we hold identifies them".

Two leads were on the record when this item opened:

  (i)  WEAKENED -- "8/9 are the p. 2-44 special credit message". A special
       credit message must carry the local Pending Receive Credit COUNT, and
       8/9 carry a constant 1.
  (ii) LIVE -- the constant 1 is itself the lead: a field that is 1 on every
       frame of a paired type is a version, a flag, or a count-of-one.

Lead (ii) is what this script settles, and the answer is COUNT-OF-ONE. It gets
there by measuring the credit field as an ACCOUNT rather than as a value
histogram, which is the thing no previous census did.

============================================================================
WHAT IT MEASURES, AND WHY EACH PART EXISTS
============================================================================

(A) THE UNRESTRICTED CENSUS, BY LENGTH CLASS AND BY ORIGIN. Every
    envelope-conformant SCA frame in the library, keyed
    (SCA content length, message type [46:48], origin), with NO length filter
    -- the vms-c11 discipline, enforced by census_guard.check_census() rather
    than by intention. The non-conformant classes (the 70-content class and
    friends) are counted and REPORTED but never read at [46:48]: that is the
    vms-c11 mirror failure (spec sec 4(h)(1d)) and the guard refuses it too.

(B) THE OWNING SYSAP. Every dialogue is keyed by its Con.ID handle pair and
    attributed to the SYSAP its CONNECT_REQ named at [62:78] (spec sec
    4(h)(2)). This is the part the item asked for and it returns a NEGATIVE
    that has to be stated plainly: 131 of 131 type-8 dialogues are
    SCS$DIRECTORY -- and so are 131 of 131 dialogues that disconnect AT ALL.
    MSCP$DISK (101 accepted), VMS$VAXcluster (76) and SCA$TRANSPORT (16) never
    tear a connection down inside this library. So the SYSAP census CANNOT
    discriminate "an SCA-level exchange" from "an SCS$DIRECTORY-level
    exchange": the entire teardown population is one SYSAP. Recorded as a
    measured limit, not smoothed over.

(C) THE STRUCTURAL INVARIANTS of the 8/9 exchange. Biconditional with
    teardown, one exchange per dialogue, handles swapped, sender identity,
    ordering. All are N-of-N over the library.

(D) THE CREDIT ACCOUNT -- the part that answers the constant-1 lead.
    *VAXcluster Principles* pp. 2-43..2-44 (quoted with page cites in
    src/vmsscs/include/scs_credit.h) states the rule: "local SCS copies the
    local Pending Receive Credit count into this credit field, and then resets
    to 0 the local Pending Receive Credit count", the remote adding it to its
    Send Credit count. That is a per-connection LEDGER, so it is falsifiable
    frame by frame, and this part runs it: for every frame a node sends,
    predict [48:50] from the number of messages it has received since its own
    previous credit-bearing frame.

    Result: with types {8, 9, 10} counted as consuming a receive buffer and
    every other type outside the account, the ledger predicts the credit field
    EXACTLY -- 938 of 938 frames, zero residuals. Types 8 and 9 are therefore
    inside the credit account and behave identically to the type-10
    application messages around them.

    That kills lead (ii): the field is not a version and not a flag. It is a
    COUNT, and it is 1 because every dialogue in this library is the strictly
    alternating SCS$DIRECTORY lookup dialogue, which never leaves more than one
    buffer outstanding in either direction. The same field, in the same
    dialogues, carries 0 on the first application message of every connection
    (131 of them) and 1 on the other 545 -- so "constant" was a property of the
    WORKLOAD, not of the field.

    It does NOT restore lead (i). See sec 4(h)(1f) and sec 5 for the two
    position arguments that stand against the p. 2-44 reading independently of
    the constant: the exchange is UNCONDITIONAL (131/131 teardowns, never
    mid-dialogue on a low-credit trigger) and it is ANSWERED (131/131), where
    p. 2-44 describes a triggered message with no reply.

(E) THE ONE RESIDUAL, kept because it is evidence and not noise: the
    initiating DISCONNECT_REQ carries credit 0 while the ledger says 1 is
    owed, in 131 of 131 dialogues. Connection-control frames (types 1, 3, 4,
    5, 6, 7) carry credit 0 in 100% of frames in this library; they neither
    consume nor return. The last credit on a connection is simply never
    returned, because the connection is being destroyed.

(F) LATENCY of 8->9 and of 9->the first DISCONNECT_REQ, which is what says the
    exchange is a machine-speed prologue to teardown and not a periodic timer.

ORIGIN SPLIT -- THE OUI RULE, same as scs_disc_measure.py and
scs_reason_measure.py: a real lab VAX sources from 08:00:2b (DEC OUI) or
aa:00:04 (DECnet logical). OVMX sources from a locally-administered Linux tap
MAC. OVMX frames are counted and REPORTED separately, never mixed into a claim
about VMS -- counting our own emissions as evidence about a VAX is circular,
and it matters here: OVMX emits 42 type-9 frames in this library (it already
answers a received type 8) and ZERO type-8 frames.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""
import argparse
import collections
import glob
import os
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

SCA_ETHERTYPE = b"\x60\x07"
MSGTYPE_OFF = 46          # SCA content [46:48], LE u16 -- the message type
CREDIT_OFF = 48           # SCA content [48:50], LE u16 -- the credit field
CONID_OFF = 50            # [50:54] remote handle, [54:58] local handle
SYSAP_NAME_OFF = 62       # [62:78] on the 110-content CONNECT_REQ (sec 4(h)(2))

T8 = 8
T9 = 9
T_APP = 10                # the application-message MTYPE (vms-54f)
T_CONNECT_REQ = 0
T_ACCEPT_RSP = 3
T_DISCONNECT_REQ = 6

# The types that consume a receive buffer and therefore participate in the
# p. 2-43 debit/credit account. This is the MODEL under test in part (D) -- it
# is not an assumption, it is the hypothesis the ledger residual falsifies.
LEDGER_TYPES = (T8, T9, T_APP)

# vms-69c: this census is UNRESTRICTED by construction -- it selects every
# envelope-conformant length class the library contains, so check_census() has
# nothing to exclude and needs no restrict_reason. It is called anyway, because
# a future edit that narrows the selection must hit the refusal rather than
# slip through. The one thing it is asked to justify is the OTHER direction:
# nothing non-conformant is read at [46:48], which is spec sec 4(h)(1d).


def lab1_only(paths):
    """Return `paths` unchanged, or die naming every non-lab-1 capture in them,
    checked against tools/cluster/capture_manifest.py's declared manifest
    (vms-beb). Imported LAZILY for the same reason scs_disc_measure.py does it:
    the figures gate reads EXPECTED out of this module without touching a
    capture, and its mutation harness copies this file into a scratch tree."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import capture_manifest
    return capture_manifest.check_paths(paths, capture_manifest.LAB1)


def _read_pcap():
    """The pcap reader, imported LAZILY (see lab1_only)."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from dissect_sca import read_pcap
    return read_pcap


def _census_guard():
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import census_guard
    return census_guard


def is_vms_origin(frame):
    """The OUI rule. True for a real lab VAX, False for OVMX's tap."""
    mac = frame[6:12].hex()
    return mac.startswith("08002b") or mac.startswith("aa000400")


# Every structural invariant this script counts, named INDEPENDENTLY of the
# EXPECTED table below. measure() seeds its counter from this tuple so that an
# invariant whose whole point is to come out 0 is REPORTED as a measured zero
# rather than dropped as an absent key -- and so that deleting a row from
# EXPECTED cannot also delete the measurement that would have contradicted it.
INVARIANT_NAMES = (
    "t8_frames", "t9_frames", "dialogues_with_t8",
    "t8_dialogues_that_disconnect", "disconnecting_dialogues_without_t8",
    "t8_sender_is_connection_initiator", "t8_sender_is_disconnect_initiator",
    "t8_sourced_by_ovmx", "t9_answers_t8_with_handles_swapped",
    "t8_to_disc_gap_is_exactly_t9", "t8_before_last_application_message",
    "t8_unanswered",
    # OVMX's own posture, needed for the sec 5 emission ruling: has OVMX ever
    # INITIATED a teardown (rank 0) as opposed to matching one (rank 1)?
    "ovmx_disconnect_req_rank0", "ovmx_disconnect_req_rank1",
)

# ---------------------------------------------------------------------------
# THE RECORDED MEASUREMENT. Every figure here is quoted in
# docs/cluster-protocol-spec.md sec 4(h)(1f) / sec 5, and
# tests/vmsscs/test_scs_t89_figures.py asserts it is still there AND (on a lab
# host) still comes out of the packets.
# Last re-derived 2026-08-06 on workshop against the 47-capture lab-1 library.
# ---------------------------------------------------------------------------
EXPECTED = {
    "n_captures": 47,

    # (A) the unrestricted census: (SCA content length, msgtype) -> frames,
    #     split VMS-origin / OVMX-origin. NO length filter.
    "census_vms": {
        (58, 5): 697, (58, 7): 223, (58, 8): 131, (58, 9): 89,
        (62, 3): 258, (62, 4): 453, (62, 6): 220,
        (66, 1): 778,
        (86, 10): 202, (94, 10): 3621,
        (110, 0): 1101, (110, 2): 324, (110, 10): 2889,
        (190, 10): 299224,
    },
    # Zero entries are CARRIED, not dropped: "OVMX has emitted no type 8" is a
    # claim the sec 5 ruling rests on, and a missing key would not state it.
    "census_ovmx": {
        (58, 5): 1, (58, 7): 42, (58, 8): 0, (58, 9): 42,
        (62, 3): 123, (62, 4): 280, (62, 6): 42,
        (66, 1): 338,
        (86, 10): 0, (94, 10): 585,
        (110, 0): 396, (110, 2): 70, (110, 10): 0,
        (190, 10): 7446,
    },
    # Frames that carry the SCA ethertype but FAIL the envelope-conformance
    # test, so [46:48] is not a message type in them (spec sec 4(h)(1d)).
    # Reported so the reader can see what the census did NOT read, and how big
    # it is; the biggest two are the 120-content HELLO and the 41-content class.
    "non_conformant_total": 93232,
    "non_conformant_classes": 39,

    # (B) dialogues by owning SYSAP, from the CONNECT_REQ target name [62:78].
    #     "torn_down" = the dialogue carries a DISCONNECT_REQ (type 6);
    #     "with_t8" = it carries a type 8.
    "sysap": {
        "MSCP$DISK":      {"dialogues": 1645, "accepted": 101, "torn_down": 0, "with_t8": 0},
        "SCA$TRANSPORT":  {"dialogues": 32, "accepted": 16, "torn_down": 0, "with_t8": 0},
        "SCS$DIRECTORY":  {"dialogues": 353, "accepted": 188, "torn_down": 131, "with_t8": 131},
        "VMS$VAXcluster": {"dialogues": 154, "accepted": 76, "torn_down": 0, "with_t8": 0},
        "?":              {"dialogues": 150, "accepted": 0, "torn_down": 0, "with_t8": 0},
    },

    # (C) the structural invariants. Every one is N-of-N over the library.
    "invariants": {
        "t8_frames": 131,
        "t9_frames": 131,
        "dialogues_with_t8": 131,
        # biconditional with teardown, both directions:
        "t8_dialogues_that_disconnect": 131,
        "disconnecting_dialogues_without_t8": 0,
        # who sends the 8:
        "t8_sender_is_connection_initiator": 131,
        "t8_sender_is_disconnect_initiator": 131,
        "t8_sourced_by_ovmx": 0,
        # the 9:
        "t9_answers_t8_with_handles_swapped": 131,
        # ordering: nothing but the 9 between the 8 and the DISCONNECT_REQ,
        # and the 8 never precedes an application message on its connection.
        "t8_to_disc_gap_is_exactly_t9": 131,
        "t8_before_last_application_message": 0,
        # OVMX has never INITIATED a teardown in this library -- every one of
        # its DISCONNECT_REQ frames is the matching half. That is why its
        # missing type 8 has not yet violated the invariant above.
        "ovmx_disconnect_req_rank0": 0,
        "ovmx_disconnect_req_rank1": 42,
    },

    # (D) the credit account. Predicted-vs-observed [48:50] under the p. 2-43
    #     ledger with LEDGER_TYPES consuming a buffer. Zero residuals.
    "ledger_ok": {8: 131, 9: 131, 10: 676},
    "ledger_bad": {},
    # the first application message of a connection has received nothing yet:
    "app_credit_first_in_dialogue": {0: 131},
    "app_credit_later": {1: 545},

    # (E) the residual that is evidence: the initiating DISCONNECT_REQ carries
    #     0 while the ledger says 1 is owed, and connection-control types carry
    #     0 in 100% of frames.
    "disc_leaves_one_credit_unreturned": 131,
    "credit_zero_only_types": (1, 3, 4, 5, 6, 7),

    # (F) latency, seconds, VMS-origin type 8. Machine-speed, not a timer.
    "latency": {
        "t8_to_t9_n": 131,
        "t8_to_t9_max": 0.003122,
        "t9_to_disc_n": 131,
        "t9_to_disc_max": 0.002112,
        "t8_unanswered": 0,
    },

    # The always-constant SCA content offsets of each frame over its VMS-origin
    # population -- i.e. exactly what a template may replay.
    "const_offsets": {
        8: (0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 23, 24, 25,
            28, 29, 32, 33, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
            49, 51, 55),
        9: (0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15, 17, 23, 24, 25, 28,
            29, 32, 33, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
            51, 55),
    },
}


def load(path, read_pcap):
    """Every envelope-conformant SCA frame in one pcap, in wire order."""
    cg = _census_guard()
    out = []
    for ts, tu, _o, frame in read_pcap(path):
        if len(frame) < 16 or frame[12:14] != SCA_ETHERTYPE:
            continue
        pl = frame[14:]
        if len(pl) < 2:
            continue
        scalen = (pl[0] | (pl[1] << 8)) + 2
        if len(pl) < scalen:
            continue
        pl = bytes(pl[:scalen])
        if not cg.envelope_conformant(pl):
            continue
        out.append({
            "t": ts + tu / 1e6,
            "len": scalen,
            "mt": pl[MSGTYPE_OFF] | (pl[MSGTYPE_OFF + 1] << 8),
            "credit": pl[CREDIT_OFF] | (pl[CREDIT_OFF + 1] << 8),
            "remote": int.from_bytes(pl[CONID_OFF:CONID_OFF + 4], "little"),
            "local": int.from_bytes(pl[CONID_OFF + 4:CONID_OFF + 8], "little"),
            "src": frame[6:12].hex(),
            "vms": is_vms_origin(frame),
            "pl": pl,
        })
    return out


def measure(capdir):
    read_pcap = _read_pcap()
    cg = _census_guard()
    files = lab1_only(sorted(glob.glob(os.path.join(capdir, "*.pcap"))))

    # vms-69c: the guard. This census selects EVERY envelope-conformant class,
    # so there is nothing to justify -- and that is the point of calling it: an
    # edit that narrows the selection reds instead of silently under-sampling.
    conformant, raw = cg.population(files, read_pcap)
    guard_report = cg.check_census(sorted(conformant), conformant, raw,
                                   label="scs_t89_measure.py: ")

    skipped = []
    census_vms = collections.Counter()
    census_ovmx = collections.Counter()
    non_conf = collections.Counter()
    sysap = collections.defaultdict(
        lambda: {"dialogues": 0, "accepted": 0, "torn_down": 0, "with_t8": 0})
    # Seeded with every invariant name, INCLUDING the ones whose whole point is
    # to come out 0: a Counter that never sees the event drops the key, and a
    # missing key is not the same claim as a measured zero.
    inv = collections.Counter({k: 0 for k in INVARIANT_NAMES})
    ledger_ok = collections.Counter()
    ledger_bad = collections.Counter()
    app_first = collections.Counter()
    app_later = collections.Counter()
    credit_hist = collections.defaultdict(collections.Counter)
    disc_unreturned = 0
    lat_89 = []
    lat_96 = []
    byte_vals = {T8: collections.defaultdict(collections.Counter),
                 T9: collections.defaultdict(collections.Counter)}

    for path in files:
        try:
            frames = load(path, read_pcap)
        except Exception as exc:              # a truncated file must not hide
            skipped.append((os.path.basename(path), str(exc)))   # the rest
            continue

        # (A) the unrestricted census, and the non-conformant population.
        for f in frames:
            (census_vms if f["vms"] else census_ovmx)[(f["len"], f["mt"])] += 1
            credit_hist[f["mt"]][f["credit"]] += 1
        for _s, _u, _o, frame in read_pcap(path):
            if len(frame) < 16 or frame[12:14] != SCA_ETHERTYPE:
                continue
            pl = frame[14:]
            if len(pl) < 2:
                continue
            n = (pl[0] | (pl[1] << 8)) + 2
            if len(pl) < n:
                continue
            if not cg.envelope_conformant(bytes(pl[:n])):
                non_conf[n] += 1

        # handle -> the SYSAP its CONNECT_REQ named, and who opened it.
        handle_sysap = {}
        connect_src = {}
        for f in frames:
            if f["len"] == 110 and f["mt"] == T_CONNECT_REQ and f["local"]:
                handle_sysap.setdefault(
                    f["local"], f["pl"][SYSAP_NAME_OFF:SYSAP_NAME_OFF + 16]
                    .decode("latin1").strip())
                connect_src.setdefault(f["local"], f["src"])

        dialogues = collections.defaultdict(list)
        for f in frames:
            if f["local"] == 0 and f["remote"] == 0:
                continue
            dialogues[tuple(sorted((f["remote"], f["local"])))].append(f)

        for key, seq in dialogues.items():
            seq.sort(key=lambda x: x["t"])
            mts = [f["mt"] for f in seq]
            name = handle_sysap.get(key[0]) or handle_sysap.get(key[1]) or "?"
            # OVMX's DISCONNECT_REQ rank within its own dialogue: 0 = it
            # initiated the teardown, 1 = it matched one the peer initiated.
            rank = 0
            for f in seq:
                if f["mt"] != T_DISCONNECT_REQ:
                    continue
                if not f["vms"]:
                    inv["ovmx_disconnect_req_rank%d" % (1 if rank else 0)] += 1
                rank += 1

            row = sysap[name]
            row["dialogues"] += 1
            if T_ACCEPT_RSP in mts:
                row["accepted"] += 1
            if T_DISCONNECT_REQ in mts:
                row["torn_down"] += 1
            if T8 in mts:
                row["with_t8"] += 1

            if T8 not in mts:
                if T_DISCONNECT_REQ in mts:
                    inv["disconnecting_dialogues_without_t8"] += 1
                continue

            # ---- (C) the invariants -------------------------------------
            inv["dialogues_with_t8"] += 1
            f8 = next(f for f in seq if f["mt"] == T8)
            if not f8["vms"]:
                inv["t8_sourced_by_ovmx"] += 1
            opener = connect_src.get(f8["local"]) or connect_src.get(f8["remote"])
            if opener is not None and opener == f8["src"]:
                inv["t8_sender_is_connection_initiator"] += 1
            nines = [g for g in seq if g["mt"] == T9 and g["t"] > f8["t"]]
            if nines and nines[0]["remote"] == f8["local"] \
                    and nines[0]["local"] == f8["remote"]:
                inv["t9_answers_t8_with_handles_swapped"] += 1
            if nines:
                lat_89.append(nines[0]["t"] - f8["t"])
            else:
                inv["t8_unanswered"] += 1
            discs = [g for g in seq if g["mt"] == T_DISCONNECT_REQ]
            if discs:
                inv["t8_dialogues_that_disconnect"] += 1
                if discs[0]["src"] == f8["src"]:
                    inv["t8_sender_is_disconnect_initiator"] += 1
                if nines:
                    lat_96.append(discs[0]["t"] - nines[0]["t"])
                between = [g["mt"] for g in seq[seq.index(f8) + 1:seq.index(discs[0])]]
                if between == [T9]:
                    inv["t8_to_disc_gap_is_exactly_t9"] += 1
            last_app = max((g["t"] for g in seq if g["mt"] == T_APP), default=None)
            if last_app is not None and f8["t"] < last_app:
                inv["t8_before_last_application_message"] += 1

            # ---- (D)/(E) the credit ledger ------------------------------
            # p. 2-43: the credit field carries the sender's Pending Receive
            # Credit count, and sending resets it to 0. Start at the ACCEPT_RSP,
            # after the initial grant that types 0/2 carry.
            start = [i for i, g in enumerate(seq) if g["mt"] == T_ACCEPT_RSP]
            if not start:
                continue
            owed = collections.Counter()
            per_dir = collections.Counter()
            for g in seq[start[0]:]:
                s = g["src"]
                if g["mt"] in LEDGER_TYPES:
                    if g["credit"] == owed[s]:
                        ledger_ok[g["mt"]] += 1
                    else:
                        ledger_bad[g["mt"]] += 1
                    if g["mt"] == T_APP:
                        per_dir[s] += 1
                        (app_first if per_dir[s] == 1 and sum(per_dir.values()) == 1
                         else app_later)[g["credit"]] += 1
                elif g["mt"] == T_DISCONNECT_REQ and owed[s]:
                    disc_unreturned += 1
                owed[s] = 0
                if g["mt"] in LEDGER_TYPES:
                    for other in set(h["src"] for h in seq) - {s}:
                        owed[other] += 1

            # ---- the replayable template --------------------------------
            for g in seq:
                if g["mt"] in byte_vals and g["vms"]:
                    for i, b in enumerate(g["pl"]):
                        byte_vals[g["mt"]][i][b] += 1

    # Both origin columns span the SAME key set, so a class one population
    # never sourced is reported as a measured 0 rather than dropped -- "OVMX
    # emitted no type 8" has to be sayable.
    for k in set(census_vms) | set(census_ovmx):
        census_vms[k] += 0
        census_ovmx[k] += 0

    inv["t8_frames"] = sum(census_vms[k] + census_ovmx[k]
                           for k in set(census_vms) | set(census_ovmx) if k[1] == T8)
    inv["t9_frames"] = sum(census_vms[k] + census_ovmx[k]
                           for k in set(census_vms) | set(census_ovmx) if k[1] == T9)

    lat_89.sort()
    lat_96.sort()
    return {
        "n_captures": len(files),
        "skipped": skipped,
        "census_guard": guard_report,
        "census_vms": dict(census_vms),
        "census_ovmx": dict(census_ovmx),
        "non_conformant_total": sum(non_conf.values()),
        "non_conformant_classes": len(non_conf),
        "sysap": {k: dict(v) for k, v in sysap.items()},
        "invariants": dict(inv),
        "ledger_ok": dict(ledger_ok),
        "ledger_bad": dict(ledger_bad),
        "app_credit_first_in_dialogue": dict(app_first),
        "app_credit_later": dict(app_later),
        "disc_leaves_one_credit_unreturned": disc_unreturned,
        "credit_zero_only_types": tuple(
            sorted(mt for mt, h in credit_hist.items() if set(h) == {0})),
        "credit_hist": {mt: dict(h) for mt, h in credit_hist.items()},
        "latency": {
            "t8_to_t9_n": len(lat_89),
            "t8_to_t9_max": round(lat_89[-1], 6) if lat_89 else 0.0,
            "t9_to_disc_n": len(lat_96),
            "t9_to_disc_max": round(lat_96[-1], 6) if lat_96 else 0.0,
            "t8_unanswered": inv.get("t8_unanswered", 0),
        },
        "const_offsets": {
            mt: tuple(i for i in sorted(byte_vals[mt]) if len(byte_vals[mt][i]) == 1)
            for mt in byte_vals
        },
    }


def report(m):
    print(f"captures: {m['n_captures']}")
    for base, err in m["skipped"]:
        print(f"  SKIPPED {base}: {err}")
    print("\n=== CENSUS GUARD (vms-69c) ===")
    print(_census_guard().format_report(m["census_guard"]))

    print("\n(A) UNRESTRICTED census -- (SCA content length, msgtype) -> frames")
    keys = sorted(set(m["census_vms"]) | set(m["census_ovmx"]))
    print("    %-18s %10s %10s" % ("class", "VMS", "OVMX"))
    for k in keys:
        print("    %3d B  msgtype %-4d %10d %10d"
              % (k[0], k[1], m["census_vms"].get(k, 0), m["census_ovmx"].get(k, 0)))
    print("    NOT read at [46:48] (fails the envelope test, spec 4(h)(1d)): "
          "%d frames across %d length classes"
          % (m["non_conformant_total"], m["non_conformant_classes"]))

    print("\n(B) dialogues by owning SYSAP (CONNECT_REQ target name [62:78])")
    for name in sorted(m["sysap"]):
        r = m["sysap"][name]
        print("    %-16s dialogues=%-6d accepted=%-5d torn-down=%-5d with type 8=%d"
              % (name, r["dialogues"], r["accepted"], r["torn_down"], r["with_t8"]))

    print("\n(C) structural invariants of the 8/9 exchange")
    for k in sorted(m["invariants"]):
        print("    %-42s %d" % (k, m["invariants"][k]))

    print("\n(D) the p. 2-43 credit ledger, predicted vs observed [48:50]")
    print("    types counted as consuming a receive buffer: %s" % (LEDGER_TYPES,))
    print("    agreeing: %s" % m["ledger_ok"])
    print("    RESIDUALS: %s" % (m["ledger_bad"] or "none"))
    print("    first application message of a dialogue: credit %s"
          % m["app_credit_first_in_dialogue"])
    print("    every later one:                         credit %s"
          % m["app_credit_later"])

    print("\n(E) types whose credit field is 0 in 100%% of frames: %s"
          % (m["credit_zero_only_types"],))
    print("    initiating DISCONNECT_REQ frames leaving 1 credit unreturned: %d"
          % m["disc_leaves_one_credit_unreturned"])
    print("    full credit histogram by msgtype:")
    for mt in sorted(m["credit_hist"]):
        print("      msgtype %-3d %s" % (mt, dict(sorted(m["credit_hist"][mt].items()))))

    print("\n(F) latency, seconds")
    L = m["latency"]
    print("    8 -> 9        n=%d max=%.6f unanswered=%d"
          % (L["t8_to_t9_n"], L["t8_to_t9_max"], L["t8_unanswered"]))
    print("    9 -> DISC_REQ n=%d max=%.6f" % (L["t9_to_disc_n"], L["t9_to_disc_max"]))

    print("\nconstant SCA content offsets (VMS-origin population)")
    for mt in sorted(m["const_offsets"]):
        print("    msgtype %d (%d of %d offsets): %s"
              % (mt, len(m["const_offsets"][mt]),
                 58, list(m["const_offsets"][mt])))


def compare_results(m):
    """Every EXPECTED figure re-derived, as [(ok, message), ...]."""
    out = []

    def ck(cond, what):
        out.append((bool(cond), what))

    ck(m["n_captures"] == EXPECTED["n_captures"],
       f"n_captures {m['n_captures']} != {EXPECTED['n_captures']}")

    for name in ("census_vms", "census_ovmx"):
        want = EXPECTED[name]
        got = m[name]
        ck(got == want, f"{name} changed: extra="
                        f"{ {k: v for k, v in got.items() if want.get(k) != v} } "
                        f"missing={ {k: v for k, v in want.items() if got.get(k) != v} }")
    for k in ("non_conformant_total", "non_conformant_classes",
              "disc_leaves_one_credit_unreturned"):
        ck(m[k] == EXPECTED[k], f"{k} {m[k]} != {EXPECTED[k]}")
    ck(tuple(m["credit_zero_only_types"]) == tuple(EXPECTED["credit_zero_only_types"]),
       f"credit_zero_only_types {m['credit_zero_only_types']} != "
       f"{EXPECTED['credit_zero_only_types']}")

    ck(set(m["sysap"]) == set(EXPECTED["sysap"]),
       f"sysap names {sorted(m['sysap'])} != {sorted(EXPECTED['sysap'])}")
    for name, want in EXPECTED["sysap"].items():
        got = m["sysap"].get(name, {})
        ck(got == want, f"sysap {name} {got} != {want}")

    ck(set(m["invariants"]) >= set(EXPECTED["invariants"]),
       f"invariants missing {sorted(set(EXPECTED['invariants']) - set(m['invariants']))}")
    for name, want in EXPECTED["invariants"].items():
        ck(m["invariants"].get(name, 0) == want,
           f"invariant {name} {m['invariants'].get(name, 0)} != {want}")

    for name in ("ledger_ok", "ledger_bad", "app_credit_first_in_dialogue",
                 "app_credit_later"):
        ck(m[name] == EXPECTED[name], f"{name} {m[name]} != {EXPECTED[name]}")

    for field, want in EXPECTED["latency"].items():
        ck(m["latency"][field] == want,
           f"latency {field} {m['latency'][field]} != {want}")

    for mt, want in EXPECTED["const_offsets"].items():
        ck(tuple(m["const_offsets"].get(mt, ())) == tuple(want),
           f"constant offsets for msgtype {mt} changed: "
           f"{list(m['const_offsets'].get(mt, ()))}")
    return out


WIRE_KEYS = ("n_captures", "census_vms", "census_ovmx", "non_conformant_total",
             "non_conformant_classes", "sysap", "invariants", "ledger_ok",
             "ledger_bad", "app_credit_first_in_dialogue", "app_credit_later",
             "disc_leaves_one_credit_unreturned", "credit_zero_only_types",
             "latency", "const_offsets")
NON_WIRE_KEYS = ()


def rederive(capdir, **_kw):
    """THE ctest GATE'S ENTRY POINT (vms-371)."""
    return compare_results(measure(capdir)), set(WIRE_KEYS)


def compare(m):
    out = compare_results(m)
    fails = [what for ok, what in out if not ok]
    for what in fails:
        print(f"  FAIL {what}")
    print(f"{'FAIL' if fails else 'PASS'}: {len(out)} checks, {len(fails)} failure(s)")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.captures):
        print(f"no capture directory at {args.captures} -- this script needs the "
              f"host-only lab captures (CLAUDE.md rule 8). Use --captures.",
              file=sys.stderr)
        return 2
    m = measure(args.captures)
    report(m)
    if args.just_print:
        return 0
    print()
    return compare(m)


if __name__ == "__main__":
    sys.exit(main())
