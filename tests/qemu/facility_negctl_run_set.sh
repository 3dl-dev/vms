#!/bin/sh
# facility_negctl_run_set.sh - which suites a facility defect's image runs
# (rd vms-3264). Sourced by run_facility_negctl.sh; checked, with no QEMU, by
# facility_negctl_run_set_check.sh.
#
# A defect's image used to boot EVERY suite. A defect that hangs one facility
# (cli-wait-no-termination-message: DCL waits, as VMS DCL would, for a
# termination message the executive withholds) then made every later
# DCL-driving suite wait out its 300 s watchdog -- 26 min, the whole-VM wall,
# and the tail counted as "NEVER RAN" (PR #2082 kicked from the queue). Those
# out-of-set suites fed only the stray-suite LINT (non-gating, vms-49f), so they
# bought no verdict. The run set is exactly what the gating checks read:
#   isolated -- the declared suites (suites_red), in the order the manifest
#               lists them. The verdict-line check, the own-suite-red check
#               and FINAL RESULTS read only these.
#   fatal    -- every suite init.sh runs BEFORE the first declared one, then
#               that one: the verdict-line check requires those earlier
#               verdicts to prove the guest got as far as the crash (not a
#               boot failure). The guest boots in init.sh's normal order.
# blind_suites are a non-gating declaration and are not run under the defect.
# The positive control (pristine image) still runs every suite.
#
# fnrs_run_set <exec-order> <isolation> <suites_red...>
#   <exec-order>: newline/space list of every suite, in init.sh's run order.
#   Prints the run set, one suite per line, in run order, no duplicates.
fnrs_run_set() {
    _fnrs_order=$1
    _fnrs_iso=$2
    shift 2
    if [ "$_fnrs_iso" = "fatal" ]; then
        for _fnrs_s in $_fnrs_order; do
            echo "$_fnrs_s"
            for _fnrs_g in "$@"; do
                # shellcheck disable=SC2254
                case "$_fnrs_s" in $_fnrs_g) return 0 ;; esac
            done
        done
        return 0
    fi
    for _fnrs_g in "$@"; do
        for _fnrs_s in $_fnrs_order; do
            # shellcheck disable=SC2254
            case "$_fnrs_s" in $_fnrs_g) echo "$_fnrs_s" ;; esac
        done
    done | awk '!seen[$0]++'
}
