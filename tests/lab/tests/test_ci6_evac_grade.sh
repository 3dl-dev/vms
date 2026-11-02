#!/bin/bash
# test_ci6_evac_grade.sh -- the ci.6 evacuation grader (tests/lab/tools/
# ci6_evac.sh grade, rd vms-1e58) is proven before it grades a release gate.
#
# HERMETIC: no pod, no kubectl. Grades
#  * a passing fixture (tests/lab/tests/fixtures/ci6_evac_pass/, built from
#    run16 console excerpts plus a synthesised EVPROBE line) -> all PASS;
#  * mutated copies of it (Rule 7 negative controls), each of which must FAIL
#    exactly the criterion it breaks: a seq gap, a duplicate seq, a bugcheck
#    line, "quorum lost", no NOQUEUE probe, a probe that was GRANTED, a probe
#    outside OVMX's hold window, a takeover off seq+1, a shutdown without
#    REMOVE_NODE, and a console with no begin marker (an old run's INVLOCKID
#    is no longer windowed out);
#  * the real 2026-10-10 run16 hand-run capture -> criteria 1,2,3,5 PASS and
#    4 FAIL: run16 ran no $ENQ/NOQUEUE probe on VAX2.
# Also checks the fire subcommand's dry-run console order.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S=$HERE/../tools/ci6_evac.sh
FIX=$HERE/fixtures/ci6_evac_pass
RUN16=$HERE/../captures/vms-ci6-evac-20261010/run16
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL: $*"; fail=1; }

bash -n "$S" || bad "bash -n ci6_evac.sh"
grep -q '^ \.TITLE EVPROBE' "$HERE/../ci6/EVPROBE.MAR" || bad "EVPROBE.MAR is not the probe source"
grep -q 'FLAGS=#LCK\$M_NOQUEUE' "$HERE/../ci6/EVPROBE.MAR" || bad "EVPROBE.MAR does not ask NOQUEUE"

# expect <dir> <want-exit> <want-failed-criteria, comma list or ""> [reason]
expect() {
	local d=$1 want=$2 crit=$3 why=${4:-} out rc got
	out=$("$S" grade "$d" 2>&1); rc=$?
	got=$(echo "$out" | sed -n 's/^C\([0-9]\) FAIL.*/\1/p' | paste -sd, -)
	if [ "$rc" != "$want" ] || [ "$got" != "$crit" ]; then
		bad "$(basename "$d"): exit $rc (want $want), failed criteria '$got' (want '$crit')"
		echo "$out" | sed 's/^/    /'
		return 1
	fi
	if [ -n "$why" ] && ! echo "$out" | grep -qF -- "$why"; then
		bad "$(basename "$d"): right verdict, wrong reason (want '$why')"
		echo "$out" | sed 's/^/    /'
		return 1
	fi
	return 0
}
mut() {     # mut <name> <file> <sed-or-shell>: copy the fixture, apply a change
	rm -rf "${T:?}/$1"; cp -r "$FIX" "$T/$1"
	(cd "$T/$1" && eval "$3")
}

expect "$FIX" 0 "" || true

mut gap x 'sed -i "/^OVMXE  *268435461 *1510-OCT/d" vax2.log'
grep -q '268435461        1510-OCT' "$T/gap/vax2.log" && bad "gap mutation did not apply"
expect "$T/gap" 1 "5" "gap: seq 15 missing" || true

mut dup x 'sed -i "/268435461        1510-OCT/p" vax2.log'
expect "$T/dup" 1 "5" "duplicate seq 15" || true

mut bugcheck x 'sed -i "/^\$ EVPROBE\$/i **** FATAL BUG CHECK, version = V7.3  INVEXCEPTN, Exception while above ASTDEL" vax2.log'
expect "$T/bugcheck" 1 "5" "FATAL BUG CHECK" || true

mut quorum x 'echo "%CNXMAN,  Quorum lost, blocking activity" >> OVMXE.console.log'
expect "$T/quorum" 1 "5" "Quorum lost" || true

mut noprobe x 'sed -i "/EVPROBE/d" vax2.log'
expect "$T/noprobe" 1 "4" "no EVPROBE line" || true

mut granted x 'sed -i "s/R0=000009B8 %SYSTEM-W-NOTQUEUED, request not queued/R0=00000001 %SYSTEM-S-NORMAL, normal successful completion/" vax2.log'
expect "$T/granted" 1 "4" "not SS\$_NOTQUEUED" || true

mut early x 'sed -i "s/EVPROBE: 10-OCT-2026 17:23:55.50/EVPROBE: 10-OCT-2026 17:23:15.50/" vax2.log'
expect "$T/early" 1 "4" "hold window" || true

mut seam x 'sed -i "s/took EVAC\$WORKLOAD EX at seq 11/took EVAC\$WORKLOAD EX at seq 12/" OVMXE.console.log'
expect "$T/seam" 1 "3" "took EX at seq 12" || true

mut noremove x 'sed -i "s/^Shutdown options \[NONE\]: REMOVE_NODE/Shutdown options [NONE]: /" vax1.log'
expect "$T/noremove" 1 "2" "REMOVE_NODE" || true

mut nomarker x 'sed -i "/CI6EVAC-BEGIN/d" vax1.log'
expect "$T/nomarker" 1 "5" "INVLOCKID" || true

# The real hand-run capture: no NOQUEUE probe was run, so criterion 4 FAILs.
expect "$RUN16" 1 "4" "no \$ENQ/NOQUEUE probe was run" || true

# fire, dry run: the console sequence is in the evacuation's order.
out=$(CI6_DRY=1 "$S" fire podx "$T/dry" 2>&1)
ln=0
for w in 'CI6EVAC-BEGIN' 'CI6Q Q=' 'STAGE EVACWL.MAR' 'MACRO EVPROBE' 'MOUNT/CLUSTER $2$DUA1: VAX1DATA' \
         'EVACWL STANDBY NL=90' 'WAIT ovmxe: /standby holds' 'PROCESS=EVACVAX1' \
         'SEND vax1: STOP EVACVAX1' 'SEND vax1: DISMOUNT/CLUSTER $2$DUA1:' 'SEND ovmxe: MOUNT VDA100: VAX1DATA' \
         'SEND vax2: EVPROBE' 'SEND vax1: @SYS$SYSTEM:SHUTDOWN' 'SEND vax1: REMOVE_NODE' \
         'SEND ovmxe: DISMOUNT VDA100:' 'SEND vax2: MOUNT $2$DUA1: VAX1DATA' 'SEND vax2: TYPE $2$DUA1:[' 'GRADE'; do
	n=$(echo "$out" | tail -n +$((ln + 1)) | grep -nF -- "$w" | head -1 | cut -d: -f1)
	[ -n "$n" ] || { bad "fire dry run: missing or out of order after line $ln: $w"; break; }
	ln=$((ln + n))
done

[ $fail = 0 ] && echo "PASS: ci6_evac.sh grade -- fixture PASS, 10 negative controls FAIL their criterion, run16 FAILs only C4 (no NOQUEUE probe)"
exit $fail
