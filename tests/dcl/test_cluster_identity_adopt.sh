#!/bin/bash
# TEST: R1 config-authoring proof (vms-9cf) -- the cluster IDENTITY params
# SCSNODE, SCSSYSTEMID and ALLOCLASS, authored the VMS way (SYSMAN PARAMETERS
# SET + WRITE CURRENT to SYS$SYSTEM:OVMXVMSSYS.PAR), are ADOPTED after a reboot:
# a FRESH SCSD (--show-identity) and a FRESH DCL (F$GETSYI) both reflect the
# authored values, BRACKETED against a control that shows the defaults. This is
# rung R1 of the cluster config-authoring epic (docs/design-cluster-config-
# authoring.md) -- it DEMONSTRATES the vms-ci.8 param-reading path that already
# ships, end to end through the store->boot->reader adoption loop.
#
# The "reboot" is a fresh process re-reading the persisted, versioned param
# store (WRITE CURRENT mints a new OVMXVMSSYS.PAR version; a fresh reader picks
# up the highest). The value genuinely round-trips through the on-disk store --
# there is no per-process fake (Rule 9 / INV-6): SCSD and DCL are separate link
# images that share ONLY the file.
#
# SCOPE: IDENTITY params only. VOTES / EXPECTED_VOTES / quorum are EXCLUDED --
# that reconciliation is vms-41d, owned by the cluster-wire session, and SCSD's
# deliberate VOTES=0 non-voting join is untouched here. This proof neither sets
# VOTES nor asserts anything about VOTES adoption.
#
# Grounded (Rule 8): param NAMES + the F$GETSYI/SYSMAN surfaces are from public
# OpenVMS docs; OVMXVMSSYS.PAR's byte layout is an OVMX-labeled invention
# (sysgen_params.h). ALLOCLASS defaults to 0 ("no allocation class").
# EXPECT: regex:(CLUSTER_IDENTITY_ADOPT_OK|CLUSTER_IDENTITY_ADOPT_SKIPPED)
# EXPECT_NOT: contains:CLUSTER_IDENTITY_ADOPT_FAIL
# EXPECT_NOT: contains:Segmentation

VMSDCL="${VMSDCL:-vmsdcl}"
BINDIR="$(dirname "$VMSDCL")"
SYSMAN="${SYSMAN:-$BINDIR/SYSMAN.EXE}"
SYSGEN="${SYSGEN:-$BINDIR/SYSGEN.EXE}"
SCSD="${SCSD:-$BINDIR/SCSD.EXE}"

if [ ! -x "$SYSMAN" ] || [ ! -x "$SYSGEN" ] || [ ! -x "$SCSD" ] || ! command -v "$VMSDCL" >/dev/null 2>&1; then
    echo "CLUSTER_IDENTITY_ADOPT_SKIPPED: need DCL + SYSMAN.EXE + SYSGEN.EXE + SCSD.EXE"
    echo "  next to VMSDCL (BUILD_TOOLS=ON builds them into the same bin/). If any"
    echo "  is genuinely absent this is an honest skip, not a fabricated pass."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
# Point every tool at one private, versioned param store (no /vms mount needed);
# the readers honor OVMX_SYSGEN_PATH exactly as SCSD/F$GETSYI do on a real boot,
# where PID 1 exports the same env to both images.
export OVMX_SYSGEN_PATH="$TMPDIR/OVMXVMSSYS.PAR"

FAILURES=0
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# The authored (non-default) identity, chosen distinct from every default so the
# bracket is unambiguous: default SCSNODE OVMX, F$GETSYI SCSSYSTEMID 0, SCSD
# fallback SCSSYSTEMID 1030, ALLOCLASS 0.
AUTH_NODE="CLSTX"
AUTH_SID="1042"
AUTH_ALLOC="7"
# vms-c3b: RECNXINTERVAL is now an AUTHORED SYSGEN param SCSD adopts on the same
# --show-identity read path. Authored value distinct from the default (20).
AUTH_RECNX="30"

# Read helpers ---------------------------------------------------------------
# F$GETSYI via SHOW SYMBOL (the proven pattern from test_lexical_scsnode.sh).
getsyi() {   # $1 = item name -> echoes the value
    printf 'X = F$GETSYI("%s")\nSHOW SYMBOL X\n' "$1" \
        | "$VMSDCL" 2>/dev/null \
        | sed -n 's/^ *X = "\{0,1\}\([^" ]*\).*/\1/p' | head -1
}
# SCSD's own adopted identity (opens NO socket; pure store read).
scsd_ident() { "$SCSD" --show-identity 2>/dev/null | grep '^SCSD-I-IDENT'; }

# --- CONTROL: factory defaults, no authored identity -----------------------
printf 'USE DEFAULT\nWRITE %s\nEXIT\n' "$OVMX_SYSGEN_PATH" | "$SYSGEN" >/dev/null 2>&1

c_node=$(getsyi SCSNODE)
c_sid=$(getsyi SCSSYSTEMID)
c_alloc=$(getsyi ALLOCLASS)
c_scsd=$(scsd_ident)
echo "control: F\$GETSYI SCSNODE=$c_node SCSSYSTEMID=$c_sid ALLOCLASS=$c_alloc"
echo "control: $c_scsd"

[ "$c_node" = "OVMX" ] || fail "control SCSNODE was '$c_node', expected the default OVMX"
[ "$c_sid" = "0" ]     || fail "control SCSSYSTEMID was '$c_sid', expected the default 0"
[ "$c_alloc" = "0" ]   || fail "control ALLOCLASS was '$c_alloc', expected the default 0"
echo "$c_scsd" | grep -q 'ALLOCLASS=0' \
    || fail "control SCSD --show-identity did not report the default ALLOCLASS=0"
# vms-c3b: on the unauthored store SCSD reports the documented default
# RECNXINTERVAL=20 (OpenVMS System Management Utilities Reference Manual).
echo "$c_scsd" | grep -q 'RECNXINTERVAL=20' \
    || fail "control SCSD --show-identity did not report the default RECNXINTERVAL=20"

# --- AUTHOR the identity the VMS way, then WRITE CURRENT --------------------
setout=$(printf 'PARAMETERS SET SCSNODE %s\nPARAMETERS SET SCSSYSTEMID %s\nPARAMETERS SET ALLOCLASS %s\nPARAMETERS SET RECNXINTERVAL %s\nPARAMETERS WRITE CURRENT\nEXIT\n' \
    "$AUTH_NODE" "$AUTH_SID" "$AUTH_ALLOC" "$AUTH_RECNX" | "$SYSMAN" 2>&1)
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, SCSNODE changed from OVMX to ${AUTH_NODE}" \
    || fail "SET SCSNODE did not report the real change"
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, SCSSYSTEMID changed from 0 to ${AUTH_SID}" \
    || fail "SET SCSSYSTEMID did not report the real change"
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, ALLOCLASS changed from 0 to ${AUTH_ALLOC}" \
    || fail "SET ALLOCLASS did not report the real change"
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, RECNXINTERVAL changed from 20 to ${AUTH_RECNX}" \
    || fail "SET RECNXINTERVAL did not report the real change"

# --- REBOOT: fresh SCSD + fresh DCL adopt the authored identity ------------
r_node=$(getsyi SCSNODE)
r_sid=$(getsyi SCSSYSTEMID)
r_alloc=$(getsyi ALLOCLASS)
r_scsd=$(scsd_ident)
echo "reboot:  F\$GETSYI SCSNODE=$r_node SCSSYSTEMID=$r_sid ALLOCLASS=$r_alloc"
echo "reboot:  $r_scsd"

# F$GETSYI (the DCL reader surface) reflects the authored identity.
[ "$r_node" = "$AUTH_NODE" ]   || fail "F\$GETSYI SCSNODE did not adopt authored $AUTH_NODE (got '$r_node')"
[ "$r_sid" = "$AUTH_SID" ]     || fail "F\$GETSYI SCSSYSTEMID did not adopt authored $AUTH_SID (got '$r_sid')"
[ "$r_alloc" = "$AUTH_ALLOC" ] || fail "F\$GETSYI ALLOCLASS did not adopt authored $AUTH_ALLOC (got '$r_alloc')"

# SCSD (the daemon's own identity resolver) reflects the authored identity
# AND the authored RECNXINTERVAL, both on the same --show-identity read path.
echo "$r_scsd" | grep -q "SCSNODE=${AUTH_NODE} SCSSYSTEMID=${AUTH_SID} ALLOCLASS=${AUTH_ALLOC} RECNXINTERVAL=${AUTH_RECNX}" \
    || fail "SCSD --show-identity did not adopt the authored identity + RECNXINTERVAL"

# --- BRACKET: authored values are genuinely DIFFERENT from the control -----
[ "$r_node" != "$c_node" ]   || fail "SCSNODE did not change from the control default (bracket failed)"
[ "$r_sid" != "$c_sid" ]     || fail "SCSSYSTEMID did not change from the control default (bracket failed)"
[ "$r_alloc" != "$c_alloc" ] || fail "ALLOCLASS did not change from the control default (bracket failed)"
# RECNXINTERVAL bracket: authored 30 is genuinely different from the default 20.
echo "$c_scsd" | grep -q 'RECNXINTERVAL=20' && echo "$r_scsd" | grep -q "RECNXINTERVAL=${AUTH_RECNX}" \
    || fail "RECNXINTERVAL did not change from the control default 20 to authored ${AUTH_RECNX} (bracket failed)"

# --- Shared store: SYSGEN sees exactly what SYSMAN wrote (not a private copy)
sgview=$(printf 'USE %s\nSHOW ALLOCLASS\nEXIT\n' "$OVMX_SYSGEN_PATH" | "$SYSGEN" 2>&1)
echo "$sgview" | grep -qE "^  ALLOCLASS +${AUTH_ALLOC} " \
    || fail "SYSGEN did not see the ALLOCLASS SYSMAN wrote (stores diverged)"

if [ $FAILURES -eq 0 ]; then
    echo "CLUSTER_IDENTITY_ADOPT_OK"
else
    echo "CLUSTER_IDENTITY_ADOPT_FAIL ($FAILURES check(s) failed)"
fi
