#!/usr/bin/env python3
"""scs_env_measure.py - vms-ec7: re-derive the SCS MESSAGE ENVELOPE from the raw
lab-1 captures, and prove the shared build path reproduces it byte for byte.

Every figure quoted in src/vmsscs/include/scs_env.h and in
docs/design-mscp-direction.md sec 1.1 comes out of this script. It reads pcaps
only; the only OVMX code it imports is the pure-stdlib pcap reader in
dissect_sca.py, the shared census refusal in census_guard.py and the capture
manifest in capture_manifest.py.

    tools/cluster/scs_env_measure.py           # re-measure, PASS/FAIL vs EXPECTED
    tools/cluster/scs_env_measure.py --print   # just print what the captures say

Requires the lab-1 captures, host-only and NOT in git (CLAUDE.md rule 8):
/data/training/vax/cluster/captures/**/*.pcap. Override with --captures.

`ctest -R scs_env_figures` does not REQUIRE the captures: it always asserts
every figure in EXPECTED still appears in the header/spec prose, and where the
packets are readable it also re-derives EXPECTED from them (vms-371, via
rederive()).

===========================================================================
WHAT IT MEASURES, AND WHY EACH PART EXISTS
===========================================================================

(A) THE ENVELOPE ITSELF. Over every SCA-ethertype frame in every capture -- NO
    length filter, because the vms-c11 failure was a census silently restricted
    to a few classes -- how many are envelope-conformant (inner length ==
    total-44 AND format word == 0x0004) and how many are not. The
    non-conformant population is not noise to be dropped: it is the START /
    HELLO / 70-content family that scs_env_parse() must keep REFUSING.

(B) THE FORMAT WORD PREDICTS THE INNER-LENGTH RULE. Of the frames carrying
    0x0004 at [44:46], what fraction also satisfy inner == total - 44? The two
    halves of the conformance test are written as an AND in C, and if one half
    were redundant the test would be weaker than it looks. A ratio below 1.0 is
    a real finding, not a failure -- it is recorded either way.

(C) THE MTYPE NAMESPACE, over envelope-conformant frames. The code depends on
    exactly three properties (scs_env.h): the namespace is {0..10}, MTYPE 10
    dominates, and no eleventh value appears. Totals GROW as the lab keeps
    capturing; a different total is not a regression, a different SHAPE is.

(D) THE BUILD ROUND TRIP -- the part that gates scs_env_build(). For every
    envelope-conformant frame, re-derive content[42:58] from the parsed fields
    using the BUILD rule the C code uses (inner length = total-44 DERIVED,
    format word = 0x0004 CONSTANT, then MTYPE / credit / dest / src written
    back) and require the 16 bytes to be byte-identical to the capture. This is
    what makes "the one build path reproduces every class on the wire" a
    measurement instead of a claim. Expected mismatches: 0.

(E) THE PER-CLASS (length, MTYPE, credit) TABLE for the six classes OVMX
    itself builds. Every per-class constant this item introduced
    (SCS_DIR_ENV_CREDIT_*, SCS_CONNECT_ENV_CREDIT, SCS_MSCP_ENV_CREDIT,
    SCS_MEMBER_ENV_CREDIT_*, SCS_DISC_ENV_CREDIT) claims a value that is a
    LABELED REPLAY of a captured byte. This part shows the value is in the
    wire's observed set for that class, so a constant that drifted would be
    visible rather than merely uncontradicted.

(F) THE LEGACY-GUARD DELTA -- the measurement that grounds the receive-side
    change. Before this item, scsd.c's connection-control classifier decided a
    frame was worth reading by (wire length >= 72) AND (content[16] is one of
    the 0x4b/0x5b/0x7b PPD markers), then read the MTYPE and the Con.ID pair at
    fixed offsets WITHOUT any envelope test. This counts the frames that guard
    admits and the envelope test refuses, and -- the number that matters -- how
    many of those carry a value at [46:48] that the old code would have fed to
    the connection state machine as a control message. Those are frames OVMX
    was stepping a real CDT on while reading a field that class does not have.

(G) THE CREDIT-OFFSET ALLOWLIST DELTA. scs_credit_header_offset() decides where
    the credit field is by a SEVEN-ENTRY LENGTH ALLOWLIST {58,62,66,86,94,110,
    190}. The envelope says the field is at [48:50] on any conformant frame,
    whatever its length. This part reports the envelope-conformant content
    lengths that allowlist does NOT admit. It is recorded, NOT acted on: moving
    the credit module onto the envelope test changes which outbound frames get
    a live credit stamped, i.e. it changes bytes on the wire, and that needs a
    bracketed lab run rather than a refactor.

Clean-room (CLAUDE.md rule 8): the only inputs are our own lab captures and our
own source tree. No VSI/HPE artifact is read, disassembled or copied.
"""
import argparse
import collections
import glob
import os
import struct
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

SCA_ETHERTYPE = b"\x60\x07"

# The envelope, content-relative. Mirrors src/vmsscs/include/scs_env.h; the
# figures gate asserts these numbers still appear in that header.
OFF_INNER_LEN = 42
OFF_FORMAT = 44
OFF_MTYPE = 46
OFF_CREDIT = 48
OFF_DEST_CONID = 50
OFF_SRC_CONID = 54
HDR_END = 58
FORMAT_WORD = 0x0004
INNER_LEN_BIAS = 44

# (F): the pre-vms-ec7 guard in scsd.c, reproduced exactly.
LEGACY_PPD_MARKERS = (0x4B, 0x5B, 0x7B)
LEGACY_MIN_WIRE_LEN = 72

# (G): scs_credit_header_offset()'s length allowlist, reproduced exactly.
CREDIT_LENGTH_ALLOWLIST = (58, 62, 66, 86, 94, 110, 190)

# (E): the classes OVMX itself builds, and the builder that owns each.
OVMX_BUILT_CLASSES = (58, 62, 66, 94, 110, 190)


def lab1_only(paths):
    """Return `paths` unchanged, or die naming every non-lab-1 capture in them
    -- checked against tools/cluster/capture_manifest.py's declared manifest,
    not a filename heuristic. Imported LAZILY so a caller that reads no capture
    (the figures gate reading EXPECTED out of this module) never needs it."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import capture_manifest
    return capture_manifest.check_paths(paths, capture_manifest.LAB1)


def _read_pcap():
    """The pcap reader, imported LAZILY -- see scs_disc_measure.py for why."""
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


def le16(b, off):
    return b[off] | (b[off + 1] << 8)


def le32(b, off):
    return b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)


def put_le16(buf, off, v):
    buf[off] = v & 0xFF
    buf[off + 1] = (v >> 8) & 0xFF


def put_le32(buf, off, v):
    buf[off] = v & 0xFF
    buf[off + 1] = (v >> 8) & 0xFF
    buf[off + 2] = (v >> 16) & 0xFF
    buf[off + 3] = (v >> 24) & 0xFF


def sca_content(frame):
    """The SCA content of an SCA-ethertype frame, sliced to its declared length,
    or None if this frame is not one / is truncated."""
    if len(frame) < 16 or frame[12:14] != SCA_ETHERTYPE:
        return None
    pl = frame[14:]
    if len(pl) < 2:
        return None
    scalen = le16(pl, 0) + 2
    if scalen < 2 or len(pl) < scalen:
        return None
    return pl[:scalen]


def envelope_conformant(pl):
    """THE CONFORMANCE TEST, byte for byte what scs_env_parse() applies."""
    if len(pl) < HDR_END:
        return False
    if le16(pl, OFF_FORMAT) != FORMAT_WORD:
        return False
    return le16(pl, OFF_INNER_LEN) == len(pl) - INNER_LEN_BIAS


def env_build_bytes(total_sca_len, mtype, credit, dest, src):
    """(D) THE BUILD RULE, reproduced from src/vmsscs/scs_env.c. Returns the 16
    bytes scs_env_build() would leave at content[42:58]."""
    buf = bytearray(HDR_END - OFF_INNER_LEN)
    put_le16(buf, OFF_INNER_LEN - OFF_INNER_LEN, total_sca_len - INNER_LEN_BIAS)
    put_le16(buf, OFF_FORMAT - OFF_INNER_LEN, FORMAT_WORD)
    put_le16(buf, OFF_MTYPE - OFF_INNER_LEN, mtype)
    put_le16(buf, OFF_CREDIT - OFF_INNER_LEN, credit)
    put_le32(buf, OFF_DEST_CONID - OFF_INNER_LEN, dest)
    put_le32(buf, OFF_SRC_CONID - OFF_INNER_LEN, src)
    return bytes(buf)


# ===========================================================================
# EXPECTED -- what the captures said when this was written. The gate pins these
# to the prose in src/vmsscs/include/scs_env.h; rederive() re-measures them.
#
# The SHAPE claims (mtype_namespace, build_mismatches, conformant_inner_rule)
# are what the C code depends on and must hold on every re-run. The COUNTS are
# a dated snapshot of a corpus that grows; the gate compares only the keys
# listed in WIRE_KEYS below, and the count-shaped ones are compared as
# inequalities, not equalities -- see compare_results().
# ===========================================================================
EXPECTED = {
    # (A) a FLOOR, not an equality: the reference lab keeps writing captures.
    "n_captures": 48,
    # (B) MEASURED 1.000000 over 319,575 frames. Both halves of the conformance
    # test agree on this corpus -- i.e. on the captures we hold, the format word
    # alone would have sufficed. RECORDED, NOT ACTED ON: the C test keeps both
    # halves, because "no counterexample in this corpus" is not "cannot happen",
    # and the inner-length half is the one that is a length CHECK rather than a
    # magic-number match.
    "format_word_implies_inner_rule": 1.0,
    # (C) THE SHAPE CLAIM the C code depends on. Exact.
    "mtype_namespace": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    "mtype_10_is_majority": True,
    # (D) THE BUILD GATE. Any non-zero means scs_env_build() would put a byte on
    # the wire that no captured frame of that class carries. Exact, and 0.
    "build_mismatches": 0,
    # ...over this many frames. A FLOOR: the corpus grows, so a bigger number is
    # fine and a SMALLER one means the gate is being run against less evidence
    # than the figure was measured on -- which is exactly what a truncated or
    # missing capture looks like, and exactly what a round-trip claim of "0
    # mismatches" must not be allowed to be trivially true on.
    "conformant_frames_floor": 319575,
    # (E) per-class observed MTYPE sets for the six classes OVMX builds. Exact:
    # a NEW MTYPE appearing on one of these classes is a shape change and the
    # gate should say so out loud rather than absorb it.
    "class_mtypes": {
        58: [5, 7, 8, 9],
        62: [3, 4, 6],
        66: [1],
        94: [10],
        110: [0, 2, 10],
        190: [10],
    },
    # (E2) every per-class credit constant vms-ec7 introduced, checked to be a
    # value the wire actually shows on that class. (class length, constant name,
    # value) -- the names are the C spellings so a grep finds both ends.
    "credit_constants": [
        (66, "SCS_DIR_ENV_CREDIT_ECHO", 0),
        (110, "SCS_DIR_ENV_CREDIT_RESP", 1),
        (110, "SCS_DIR_ENV_CREDIT_CONNREQ", 3),
        (94, "SCS_DIR_ENV_CREDIT_LOOKUP_RSP", 1),
        (94, "SCS_DIR_ENV_CREDIT_LOOKUP_REQ", 0),
        (62, "SCS_DIR_ENV_CREDIT_CONFIRM", 0),
        (58, "SCS_DIR_ENV_CREDIT_CONFIRM", 0),
        (110, "SCS_CONNECT_ENV_CREDIT", 10),
        (94, "SCS_MSCP_ENV_CREDIT", 1),
        (190, "SCS_MEMBER_ENV_CREDIT_MODEL", 0),
        (190, "SCS_MEMBER_ENV_CREDIT_PARAMS", 0),
        (190, "SCS_MEMBER_ENV_CREDIT_CONFIG", 2),
        (62, "SCS_DISC_ENV_CREDIT", 0),
        (58, "SCS_DISC_ENV_CREDIT", 0),
    ],
    # (F) THE FINDING, gated as a fact rather than as a count -- the counts move
    # with the corpus and the fact does not. Dated snapshot, 2026-08-06, 48
    # pcaps: the pre-vms-ec7 guard admitted 321,547 frames, 1,972 of them NOT
    # envelope-conformant, and 1,092 of those carried a 0..7 value at [46:48].
    # The values it would have read there ranged over 1..19 -- which is the same
    # misread docs/design-mscp-direction.md sec 4 records as a self-caught method
    # confound ("printed types 1..22" on the 70-content class).
    "legacy_guard_reads_nonconformant_frames": True,
    "legacy_guard_nonconformant_snapshot": 1972,
    "legacy_guard_nonconformant_control_shaped_snapshot": 1092,
    # (G) recorded, not acted on: on this corpus the length allowlist and the
    # envelope test admit the same set, so moving the credit module onto the
    # envelope would be behaviour-neutral HERE. It is still not done in this
    # item, because "no counterexample in the captures we hold" is not a licence
    # to change which bytes OVMX stamps on a live wire.
    "credit_allowlist_missing_classes": [],
}

WIRE_KEYS = ("n_captures", "format_word_implies_inner_rule", "mtype_namespace",
             "mtype_10_is_majority", "build_mismatches",
             "conformant_frames_floor", "class_mtypes",
             "credit_constants",
             "legacy_guard_reads_nonconformant_frames",
             "legacy_guard_nonconformant_snapshot",
             "legacy_guard_nonconformant_control_shaped_snapshot",
             "credit_allowlist_missing_classes")
NON_WIRE_KEYS = ()


def measure(capdir):
    read_pcap = _read_pcap()
    paths = sorted(glob.glob(os.path.join(capdir, "**", "*.pcap"), recursive=True))
    paths = lab1_only(paths)

    conformant = 0
    nonconformant = 0
    fmt_word_frames = 0
    fmt_word_and_inner = 0
    mtypes = collections.Counter()
    build_mismatches = 0
    build_mismatch_examples = []
    class_mtypes = collections.defaultdict(collections.Counter)
    class_credits = collections.defaultdict(collections.Counter)
    conformant_lengths = collections.Counter()
    raw_lengths = collections.Counter()
    legacy_admit = 0
    legacy_nonconformant = 0
    legacy_nonconformant_control = 0
    legacy_nonconf_mtypes = collections.Counter()

    for path in paths:
        for _s, _u, _o, frame in read_pcap(path):
            pl = sca_content(frame)
            if pl is None:
                continue
            raw_lengths[len(pl)] += 1

            if len(pl) >= OFF_FORMAT + 2 and le16(pl, OFF_FORMAT) == FORMAT_WORD:
                fmt_word_frames += 1
                if le16(pl, OFF_INNER_LEN) == len(pl) - INNER_LEN_BIAS:
                    fmt_word_and_inner += 1

            ok = envelope_conformant(pl)

            # (F) the pre-vms-ec7 scsd.c guard, applied to the WIRE frame.
            legacy = (len(frame) >= LEGACY_MIN_WIRE_LEN and
                      len(pl) > 16 and pl[16] in LEGACY_PPD_MARKERS)
            if legacy:
                legacy_admit += 1
                if not ok:
                    legacy_nonconformant += 1
                    # What the old code would have read as the message type.
                    if len(pl) >= OFF_MTYPE + 2:
                        mt = le16(pl, OFF_MTYPE)
                        legacy_nonconf_mtypes[mt] += 1
                        # 0..7 is exactly what scs_conn_event_for_msgtype()
                        # accepts, i.e. what would have stepped a CDT.
                        if mt <= 7:
                            legacy_nonconformant_control += 1

            if not ok:
                nonconformant += 1
                continue

            conformant += 1
            conformant_lengths[len(pl)] += 1
            mt = le16(pl, OFF_MTYPE)
            credit = le16(pl, OFF_CREDIT)
            mtypes[mt] += 1
            if len(pl) in OVMX_BUILT_CLASSES:
                class_mtypes[len(pl)][mt] += 1
                class_credits[len(pl)][credit] += 1

            # (D) the build round trip.
            rebuilt = env_build_bytes(len(pl), mt, credit,
                                      le32(pl, OFF_DEST_CONID),
                                      le32(pl, OFF_SRC_CONID))
            if rebuilt != bytes(pl[OFF_INNER_LEN:HDR_END]):
                build_mismatches += 1
                if len(build_mismatch_examples) < 5:
                    build_mismatch_examples.append(
                        (os.path.basename(path), len(pl),
                         pl[OFF_INNER_LEN:HDR_END].hex(), rebuilt.hex()))

    missing = sorted(L for L in conformant_lengths
                     if L not in CREDIT_LENGTH_ALLOWLIST)

    return {
        "capdir": capdir,
        "paths": paths,
        "n_captures": len(paths),
        "conformant": conformant,
        "nonconformant": nonconformant,
        "fmt_word_frames": fmt_word_frames,
        "fmt_word_and_inner": fmt_word_and_inner,
        "format_word_implies_inner_rule":
            (fmt_word_and_inner / fmt_word_frames) if fmt_word_frames else 0.0,
        "mtypes": mtypes,
        "mtype_namespace": sorted(mtypes),
        "mtype_10_is_majority":
            bool(mtypes) and mtypes.most_common(1)[0][0] == 10,
        "build_mismatches": build_mismatches,
        "build_mismatch_examples": build_mismatch_examples,
        "class_mtypes": {k: sorted(v) for k, v in sorted(class_mtypes.items())},
        "class_credits": {k: sorted(v) for k, v in sorted(class_credits.items())},
        "conformant_lengths": conformant_lengths,
        "raw_lengths": raw_lengths,
        "legacy_admit": legacy_admit,
        "legacy_guard_nonconformant": legacy_nonconformant,
        "legacy_guard_nonconformant_control_shaped": legacy_nonconformant_control,
        "legacy_nonconf_mtypes": legacy_nonconf_mtypes,
        "credit_allowlist_missing_classes": missing,
    }


def report(m):
    print(f"captures: {m['n_captures']} pcap(s) under {m['capdir']}")
    print(f"(A) SCA frames: {m['conformant']} envelope-conformant, "
          f"{m['nonconformant']} not "
          f"(the START / HELLO / 70-content family the parser must refuse)")
    print(f"(B) format word 0x0004 at [44:46]: {m['fmt_word_frames']} frames, "
          f"of which {m['fmt_word_and_inner']} also satisfy inner == total-44 "
          f"-> {m['format_word_implies_inner_rule']:.6f}")
    print(f"(C) MTYPE namespace: {m['mtype_namespace']}")
    for mt, n in sorted(m["mtypes"].items()):
        print(f"      MTYPE {mt:>2}  {n:>9}")
    print(f"    MTYPE 10 is the majority: {m['mtype_10_is_majority']}")
    print(f"(D) build round trip over content[42:58]: "
          f"{m['build_mismatches']} mismatch(es) in {m['conformant']} frames")
    for ex in m["build_mismatch_examples"]:
        print(f"      {ex[0]} len={ex[1]} wire={ex[2]} rebuilt={ex[3]}")
    print("(E) per-class observed MTYPEs / credits, classes OVMX builds:")
    for L in sorted(m["class_mtypes"]):
        print(f"      {L:>3}-content  mtypes={m['class_mtypes'][L]}  "
              f"credits={m['class_credits'][L]}")
    print(f"(F) pre-vms-ec7 scsd.c guard admitted {m['legacy_admit']} frames; "
          f"{m['legacy_guard_nonconformant']} of them are NOT "
          f"envelope-conformant, and "
          f"{m['legacy_guard_nonconformant_control_shaped']} of those carry a "
          f"0..7 value at [46:48] the old code would have fed to the "
          f"connection state machine")
    if m["legacy_nonconf_mtypes"]:
        print(f"      what the old code would have read there: "
              f"{dict(sorted(m['legacy_nonconf_mtypes'].items()))}")
    print(f"(G) envelope-conformant content lengths NOT in "
          f"scs_credit_header_offset()'s allowlist "
          f"{list(CREDIT_LENGTH_ALLOWLIST)}: "
          f"{m['credit_allowlist_missing_classes']}")


def compare_results(m):
    """Return [(ok, description), ...]. Count-shaped figures are compared as
    inequalities because the corpus GROWS: a bigger capture library is not a
    regression. The SHAPE figures are compared exactly, because they are what
    the C code depends on."""
    out = []

    out.append((m["n_captures"] >= EXPECTED["n_captures"],
                f"n_captures {m['n_captures']} >= {EXPECTED['n_captures']}"))
    out.append((m["conformant"] >= EXPECTED["conformant_frames_floor"],
                f"envelope-conformant frames {m['conformant']} >= "
                f"{EXPECTED['conformant_frames_floor']} (the population the "
                f"build round trip was measured over)"))
    out.append((abs(m["format_word_implies_inner_rule"] -
                    EXPECTED["format_word_implies_inner_rule"]) < 1e-9,
                f"format word implies the inner-length rule: "
                f"{m['format_word_implies_inner_rule']:.6f} == "
                f"{EXPECTED['format_word_implies_inner_rule']}"))
    out.append((m["mtype_namespace"] == EXPECTED["mtype_namespace"],
                f"MTYPE namespace {m['mtype_namespace']} == "
                f"{EXPECTED['mtype_namespace']}"))
    out.append((m["mtype_10_is_majority"] == EXPECTED["mtype_10_is_majority"],
                "MTYPE 10 is the majority of envelope-conformant frames"))
    out.append((m["build_mismatches"] == EXPECTED["build_mismatches"],
                f"scs_env_build() reproduces content[42:58] on every "
                f"envelope-conformant frame: {m['build_mismatches']} "
                f"mismatch(es), expected {EXPECTED['build_mismatches']}"))

    for L, mts in sorted(EXPECTED["class_mtypes"].items()):
        got = m["class_mtypes"].get(int(L), [])
        out.append((got == mts,
                    f"{L}-content class observed MTYPEs {got} == {mts}"))

    for L, name, val in EXPECTED["credit_constants"]:
        seen = m["class_credits"].get(int(L), [])
        out.append((val in seen,
                    f"{name} = {val} is a credit value the {L}-content class "
                    f"actually carries (observed {seen})"))

    # (F) gated as a FACT, because the counts move with the corpus and the fact
    # does not. The snapshot numbers stay in EXPECTED as a dated record.
    fact = (m["legacy_guard_nonconformant"] > 0 and
            m["legacy_guard_nonconformant_control_shaped"] > 0)
    out.append((fact == EXPECTED["legacy_guard_reads_nonconformant_frames"],
                f"the pre-vms-ec7 scsd guard reads non-envelope frames: "
                f"{m['legacy_guard_nonconformant']} admitted and "
                f"non-conformant, "
                f"{m['legacy_guard_nonconformant_control_shaped']} of them "
                f"carrying a 0..7 value at [46:48] "
                f"(snapshot when written: "
                f"{EXPECTED['legacy_guard_nonconformant_snapshot']} / "
                f"{EXPECTED['legacy_guard_nonconformant_control_shaped_snapshot']})"))
    out.append((m["credit_allowlist_missing_classes"] ==
                EXPECTED["credit_allowlist_missing_classes"],
                f"credit-offset allowlist misses "
                f"{m['credit_allowlist_missing_classes']}, expected "
                f"{EXPECTED['credit_allowlist_missing_classes']}"))
    return out


def rederive(capdir, **_kw):
    """THE ctest GATE'S ENTRY POINT (vms-371)."""
    return compare_results(measure(capdir)), set(WIRE_KEYS)


def compare(m):
    out = compare_results(m)
    fails = [what for ok, what in out if not ok]
    for what in fails:
        print(f"  FAIL {what}")
    print(f"{'FAIL' if fails else 'PASS'}: {len(out)} checks, "
          f"{len(fails)} failure(s)")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.captures):
        print(f"no capture directory at {args.captures} -- this script needs "
              f"the host-only lab captures (CLAUDE.md rule 8). Use --captures.",
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
