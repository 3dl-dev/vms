#!/bin/sh
#
# facility_negctl_total_death_check.sh - check on
# facility_negctl_total_death.sh's fnd_all_suites_missing() (rd vms-df4).
#
# WHY THIS IS GATED, UNLIKE THE _negctl.sh family vms-49f tore out (see
# tests/qemu/CMakeLists.txt's "TORN OUT AS GATES" note). Those checked whether
# the MANIFEST's own declarations were internally consistent -- a string
# relation over the tree's self-description, provable without ever exercising
# the driver against real captured output. This file is the same shape as the
# kernel_executive_negctl_crash ctest that survived that cull: it feeds the
# REAL function a FABRICATED but realistically-shaped $OUTFILE and asserts the
# function's observable behaviour on it -- a property of the driver script
# itself, not a claim about the manifest or the executive. No QEMU, no
# container, no /dev/vms needed; it runs in well under a second.
#
# Usage: facility_negctl_total_death_check.sh [<repo-root>]

set -u

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
LIB="$ROOT/tests/qemu/facility_negctl_total_death.sh"

passed=0
failed=0

ok()  { echo "  ok: $*"; passed=$((passed + 1)); }
bad() { echo "  FAIL: $*"; failed=$((failed + 1)); }

echo "=========================================================="
echo " Check on TOTAL GUEST DEATH detection (rd vms-df4)"
echo "=========================================================="

[ -f "$LIB" ] || { echo "FAIL: BROKEN FIXTURE: $LIB is missing"; exit 1; }
. "$LIB"

TMP=$(mktemp -d) || { echo "FAIL: mktemp failed"; exit 1; }
trap 'rm -rf "$TMP"' EXIT

echo ""
echo "--- 1. the MEASURED shape (rd vms-df4, CI run 36179229516): a defect ---"
echo "---    injects cleanly, vms.ko loads, but ZERO suites ever report ---"
cat >"$TMP/total_death.out" <<'EOF'
=== FACILITY_DEFECT=rightslist-general-hex-as-decimal ===
  injected 'rightslist-general-hex-as-decimal' into /src/repo/src/vmsrms/rightslist_live.c
PASS: vms.ko loaded, /dev/vms present
EOF
_r=$(fnd_all_suites_missing "$TMP/total_death.out" test_kmod_access test_kmod_ast \
    test_syssvc_rightslist test_syssvc_ident)
if [ "$_r" = "1" ]; then
    ok "a capture with no '=== SUITE' line at all for ANY named suite is TOTAL GUEST DEATH"
else
    bad "expected 1 (total death), got '$_r' for the zero-verdict capture"
fi

echo ""
echo "--- 2. BASELINE: even ONE unrelated suite reporting is NOT total death ---"
# The true-negative case: this file must not fire just because the FACILITY's
# own suite is silent -- that is check 3's job (a facility-scoped MISSING),
# not this one's. Only when NOTHING AT ALL ran does this fire.
cat >"$TMP/partial.out" <<'EOF'
=== FACILITY_DEFECT=rightslist-general-hex-as-decimal ===
  injected 'rightslist-general-hex-as-decimal' into /src/repo/src/vmsrms/rightslist_live.c
PASS: vms.ko loaded, /dev/vms present
=== SUITE test_kmod_access rc=0 ===
EOF
_r=$(fnd_all_suites_missing "$TMP/partial.out" test_kmod_access test_kmod_ast \
    test_syssvc_rightslist test_syssvc_ident)
if [ "$_r" = "0" ]; then
    ok "a capture where an UNRELATED suite ran is NOT total death (false positive would hide a real, narrower defect-caused drop)"
else
    bad "expected 0 (not total death), got '$_r' when test_kmod_access clearly ran"
fi

echo ""
echo "--- 3. BASELINE: only the facility's OWN suite reporting also clears it ---"
cat >"$TMP/own_suite_only.out" <<'EOF'
=== SUITE test_syssvc_rightslist rc=1 ===
EOF
_r=$(fnd_all_suites_missing "$TMP/own_suite_only.out" test_kmod_access test_kmod_ast \
    test_syssvc_rightslist test_syssvc_ident)
if [ "$_r" = "0" ]; then
    ok "a capture where only the facility's own suite ran is NOT total death"
else
    bad "expected 0 (not total death), got '$_r' when test_syssvc_rightslist clearly ran"
fi

echo ""
echo "--- 4. an absent/unreadable outfile is total death, not a crash ---"
_r=$(fnd_all_suites_missing "$TMP/does_not_exist.out" test_kmod_access)
if [ "$_r" = "1" ]; then
    ok "a missing capture file is reported as total death (and the function did not itself crash)"
else
    bad "expected 1 for a missing capture file, got '$_r'"
fi

echo ""
echo "=========================================================="
echo " $passed passed, $failed failed"
echo "=========================================================="
[ "$failed" -eq 0 ] && exit 0
exit 1
