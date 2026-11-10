#!/bin/bash
# ci6_evac.sh -- the ci.6 rolling-evacuation release gate (rd vms-1e58): drive
# the evacuation on a lab pod, then grade the capture.
#
#   ci6_evac.sh fire <pod> [capture-dir]    drive the run, capture, then grade
#   ci6_evac.sh grade <capture-dir>         grade a capture (pure, no lab)
#   ci6_evac.sh share <pod> [capture-dir]   rd vms-5fa: one MSCP-served volume
#                                           mounted by VAX1 and OVMX at once
#   ci6_evac.sh grade-share <capture-dir>   grade a share capture (pure)
#
# GRADE (tests/lab/tools/ci6_evac_grade.py) reads only the capture directory
# and prints PASS/FAIL for each criterion; exit 1 on any FAIL:
#   1 EVACWL running on VAX1, the OVMX standby queued behind it (NL first)
#   2 the workload stopped on VAX1, VAX1 removed (SHUTDOWN REMOVE_NODE)
#   3 OVMX granted, continues at seq+1 on the shared volume
#   4 a VAX2 $ENQ EX/NOQUEUE probe (tests/lab/ci6/EVPROBE.MAR) gets
#     SS$_NOTQUEUED while OVMX holds the lock
#   5 quorum never lost, 0 bugchecks (console grep), EVAC.DAT contiguous
#
# FIRE drives the run16 sequence (tests/lab/captures/vms-ci6-evac-20261010/)
# through lab-console.sh only, one line per call (AGENTS.md "Lab consoles"),
# writes a timeline, collects the consoles windowed from its own begin marker,
# and grades. Default capture dir: tests/lab/captures/vms-ci6-evac-<date>/<run>.
# CI6_DRY=1 prints the console sequence instead of sending it (no pod).
#
# PRECONDITIONS (fire checks what it can and refuses otherwise):
#  * VAX1 and VAX2 (real V7.3) and the OVMX member are one cluster, every
#    console logged in as SYSTEM at the DCL prompt (prompt-synchronised login,
#    tests/lab/README.md).
#  * Votes leave the survivors quorate when VAX1 leaves. With VAX1/VAX2 at
#    VOTES 1, boot the OVMX member with OVMX_FIRE_VOTES=2 (ovmx_fire.sh):
#    total 4, quorum 3, survivors 3. fire reads CLUSTER_QUORUM/CLUSTER_VOTES
#    on VAX2 and VOTES on VAX1 and refuses if (votes - VAX1's) < quorum.
#  * The OVMX member runs LOCKDIRWT 1 (so its standby NL makes it directory
#    and master, the interim configuration), sees the shared VAX1DATA disk
#    ($2$DUA1: on the VAXes) as VDA100:, and carries SYS$SYSTEM:EVACWL.EXE
#    (tests/lab/ci6/mk_evacwl_native.sh). fire checks EVACWL.EXE.
#
# CLOCKS (rd vms-6731): C3 compares timestamps written by different nodes, and
# each node's clock free-runs (OVMX is a TCG guest with no RTC sync or NTP,
# the VAXes run under SIMH). Just before the workload, fire sets every node's
# clock from the pod's the way an operator does, DCL SET TIME=<time> on each
# console (lab-console.sh stamp computes <time> in the pod at the write), then
# reads each back with F$TIME() into $CAP/clock.txt and aborts the run as a
# SETUP FAULT if any node is off by more than CI6_CLOCK_TOL (0.30 s). The
# probe repeats (PREEVAC) just before the STOP; the grader re-checks both.
#
# CI6_SHARED=1 fires on the vms-5fa bed instead (an ASYM_MSCP=1 pod, the
# share subcommand's volume): $2$DUA<CI6_VAX_UNIT>: is VAX1's alone, so OVMX
# and VAX2 reach it only through VAX1's MSCP server. STEP 0 mounts it /SYSTEM
# on VAX1 and on OVMX's MSCP unit (as share's S1; CI6_INIT=1 INITIALIZEs it),
# so there is no STEP 4 volume handoff. VAX1 is the volume's only server, so
# OVMX's standby writes CI6_TAIL records and ends, OVMX dismounts, and VAX2
# reads EVAC.DAT back over VAX1's MSCP server BEFORE VAX1 leaves (STEP 6).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CI6=$HERE/../ci6
LC=$HERE/lab-console.sh
NS=ovmx-lab
OVMX=${CI6_OVMX_NODE:-OVMXE}
OCON=$(echo "$OVMX" | tr A-Z a-z)
DRY=${CI6_DRY:-0}

usage() { sed -n '2,11p' "$0" >&2; exit 2; }

case "${1:-}" in
grade)
	[ $# -eq 2 ] || usage
	exec python3 -I "$HERE/ci6_evac_grade.py" "$2" ;;
grade-share)
	[ $# -eq 2 ] || usage
	exec python3 -I "$HERE/ci6_evac_grade.py" --share "$2" ;;
fire|share) [ $# -ge 2 ] || usage ;;
*) usage ;;
esac

POD=$2
case "$POD" in *[!A-Za-z0-9-]*|"") echo "ci6_evac.sh: bad pod name" >&2; exit 2;; esac
MODE=$1
STAMP=$(date -u +%Y%m%d)
RUN=ci6evac-$(date -u +%H%M%S)
if [ "$MODE" = share ]; then
	CAP=${3:-$HERE/../captures/vms-5fa-shared-volume-$STAMP/$RUN}
else
	CAP=${3:-$HERE/../captures/vms-ci6-evac-$STAMP/$RUN}
fi
# The OVMX name of the shared volume. fire: the dual-pathed VDA100: of the
# run16 bed. share: discovered from OVMX's SHOW DEVICE (ovmx_mscp_dev).
ODEV=${CI6_OVMX_DEV:-VDA100:}
# A fresh directory per run on the shared volume, so EVAC.DAT starts at seq 1
# by construction (run16's DELETE of the old file failed BADFILEHDR).
DIR=CI6E$(date -u +%m%d%H%M)
TL=$CAP/timeline.txt
mkdir -p "$CAP"

tl() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$TL"; }
die() { tl "ABORT: $*"; exit 3; }
send() {    # send <node> <line> [settle-seconds]
	if [ "$DRY" = 1 ]; then echo "SEND $1: $2"; return 0; fi
	timeout 60 "$LC" send "$POD" "$1" "$2" || die "lab-console send to $1 failed"
	sleep "${3:-3}"
}
ctrl() {
	if [ "$DRY" = 1 ]; then echo "CTRL $1: ^$2"; return 0; fi
	timeout 60 "$LC" ctrl "$POD" "$1" "$2" || die "lab-console ctrl to $1 failed"
	sleep 2
}
# The console since this fire's begin marker (consoles are append-only).
# since <node> [lines] [marker]: a later marker (e.g. CI6EVAC-S3, sent just
# before a phase) scopes the read to that phase, so an earlier phase's lines
# -- S2's legitimate OVMX EX grants -- never answer for it.
since() {
	[ "$DRY" = 1 ] && return 0
	timeout 120 "$LC" read "$POD" "$1" "${2:-4000}" | tr -d '\r' | awk -v m="${3:-CI6EVAC-BEGIN} $RUN" 'index($0,m){b=NR} {l[NR]=$0} END{for(i=b;i<=NR&&b;i++)print l[i]}'
}
# wait_for <node> <ERE> <seconds> [marker]: poll the windowed console for a
# line, read from [marker] (default the fire's begin marker) on.
wait_for() {
	[ "$DRY" = 1 ] && { echo "WAIT $1: /$2/"; return 0; }
	local i
	for i in $(seq 0 10 "$3"); do
		since "$1" 4000 "${4:-CI6EVAC-BEGIN}" | grep -qE -- "$2" && return 0
		sleep 10
	done
	return 1
}

# stage_mar <node> <vax-ip> <NAME>: FTP tests/lab/ci6/<NAME>.MAR to a real VAX's
# SYS$LOGIN, then MACRO + LINK it there and define the foreign command <NAME>.
stage_mar() {
	local n=$1 ip=$2 m=$3 st=/lab/run-ci6evac
	if [ "$DRY" = 1 ]; then
		echo "STAGE $m.MAR -> $n (vput.py)"
	else
		kubectl -n $NS exec "$POD" -- mkdir -p $st || die "stage dir"
		kubectl -n $NS cp "$CI6/$m.MAR" "$POD:$st/$m.MAR" || die "kubectl cp $m.MAR"
		kubectl -n $NS cp "$HERE/vput.py" "$POD:$st/vput.py" || die "kubectl cp vput.py"
		kubectl -n $NS exec "$POD" -- python3 $st/vput.py "$ip" $st/$m.MAR || die "FTP $m.MAR"
	fi
	send "$n" 'SET DEFAULT SYS$LOGIN'
	send "$n" "MACRO $m" 25
	send "$n" "LINK $m" 15
	send "$n" "$m :== \$SYS\$LOGIN:$m.EXE"
	send "$n" "DIRECTORY/SIZE SYS\$LOGIN:$m.EXE"
	[ "$DRY" = 1 ] || since "$n" | grep -q "$m.EXE;" || die "$m did not build on $n"
}

# ---- node clocks (rd vms-6731) ---------------------------------------------
CLK=$CAP/clock.txt
# clock_probe <console> <NODE> <TAG>: F$TIME() on the node, timed by the pod's
# clock; one clock.txt line. Echoes the probe's DT (hundredths) for a lead.
clock_probe() {
	local out
	if [ "$DRY" = 1 ]; then echo "STAMP $1: CI6CLK $3 $2 F\$TIME()" >&2; echo 0; return 0; fi
	out=$(timeout 90 "$LC" stamp "$POD" "$1" 0 "CI6CLK $3 $2 +[ 0-9]?[0-9]-[A-Z]{3}-[0-9]{4} " \
		"WRITE SYS\$OUTPUT \"CI6CLK $3 $2 \",F\$TIME()")
	echo "$3 $2 ${out:-WRITE - SEEN - DT - LINE -}" >> "$CLK"
	tl "CLOCK $3 $2 ${out:-no read-back}" >&2
	set -- $out
	case "${6:-}" in ''|*[!0-9]*) echo 0;; *) echo "$6";; esac
}
# align_clocks: SET TIME on every node from the pod clock, then gate on the
# read-back. The SET TIME line carries the pod time plus the node's console
# delay as measured by its PRE probe, so the time is right when DCL runs it.
align_clocks() {
	local n N lead
	for n in vax1 vax2 "$OCON"; do
		N=$(echo "$n" | tr a-z A-Z)
		lead=$(clock_probe "$n" "$N" PRE)
		if [ "$DRY" = 1 ]; then echo "STAMP $n: SET TIME=%NOW%"
		else timeout 90 "$LC" stamp "$POD" "$n" "$lead" - 'SET TIME=%NOW%' >/dev/null || die "lab-console stamp to $n failed"
			sleep 3
		fi
		clock_probe "$n" "$N" ALIGN >/dev/null
	done
	[ "$DRY" = 1 ] && return 0
	python3 -I "$HERE/ci6_evac_grade.py" --clock "$CAP" | tee -a "$TL"
	[ "${PIPESTATUS[0]}" = 0 ] || die "SETUP FAULT: node clocks not aligned (clock.txt); C3 would compare misaligned clocks"
}

# ovmx_mscp_dev <vax-unit>: the OVMX name of a unit VAX1 serves. FC-P7.1 names
# a served unit <SCSNODE>$DUA<n>: (no grounded allocation class); n is the MSCP
# unit number off the wire, whose low 14 bits are the VAX's DUA unit (the
# vms-4822 capture: $2$DUA2: -> VAX1$DUA16386:). Reads OVMX's own SHOW DEVICE.
ovmx_mscp_dev() {
	since "$OCON" | grep -aoE 'VAX1\$DUA[0-9]+:' | sort -u | while IFS= read -r d; do
		n=${d#VAX1\$DUA}; n=${n%:}
		[ $((n % 16384)) -eq "$1" ] && { echo "$d"; break; }
	done
}

# ============================================================================
# share (rd vms-5fa): ONE ODS-2 volume, served by VAX1's MSCP server, mounted
# by VAX1 (/SYSTEM, the served path) and by OVMX (/SYSTEM, through its MSCP
# class driver) AT THE SAME TIME. Both nodes append EVAC.DAT alternately under
# EVAC$WORKLOAD; then the workload evacuates from VAX1 to OVMX on the still
# shared volume. OVMX dismounts; VAX1 types EVAC.DAT and runs
# ANALYZE/DISK_STRUCTURE. Bed: an ASYM_MSCP=1 pod (tests/lab/README.md), so
# $2$DUA2: is VAX1's alone and every other node reaches it over MSCP.
# CI6_INIT=1 INITIALIZEs it first (a fresh pod's d2.dsk is blank).
# ============================================================================
if [ "$MODE" = share ]; then
	LABEL=${CI6_LABEL:-EVAC5FA}
	VUNIT=${CI6_VAX_UNIT:-2}
	VDEV="\$2\$DUA$VUNIT:"
	PADL=$(printf '%-12s' "$LABEL")
	ROUNDS=${CI6_ROUNDS:-2}; PER=${CI6_PER:-10}; TAIL=${CI6_TAIL:-15}
	tl "T0 share $RUN on pod $POD; OVMX node $OVMX; volume $LABEL = VAX1 $VDEV; [$DIR]; capture $CAP"
	for n in vax1 vax2 "$OCON"; do send "$n" "! CI6EVAC-BEGIN $RUN" 2; done

	# ---- preconditions: a 3-member cluster, EVACWL.EXE on OVMX ----------
	send vax2 'WRITE SYS$OUTPUT "Q=",F$GETSYI("CLUSTER_QUORUM")," V=",F$GETSYI("CLUSTER_VOTES")," N=",F$GETSYI("CLUSTER_NODES")'
	send "$OCON" 'DIRECTORY SYS$SYSTEM:EVACWL.EXE' 5
	if [ "$DRY" != 1 ]; then
		nn=$(since vax2 | sed -n 's/^Q=[0-9]* V=[0-9]* N=\([0-9]*\).*/\1/p' | tail -1)
		[ "${nn:-0}" -ge 3 ] || die "cluster has '${nn}' members, want VAX1+VAX2+$OVMX"
		since "$OCON" | grep -q 'EVACWL.EXE;' || die "$OVMX has no SYS\$SYSTEM:EVACWL.EXE"
	fi
	stage_mar vax1 10.2.1.1 EVACWL
	tl "STAGED EVACWL.EXE on VAX1"

	# ---- S1: the shared volume, mounted by both ------------------------------
	[ "${CI6_INIT:-0}" = 1 ] && send vax1 "INITIALIZE $VDEV $LABEL" 30
	send vax1 "MOUNT/SYSTEM $VDEV $LABEL" 20
	wait_for vax1 "MOUNT-I-MOUNTED, $LABEL mounted" 60 || die "VAX1 could not mount $VDEV"
	send vax1 "CREATE/DIRECTORY $VDEV[$DIR]" 5
	send vax1 "DEFINE/SYSTEM EVAC\$DATA $VDEV[$DIR]"
	send "$OCON" 'SHOW DEVICE D' 8	# vms-582: a prefix matches past the VAX1$ node prefix
	if [ "$DRY" = 1 ]; then ODEV=${CI6_OVMX_DEV:-VAX1\$DUA16386:}
	elif [ -z "${CI6_OVMX_DEV:-}" ]; then ODEV=$(ovmx_mscp_dev "$VUNIT"); fi
	[ -n "$ODEV" ] && [ "$ODEV" != VDA100: ] || die "$OVMX lists no MSCP unit for VAX1 unit $VUNIT"
	tl "S1 $OVMX names VAX1's $VDEV as $ODEV"
	send "$OCON" "MOUNT/SYSTEM $ODEV $LABEL" 20
	wait_for "$OCON" "MOUNT-I-MOUNTED, $LABEL mounted" 120 || die "$OVMX could not mount $ODEV"
	send "$OCON" "DEFINE/SYSTEM EVAC\$DATA $ODEV[$DIR]"
	send vax1 "SHOW DEVICE/FULL $VDEV" 5
	send vax2 "SHOW DEVICE/FULL $VDEV" 5
	send "$OCON" "SHOW DEVICE/FULL $ODEV" 5
	# The Files-11 volume lock both MOUNTs hold: one RSB, mastered on one node.
	send vax1 'ANALYZE/SYSTEM' 5
	send vax1 "SHOW RESOURCE/NAME=\"F11B\$v$PADL\"" 5
	if [ "$DRY" != 1 ]; then
		for lk in $(since vax1 | awk '/SHOW RESOURCE\/NAME="F11B\$v/{f=1} f&&/^Granted queue/{g=1;next} g&&/queue/{exit} g{for(i=1;i<=NF;i++) if($i~/^[0-9A-F]{8}$/ && $(i+1)~/^(NL|CR|CW|PR|PW|EX)$/) print $i}'); do
			send vax1 "SHOW LOCK $lk" 3
		done
	fi
	send vax1 'EXIT' 3
	tl "S1 $LABEL mounted on VAX1 ($VDEV) and $OVMX ($ODEV)"

	# ---- S2: alternate writes under EVAC$WORKLOAD ------------------------------
	send "$OCON" 'CREATE SYS$LOGIN:OVWL.COM' 2
	send "$OCON" '$ EVACWL :== $SYS$SYSTEM:EVACWL.EXE' 1
	send "$OCON" "\$ EVACWL $PER" 1
	ctrl "$OCON" Z
	r=1
	while [ $r -le "$ROUNDS" ]; do
		tl "S2 round $r: VAX1 EVACWL $PER (EX), then $OVMX EVACWL $PER queued behind it"
		send vax1 "SPAWN/NOWAIT/PROCESS=EVW1R$r/OUTPUT=SYS\$LOGIN:EVW1R$r.LOG EVACWL $PER" 5
		send "$OCON" 'SPAWN/NOWAIT @SYS$LOGIN:OVWL.COM' 3
		[ "$DRY" = 1 ] || sleep $((PER * 2 + 30))
		r=$((r + 1))
	done
	send vax1 "DIRECTORY/SIZE=ALL $VDEV[$DIR]EVAC.DAT" 5
	send "$OCON" "DIRECTORY/SIZE=ALL $ODEV[$DIR]EVAC.DAT" 5

	# ---- S3: evacuation: VAX1 runs it, OVMX stands by, VAX1's copy is stopped --
	tl "S3 VAX1 EVACWL (EX, forever); $OVMX EVACWL STANDBY $TAIL queued"
	send vax1 'SPAWN/NOWAIT/PROCESS=EVACVAX1/OUTPUT=SYS$LOGIN:VAX1WL.LOG EVACWL' 10
	send "$OCON" "! CI6EVAC-S3 $RUN" 2	# S2's OVMX EX grants precede this
	send "$OCON" 'CREATE SYS$LOGIN:OVSTBY.COM' 2
	send "$OCON" '$ EVACWL :== $SYS$SYSTEM:EVACWL.EXE' 1
	send "$OCON" "\$ EVACWL STANDBY NL=5 $TAIL" 1
	ctrl "$OCON" Z
	send "$OCON" 'SPAWN/NOWAIT @SYS$LOGIN:OVSTBY.COM' 5
	wait_for "$OCON" 'standby holds EVAC\$WORKLOAD NL' 60 || die "$OVMX standby never took NL"
	[ "$DRY" = 1 ] || sleep 30
	since "$OCON" 4000 CI6EVAC-S3 | grep -q 'took EVAC\$WORKLOAD EX' && die "$OVMX took EX while VAX1 ran"
	tl "S3 EVACUATE: STOP EVACVAX1 on VAX1"
	send vax1 'STOP EVACVAX1' 5
	wait_for "$OCON" "EVACWL: $OVMX took EVAC\\\$WORKLOAD EX at seq" 180 CI6EVAC-S3 || die "$OVMX standby was not granted EX"
	tl "TAKEOVER: $(since "$OCON" 4000 CI6EVAC-S3 | grep -ao "EVACWL: $OVMX took.*" | tail -1)"
	[ "$DRY" = 1 ] || sleep $((TAIL * 2 + 20))

	# ---- S4: OVMX leaves the volume; VAX1 reads it back and checks it ----------
	send "$OCON" "DISMOUNT $ODEV" 15
	wait_for "$OCON" 'DISMOUNT-I-DISMOUNTED' 90 || die "$OVMX could not dismount $ODEV"
	send vax1 "TYPE $VDEV[$DIR]EVAC.DAT" 20
	send vax1 "ANALYZE/DISK_STRUCTURE $VDEV" 90
	send vax1 'WRITE SYS$OUTPUT "ANALDISK STATUS=",$STATUS' 5
	send vax2 'WRITE SYS$OUTPUT "Q=",F$GETSYI("CLUSTER_QUORUM")," V=",F$GETSYI("CLUSTER_VOTES")," N=",F$GETSYI("CLUSTER_NODES")' 5
	tl "END"
	[ "$DRY" = 1 ] && { echo "GRADE-SHARE $CAP"; exit 0; }
	since vax1 50000 > "$CAP/vax1.log"
	since vax2 50000 > "$CAP/vax2.log"
	since "$OCON" 50000 > "$CAP/$OVMX.console.log"
	"$0" grade-share "$CAP" | tee "$CAP/grade.txt"
	exit "${PIPESTATUS[0]}"
fi

SHARED=${CI6_SHARED:-0}
if [ "$SHARED" = 1 ]; then
	LABEL=${CI6_LABEL:-EVAC1E58}; VUNIT=${CI6_VAX_UNIT:-2}; VDEV="\$2\$DUA$VUNIT:"
	TAIL=${CI6_TAIL:-60}; NLARG="NL=90 $TAIL"
else
	LABEL=VAX1DATA; VDEV='$2$DUA1:'; NLARG=NL=90
fi
tl "T0 $RUN on pod $POD; OVMX node $OVMX; data directory [$DIR]; volume $LABEL = VAX1 $VDEV (shared=$SHARED); capture $CAP"
for n in vax1 vax2 "$OCON"; do send "$n" "! CI6EVAC-BEGIN $RUN" 2; done

# ---- preconditions ----------------------------------------------------------
send vax2 'WRITE SYS$OUTPUT "CI6Q Q=",F$GETSYI("CLUSTER_QUORUM")," V=",F$GETSYI("CLUSTER_VOTES")," N=",F$GETSYI("CLUSTER_NODES")'
send vax1 'WRITE SYS$OUTPUT "CI6V VOTES=",F$GETSYI("VOTES")'
send "$OCON" 'DIRECTORY SYS$SYSTEM:EVACWL.EXE' 5
if [ "$DRY" != 1 ]; then
	qv=$(since vax2 | sed -n 's/^CI6Q Q=\([0-9]*\) V=\([0-9]*\) N=\([0-9]*\).*/\1 \2 \3/p' | tail -1)
	v1=$(since vax1 | sed -n 's/^CI6V VOTES=\([0-9]*\).*/\1/p' | tail -1)
	set -- $qv
	[ $# -eq 3 ] && [ -n "$v1" ] || die "could not read quorum/votes (VAX2 '$qv', VAX1 '$v1')"
	[ "$3" -ge 3 ] || die "cluster has $3 members, want VAX1+VAX2+$OVMX"
	[ $(($2 - v1)) -ge "$1" ] || die "survivors keep $(($2 - v1)) votes < quorum $1: boot $OVMX with OVMX_FIRE_VOTES=2"
	tl "PRECHECK Q=$1 V=$2 N=$3, VAX1 VOTES $v1: survivors keep $(($2 - v1)) >= $1"
	since "$OCON" | grep -q 'EVACWL.EXE;' || die "$OVMX has no SYS\$SYSTEM:EVACWL.EXE"
fi

# ---- stage EVACWL (VAX1) and EVPROBE (VAX2): FTP, then MACRO + LINK on the VAX
STAGE=/lab/run-ci6evac
if [ "$DRY" = 1 ]; then
	echo "STAGE EVACWL.MAR -> vax1, EVPROBE.MAR -> vax2 (vput.py)"
else
	kubectl -n $NS exec "$POD" -- mkdir -p $STAGE || die "stage dir"
	for f in "$CI6/EVACWL.MAR" "$CI6/EVPROBE.MAR" "$HERE/vput.py"; do
		kubectl -n $NS cp "$f" "$POD:$STAGE/$(basename "$f")" || die "kubectl cp $f"
	done
	kubectl -n $NS exec "$POD" -- python3 $STAGE/vput.py 10.2.1.1 $STAGE/EVACWL.MAR || die "FTP EVACWL.MAR"
	kubectl -n $NS exec "$POD" -- python3 $STAGE/vput.py 10.2.1.2 $STAGE/EVPROBE.MAR || die "FTP EVPROBE.MAR"
fi
send vax1 'SET DEFAULT SYS$LOGIN'
send vax1 'MACRO EVACWL' 25
send vax1 'LINK EVACWL' 15
send vax1 'EVACWL :== $SYS$LOGIN:EVACWL.EXE'
send vax2 'SET DEFAULT SYS$LOGIN'
send vax2 'MACRO EVPROBE' 25
send vax2 'LINK EVPROBE' 15
send vax2 'EVPROBE :== $SYS$LOGIN:EVPROBE.EXE'
send vax1 'DIRECTORY/SIZE SYS$LOGIN:EVACWL.EXE'
send vax2 'DIRECTORY/SIZE SYS$LOGIN:EVPROBE.EXE'
if [ "$DRY" != 1 ]; then
	since vax1 | grep -q 'EVACWL.EXE;' || die "EVACWL did not build on VAX1"
	since vax2 | grep -q 'EVPROBE.EXE;' || die "EVPROBE did not build on VAX2"
fi
tl "STAGED EVACWL.EXE on VAX1, EVPROBE.EXE on VAX2"

# ---- STEP 0: the shared volume, cluster-mounted from VAX1 ---------------------
if [ "$SHARED" = 1 ]; then
	tl "STEP 0 VAX1: MOUNT/SYSTEM $VDEV $LABEL; $OVMX: MOUNT/SYSTEM its MSCP unit; [$DIR]"
	[ "${CI6_INIT:-0}" = 1 ] && send vax1 "INITIALIZE $VDEV $LABEL" 30
	send vax1 "MOUNT/SYSTEM $VDEV $LABEL" 20
	wait_for vax1 "MOUNT-I-MOUNTED, $LABEL mounted" 60 || die "VAX1 could not mount $VDEV"
	send vax1 "CREATE/DIRECTORY $VDEV[$DIR]" 5
	send vax1 "DEFINE/SYSTEM EVAC\$DATA $VDEV[$DIR]"
	send "$OCON" 'SHOW DEVICE D' 8	# vms-582: a prefix matches past the VAX1$ node prefix
	if [ "$DRY" = 1 ]; then ODEV=${CI6_OVMX_DEV:-VAX1\$DUA16386:}
	elif [ -z "${CI6_OVMX_DEV:-}" ]; then ODEV=$(ovmx_mscp_dev "$VUNIT"); fi
	[ -n "$ODEV" ] && [ "$ODEV" != VDA100: ] || die "$OVMX lists no MSCP unit for VAX1 unit $VUNIT"
	send "$OCON" "MOUNT/SYSTEM $ODEV $LABEL" 20
	wait_for "$OCON" "MOUNT-I-MOUNTED, $LABEL mounted" 120 || die "$OVMX could not mount $ODEV"
	send "$OCON" "DEFINE/SYSTEM EVAC\$DATA $ODEV[$DIR]"
	send vax1 "SHOW DEVICE $VDEV" 5
	tl "STEP 0 $LABEL mounted on VAX1 ($VDEV) and $OVMX ($ODEV)"
else
tl "STEP 0 VAX1: DISMOUNT/CLUSTER, MOUNT/CLUSTER \$2\$DUA1: VAX1DATA; [$DIR]"
send vax1 'DISMOUNT/CLUSTER $2$DUA1:' 30
send vax1 'MOUNT/CLUSTER $2$DUA1: VAX1DATA' 40
send vax1 "CREATE/DIRECTORY \$2\$DUA1:[$DIR]" 5
send vax1 "DEFINE/SYSTEM EVAC\$DATA \$2\$DUA1:[$DIR]"
send "$OCON" "DEFINE/SYSTEM EVAC\$DATA $ODEV[$DIR]"
send vax1 'SHOW DEVICE $2$DUA1:' 5
fi

# ---- CLOCKS: every node's clock set from the pod's, just before the workload --
tl "CLOCKS SET TIME on VAX1, VAX2, $OVMX from the pod clock; tolerance ${CI6_CLOCK_TOL:-0.30} s"
align_clocks

# ---- STEP 1: OVMX standby takes NL first (directory + master), converts at 90 s
tl "STEP 1 $OVMX: EVACWL STANDBY $NLARG (spawned)"
send "$OCON" 'CREATE SYS$LOGIN:OVSTBY.COM' 2
send "$OCON" '$ EVACWL :== $SYS$SYSTEM:EVACWL.EXE' 1
send "$OCON" "\$ EVACWL STANDBY $NLARG" 1
ctrl "$OCON" Z
send "$OCON" 'SPAWN/NOWAIT @SYS$LOGIN:OVSTBY.COM' 5
wait_for "$OCON" 'standby holds EVAC\$WORKLOAD NL' 60 || die "$OVMX standby never took NL"

# ---- STEP 2: the workload on the real VAX ----------------------------------------
tl "STEP 2 VAX1: EVACWL (direct EX, forever)"
send vax1 'SPAWN/NOWAIT/PROCESS=EVACVAX1/OUTPUT=SYS$LOGIN:VAX1WL.LOG EVACWL' 5
[ "$DRY" = 1 ] || sleep 120           # past the standby's 90 s NL hold: its CONVERT queues
since "$OCON" | grep -q 'took EVAC\$WORKLOAD EX' && die "$OVMX took EX while VAX1 ran"
send vax1 'SHOW SYSTEM' 5
send vax1 "DIRECTORY/SIZE=ALL $VDEV[$DIR]EVAC.DAT" 5
tl "SNAPSHOT VAX1 workload running, $OVMX standby converting behind it"

# ---- STEP 3: evacuate -----------------------------------------------------------
for n in vax1 vax2 "$OCON"; do clock_probe "$n" "$(echo "$n" | tr a-z A-Z)" PREEVAC >/dev/null; done
tl "STEP 3 EVACUATE: STOP EVACVAX1 on VAX1"
send vax1 'STOP EVACVAX1' 5
if [ "$SHARED" = 1 ]; then	# the volume is already OVMX's too: no handoff
	wait_for "$OCON" "EVACWL: $OVMX took EVAC\\\$WORKLOAD EX at seq" 180 || die "$OVMX standby was not granted EX"
	tl "TAKEOVER: $(since "$OCON" | grep -ao "EVACWL: $OVMX took.*" | tail -1)"
else
wait_for "$OCON" 'holding EVAC\$WORKLOAD, waiting for' 180 || die "$OVMX standby was not granted EX"
tl "STEP 3 result: $OVMX standby granted EX"

# ---- STEP 4: volume handoff, OVMX continues on the shared volume ----------------
tl "STEP 4 VAX1: DISMOUNT/CLUSTER \$2\$DUA1:; $OVMX: MOUNT $ODEV VAX1DATA"
send vax1 'DISMOUNT/CLUSTER $2$DUA1:' 45
send vax1 'SHOW DEVICE $2$DUA1:' 5
send "$OCON" "MOUNT $ODEV VAX1DATA" 10
wait_for "$OCON" "EVACWL: $OVMX took EVAC\\\$WORKLOAD EX at seq" 180 || die "$OVMX never continued the workload"
tl "TAKEOVER: $(since "$OCON" | grep -ao "EVACWL: $OVMX took.*" | tail -1)"
fi

# ---- STEP 5: the exclusion probe, while OVMX holds EX -----------------------------
[ "$DRY" = 1 ] || sleep 20
tl "STEP 5 VAX2: EVPROBE (\$ENQW EX NOQUEUE EVAC\$WORKLOAD)"
send vax2 'EVPROBE' 10
tl "PROBE: $(since vax2 | grep -ao 'EVPROBE: .*' | tail -1)"

# ---- STEP 5a (shared): VAX1 is the volume's only server, so the seam is read
# back over its MSCP server before it leaves: OVMX's TAIL ends, OVMX dismounts,
# VAX2 mounts and types EVAC.DAT.
if [ "$SHARED" = 1 ]; then
	[ "$DRY" = 1 ] || sleep $((TAIL * 2 + 20))
	tl "STEP 5a $OVMX: DISMOUNT $ODEV; VAX2: MOUNT/SYSTEM $VDEV, TYPE EVAC.DAT, DISMOUNT"
	send "$OCON" "DISMOUNT $ODEV" 15
	wait_for "$OCON" 'DISMOUNT-I-DISMOUNTED' 90 || die "$OVMX could not dismount $ODEV -- no coherent read-back"
	send vax2 "MOUNT/SYSTEM $VDEV $LABEL" 20
	send vax2 "TYPE $VDEV[$DIR]EVAC.DAT" 30
	send vax2 "DISMOUNT $VDEV" 10
fi

# ---- STEP 6: VAX1 leaves the cluster ----------------------------------------------
tl "STEP 6 VAX1: SHUTDOWN, REMOVE_NODE"
send vax1 '@SYS$SYSTEM:SHUTDOWN' 10
for a in 0 CI6-EVACUATION NO NO NO '' REMOVE_NODE; do send vax1 "$a" 4; done
wait_for vax2 'removed from VAXcluster system VAX1|Node VAX1 .* has been removed' 600 || die "VAX2 never removed VAX1"
wait_for vax1 'SYSTEM SHUTDOWN COMPLETE' 300 || die "VAX1 never completed SHUTDOWN"
send vax2 'WRITE SYS$OUTPUT "Q=",F$GETSYI("CLUSTER_QUORUM")," V=",F$GETSYI("CLUSTER_VOTES")," N=",F$GETSYI("CLUSTER_NODES")' 5
send vax2 'SHOW CLUSTER' 10
tl "STEP 6 result: VAX1 removed"

# ---- STEP 7: end of workload; read the seam back on real VMS --------------------
if [ "$SHARED" != 1 ]; then	# shared: read back in STEP 5a; STEP 6 read Q/V last
[ "$DRY" = 1 ] || sleep 60
tl "STEP 7 $OVMX: STOP SYSTEM_1 (EVACWL), DISMOUNT $ODEV"
send "$OCON" 'STOP SYSTEM_1' 5
send "$OCON" 'SHOW SYSTEM' 5
if [ "$DRY" != 1 ] && since "$OCON" 40 | grep -q 'SYSTEM_1'; then
	tl "SYSTEM_1 survived the first STOP (as in run16): STOP again"
	send "$OCON" 'STOP SYSTEM_1' 5
fi
send "$OCON" "DISMOUNT $ODEV" 10
wait_for "$OCON" "DISMOUNT-I-DISMOUNTED|${ODEV%:}: dismounted" 60 || die "$OVMX could not dismount $ODEV -- no coherent read-back"
tl "STEP 7 VAX2: MOUNT \$2\$DUA1: VAX1DATA, TYPE EVAC.DAT, DISMOUNT"
send vax2 'MOUNT $2$DUA1: VAX1DATA' 20
send vax2 "TYPE \$2\$DUA1:[$DIR]EVAC.DAT" 30
send vax2 'DISMOUNT $2$DUA1:' 10
send vax2 'WRITE SYS$OUTPUT "Q=",F$GETSYI("CLUSTER_QUORUM")," V=",F$GETSYI("CLUSTER_VOTES")," N=",F$GETSYI("CLUSTER_NODES")' 5
fi
tl "END"

# ---- collect + grade ---------------------------------------------------------------
[ "$DRY" = 1 ] && { echo "GRADE $CAP"; exit 0; }
since vax1 50000 > "$CAP/vax1.log"
since vax2 50000 > "$CAP/vax2.log"
since "$OCON" 50000 > "$CAP/$OVMX.console.log"
"$0" grade "$CAP" | tee "$CAP/grade.txt"
exit "${PIPESTATUS[0]}"
