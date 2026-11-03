#!/bin/bash
# test_ci6_evac_share.sh -- the shared-volume grader (`ci6_evac.sh grade-share`,
# rd vms-5fa) and the share fire's dry-run order, proven before they grade a lab run.
#
# HERMETIC: no pod, no kubectl. Grades
#  * fixtures/ci6_share_pass: excerpts of the real 2026-11-02 capture
#    (captures/vms-5fa-shared-volume-20261102/ci6evac-182708: both MOUNTs, both
#    SHOW DEVICE/FULLs, the VAX1 SDA RSB, ANALYZE/DISK_STRUCTURE, VAX2's Q/V)
#    plus SYNTHESISED lines for what that run never produced: Lock Count 2 and
#    one SDA "Master copy ... on system 00010003" lock, the alternating
#    EVAC.DAT records and OVMX's takeover banner -> all PASS;
#  * mutated copies (Rule 7 negative controls), each failing exactly its criterion;
#  * the real capture -> S1,S5,S6 PASS and S2,S3,S4 FAIL, which is what the
#    lab measured (OVMX's F11B$v lock never reached VAX1; OVMX never wrote).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S=$HERE/../tools/ci6_evac.sh
FIX=$HERE/fixtures/ci6_share_pass
REAL=$HERE/../captures/vms-5fa-shared-volume-20261102/ci6evac-182708
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL: $*"; fail=1; }

expect() {	# expect <dir> <want-exit> <want-failed, e.g. "2,3"> [reason]
	local d=$1 want=$2 crit=$3 why=${4:-} out rc got
	out=$("$S" grade-share "$d" 2>&1); rc=$?
	got=$(echo "$out" | sed -n 's/^S\([0-9]\) FAIL.*/\1/p' | paste -sd, -)
	if [ "$rc" != "$want" ] || [ "$got" != "$crit" ]; then
		bad "$(basename "$d"): exit $rc (want $want), failed '$got' (want '$crit')"
		echo "$out" | sed 's/^/    /'; return 1
	fi
	if [ -n "$why" ] && ! echo "$out" | grep -qF -- "$why"; then
		bad "$(basename "$d"): right verdict, wrong reason (want '$why')"
		echo "$out" | sed 's/^/    /'; return 1
	fi
}
mut() { rm -rf "${T:?}/$1"; cp -r "$FIX" "$T/$1"; (cd "$T/$1" && eval "$2"); }

expect "$FIX" 0 "" || true

mut nomount 'sed -i "/MOUNT-I-MOUNTED/d" OVMXE.console.log'
expect "$T/nomount" 1 "1" "never mounted" || true

mut noserve 'sed -i "s/served to cluster via MSCP Server/not served/" vax1.log'
expect "$T/noserve" 1 "1" "MSCP-served" || true

mut onelock 'sed -i "s/Lock Count:             2/Lock Count:             1/" vax1.log'
expect "$T/onelock" 1 "2" "Lock Count 1" || true

mut nomstcpy 'sed -i "/^Master copy of lock 01000401/d" vax1.log'
expect "$T/nomstcpy" 1 "2" "none is a master copy" || true

mut onlyvax 'sed -i "s/^OVMXE           /VAX1            /" vax1.log'
expect "$T/onlyvax" 1 "3,4" "not alternating" || true

mut seam 'sed -i "s/at seq 16/at seq 17/" OVMXE.console.log'
expect "$T/seam" 1 "4" "took EX at seq 17" || true

mut nostop 'sed -i "/STOP EVACVAX1/d" vax1.log'
expect "$T/nostop" 1 "4" "STOP EVACVAX1" || true

mut anal 'sed -i "/OPENQUOTA/a %ANALDISK-W-BADHEADER, file (5,5,1) EVAC.DAT;1 invalid file header" vax1.log'
expect "$T/anal" 1 "5" "ANALDISK-W-BADHEADER" || true

mut status 'sed -i "s/ANALDISK STATUS=%X00000001/ANALDISK STATUS=%X00000000/" vax1.log'
expect "$T/status" 1 "5" "STATUS 00000000" || true

mut bugcheck 'echo "**** FATAL BUG CHECK, version = V7.3  VAXPORT, Fatal error detected by VAX port driver" >> vax2.log'
expect "$T/bugcheck" 1 "6" "FATAL BUG CHECK" || true

mut gap 'sed -i "/538968607         5 /d" vax1.log'
expect "$T/gap" 1 "6" "contiguous" || true

[ -d "$REAL" ] && { expect "$REAL" 1 "2,3,4" "Lock Count 1" || true; } || bad "real capture missing: $REAL"

# The share fire's console order (dry run, no pod).
dry=$(CI6_DRY=1 "$S" share podx "$T/dry" 2>&1)
order=$(echo "$dry" | grep -nE 'MOUNT/SYSTEM \$2\$DUA2|MOUNT/SYSTEM VAX1\$DUA|SHOW RESOURCE/NAME="F11B\$vEVAC5FA     "|EVACWL 10$|STOP EVACVAX1|DISMOUNT VAX1|ANALYZE/DISK_STRUCTURE' | cut -d: -f1 | paste -sd' ')
n=$(echo "$order" | wc -w); sorted=$(echo "$order" | tr ' ' '\n' | sort -n | paste -sd' ')
[ "$n" -ge 7 ] && [ "$order" = "$sorted" ] || bad "share dry-run order: VAX1 mount, OVMX mount, SDA, writes, STOP, OVMX dismount, ANALYZE (got lines '$order')"
echo "$dry" | grep -q 'SHOW DEVICE D$' || bad "share discovers OVMX's MSCP unit with SHOW DEVICE D (vms-582: the prefix matches past the VAX1$ node prefix)"
# S3's "took EX while VAX1 ran" check and its takeover wait read from an S3
# marker sent after S2, so S2's legitimate OVMX EX grants answer for neither
# (the 2026-11-02 r2 fire aborted on S2's lines).
s2=$(echo "$dry" | grep -n 'EVACWL 10$' | tail -1 | cut -d: -f1)
s3=$(echo "$dry" | grep -n 'CI6EVAC-S3 ' | head -1 | cut -d: -f1)
sb=$(echo "$dry" | grep -n 'OVSTBY.COM$' | tail -1 | cut -d: -f1)
[ -n "$s2" ] && [ -n "$s3" ] && [ -n "$sb" ] && [ "$s2" -lt "$s3" ] && [ "$s3" -lt "$sb" ] || bad "share sends the CI6EVAC-S3 marker after S2's rounds and before the STANDBY spawn (got S2=$s2 S3=$s3 spawn=$sb)"
grep -q "since \"\$OCON\" 4000 CI6EVAC-S3 | grep -q 'took EVAC" "$S" || bad "S3's took-EX-while-VAX1-ran check reads from the S3 marker"
grep -q 'EX at seq" 180 CI6EVAC-S3' "$S" || bad "S3's takeover wait reads from the S3 marker"

[ $fail = 0 ] && echo "PASS: ci6_evac.sh grade-share -- fixture PASS, 11 negative controls FAIL their criterion, the 2026-11-02 capture FAILs S2,S3,S4"
exit $fail
