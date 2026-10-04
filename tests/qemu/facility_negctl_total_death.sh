#!/bin/sh
#
# facility_negctl_total_death.sh - distinguishes TOTAL GUEST DEATH from a
# genuine per-facility defect red (rd vms-df4).
#
# MEASURED, not hypothetical. CI run 36179229516 (2026-09-25), shard 2/22:
# the 'rightslist-general-hex-as-decimal' negative control injected cleanly
# (`ok: harness exited 1 (nonzero)`, `ok: vms.ko still loaded and /dev/vms
# present`), then EVERY suite EXEC_ORDER names -- including test_kmod_access,
# the very FIRST probe, which has nothing to do with the rights database --
# came back MISSING. Re-running the identical defect in isolation (5/5,
# negctl-adhoc.yml) never reproduced it: the mutation itself is not the
# problem. What run_facility_negctl.sh could not do, before this file, was
# say so -- check 3 just printed ~90 "NEVER RAN" lines, one per suite,
# indistinguishable from the shape a real defect produces when it legitimately
# breaks more than its declared set, and the raw QEMU/boot output that would
# have explained WHY was never surfaced at all.
#
# THE DISTINCTION THIS FILE MAKES. An ISOLATED defect (the common case; see
# facility_defects.sh's `isolation` field) can only ever reach the facility it
# mutates -- it has no path to stopping an UNRELATED suite like
# test_kmod_access from running at all. (A `fatal` defect is different by
# design -- it crashes the guest mid-run on purpose, see run_facility_negctl.sh
# check 3's handling of `stop_at` -- so this file is never consulted for one.)
# When LITERALLY NOTHING ran -- not even the unrelated probes that execute
# before the facility's own suites in init.sh's run order -- the guest died
# (or never booted) for a reason that predates the mutation ever being
# exercised: a boot crash, QEMU never coming up, or the whole-VM wall firing
# before FINAL RESULTS. That is an INFRASTRUCTURE failure, the same class of
# "not a verdict about '$defect'" that run_facility_negctl.sh already carves
# out for a container-engine RC 125 or a broken-fixture RC 3 -- just
# discovered from the captured output after the fact instead of from the exit
# code up front.
#
# This does not retry, does not weaken any assertion, and does not suppress
# the failure: the run still counts as failed and still needs a human re-run.
# It only stops a one-off boot/infra death from being misattributed to the
# defect under test, exactly as the RC 125/3/4 cases already are not.
#
# Sourced by run_facility_negctl.sh (the real driver) and by
# facility_negctl_total_death_check.sh (this file's own check, gated as a
# ctest -- see that file's header for why this one IS gated unlike the
# declaration-only checks vms-49f tore out).

# fnd_all_suites_missing <outfile> <exec-order-suite-name>...
#
# Prints "1" (true) if NONE of the given suite names has a verdict line
# ("=== SUITE <name> rc=...") anywhere in <outfile> -- total guest death.
# Prints "0" (false) the moment ANY of them does -- at least SOMETHING ran, so
# a facility-scoped MISSING is check 3's job, not this file's.
fnd_all_suites_missing() {
    _fnd_outfile="$1"
    shift
    for _fnd_suite in "$@"; do
        if grep -qF "=== SUITE $_fnd_suite rc=" "$_fnd_outfile" 2>/dev/null; then
            echo 0
            return
        fi
    done
    echo 1
}
