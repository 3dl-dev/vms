#!/bin/sh
# facility_negctl_run_set_check.sh - check on facility_negctl_run_set.sh's
# fnrs_run_set() (rd vms-3264): the REAL function, fed a realistic run order
# and real manifest shapes. No QEMU, no container.
#
# Usage: facility_negctl_run_set_check.sh [<repo-root>]
set -u
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
. "$ROOT/tests/qemu/facility_negctl_run_set.sh"
passed=0; failed=0
ok()  { echo "  ok: $*"; passed=$((passed + 1)); }
bad() { echo "  FAIL: $*"; failed=$((failed + 1)); }
eq()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: got [$2] want [$3]"; fi; }

ORDER="test_kmod_a
test_kmod_b
test_syssvc_cli_wait
test_syssvc_crembx
test_syssvc_pin
test_imgact_bind"
j() { tr '\n' ' ' | sed 's/ $//'; }

eq "isolated: the declared suite only" \
   "$(fnrs_run_set "$ORDER" isolated test_syssvc_cli_wait | j)" "test_syssvc_cli_wait"
eq "isolated: the MANIFEST's order, not the run order" \
   "$(fnrs_run_set "$ORDER" isolated test_imgact_bind test_kmod_b | j)" "test_imgact_bind test_kmod_b"
eq "isolated: a glob expands in run order, no duplicates" \
   "$(fnrs_run_set "$ORDER" isolated test_syssvc_crembx 'test_syssvc_*' | j)" \
   "test_syssvc_crembx test_syssvc_cli_wait test_syssvc_pin"
eq "isolated: a name no suite carries runs nothing (the runner reports the manifest broken)" \
   "$(fnrs_run_set "$ORDER" isolated test_syssvc_nosuch | j)" ""
eq "fatal: every suite before the declared one, then it" \
   "$(fnrs_run_set "$ORDER" fatal test_syssvc_pin | j)" \
   "test_kmod_a test_kmod_b test_syssvc_cli_wait test_syssvc_crembx test_syssvc_pin"

# Against the checkout: every manifest entry's isolated run set is non-empty
# and a subset of its declared suites.
MAN="$ROOT/tests/qemu/facility_defects.sh"
REAL=$(cd "$ROOT" && { ls tests/qemu/test_kmod_*.c | sort; ls tests/qemu/test_syssvc_*.c | sort
       ls tests/qemu/test_imgact_*.c | sort; } | xargs -n1 basename | sed 's/\.c$//')
nbad=0; n=0
for d in $(sh "$MAN" list); do
    iso=$(sh "$MAN" field "$d" isolation); [ "$iso" = fatal ] && continue
    red=$(sh "$MAN" field "$d" suites_red)
    # shellcheck disable=SC2086
    rs=$(fnrs_run_set "$REAL" "$iso" $red | j)
    n=$((n + 1))
    if [ -z "$rs" ]; then nbad=$((nbad + 1)); echo "    $d: [$red] matches no suite"; fi
done
[ "$nbad" -eq 0 ] && ok "all $n isolated manifest entries have a non-empty run set" \
                  || bad "$nbad isolated manifest entries run nothing"

echo "facility_negctl_run_set_check: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
