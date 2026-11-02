#!/bin/bash
# stage_rmhold.sh -- stage RMHOLD (tests/lab/ci6/RMHOLD.MAR) on the real VAX
# nodes of an ovmx-lab pod, so a remaster/rebuild fire never has to hand-FTP it
# (the vms-93fa re-fire found it missing from the pod's VAX disks).
#
#   stage_rmhold.sh [--dry-run] <pod> [vaxN ...]     (default: vax1 vax2)
#
# Precondition: each named VAX console is logged in as SYSTEM at the DCL prompt
# (the lab recipe: wait for DECW-I-NODECW, then log in). vaxN is reached on the
# pod's br0 at 10.2.1.N. Per node: FTP RMHOLD.MAR to SYS$LOGIN, then on its
# console MACRO, LINK, COPY to SYS$COMMON:[SYSEXE] and define RMH. Consoles are
# driven only through lab-console.sh, one line per call (AGENTS.md).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
MAR="$HERE/../ci6/RMHOLD.MAR"
DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; shift; fi
POD=${1:?usage: stage_rmhold.sh [--dry-run] <pod> [vaxN ...]}
shift
NODES=${*:-vax1 vax2}
NS=ovmx-lab
STAGE=/lab/run-rmhold

[ -f "$MAR" ] || { echo "FATAL: $MAR missing" >&2; exit 1; }

run() {
	if [ "$DRY" = 1 ]; then echo "RUN: $*"; else timeout 120 "$@"; fi
}
con() {     # one console line, then let DCL finish it
	run "$HERE/lab-console.sh" send "$POD" "$1" "$2"
	[ "$DRY" = 1 ] || sleep "${3:-5}"
}

run kubectl -n "$NS" exec "$POD" -- mkdir -p "$STAGE"
run kubectl -n "$NS" cp "$MAR" "$POD:$STAGE/RMHOLD.MAR"
run kubectl -n "$NS" cp "$HERE/vput.py" "$POD:$STAGE/vput.py"
for n in $NODES; do
	case "$n" in
	vax[1-9]) ip=10.2.1.${n#vax} ;;
	*) echo "FATAL: node '$n' is not vaxN" >&2; exit 2 ;;
	esac
	run kubectl -n "$NS" exec "$POD" -- python3 "$STAGE/vput.py" "$ip" "$STAGE/RMHOLD.MAR"
	con "$n" 'MACRO RMHOLD' 20
	con "$n" 'LINK RMHOLD' 10
	con "$n" 'COPY RMHOLD.EXE SYS$COMMON:[SYSEXE]RMHOLD.EXE' 5
	con "$n" 'RMH :== $SYS$SYSTEM:RMHOLD' 2
	con "$n" 'DIRECTORY/SIZE SYS$SYSTEM:RMHOLD.EXE' 3
	if [ "$DRY" = 0 ]; then
		"$HERE/lab-console.sh" read "$POD" "$n" 12 | grep -q 'RMHOLD.EXE;' ||
			{ echo "FATAL: RMHOLD.EXE not staged on $n" >&2; exit 3; }
	fi
	echo "staged RMHOLD on $n ($ip)"
done
