#!/bin/bash
# TEST: R1 config-authoring proof (vms-9cf) -- the cluster IDENTITY params
# SCSNODE, SCSSYSTEMID, ALLOCLASS and RECNXINTERVAL, authored the VMS way
# (SYSMAN PARAMETERS SET + WRITE CURRENT to SYS$SYSTEM:OVMXVMSSYS.PAR), are
# ADOPTED after a reboot: FRESH, separately-linked reader images -- DCL
# (F$GETSYI) and SYSGEN (SHOW) -- both reflect the authored values,
# BRACKETED against a control that shows the defaults. This is rung R1 of
# the cluster config-authoring epic (docs/design-cluster-config-authoring.md)
# -- it DEMONSTRATES the vms-ci.8 param-reading path that already ships, end
# to end through the store->boot->reader adoption loop.
#
# READERS, POST FC-P3.9 (vms-406). The userspace SCS daemon (SCSD.EXE) that
# used to supply this proof's --show-identity read is retired: the cluster
# stack is executive-resident (src/kernel-core/vms_pe.c, vms_scs.c,
# vms_cnxman.c, shipped in vms.ko; see tests/integration/test_no_scsd_image.sh
# for the retirement gate). Gating this test on $BINDIR/SCSD.EXE would make it
# skip forever -- a permanent silent skip, which is a failing test. Its
# replacement here is not a stand-in: F$GETSYI (src/libvms/syssvc/sys_misc.c,
# src/vmsdcl/dcl_lexical.c) and SYSGEN SHOW (tools/vms_sysgen.c) call the
# IDENTICAL sysgen_read_string()/sysgen_read_param() functions
# src/ovmx_init/ovmx_init.c calls at real boot to fill VMS_IOCTL_SYSGEN_LOAD's
# args before VMS_IOCTL_CLUSTER_START -- the same store, the same readers, a
# different (still separately-linked) caller. The full boot-time adoption (a
# real QEMU reboot, the executive's own %OVMX-I-SCSNODE console line,
# F$GETSYI("NODENAME")) is separately proven end-to-end by
# tests/qemu/test_cluster_config_lan_e2e.sh and
# tests/qemu/test_boot_scsnode_hostname_e2e.sh; this host-level gate covers
# the ALLOCLASS/RECNXINTERVAL identity fields and the SYSMAN-authoring path at
# ctest's fast host budget (DCL_TEST_TIMEOUT, no boot involved).
#
# The "reboot" is a fresh process re-reading the persisted, versioned param
# store (WRITE CURRENT mints a new OVMXVMSSYS.PAR version; a fresh reader picks
# up the highest). The value genuinely round-trips through the on-disk store --
# there is no per-process fake (Rule 9 / INV-6): SYSMAN, SYSGEN and DCL are
# separate link images that share ONLY the file.
#
# SCOPE: IDENTITY params only. VOTES / EXPECTED_VOTES / quorum are EXCLUDED --
# that reconciliation is vms-41d, owned by the cluster-wire session, and the
# executive's deliberate VOTES=0 non-voting join is untouched here. This proof
# neither sets VOTES nor asserts anything about VOTES adoption.
#
# Grounded (Rule 8): param NAMES + the F$GETSYI/SYSMAN/SYSGEN surfaces are from
# public OpenVMS docs; OVMXVMSSYS.PAR's byte layout is an OVMX-labeled invention
# (sysgen_params.h). ALLOCLASS defaults to 0 ("no allocation class").
# EXPECT: regex:(CLUSTER_IDENTITY_ADOPT_OK|CLUSTER_IDENTITY_ADOPT_SKIPPED)
# EXPECT_NOT: contains:CLUSTER_IDENTITY_ADOPT_FAIL
# EXPECT_NOT: contains:Segmentation

VMSDCL="${VMSDCL:-vmsdcl}"
BINDIR="$(dirname "$VMSDCL")"
SYSMAN="${SYSMAN:-$BINDIR/SYSMAN.EXE}"
SYSGEN="${SYSGEN:-$BINDIR/SYSGEN.EXE}"

if [ ! -x "$SYSMAN" ] || [ ! -x "$SYSGEN" ] || ! command -v "$VMSDCL" >/dev/null 2>&1; then
    echo "CLUSTER_IDENTITY_ADOPT_SKIPPED: need DCL + SYSMAN.EXE + SYSGEN.EXE"
    echo "  next to VMSDCL (BUILD_TOOLS=ON builds them into the same bin/). If any"
    echo "  is genuinely absent this is an honest skip, not a fabricated pass."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
# Point every tool at one private, versioned param store (no /vms mount needed);
# the readers honor OVMX_SYSGEN_PATH exactly as F$GETSYI/SYSGEN do on a real
# boot, where PID 1 exports the same env to every image.
export OVMX_SYSGEN_PATH="$TMPDIR/OVMXVMSSYS.PAR"

FAILURES=0
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# The authored (non-default) identity, chosen distinct from every default so the
# bracket is unambiguous: default SCSNODE OVMX, SCSSYSTEMID 0, ALLOCLASS 0.
AUTH_NODE="CLSTX"
AUTH_SID="1042"
AUTH_ALLOC="7"
# vms-c3b: RECNXINTERVAL is now an AUTHORED SYSGEN param, read back on the
# same sysgen_read_param() path SCSNODE/SCSSYSTEMID/ALLOCLASS use. Authored
# value distinct from the default (20).
AUTH_RECNX="30"

# Read helpers ---------------------------------------------------------------
# F$GETSYI via SHOW SYMBOL (the proven pattern from test_lexical_scsnode.sh).
getsyi() {   # $1 = item name -> echoes the value
    printf 'X = F$GETSYI("%s")\nSHOW SYMBOL X\n' "$1" \
        | "$VMSDCL" 2>/dev/null \
        | sed -n 's/^ *X = "\{0,1\}\([^" ]*\).*/\1/p' | head -1
}
# SYSGEN's own reader (a THIRD separately-linked image, distinct from SYSMAN
# and DCL) reads the identical store back with a generic SHOW <parameter> --
# RECNXINTERVAL has no F$GETSYI item, so this is its adoption-proof reader.
sysgen_show() {   # $1 = param name -> echoes the Current column
    printf 'USE %s\nSHOW %s\nEXIT\n' "$OVMX_SYSGEN_PATH" "$1" | "$SYSGEN" 2>&1 \
        | sed -n "s/^  $1[[:space:]]\+\([0-9][0-9]*\).*/\1/p" | head -1
}

# --- CONTROL: factory defaults, no authored identity -----------------------
printf 'USE DEFAULT\nWRITE %s\nEXIT\n' "$OVMX_SYSGEN_PATH" | "$SYSGEN" >/dev/null 2>&1

c_node=$(getsyi SCSNODE)
c_sid=$(getsyi SCSSYSTEMID)
c_alloc=$(getsyi ALLOCLASS)
c_recnx=$(sysgen_show RECNXINTERVAL)
echo "control: F\$GETSYI SCSNODE=$c_node SCSSYSTEMID=$c_sid ALLOCLASS=$c_alloc"
echo "control: SYSGEN SHOW RECNXINTERVAL=$c_recnx"

[ "$c_node" = "OVMX" ] || fail "control SCSNODE was '$c_node', expected the default OVMX"
[ "$c_sid" = "0" ]     || fail "control SCSSYSTEMID was '$c_sid', expected the default 0"
[ "$c_alloc" = "0" ]   || fail "control ALLOCLASS was '$c_alloc', expected the default 0"
# vms-c3b: on the unauthored store SYSGEN reports the documented default
# RECNXINTERVAL=20 (OpenVMS System Management Utilities Reference Manual).
[ "$c_recnx" = "20" ]  || fail "control SYSGEN SHOW RECNXINTERVAL was '$c_recnx', expected the documented default 20"

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

# --- REBOOT: fresh DCL + fresh SYSGEN adopt the authored identity ----------
r_node=$(getsyi SCSNODE)
r_sid=$(getsyi SCSSYSTEMID)
r_alloc=$(getsyi ALLOCLASS)
r_recnx=$(sysgen_show RECNXINTERVAL)
echo "reboot:  F\$GETSYI SCSNODE=$r_node SCSSYSTEMID=$r_sid ALLOCLASS=$r_alloc"
echo "reboot:  SYSGEN SHOW RECNXINTERVAL=$r_recnx"

# F$GETSYI (the DCL reader surface) reflects the authored identity.
[ "$r_node" = "$AUTH_NODE" ]   || fail "F\$GETSYI SCSNODE did not adopt authored $AUTH_NODE (got '$r_node')"
[ "$r_sid" = "$AUTH_SID" ]     || fail "F\$GETSYI SCSSYSTEMID did not adopt authored $AUTH_SID (got '$r_sid')"
[ "$r_alloc" = "$AUTH_ALLOC" ] || fail "F\$GETSYI ALLOCLASS did not adopt authored $AUTH_ALLOC (got '$r_alloc')"

# SYSGEN (a separate link image, the same reader path RECNXINTERVAL has since
# it carries no F$GETSYI item) reflects the authored RECNXINTERVAL.
[ "$r_recnx" = "$AUTH_RECNX" ] \
    || fail "SYSGEN SHOW RECNXINTERVAL did not adopt authored $AUTH_RECNX (got '$r_recnx')"

# --- BRACKET: authored values are genuinely DIFFERENT from the control -----
[ "$r_node" != "$c_node" ]   || fail "SCSNODE did not change from the control default (bracket failed)"
[ "$r_sid" != "$c_sid" ]     || fail "SCSSYSTEMID did not change from the control default (bracket failed)"
[ "$r_alloc" != "$c_alloc" ] || fail "ALLOCLASS did not change from the control default (bracket failed)"
# RECNXINTERVAL bracket: authored 30 is genuinely different from the default 20.
[ "$c_recnx" = "20" ] && [ "$r_recnx" = "$AUTH_RECNX" ] \
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
