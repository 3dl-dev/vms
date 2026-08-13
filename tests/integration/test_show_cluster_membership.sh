#!/bin/sh
#
# test_show_cluster_membership.sh - BEHAVIOURAL gate (vms-8d4, INV-DCL): DCL
# SHOW CLUSTER reflects the SCS daemon's REAL membership, and prints
# %SYSTEM-I-NOTMEMBER only when this node is genuinely not a member.
#
# WHY THIS EXISTS. cmd_show_cluster() used to hardcode "%SYSTEM-I-NOTMEMBER"
# unconditionally -- so a genuinely-clustered node lied about being standalone,
# even though src/vmsscs/ implements the full SCS stack. The fix wires SHOW
# CLUSTER to the connection manager's live member set, which SCSD publishes to a
# well-known file (src/vmsscs/include/scs_membership.h). That file IS the real
# boundary between the daemon and the DCL process; this gate drives the REAL
# cmd_show_cluster against a membership file written in the SAME format SCSD
# writes -- it does NOT mock the function under test.
#
#   - No published membership (daemon down / not joined) -> NOTMEMBER, and
#     nothing else. This is the honest standalone answer.
#   - A published two-node member set -> the member NODES and STATUS are shown,
#     and NOTMEMBER must NOT appear.
#   - An unknown-name peer (SCSD could not learn its SCSNODE) is identified by
#     its SCSSYSTEMID, never dropped.
#
# The membership FILE FORMAT is an OVMX invention (Rule 8); the SHOW CLUSTER
# DISPLAY fields (NODE/SOFTWARE/STATUS banner) are modeled on the public
# OpenVMS SHOW CLUSTER utility. Full fidelity against a live 2-node cluster is
# verified on lab-2 (see the note at the end); this gate proves the wiring and
# the honest standalone/member split without needing a cluster.
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

echo "vms-8d4 behavioural gate: SHOW CLUSTER reflects real SCS membership"

if [ ! -x "$DCL" ]; then
    echo "FAIL: DCL.EXE not found (looked at '$DCL')"
    echo "  -> this gate must RUN the command; it cannot be evaluated by reading"
    exit 1
fi

# run SHOW CLUSTER with the membership file resolved to $STATE (absent or
# present per the case). A liveness marker follows so we can tell "empty
# output" from "DCL never ran".
run_cluster() {
    printf 'SHOW CLUSTER\nWRITE SYS$OUTPUT "%s"\n' "$MARK" \
        | env OVMX_CLUSTER_STATE_PATH="$STATE" "$DCL" \
              >"$WORK/out" 2>"$WORK/err"
}

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

# --- Case A: no published membership -> NOTMEMBER (genuinely standalone) ----
rm -f "$STATE"
run_cluster
if ! grep -qF "$MARK" "$WORK/out"; then
    fail "DCL did not run to completion (no liveness marker on stdout)" \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
elif grep -q 'NOTMEMBER, this system is not a member of a VMScluster' "$WORK/out"; then
    if grep -q 'View of Cluster' "$WORK/out"; then
        fail "standalone SHOW CLUSTER printed NOTMEMBER AND a member view" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    else
        echo "  OK: with no published membership, SHOW CLUSTER reports NOTMEMBER"
    fi
else
    fail "standalone SHOW CLUSTER did not report NOTMEMBER" \
         "with no membership file this node is not a member; NOTMEMBER is the" \
         "honest answer. stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
fi

# --- Case B: a live two-node member set -> members shown, NO NOTMEMBER ------
# Written in SCSD's own publication format (scs_membership.h): the local node
# leads, then peers. This is the exact file SCSD's scsd_publish_membership()
# produces on a real join.
cat > "$STATE" <<'EOF'
version=1
member=VAX3 1027 MEMBER
member=VAX1 1025 MEMBER
EOF
run_cluster
if grep -q 'NOTMEMBER' "$WORK/out"; then
    fail "clustered SHOW CLUSTER still reported NOTMEMBER" \
         "a two-node member set is published; NOTMEMBER is a lie here." \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
else
    ok=1
    grep -q 'View of Cluster from system ID 1027 node: VAX3' "$WORK/out" || {
        ok=0
        fail "clustered SHOW CLUSTER did not name the local node in the banner" \
             "expected 'View of Cluster from system ID 1027 node: VAX3'." \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    }
    grep -q 'VAX3' "$WORK/out" && grep -q 'VAX1' "$WORK/out" || {
        ok=0
        fail "clustered SHOW CLUSTER did not list both member nodes" \
             "expected VAX3 and VAX1 rows. stdout was:" \
             "$(sed 's/^/       | /' "$WORK/out")"
    }
    grep -q 'MEMBER' "$WORK/out" || {
        ok=0
        fail "clustered SHOW CLUSTER showed no MEMBER status column" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    }
    [ "$ok" -eq 1 ] && echo "  OK: a published two-node member set is rendered, no NOTMEMBER"
fi

# --- Case C: an unknown-name peer is identified by its SCSSYSTEMID ----------
# SCSD writes "?" for a peer whose SCSNODE it has not learned; the row must
# still appear, keyed on the SCSSYSTEMID, never silently dropped.
cat > "$STATE" <<'EOF'
version=1
member=VAX3 1027 MEMBER
member=? 1026 MEMBER
EOF
run_cluster
if grep -q 'NOTMEMBER' "$WORK/out"; then
    fail "SHOW CLUSTER reported NOTMEMBER with an unknown-name peer present" \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
elif grep -q '1026' "$WORK/out"; then
    echo "  OK: an unknown-name peer is shown by its SCSSYSTEMID (1026)"
else
    fail "SHOW CLUSTER dropped the unknown-name peer" \
         "expected its SCSSYSTEMID 1026 in the output. stdout was:" \
         "$(sed 's/^/       | /' "$WORK/out")"
fi

# --- Case D: a file with no member lines -> NOTMEMBER ----------------------
printf 'version=1\n' > "$STATE"
run_cluster
if grep -q 'NOTMEMBER' "$WORK/out" && ! grep -q 'View of Cluster' "$WORK/out"; then
    echo "  OK: a membership file with zero members reports NOTMEMBER"
else
    fail "SHOW CLUSTER did not report NOTMEMBER for an empty member set" \
         "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
fi

if [ "$status" -eq 0 ]; then
    echo "vms-8d4 behavioural gate: PASS"
else
    echo "vms-8d4 behavioural gate: FAIL"
fi
exit $status
