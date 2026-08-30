#!/bin/sh
#
# test_show_cluster_membership.sh - HONEST-ABSENCE gate (vms-8d4 -> CONVERTED by
# vms-551, INV-6 / Rule 9): DCL SHOW CLUSTER reads cluster membership from the
# EXECUTIVE (the vms.ko membership block, via /dev/vms), NOT from a userspace
# file. On a host with NO /dev/vms it must FAIL HONESTLY with %SYSTEM-W-NOSUCHDEV
# and must NOT fabricate a member view -- and specifically must NOT read the old
# scsd-published membership file even when one is present.
#
# WHY THIS CHANGED (vms-551). SHOW CLUSTER used to read the SCSD-published FILE
# (/var/run/ovmx/cluster_state, scs_membership.h) -- a userspace-to-userspace
# path with nothing in the executive behind it (the exact facade Rule 9 names:
# "a silent userspace fallback for an executive facility"). vms-551 moved the
# live member set INTO the executive (vms_cluster_members[], read via
# VMS_IOCTL_CLUSTER_MEMBER_GET / vms_kif_cluster_get_members). The conductor
# ruling: absent /dev/vms -> honest SS$_NOSUCHDEV, never a file fallback.
#
# WHERE THE POSITIVE PROOF LIVES NOW. The MEMBER-column / NOTMEMBER-with-members
# behaviour (a real member set rendered, distinct from NOTMEMBER) is proven
# against a REAL /dev/vms in the QEMU path -- tests/qemu/test_syssvc_cluster_member.c
# (SET/GET/CLEAR the executive block, 26 assertions) plus the real DCL.EXE
# SHOW CLUSTER drive in that harness. That is the Rule-9 proof ("not done until
# exercised against real /dev/vms"). THIS host gate keeps the cheap, worth-having
# anti-LARP half: SHOW CLUSTER fails honestly without the executive.
#
# Usage: test_show_cluster_membership.sh [PATH_TO_DCL.EXE]

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
STATE="$WORK/cluster_state"

echo "vms-551 honest-absence gate: SHOW CLUSTER reads the executive, fails honestly without /dev/vms"

if [ ! -x "$DCL" ]; then
    echo "FAIL: DCL.EXE not found (looked at '$DCL')"
    echo "  -> this gate must RUN the command; it cannot be evaluated by reading"
    exit 1
fi

# This gate runs on a plain host (no vms.ko / no /dev/vms), so
# vms_kif_cluster_get_members() returns SS$_NOSUCHDEV and cmd_show_cluster()
# renders "%SYSTEM-W-NOSUCHDEV" -- on SYS$ERROR (stderr), like every VMS
# condition message, NOT SYS$OUTPUT. So capture BOTH streams into $WORK/out
# (2>&1): the liveness marker (SYS$OUTPUT) and the NOSUCHDEV message (SYS$ERROR)
# both land there. OVMX_CLUSTER_STATE_PATH still points at $STATE so we can prove
# the file is IGNORED (Case 2), not read.
run_cluster() {
    printf 'SHOW CLUSTER\nWRITE SYS$OUTPUT "%s"\n' "$MARK" \
        | env OVMX_CLUSTER_STATE_PATH="$STATE" "$DCL" \
              >"$WORK/out" 2>&1
}

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

# --- Case 1: no /dev/vms -> honest NOSUCHDEV, no fabricated member view ------
rm -f "$STATE"
run_cluster
if ! grep -qF "$MARK" "$WORK/out"; then
    fail "DCL did not run to completion (no liveness marker on stdout)" \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
elif grep -q 'NOSUCHDEV' "$WORK/out"; then
    if grep -q 'View of Cluster' "$WORK/out"; then
        fail "SHOW CLUSTER printed NOSUCHDEV AND a member view (fabricated)" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    else
        echo "  OK: with no /dev/vms, SHOW CLUSTER fails honestly (%SYSTEM-W-NOSUCHDEV), no member view"
    fi
else
    fail "SHOW CLUSTER did not report NOSUCHDEV without the executive" \
         "membership is executive-resident (vms-551); absent /dev/vms the honest" \
         "answer is %SYSTEM-W-NOSUCHDEV, never a file-derived member table." \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
fi

# --- Case 2 (ANTI-FACADE): a membership FILE is present, but SHOW CLUSTER must
# IGNORE it and still report NOSUCHDEV -- proving it reads the executive, not the
# file. This is the exact facade vms-551 excised: a populated file must NOT
# resurrect a member view when the executive is absent.
cat > "$STATE" <<'EOF'
version=1
member=VAX3 1027 MEMBER
member=VAX1 1025 MEMBER
EOF
run_cluster
if ! grep -qF "$MARK" "$WORK/out"; then
    fail "DCL did not run to completion in Case 2" \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
elif grep -q 'VAX3' "$WORK/out" || grep -q 'VAX1' "$WORK/out" || grep -q 'View of Cluster' "$WORK/out"; then
    fail "SHOW CLUSTER read the membership FILE (facade not excised)" \
         "a populated cluster_state file must NOT produce a member view; SHOW" \
         "CLUSTER reads the executive (vms-551), and with no /dev/vms the answer" \
         "is NOSUCHDEV. stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
elif grep -q 'NOSUCHDEV' "$WORK/out"; then
    echo "  OK: a populated membership file is IGNORED -- SHOW CLUSTER still reports NOSUCHDEV (reads the executive, not the file)"
else
    fail "SHOW CLUSTER neither showed the file's members nor reported NOSUCHDEV" \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
fi

if [ "$status" -eq 0 ]; then
    echo "vms-551 honest-absence gate: PASS"
else
    echo "vms-551 honest-absence gate: FAIL"
fi
exit $status
