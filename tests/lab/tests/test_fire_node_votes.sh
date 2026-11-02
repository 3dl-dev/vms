#!/bin/bash
# test_fire_node_votes.sh - a lab fire boots OVMXE with the VOTES it was asked
# for (rd vms-1b68).
#
# WHY. ovmx_fire_node.sh typed VOTES 1 at SYSBOOT> whatever the plan needed.
# The 2026-11-02 standby-takeover fire halted VAX1 (VOTES 1) with OVMXE at
# VOTES 1: cluster votes 2, quorum 2, and the survivors lost quorum, so the
# departure rebuild the fire existed to prove never ran. OVMX_VOTES (passed by
# ovmx_fire.sh as OVMX_FIRE_VOTES) is now the value that reaches the SYSGEN
# prompt; the executive reads it from the parameter set like any node's VOTES.
#
# HERMETIC: no pod, no kubectl, no QEMU. `ip` and `setsid` are stubs on PATH;
# the setsid stub records the env line the node driver would be started with.
#
# NEGATIVE CONTROL (Rule 7): a copy of the script with VOTES hard-coded to 1
# must fail the VOTES=2 case.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../tools/ovmx_fire_node.sh
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/ip"
printf '#!/bin/sh\necho "$*" >> "$STUB_LOG"\n' > "$T/bin/setsid"
chmod +x "$T/bin/ip" "$T/bin/setsid"

# run <script> <want-votes> [OVMX_VOTES value or empty]
run() {
	local s=$1 want=$2 v=$3 log=$T/stub.$RANDOM
	(export PATH="$T/bin:$PATH" STUB_LOG=$log OVMX_LAB_ROOT=$T/lab POD_NAME=p OVMX_NOCAP=1
	 [ -n "$v" ] && export OVMX_VOTES=$v
	 # sourced, then `wait`: the driver launch is a background job, and the
	 # recorded line must exist before it is read (no sleep, no poll).
	 bash -c '. "$0" "$@"; wait' "$s" /art t 3 >/dev/null 2>&1)
	grep -q "ovmx_evnode.sh" "$log" 2>/dev/null || { echo "no driver launch recorded"; return 1; }
	grep -q " VOTES=$want " "$log" && grep -q " EXPVOTES=3 " "$log"
}
fail=0
run "$SRC" 1 ""  || { echo "FAIL: default VOTES is not 1"; fail=1; }
run "$SRC" 2 2   || { echo "FAIL: OVMX_VOTES=2 did not reach the node"; fail=1; }
run "$SRC" 3 3   || { echo "FAIL: OVMX_VOTES=3 did not reach the node"; fail=1; }
sed 's/VOTES=\$NV /VOTES=1 /' "$SRC" > "$T/broken.sh"
grep -q 'VOTES=1 EXPVOTES' "$T/broken.sh" || { echo "FAIL: negative control mutation did not apply"; fail=1; }
if run "$T/broken.sh" 2 2; then echo "FAIL: negative control (VOTES hard-coded) passed"; fail=1; fi
[ $fail = 0 ] && echo "PASS: OVMX_VOTES reaches the SYSBOOT> VOTES; hard-coded copy goes red"
exit $fail
