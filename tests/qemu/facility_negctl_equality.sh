#!/bin/sh
#
# facility_negctl_equality.sh - the (suite, assertion-text) identity that
# run_facility_negctl.sh's red-set equality is judged by (rd vms-b3b).
#
# THE BUG THIS REPLACES. run_facility_negctl.sh's fail_map() already emits
# "<suite>\t<assertion text>" rows -- the suite is right there, printed by
# init.sh's own banners and sliced out already -- but the equality check threw
# it away with `cut -f2-` before comparing against require_fail/knock_on_fail,
# which are themselves bare text with no suite attached. The result: identity
# was keyed on ASSERTION TEXT ALONE, so the check could not tell WHICH SUITE
# printed a line.
#
# MEASURED, not hypothetical: bind-client-no-register's require/knock_on set
# names "child: a LOCAL flag set by the parent is NOT visible here (local
# clusters stay per-process)", expecting it from test_syssvc_ef_mproc.c (which
# IS in that defect's suites_red). The identical string, verbatim, is also
# printed by test_kmod_eflag_mproc.c -- which is NOT in suites_red for this
# defect. Under the old text-only equality, a red from either suite satisfied
# the requirement; a red from test_syssvc_ef_mproc going missing while
# test_kmod_eflag_mproc's copy fired for an unrelated reason would have been
# indistinguishable from the real thing.
#
# THE SWEEP THIS FIX WAS MEASURED AGAINST (the standing rule: changing what a
# control names obliges a full sweep, re-derived for every affected control).
# Every require_fail/knock_on_fail text in the whole manifest, at the same
# normalisation facility_defects.sh's own selftest already applies (quotes and
# backslashes stripped, whitespace collapsed), was checked against ONLY the
# suite sources its own defect's suites_red glob admits. Every single one is
# still found there -- the manifest was never relying on an out-of-scope
# match to pass -- except this one, confirming the bug is real, isolated to
# the one instance already measured, and this fix does not silently narrow
# any other defect's requirement.
#
# THE FIX. Before the equality compares observed text against required text,
# the observed set is SCOPED: a "<suite>\t<text>" row survives only if <suite>
# matches the defect's suites_red glob (or is "(harness)", which fail_map()
# already uses for a FAIL line printed outside any suite banner -- a
# harness-level failure has no source-suite to be scoped against and is not
# in scope for this file to judge). A same-text red printed by a suite outside
# that scope is dropped from the observed set: it can no longer stand in for
# the suite the manifest actually named, so a defect that only reddens the
# WRONG suite now shows the required text as MISSING -- the failure mode this
# item exists to close.
#
# This does not require a manifest rewrite (require_fail/knock_on_fail stay
# bare text): the missing half of the identity was never the manifest's, it
# was suites_red, which every defect already carries.
#
# Sourced by run_facility_negctl.sh (the real driver) and by
# facility_negctl_equality_negctl.sh (this file's own negative control, which
# needs no QEMU -- it feeds fabricated fail_map()-shaped input straight to
# fne_scope_map()).

# fne_matches_globs <name> <glob>...
fne_matches_globs() {
    _n="$1"; shift
    for _g in "$@"; do
        # shellcheck disable=SC2254
        case "$_n" in $_g) return 0;; esac
    done
    return 1
}

# fne_scope_map <red_globs>  < "<suite>\t<text>" lines  > the subset whose
# suite is in scope for <red_globs> (or is "(harness)")
#
# <red_globs> is a single argument holding SPACE-SEPARATED shell globs (a
# defect's suites_red field, verbatim) -- word-split deliberately, the same
# way run_facility_negctl.sh's own matches_globs() callers already rely on.
fne_scope_map() {
    _fsm_globs="$1"
    while IFS="$(printf '\t')" read -r _fsm_suite _fsm_text; do
        [ -n "$_fsm_suite" ] || continue
        if [ "$_fsm_suite" = "(harness)" ]; then
            printf '%s\t%s\n' "$_fsm_suite" "$_fsm_text"
            continue
        fi
        # shellcheck disable=SC2086
        if fne_matches_globs "$_fsm_suite" $_fsm_globs; then
            printf '%s\t%s\n' "$_fsm_suite" "$_fsm_text"
        fi
    done
}
