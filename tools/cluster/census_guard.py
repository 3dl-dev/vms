#!/usr/bin/env python3
"""census_guard.py -- vms-69c: the shared refusal every SCA measurement tool
calls before it filters a census to a subset of SCA length classes, or
extends one across classes that do not share the SCA envelope.

WHY THIS EXISTS. Two real, opposite sampling errors happened in this epic:

  UNDER-SAMPLING (vms-c11). Every SCA census in the epic filtered on lengths
  {62, 66, 110} -- the four FORMATION-message classes. Both RESPONSE messages
  (msgtype 5 REJECT_RSP, 7 DISCONNECT_RSP) are 58 bytes, so every one of
  those censuses was blind to them, and that is how
  docs/cluster-protocol-spec.md came to state that message types 5 and 7
  "DO NOT EXIST ON OUR WIRE" when 958 of them are in the library (fixed by
  vms-591, but silently -- nothing would have caught the NEXT tool that
  copies the same restriction).

  OVER-GENERALISING (the mirror). A census read `[46:48]` across the
  70-content class, which does not carry the SCA connection-control envelope
  at all (a different, still-undecoded shape -- see src/vmsscs/scs_rx.h),
  and drew a conclusion from it.

Neither failure is caught by a green run, because nothing checked the
FILTER against the population it was applied to. This module is that check.

THE ENVELOPE TEST. src/vmsscs/include/scs_rx.h documents the grounded
envelope-conformance test, measured fresh for vms-7c0 across the whole
141-pcap reference-lab corpus (981,367 envelope-conformant frames, no length
filter): for the SCA content bytes `pl = frame[14:14+scalen]`,

    pl[42:44] (LE u16) == len(pl) - 44        (inner length)
    pl[44:46] (LE u16) == 0x0004               (format word)

This is what keeps the 70-content class out of a connection-control census;
it satisfies neither check. THIS module reuses that same test rather than
inventing a second one, so "shares the envelope" means one thing across the
whole SCA toolchain.

WHAT check_census() ENFORCES, both directions:

  1. A tool that wants only a SUBSET of the envelope-conformant length
     classes present in its own (unfiltered) population must pass
     restrict_reason=<non-empty string>. Without one, check_census() raises
     SystemExit naming every excluded class and its frame count -- exactly
     the population a silent restriction would have dropped.
  2. A tool that wants to include a length class that FAILS the
     envelope-conformance test (it exists in the raw capture population but
     is not connection-control-shaped) must pass
     generalize_reason=<non-empty string>. Without one, check_census() raises
     SystemExit naming the offending classes and their frame count.

Both directions REPORT the excluded/over-generalized population in the
returned dict regardless of whether a reason was given, so a caller's
--print output can always show a reader what was in and out of scope.

NEEDS NO LAB CAPTURES to be exercised: population() reads real pcaps, but
check_census() is pure -- it only ever looks at the Counters it is handed,
which is exactly what lets tests/vmsscs/test_census_guard.py prove the two
refusals with synthetic data (see that file's docstring).
"""
import collections

SCA_ETHERTYPE = b"\x60\x07"
INNER_LEN_OFF = 42
FORMAT_OFF = 44
FORMAT_WORD = 0x0004


def envelope_conformant(pl):
    """`pl` is the SCA content bytes (frame[14:14+scalen], already sliced to
    exactly scalen bytes). True iff `pl` passes the grounded
    envelope-conformance test (scs_rx.h, vms-7c0): the inner-length word at
    [42:44] agrees with the outer length, and the format word at [44:46] is
    0x0004. This is what a connection-control (or MSCP, or application-data)
    SCS message shares and the still-undecoded 70-content class does not.
    """
    if len(pl) < FORMAT_OFF + 2:
        return False
    inner = pl[INNER_LEN_OFF] | (pl[INNER_LEN_OFF + 1] << 8)
    fmt = pl[FORMAT_OFF] | (pl[FORMAT_OFF + 1] << 8)
    return inner == len(pl) - 44 and fmt == FORMAT_WORD


def population(files, read_pcap, *, origin_filter=None,
               sca_ethertype=SCA_ETHERTYPE):
    """Scan every pcap in `files` and return `(conformant, raw)`, two
    `collections.Counter`s of SCA length -> frame count, over the WHOLE
    population -- no length filter of any kind.

    `conformant` counts only frames passing `envelope_conformant()`; `raw`
    counts every SCA-ethertype frame regardless, so the two counters
    together answer both directions check_census() checks: what a length
    RESTRICTION would exclude (present in `conformant` but not selected),
    and what a length GENERALISATION would wrongly include (selected but
    absent from `conformant`, present only in `raw`).

    `origin_filter(frame) -> bool`, if given, is applied identically to both
    counters -- pass the SAME predicate the caller's own EXPECTED table was
    measured with (e.g. VMS-origin-only) so the two counts are apples to
    apples with the caller's own population, not a differently-scoped one.
    """
    conformant = collections.Counter()
    raw = collections.Counter()
    for path in files:
        for _s, _u, _o, frame in read_pcap(path):
            if len(frame) < 16 or frame[12:14] != sca_ethertype:
                continue
            if origin_filter is not None and not origin_filter(frame):
                continue
            pl = frame[14:]
            if len(pl) < 2:
                continue
            scalen = (pl[0] | (pl[1] << 8)) + 2
            if len(pl) < scalen:
                continue
            pl = pl[:scalen]
            raw[scalen] += 1
            if envelope_conformant(pl):
                conformant[scalen] += 1
    return conformant, raw


def check_census(selected_classes, conformant, raw=None, *,
                  restrict_reason=None, generalize_reason=None, label=""):
    """The refusal itself.

    `selected_classes` -- the SCA lengths the caller's census is about to
        filter on (e.g. `CONNCTL_CLASSES`).
    `conformant`, `raw` -- the two Counters `population()` returned, scanned
        over the SAME (unfiltered) capture set and origin rule the caller's
        own EXPECTED table uses. `raw` may be omitted (None) if the caller
        cannot cheaply produce it; the over-generalisation check is then
        skipped, but the under-sampling check still runs.

    Returns a report dict:
        {"excluded": {cls: n}, "excluded_total": N,
         "over_generalized": {cls: n}, "over_generalized_total": N,
         "restrict_reason": ..., "generalize_reason": ...}
    so a caller can print/log what was dropped or overreached -- whether or
    not it was justified.

    Raises SystemExit if either direction is present without its reason.
    """
    selected = set(selected_classes)
    conformant_classes = set(c for c, n in conformant.items() if n > 0)

    excluded = {c: n for c, n in conformant.items()
                if c not in selected and n > 0}
    excluded_total = sum(excluded.values())

    over_generalized = {}
    if raw is not None:
        for c in selected:
            if c not in conformant_classes:
                n = raw.get(c, 0)
                if n:
                    over_generalized[c] = n
    over_generalized_total = sum(over_generalized.values())

    problems = []
    if excluded_total and not restrict_reason:
        problems.append(
            "%sCENSUS GUARD: restricted to SCA length classes %s but the "
            "envelope-conformant population also has classes %s (%d frames "
            "total) EXCLUDED with no restrict_reason given. This is the "
            "vms-c11 under-sampling failure -- a census that silently "
            "narrows its own population. Pass restrict_reason= explaining "
            "why those classes are legitimately out of scope for this "
            "census, or widen selected_classes to include them."
            % (label, sorted(selected), dict(sorted(excluded.items())),
               excluded_total))
    if over_generalized_total and not generalize_reason:
        problems.append(
            "%sCENSUS GUARD: selected SCA length classes include %s (%d "
            "frames) that do NOT pass the envelope-conformance test "
            "([42:44] inner length / [44:46] format word, see scs_rx.h) -- "
            "they do not share the SCA envelope with the rest of this "
            "census. This is the vms-c11 over-generalising failure -- a "
            "census that spans classes with no shared envelope. Pass "
            "generalize_reason= explaining why they belong, or drop them "
            "from selected_classes."
            % (label, dict(sorted(over_generalized.items())),
               over_generalized_total))
    if problems:
        raise SystemExit("\n".join(problems))

    return {
        "excluded": excluded,
        "excluded_total": excluded_total,
        "over_generalized": over_generalized,
        "over_generalized_total": over_generalized_total,
        "restrict_reason": restrict_reason,
        "generalize_reason": generalize_reason,
    }


def format_report(report):
    """A short human-readable rendering of check_census()'s return value,
    for --print output -- "what was dropped" must be visible, not just
    non-fatal."""
    lines = []
    if report["excluded_total"]:
        lines.append(
            "  [census guard] restricted census excludes %d frame(s) across "
            "classes %s -- justified: %s"
            % (report["excluded_total"], sorted(report["excluded"]),
               report["restrict_reason"]))
    if report["over_generalized_total"]:
        lines.append(
            "  [census guard] census includes %d non-conformant frame(s) "
            "across classes %s -- justified: %s"
            % (report["over_generalized_total"],
               sorted(report["over_generalized"]), report["generalize_reason"]))
    if not lines:
        lines.append("  [census guard] no restriction, no generalisation.")
    return "\n".join(lines)
