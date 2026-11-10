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
# CLOCKS (rd vms-6731): the SETUP clock check over clock.txt -- aligned nodes
# PASS; a node off by 1.6 s, a probe window too wide to bound the offset, a
# probe line that is not in the node's console, and a missing ALIGN probe are
# each a SETUP FAULT (exit 3, C3 UNGRADED, never a C3 PASS/FAIL); a big PRE
# (before alignment) offset is reported only; a real criterion FAIL still
# exits 1. Plus console_stamp.py (lab-console.sh stamp's in-pod half) on a
# local log + input file: %NOW% substitution, the lead, and the SEEN window.
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

# ---- SETUP clock check (rd vms-6731) ----------------------------------------
# clk <dir> <tag> <NODE> <console> <write> <seen> <cs> <node-time> [noconsole]
clk() {
	local l="CI6CLK $2 $3  $8"
	echo "$2 $3 WRITE $5 SEEN $6 DT $7 LINE $l" >> "$1/clock.txt"
	[ -n "${9:-}" ] || echo "$l" >> "$1/$4"
}
aligned() {	# aligned <name> [OVMXE node time] [OVMXE seen] [VAX2 noconsole]
	mut "$1" x true
	clk "$T/$1" PRE OVMXE OVMXE.console.log 10-OCT-2026:17:20:00.00 10-OCT-2026:17:20:00.20 20 '10-OCT-2026 17:19:56.40'
	clk "$T/$1" ALIGN VAX1 vax1.log 10-OCT-2026:17:20:01.00 10-OCT-2026:17:20:01.20 20 '10-OCT-2026 17:20:01.12'
	clk "$T/$1" ALIGN VAX2 vax2.log 10-OCT-2026:17:20:02.00 10-OCT-2026:17:20:02.20 20 '10-OCT-2026 17:20:02.08' "${4:-}"
	clk "$T/$1" ALIGN OVMXE OVMXE.console.log 10-OCT-2026:17:20:03.00 "${3:-10-OCT-2026:17:20:03.20}" 20 "${2:-10-OCT-2026 17:20:03.15}"
}
aligned clk_ok
expect "$T/clk_ok" 0 "" "setup: PASS" || true
grep -q 'OVMXE -3.70' <("$S" grade "$T/clk_ok") || bad "clk_ok: the PRE offset is not reported"
python3 -I "$HERE/../tools/ci6_evac_grade.py" --clock "$T/clk_ok" >/dev/null || bad "--clock: aligned bed is not exit 0"

aligned clk_skew '10-OCT-2026 17:20:01.50'	# OVMXE 1.6 s behind the pod
expect "$T/clk_skew" 3 "" "ALIGN OVMXE -1.60 s" || true
"$S" grade "$T/clk_skew" | grep -q '^C3 UNGRADED (setup fault' || bad "clk_skew: C3 is not UNGRADED"
"$S" grade "$T/clk_skew" | grep -q '^VERDICT: SETUP FAULT' || bad "clk_skew: verdict is not SETUP FAULT"
python3 -I "$HERE/../tools/ci6_evac_grade.py" --clock "$T/clk_skew" >/dev/null; [ $? = 3 ] || bad "--clock: skewed bed is not exit 3"

aligned clk_wide '' 10-OCT-2026:17:20:04.00	# a 1 s window bounds nothing
expect "$T/clk_wide" 3 "" "(+-0.50) exceeds 0.30 s" || true

aligned clk_forged '' '' x	# a clock.txt line no console printed
expect "$T/clk_forged" 3 "" "ALIGN VAX2: probe line is not in the VAX2 console" || true

aligned clk_noovmx
sed -i '/^ALIGN OVMXE /d' "$T/clk_noovmx/clock.txt"
expect "$T/clk_noovmx" 3 "" "no ALIGN probe for OVMXE" || true

aligned clk_skew_c4 '10-OCT-2026 17:20:01.50'	# a real FAIL still exits 1
sed -i "/EVPROBE/d" "$T/clk_skew_c4/vax2.log"
expect "$T/clk_skew_c4" 1 "4" "+ SETUP FAULT" || true

# console_stamp.py: the in-pod half of lab-console.sh stamp, on a local console.
CS=$HERE/../tools/console_stamp.py
: > "$T/con.log"; : > "$T/con.log.in"
# The fake node answers 0.4 s after its input arrives, as a console does.
( for i in $(seq 200); do [ -s "$T/con.log.in" ] && break; sleep 0.05; done
  sleep 0.4; echo 'CI6CLK ALIGN VAX1   3-NOV-2026 03:25:07.81' >> "$T/con.log" ) &
out=$(python3 -I "$CS" "$T/con.log" 150 "$(printf 'SET TIME=%%NOW%%' | base64 -w0)" \
	"$(printf 'CI6CLK ALIGN VAX1 +[ 0-9]?[0-9]-[A-Z]{3}-' | base64 -w0)"); rc=$?
wait
echo "$out" | grep -qE '^WRITE [0-9]{2}-[A-Z]{3}-[0-9]{4}:[0-9:]{8}\.[0-9]{2} SEEN [0-9]{2}-[A-Z]{3}-[0-9]{4}:[0-9:]{8}\.[0-9]{2} DT [0-9]+ LINE CI6CLK ALIGN VAX1   3-NOV-2026 03:25:07.81$' \
	|| bad "console_stamp: output '$out' (rc $rc)"
dt=$(echo "$out" | awk '{print $6}')
[ "${dt:-x}" -ge 40 ] 2>/dev/null && [ "$dt" -le 300 ] || bad "console_stamp: DT '$dt' is not the ~0.4 s wait"
w=$(echo "$out" | awk '{print $2}')
inl=$(tr -d '\r' < "$T/con.log.in")
python3 -I - "$w" "$inl" <<'PY' || bad "console_stamp: '$inl' is not SET TIME=<WRITE + 1.50 s>"
import sys, datetime
f = '%d-%b-%Y:%H:%M:%S.%f'
w = datetime.datetime.strptime(sys.argv[1], f)
pre, _, t = sys.argv[2].partition('=')
d = (datetime.datetime.strptime(t, f) - w).total_seconds()
sys.exit(0 if pre == 'SET TIME' and 1.49 <= d <= 1.51 else 1)
PY
: > "$T/con.log.in"
python3 -I "$CS" "$T/con.log" 0 "$(printf 'X' | base64 -w0)" - | grep -q '^WRITE .* SEEN - DT - LINE -$' || bad "console_stamp: no-ERE output"
[ "$(cat "$T/con.log.in")" = "$(printf 'X\r')" ] || bad "console_stamp: no-ERE line not written with CR"

# The real hand-run capture: no NOQUEUE probe was run, so criterion 4 FAILs.
expect "$RUN16" 1 "4" "no \$ENQ/NOQUEUE probe was run" || true

# fire, dry run: the console sequence is in the evacuation's order.
out=$(CI6_DRY=1 "$S" fire podx "$T/dry" 2>&1)
ln=0
for w in 'CI6EVAC-BEGIN' 'CI6Q Q=' 'STAGE EVACWL.MAR' 'MACRO EVPROBE' 'MOUNT/CLUSTER $2$DUA1: VAX1DATA' \
         'STAMP vax1: CI6CLK PRE VAX1' 'STAMP vax1: SET TIME=%NOW%' 'STAMP vax1: CI6CLK ALIGN VAX1' \
         'STAMP vax2: SET TIME=%NOW%' 'STAMP ovmxe: SET TIME=%NOW%' 'STAMP ovmxe: CI6CLK ALIGN OVMXE' \
         'EVACWL STANDBY NL=90' 'WAIT ovmxe: /standby holds' 'PROCESS=EVACVAX1' \
         'STAMP ovmxe: CI6CLK PREEVAC OVMXE' 'SEND vax1: STOP EVACVAX1' 'SEND vax1: DISMOUNT/CLUSTER $2$DUA1:' 'SEND ovmxe: MOUNT VDA100: VAX1DATA' \
         'SEND vax2: EVPROBE' 'SEND vax1: @SYS$SYSTEM:SHUTDOWN' 'SEND vax1: REMOVE_NODE' \
         'SEND ovmxe: DISMOUNT VDA100:' 'SEND vax2: MOUNT $2$DUA1: VAX1DATA' 'SEND vax2: TYPE $2$DUA1:[' 'GRADE'; do
	n=$(echo "$out" | tail -n +$((ln + 1)) | grep -nF -- "$w" | head -1 | cut -d: -f1)
	[ -n "$n" ] || { bad "fire dry run: missing or out of order after line $ln: $w"; break; }
	ln=$((ln + n))
done

# fire on the vms-5fa shared-volume bed (CI6_SHARED=1): both nodes mount the
# MSCP-served volume up front, no handoff, and VAX2 reads the seam back over
# VAX1's server BEFORE VAX1 (the volume's only server) leaves.
out=$(CI6_DRY=1 CI6_SHARED=1 CI6_INIT=1 "$S" fire podx "$T/dryshared" 2>&1)
ln=0
for w in 'CI6EVAC-BEGIN' 'CI6Q Q=' 'MACRO EVPROBE' 'SEND vax1: INITIALIZE $2$DUA2: EVAC1E58' \
         'SEND vax1: MOUNT/SYSTEM $2$DUA2: EVAC1E58' 'SEND ovmxe: SHOW DEVICE D' \
         'SEND ovmxe: MOUNT/SYSTEM VAX1$DUA16386: EVAC1E58' 'STAMP ovmxe: SET TIME=%NOW%' 'EVACWL STANDBY NL=90 60' \
         'WAIT ovmxe: /standby holds' 'PROCESS=EVACVAX1' 'STAMP vax1: CI6CLK PREEVAC VAX1' 'SEND vax1: STOP EVACVAX1' \
         'WAIT ovmxe: /EVACWL: OVMXE took' 'SEND vax2: EVPROBE' 'SEND ovmxe: DISMOUNT VAX1$DUA16386:' \
         'SEND vax2: MOUNT/SYSTEM $2$DUA2: EVAC1E58' 'SEND vax2: TYPE $2$DUA2:[' 'SEND vax2: DISMOUNT $2$DUA2:' \
         'SEND vax1: @SYS$SYSTEM:SHUTDOWN' 'SEND vax1: REMOVE_NODE' 'WAIT vax2: /removed' 'SEND vax2: WRITE SYS$OUTPUT "Q=' 'GRADE'; do
	n=$(echo "$out" | tail -n +$((ln + 1)) | grep -nF -- "$w" | head -1 | cut -d: -f1)
	[ -n "$n" ] || { bad "shared fire dry run: missing or out of order after line $ln: $w"; break; }
	ln=$((ln + n))
done
for w in 'DISMOUNT/CLUSTER' 'VDA100:' 'MOUNT $2$DUA1:' 'STOP SYSTEM_1'; do
	echo "$out" | grep -qF -- "$w" && bad "shared fire dry run sends the run16 bed's '$w'"
done

[ $fail = 0 ] && echo "PASS: ci6_evac.sh grade -- fixture PASS, 10 negative controls FAIL their criterion, run16 FAILs only C4 (no NOQUEUE probe); 6 clock-alignment cases (SETUP FAULT, C3 ungraded), console_stamp; fire dry-run order on both beds"
exit $fail
