#!/bin/bash
# TEST: SYSMAN SET ENVIRONMENT / DO honestly reflect that OVMX has NO cluster
# command transport (no SMISERVER) -- this excises the two vms-495 fabrication
# LARPs (INV-6). Before this fix, SET ENVIRONMENT always printed "Environment
# set to local node OVMX" (never parsing /NODE= or /CLUSTER, never reaching any
# node), and DO printed "command execution on node OVMX" + DONEALL while forking
# a purely LOCAL DCL -- faking per-node cluster execution.
#
# Genuine OpenVMS behavior (VSI OpenVMS System Management Utilities Reference
# Manual, SET ENVIRONMENT / DO): a LOCAL environment executes DO on the node
# SYSMAN runs on; a /NODE=<remote> or /CLUSTER environment connects to the
# SMISERVER on each target node. OVMX has no SMISERVER, so a remote request must
# FAIL HONEST -- never fake success (the SMISERVER concept is public; the exact
# error mnemonic is an honest OVMX message, not a fabricated VMS-authentic one).
#
# EXPECT: regex:(SYSMAN_ENV_OK|SYSMAN_ENV_SKIPPED)
# EXPECT_NOT: contains:SYSMAN_ENV_FAIL
# EXPECT_NOT: contains:Segmentation
VMSDCL="${VMSDCL:-vmsdcl}"
SYSMAN="${SYSMAN:-$(dirname "$VMSDCL")/SYSMAN.EXE}"
SYSGEN="${SYSGEN:-$(dirname "$VMSDCL")/SYSGEN.EXE}"

if [ ! -x "$SYSMAN" ] || [ ! -x "$SYSGEN" ]; then
    echo "SYSMAN_ENV_SKIPPED: SYSMAN.EXE/SYSGEN.EXE not found next to VMSDCL"
    echo "  (BUILD_TOOLS=ON builds them into the same bin/ as VMSDCL; if they"
    echo "  are genuinely absent this is an honest skip, not a fabricated pass)."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
# Private, unversioned literal parameter store (same override the other SYSMAN
# tests use) so the local node name is deterministic.
export OVMX_SYSGEN_PATH="$TMPDIR/OVMXVMSSYS.PAR"

NODE="OVMXT1"
# Seed SCSNODE so ovmx_node_name() (which SYSMAN now reads) yields a KNOWN,
# distinctive local node name -- proving DO reports the REAL node, not a
# hardcoded "OVMX".
printf 'USE DEFAULT\nSET SCSNODE %s\nWRITE %s\nEXIT\n' "$NODE" "$OVMX_SYSGEN_PATH" \
    | "$SYSGEN" >/dev/null 2>&1

FAILURES=0

# --- (a) SET ENVIRONMENT/NODE=<remote> FAILS honest ------------------------
out=$(printf 'SET ENVIRONMENT/NODE=REMOTE1\nEXIT\n' | "$SYSMAN" 2>&1)
echo "--- (a) SET ENVIRONMENT/NODE=REMOTE1 ---"
echo "$out"
if echo "$out" | grep -qi "Environment set"; then
    echo "  FAIL: still emits the old fabricated 'Environment set' success"
    FAILURES=$((FAILURES + 1))
fi
if ! echo "$out" | grep -q "%SYSMAN-E-"; then
    echo "  FAIL: remote SET ENVIRONMENT did not fail with a %SYSMAN-E- error"
    FAILURES=$((FAILURES + 1))
fi
if ! echo "$out" | grep -qiE "SMISERVER|transport"; then
    echo "  FAIL: error does not state the real reason (no SMISERVER/transport)"
    FAILURES=$((FAILURES + 1))
fi

# --- (b) DO while a remote environment is set FAILS honest -----------------
out=$(printf 'SET ENVIRONMENT/NODE=REMOTE1\nDO SHOW TIME\nEXIT\n' | "$SYSMAN" 2>&1)
echo "--- (b) SET ENV remote + DO SHOW TIME ---"
echo "$out"
if echo "$out" | grep -q "%SYSMAN-I-OUTPUT"; then
    echo "  FAIL: DO on a remote env printed OUTPUT (forked local DCL, faked reach)"
    FAILURES=$((FAILURES + 1))
fi
if echo "$out" | grep -q "%SYSMAN-I-DONEALL"; then
    echo "  FAIL: DO reported DONEALL for an unserviceable remote environment"
    FAILURES=$((FAILURES + 1))
fi
if ! echo "$out" | grep -q "cannot execute command"; then
    echo "  FAIL: DO did not fail honest with an execute-command transport error"
    FAILURES=$((FAILURES + 1))
fi

# --- (c) local SET ENVIRONMENT + local DO work and show the REAL node ------
out=$(printf 'SET ENVIRONMENT\nDO SHOW TIME\nEXIT\n' | "$SYSMAN" 2>&1)
echo "--- (c) local SET ENV + DO SHOW TIME ---"
echo "$out"
if ! echo "$out" | grep -q "local node $NODE"; then
    echo "  FAIL: local SET ENVIRONMENT did not report the real node $NODE"
    FAILURES=$((FAILURES + 1))
fi
if ! echo "$out" | grep -q "command execution on node $NODE"; then
    echo "  FAIL: local DO did not report the real node $NODE"
    FAILURES=$((FAILURES + 1))
fi
if echo "$out" | grep -qE "execution on node OVMX$"; then
    echo "  FAIL: local DO still shows the old hardcoded 'node OVMX'"
    FAILURES=$((FAILURES + 1))
fi

if [ "$FAILURES" -eq 0 ]; then
    echo "SYSMAN_ENV_OK"
else
    echo "SYSMAN_ENV_FAIL: $FAILURES assertion(s) failed"
fi
