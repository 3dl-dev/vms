#!/bin/sh
#
# test_show_cluster_negctl.sh - HONEST-ABSENCE gate for SHOW CLUSTER
# (FC-P3.9, docs/plan-faithful-cluster-executive.md; carries forward vms-8d4 /
# vms-551's NOTMEMBER != NOSUCHDEV contract).
#
# WHAT THIS RUNS. The real DCL.EXE, on a plain host with NO /dev/vms, for every
# SHOW CLUSTER class. With no executive to ask, each class must report
# "%SYSTEM-W-NOSUCHDEV" and print NOTHING ELSE about the cluster -- no member
# table, no CLUB, no connection list, no port row. It must NOT report
# "%SYSTEM-I-NOTMEMBER" either: NOTMEMBER is a CLAIM about this node's
# membership, and a claim requires a reader; with the executive unreachable
# there is nothing that could have been read.
#
# WHY THE DISTINCTION IS THE WHOLE POINT (vms-8d4, still). SHOW CLUSTER once
# printed %SYSTEM-I-NOTMEMBER unconditionally, so a genuinely-clustered node
# lied about being standalone. Its successor read a FILE that a userspace
# daemon published. FC-P3.9 deleted that daemon and the executive-side mirror
# it wrote through: SHOW CLUSTER now reads the connection manager's own
# CLUB/CSB table. The two non-list outcomes are therefore:
#
#   SS$_NORMAL with no CSB   -> "%SYSTEM-I-NOTMEMBER"   (the executive answered)
#   SS$_NOSUCHDEV            -> "%SYSTEM-W-NOSUCHDEV"   (nothing answered)
#
# THIS GATE PROVES THE SECOND ONLY, and says so. The first -- VAXCLUSTER=0 on a
# REAL /dev/vms yields SS$_NORMAL / NOTMEMBER -- cannot be proven on a host
# with no executive, by construction. It is proven against a real /dev/vms by
# tests/qemu/test_syssvc_cluster_negctl.c (the R4 leg), which is where Rule 9
# says that half belongs.
#
# Usage: test_show_cluster_negctl.sh [PATH_TO_DCL.EXE]

set -u

DCL="${1:-${VMSDCL:-}}"
if [ -z "$DCL" ]; then
    for cand in "$(dirname "$0")/../../build/bin/DCL.EXE" \
                "$(dirname "$0")/../../build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

status=0
MARK='OVMX-PROBE-ALIVE'
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "FC-P3.9 honest-absence gate: every SHOW CLUSTER class fails honestly with no executive"

if [ ! -x "$DCL" ]; then
    echo "FAIL: DCL.EXE not found (looked at '$DCL')"
    echo "  -> this gate must RUN the command; it cannot be evaluated by reading"
    exit 1
fi

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

# A VMS condition message goes to SYS$ERROR, not SYS$OUTPUT, so both streams are
# captured: the liveness marker (SYS$OUTPUT) and NOSUCHDEV (SYS$ERROR).
run_cluster() {
    printf '%s\nWRITE SYS$OUTPUT "%s"\n' "$1" "$MARK" \
        | "$DCL" >"$WORK/out" 2>&1
}

# Every string below is one this build's SHOW CLUSTER prints only when it has
# REAL executive state to print. Seeing any of them with no /dev/vms means a
# class rendered a row it could not have read.
FABRICATION_MARKERS='View of Cluster|Cluster block|SCS connections|Local ports|Circuits|NOTMEMBER'

for cmd in \
    'SHOW CLUSTER' \
    'SHOW CLUSTER/CLUSTER' \
    'SHOW CLUSTER/CONNECTIONS' \
    'SHOW CLUSTER/LOCAL_PORTS' \
    'SHOW CLUSTER/CIRCUITS'
do
    run_cluster "$cmd"
    if ! grep -qF "$MARK" "$WORK/out"; then
        fail "DCL did not run to completion for '$cmd'" \
             "output was:" "$(sed 's/^/       | /' "$WORK/out")"
        continue
    fi
    if ! grep -q 'NOSUCHDEV' "$WORK/out"; then
        fail "'$cmd' did not report NOSUCHDEV without the executive" \
             "the cluster stack is executive-resident; with no /dev/vms the" \
             "honest answer is %SYSTEM-W-NOSUCHDEV." \
             "output was:" "$(sed 's/^/       | /' "$WORK/out")"
        continue
    fi
    if grep -qE "$FABRICATION_MARKERS" "$WORK/out"; then
        fail "'$cmd' printed NOSUCHDEV AND cluster state it could not have read" \
             "output was:" "$(sed 's/^/       | /' "$WORK/out")"
        continue
    fi
    echo "  OK: '$cmd' -> %SYSTEM-W-NOSUCHDEV, and nothing else about the cluster"
done

if [ "$status" -eq 0 ]; then
    echo "FC-P3.9 honest-absence gate: PASS"
else
    echo "FC-P3.9 honest-absence gate: FAIL"
fi
exit $status
