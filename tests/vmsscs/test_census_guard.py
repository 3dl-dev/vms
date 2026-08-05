#!/usr/bin/env python3
"""
test_census_guard.py -- vms-69c: the shared refusal proves it can fail, in
BOTH directions the epic actually got wrong.

WHY THIS EXISTS.

  UNDER-SAMPLING (vms-c11). Every SCA census in the epic filtered on lengths
  {62, 66, 110}. Both RESPONSE messages are 58 bytes, so all of them were
  blind to that class -- which is how docs/cluster-protocol-spec.md came to
  state that message types 5 and 7 appear on NO capture and instruct agents
  not to build one, when 958 of them are in the library.

  OVER-GENERALISING (the mirror). A census read [46:48] across the
  70-content class, which does not share the SCA envelope at all, and drew a
  conclusion from it.

tools/cluster/census_guard.py is the fix: check_census() REFUSES to run a
restricted or generalised census unless the caller passes an explicit
justification, and it reports the excluded/over-generalised population size
either way. This file proves the refusal itself, and that it is not a check
that cannot fail (a class of defect this epic has rejected 12 rounds of --
see CLAUDE.md's standing constraints for vms-69c).

NEEDS NO LAB CAPTURES. Two ways:

  (1) PURE, on synthetic Counters -- check_census() never touches a byte, so
      most of its behaviour (the refusal, the report, the two directions
      independently) is exercised directly against hand-built population
      dicts. This is not a hollow test: it is testing the actual function
      under actual scrutiny, with no capture I/O in the loop at all because
      the function itself has none.

  (2) END TO END, through REAL pcap bytes -- envelope_conformant() and
      population() DO read real bytes at real offsets, and a test that only
      ever fed check_census() pre-computed Counters would never notice a
      drifted offset in the byte-level test itself. So part 2 builds real
      classic-pcap files (in a scratch tmp dir, nothing under the repo is
      touched) with hand-crafted Ethernet+SCA frames at the exact byte
      layout scs_rx.h documents, and runs them through the REAL read_pcap()
      from dissect_sca.py -- the same reader every measurement tool in this
      epic uses. This is the part that would catch an off-by-one in
      INNER_LEN_OFF/FORMAT_OFF that a synthetic-Counter test cannot see.

  (3) WIRING -- the three tools vms-69c names (scs_reason_measure.py,
      scs_disc_measure.py, scs_connect_data_measure.py) must actually call
      check_census(), not merely have the module sitting unused beside them
      -- the same "a manifest nobody calls is not a fence" check
      test_capture_manifest.py already runs for capture_manifest.py.
"""
import glob
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CLUSTER_DIR = os.path.join(ROOT, "tools", "cluster")

sys.path.insert(0, CLUSTER_DIR)
import census_guard as cg  # noqa: E402
from dissect_sca import read_pcap  # noqa: E402

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


# ===========================================================================
# 1. PURE -- check_census() against hand-built population Counters
# ===========================================================================

# A population where the 58-byte class ("the response messages") exists
# alongside the {62, 66, 110} classes a census might restrict to -- the exact
# vms-c11 shape.
CONFORMANT_C11 = {58: 958, 62: 1376, 66: 1116, 110: 4780}

# --- (1a) THE ITEM'S OWN ACCEPTANCE TEST, DIRECTION 1: silent narrowing REDS
try:
    cg.check_census((62, 66, 110), CONFORMANT_C11)
    check(False, "check_census() accepted a restriction to {62,66,110} that "
                 "silently drops the 58-byte class with NO restrict_reason "
                 "-- this must RED, it is the vms-c11 failure itself")
except SystemExit as exc:
    msg = str(exc)
    check("58" in msg, "the refusal message does not name the excluded class 58")
    check("958" in msg, "the refusal message does not report the excluded "
                         "population size (958 frames)")
    check("restrict_reason" in msg,
          "the refusal message does not tell the caller what to pass")

# --- (1b) the mirror: an EMPTY selection also silently drops everything, and
#     must refuse identically -- there is no size-based exemption.
try:
    cg.check_census((), CONFORMANT_C11)
    check(False, "check_census() accepted an empty selection over a "
                 "nonempty population with no restrict_reason")
except SystemExit as exc:
    check(str(sum(CONFORMANT_C11.values())) in str(exc),
          "an empty-selection refusal must report the WHOLE population as "
          "excluded")

# --- (1c) restrict_reason PASSES it, and the report says what was excluded,
#     not just that it was allowed -- a justified restriction is still
#     visible to a reader.
report = cg.check_census((62, 66, 110), CONFORMANT_C11,
                          restrict_reason="testing: the response class does "
                          "not carry the field under study")
check(report["excluded"] == {58: 958},
      "a justified restriction's report does not name the excluded class/count")
check(report["excluded_total"] == 958,
      "a justified restriction's report does not total the excluded frames")
check(report["restrict_reason"].startswith("testing:"),
      "the report does not carry the justification back to the caller")

# --- (1d) THE ITEM'S OWN ACCEPTANCE TEST, DIRECTION 2: over-generalising REDS
# The 70-content class in `raw` but ABSENT from `conformant` -- exactly "read
# [46:48] across a class that does not share the SCA envelope".
CONFORMANT_NARROW = {62: 100}
RAW_WITH_OFFENVELOPE = {62: 100, 70: 947}
try:
    cg.check_census((62, 70), CONFORMANT_NARROW, RAW_WITH_OFFENVELOPE)
    check(False, "check_census() accepted selected_classes including 70, "
                  "which fails the envelope-conformance test, with no "
                  "generalize_reason -- this must RED")
except SystemExit as exc:
    msg = str(exc)
    check("70" in msg, "the refusal message does not name the "
                        "over-generalised class 70")
    check("947" in msg, "the refusal message does not report the "
                         "over-generalised population size (947 frames)")
    check("generalize_reason" in msg,
          "the refusal message does not tell the caller what to pass")

# --- (1e) generalize_reason PASSES it, and the report says what was pulled
#     in without envelope conformance.
report2 = cg.check_census((62, 70), CONFORMANT_NARROW, RAW_WITH_OFFENVELOPE,
                           generalize_reason="testing: intentionally "
                           "widening to study the 70-content class")
check(report2["over_generalized"] == {70: 947},
      "a justified generalisation's report does not name the "
      "non-conformant class/count")
check(report2["over_generalized_total"] == 947,
      "a justified generalisation's report does not total the "
      "over-generalised frames")

# --- (1f) BOTH directions at once, in one call, each needing its OWN
#     justification -- passing only one must still red on the other.
try:
    cg.check_census((62, 70), {62: 5, 90: 5}, {62: 5, 70: 3, 90: 5},
                     restrict_reason="only restriction justified")
    check(False, "check_census() must still red on the un-justified "
                  "over-generalisation even though the restriction side "
                  "was justified")
except SystemExit as exc:
    check("generalize_reason" in str(exc),
          "the combined-failure message dropped the un-justified direction")

# --- (1g) a clean census -- selection == conformant population exactly, no
#     raw non-conformant classes selected -- passes with NO reason needed
#     and an empty report.
report3 = cg.check_census((62, 66, 110), {62: 5, 66: 5, 110: 5},
                           {62: 5, 66: 5, 110: 5})
check(report3["excluded_total"] == 0 and report3["over_generalized_total"] == 0,
      "an unrestricted, non-generalised census produced a nonempty report")

# --- (1h) raw=None skips the over-generalisation check but NOT the
#     under-sampling one -- a caller that cannot cheaply produce `raw` still
#     gets the half of the guard it can afford.
try:
    cg.check_census((62,), {62: 5, 66: 5}, None)
    check(False, "check_census() with raw=None still must red on an "
                  "un-justified restriction")
except SystemExit:
    pass
ok = cg.check_census((62, 70), {62: 5}, None)
check(ok["over_generalized_total"] == 0,
      "raw=None must not fabricate an over-generalisation finding")


# ===========================================================================
# 2. END TO END -- real pcap bytes through the real read_pcap()
# ===========================================================================
DST = b"\xaa\x00\x04\x00\x02\x04"
SRC_VAX = b"\x08\x00\x2b\x11\x22\x33"


def build_sca_frame(scalen, *, envelope_ok=True, src=SRC_VAX):
    """One synthetic Ethernet+SCA frame. `scalen` is the TOTAL SCA content
    length census_guard/dissect_sca compute as `(pl[0]|pl[1]<<8) + 2` -- the
    same formula every measurement tool in this epic uses. `envelope_ok`
    controls whether the [42:44]/[44:46] envelope-conformance fields
    (scs_rx.h) are set correctly."""
    pl = bytearray(scalen)
    struct.pack_into("<H", pl, 0, scalen - 2)
    pl[16] = 0x4B          # SCS connection-control opcode family
    pl[17] = 0x13
    if envelope_ok:
        struct.pack_into("<H", pl, 42, len(pl) - 44)
        struct.pack_into("<H", pl, 44, 0x0004)
    else:
        struct.pack_into("<H", pl, 42, 0xDEAD)   # deliberately wrong inner len
        struct.pack_into("<H", pl, 44, 0x0000)   # deliberately wrong format word
    eth = DST + src + b"\x60\x07"
    return bytes(eth) + bytes(pl)


def write_pcap(path, frames):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
        for frame in frames:
            f.write(struct.pack("<IIII", 0, 0, len(frame), len(frame)))
            f.write(frame)


tmpdir = tempfile.mkdtemp(prefix="census_guard_test.")
try:
    # A synthetic capture with exactly the vms-c11 shape: the 58-byte
    # response class conformant and unselected, PLUS one 70-content frame
    # that fails the envelope test entirely (the OVER-GENERALISING mirror,
    # right there in the same capture).
    p = os.path.join(tmpdir, "synthetic.pcap")
    write_pcap(p, [
        build_sca_frame(58), build_sca_frame(58),
        build_sca_frame(62), build_sca_frame(62),
        build_sca_frame(66),
        build_sca_frame(110),
        build_sca_frame(70, envelope_ok=False),
        # an OVMX-sourced frame -- must be excludable via origin_filter,
        # same rule every measurement tool in this epic applies.
        build_sca_frame(62, src=b"\x02\x11\x22\x33\x44\x55"),
    ])

    def is_vms_origin(frame):
        mac = frame[6:12].hex()
        return mac.startswith("08002b") or mac.startswith("aa000400")

    conformant, raw = cg.population([p], read_pcap, origin_filter=is_vms_origin)
    check(conformant == {58: 2, 62: 2, 66: 1, 110: 1},
          "population() over the real pcap did not count the conformant "
          "classes correctly (VMS-origin only): got %r" % dict(conformant))
    check(raw == {58: 2, 62: 2, 66: 1, 110: 1, 70: 1},
          "population() over the real pcap did not count the raw classes "
          "correctly, including the non-conformant 70-byte frame: got %r"
          % dict(raw))

    # Now run the ACTUAL refusal against this REAL population: a census
    # restricted to {62, 66, 110} silently drops the real 58-byte frames --
    # must red exactly as the pure test above predicted.
    try:
        cg.check_census((62, 66, 110), conformant, raw)
        check(False, "check_census() over a REAL scanned population still "
                      "accepted the vms-c11 restriction with no reason")
    except SystemExit as exc:
        check("58" in str(exc), "real-population refusal did not name the "
                                 "excluded 58-byte class")

    # And a census that ADDS the 70-byte class (present only in `raw`, never
    # in `conformant`) must red as over-generalising -- the real-bytes proof
    # of "read [46:48] across a class that does not share the envelope".
    try:
        cg.check_census((58, 62, 66, 70, 110), conformant, raw)
        check(False, "check_census() over a REAL scanned population "
                      "accepted the non-conformant 70-byte class with no "
                      "generalize_reason")
    except SystemExit as exc:
        check("70" in str(exc), "real-population refusal did not name the "
                                 "over-generalised 70-byte class")

    # A correctly-scoped, correctly-justified census over the SAME real
    # population passes clean.
    ok = cg.check_census((58, 62, 66, 110), conformant, raw)
    check(ok["excluded_total"] == 0 and ok["over_generalized_total"] == 0,
          "the fully-covering, envelope-only census over the real "
          "population should need no justification and report nothing "
          "excluded")
finally:
    import shutil
    shutil.rmtree(tmpdir, ignore_errors=True)


# ===========================================================================
# 3. WIRING -- the three tools this item names must actually call
#    check_census(), not merely import the module unused.
# ===========================================================================
WIRED = {
    "tools/cluster/scs_reason_measure.py": "check_census(",
    "tools/cluster/scs_disc_measure.py": "check_census(",
    "tools/scs_connect_data_measure.py": "check_census(",
}
for rel, wiring in WIRED.items():
    path = os.path.join(ROOT, rel)
    check(os.path.isfile(path), "%s does not exist" % rel)
    if not os.path.isfile(path):
        continue
    with open(path, encoding="utf-8") as f:
        src = f.read()
    check("import census_guard" in src,
          "%s does not import census_guard" % rel)
    check(wiring in src,
          "%s imports census_guard but never calls %s -- a guard nobody "
          "calls is not a fence" % (rel, wiring))
    check("restrict_reason=" in src or "generalize_reason=" in src,
          "%s calls check_census() but passes no justification at all -- "
          "it would red on its own restriction" % rel)

print("%s: %d checks, %d failure(s)" % ("FAIL" if failures else "PASS",
                                        checks, len(failures)))
if failures:
    for f in failures:
        print("  - %s" % f)
    sys.exit(1)
sys.exit(0)
