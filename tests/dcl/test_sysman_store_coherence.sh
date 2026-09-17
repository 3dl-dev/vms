#!/bin/bash
# TEST: R1.2 store-coherence proof (vms-7c3) -- SYSMAN PARAMETERS SET/WRITE and
# the executive's own readers target ONE canonical parameter store, so a value
# an operator authors with SYSMAN is exactly what F$GETSYI/SYSGEN (the
# executive's own identity resolvers) and SYSGEN read back. This LOCKS the
# store-coherence fix: SYSMAN used to write a DIVERGENT SYS$MANAGER:
# SYSPARAMS.DAT that nothing in the executive ever read, while F$GETSYI/SYSGEN
# read SYS$SYSTEM:OVMXVMSSYS.PAR -- so SET SCSNODE could never reach the
# node-identity resolver. SYSMAN is now converged onto the SAME store
# (sysgen_params.h: sysgen_commit_working -> OVMXVMSSYS.PAR; the readers
# sysgen_read_string / sysgen_read_param resolve the identical path).
#
# READERS, POST FC-P3.9 (vms-406). This test used to drive its identity-read
# leg through SCSD.EXE, the userspace SCS daemon -- retired along with the
# whole userspace SCS stack (the cluster port, SCS and the connection manager
# are now executive-resident: src/kernel-core/vms_pe.c, vms_scs.c,
# vms_cnxman.c, shipped in vms.ko; see tests/integration/test_no_scsd_image.sh
# for the retirement gate). Gating on $BINDIR/SCSD.EXE, which nothing builds
# any more, made this test skip forever -- a permanent silent skip, which is a
# failing test. Its replacement is not a stand-in: F$GETSYI
# (src/libvms/syssvc/sys_misc.c, src/vmsdcl/dcl_lexical.c) calls the IDENTICAL
# sysgen_read_string()/sysgen_read_param() functions SCSD's identity resolver
# used to, and that src/ovmx_init/ovmx_init.c calls at real boot to fill
# VMS_IOCTL_SYSGEN_LOAD's args -- the same store, the same readers, a
# different (still separately-linked) caller.
#
# The proof runs REAL, SEPARATELY-LINKED images (SYSMAN.EXE, SYSGEN.EXE,
# DCL.EXE) that share ONLY the on-disk store -- there is no per-process fake
# (Rule 9 / INV-6). It covers BOTH value types:
#   - STRING  SCSNODE  : SET via SYSMAN -> read by F$GETSYI("SCSNODE")
#                        (sysgen_read_string), echoed by SYSMAN SHOW.
#   - NUMERIC SCSSYSTEMID / ALLOCLASS : SET via SYSMAN -> read by
#                        F$GETSYI("SCSSYSTEMID") / F$GETSYI("ALLOCLASS"),
#                        which call sysgen_read_param. VOTES round-trips
#                        through the shared store and is read back by SYSGEN
#                        SHOW (which loads the same OVMXVMSSYS.PAR the
#                        sysgen_read_param readers do).
#
# MEASURED NEGATIVE CONTROL (the lock): a DIFFERENT SCSNODE written to the OLD
# divergent path (SYSPARAMS.DAT) is proven present there (SYSMAN USE <old> SHOW
# echoes it) yet is NOT what F$GETSYI reads -- F$GETSYI still returns the
# canonical value. If the coherence fix regressed (SYSMAN writing, or the
# executive readers reading, the old path), F$GETSYI would report the
# divergent value and this test would FAIL.
#
# SCOPE: store coherence + the SYSMAN string/numeric SET surface. It does not
# touch the SCS wire or VOTES quorum adoption (vms-694 / vms-41d); VOTES here is
# only exercised as a numeric store round-trip.
#
# Grounded (Rule 8): param NAMES + the SYSMAN/SYSGEN/F$GETSYI surfaces are from
# public OpenVMS docs; OVMXVMSSYS.PAR's byte layout is an OVMX-labeled invention
# (sysgen_params.h). SYSPARAMS.DAT is the pre-fix divergent target this test
# proves dead.
# EXPECT: regex:(SYSMAN_STORE_COHERENCE_OK|SYSMAN_STORE_COHERENCE_SKIPPED)
# EXPECT_NOT: contains:SYSMAN_STORE_COHERENCE_FAIL
# EXPECT_NOT: contains:Segmentation

VMSDCL="${VMSDCL:-vmsdcl}"
BINDIR="$(dirname "$VMSDCL")"
SYSMAN="${SYSMAN:-$BINDIR/SYSMAN.EXE}"
SYSGEN="${SYSGEN:-$BINDIR/SYSGEN.EXE}"

if [ ! -x "$SYSMAN" ] || [ ! -x "$SYSGEN" ] || ! command -v "$VMSDCL" >/dev/null 2>&1; then
    echo "SYSMAN_STORE_COHERENCE_SKIPPED: need DCL + SYSMAN.EXE + SYSGEN.EXE"
    echo "  next to VMSDCL (BUILD_TOOLS=ON builds them into the same bin/). If any"
    echo "  is genuinely absent this is an honest skip, not a fabricated pass."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# One private, versioned canonical store -- every reader/writer honors
# OVMX_SYSGEN_PATH exactly as they do on a real boot, where PID 1 exports the
# same env to SYSMAN, SYSGEN and DCL. This IS the SYS$SYSTEM:OVMXVMSSYS.PAR
# store in production.
CANON="$TMPDIR/OVMXVMSSYS.PAR"
export OVMX_SYSGEN_PATH="$CANON"

# The pre-fix divergent target. In the bug, SYSMAN wrote SYS$MANAGER:SYSPARAMS.DAT
# and the executive never read it. Nothing consults it now, so a value planted
# here must be invisible to F$GETSYI. (A literal stand-in for VMS_MANAGER_DIR/
# SYSPARAMS.DAT; the discriminating measurement is "F$GETSYI ignores this file".)
OLDPATH="$TMPDIR/SYSPARAMS.DAT"

FAILURES=0
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# The authored identity -- chosen distinct from every default so a stale/default
# read cannot masquerade as a pass. SCSNODE max 6 chars (SYSGEN_STRVAL_LEN 8).
AUTH_NODE="NODEB"
AUTH_SID="1042"
AUTH_ALLOC="7"
AUTH_VOTES="5"
# The divergent value planted at the OLD path -- must never reach F$GETSYI.
BAD_NODE="BADXYZ"

# F$GETSYI via SHOW SYMBOL -- a separate link image from SYSMAN/SYSGEN, the
# SAME reader surface (sysgen_read_string/sysgen_read_param) the retired SCSD
# used, and the one src/ovmx_init/ovmx_init.c calls at real boot.
getsyi() {   # $1 = item name -> echoes the value
    printf 'X = F$GETSYI("%s")\nSHOW SYMBOL X\n' "$1" \
        | "$VMSDCL" 2>/dev/null \
        | sed -n 's/^ *X = "\{0,1\}\([^" ]*\).*/\1/p' | head -1
}

# --- Seed the canonical store with SYSGEN factory defaults ------------------
printf 'USE DEFAULT\nWRITE %s\nEXIT\n' "$CANON" | "$SYSGEN" >/dev/null 2>&1
[ -f "$CANON" ] || fail "SYSGEN did not create the canonical store $CANON"

# --- Author identity the VMS way through SYSMAN, then WRITE CURRENT ----------
setout=$(printf 'PARAMETERS SET SCSNODE %s\nPARAMETERS SET SCSSYSTEMID %s\nPARAMETERS SET ALLOCLASS %s\nPARAMETERS SET VOTES %s\nPARAMETERS WRITE CURRENT\nEXIT\n' \
    "$AUTH_NODE" "$AUTH_SID" "$AUTH_ALLOC" "$AUTH_VOTES" | "$SYSMAN" 2>&1)
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, SCSNODE changed from OVMX to ${AUTH_NODE}" \
    || fail "SYSMAN SET SCSNODE (string path) did not report the real change"
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, SCSSYSTEMID changed from 0 to ${AUTH_SID}" \
    || fail "SYSMAN SET SCSSYSTEMID (numeric) did not report the real change"
echo "$setout" | grep -q "%SYSMAN-I-SETPARAM, VOTES changed from 1 to ${AUTH_VOTES}" \
    || fail "SYSMAN SET VOTES (numeric) did not report the real change"
echo "$setout" | grep -q "%SYSMAN-I-WRITTEN," \
    || fail "SYSMAN WRITE CURRENT did not report a write"

# --- SYSMAN SHOW echoes the string value it just set (type-aware SHOW) -------
showout=$(printf 'PARAMETERS SHOW SCSNODE\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$showout" | grep -qE "^  SCSNODE +\"${AUTH_NODE} *\"" \
    || fail "SYSMAN SHOW SCSNODE did not echo the authored \"${AUTH_NODE}\""

# --- F$GETSYI reads the authored identity from the IDENTICAL store ---------------
# STRING SCSNODE via sysgen_read_string; NUMERIC SCSSYSTEMID/ALLOCLASS via
# sysgen_read_param. This is the coherence claim: SYSMAN wrote it, the
# executive's own reader path (F$GETSYI) reads it.
g_node=$(getsyi SCSNODE)
g_sid=$(getsyi SCSSYSTEMID)
g_alloc=$(getsyi ALLOCLASS)
echo "authored: F\$GETSYI SCSNODE=$g_node SCSSYSTEMID=$g_sid ALLOCLASS=$g_alloc"
[ "$g_node" = "$AUTH_NODE" ]   || fail "F\$GETSYI did not read the SCSNODE SYSMAN wrote (stores diverged): got '$g_node'"
[ "$g_sid" = "$AUTH_SID" ]     || fail "F\$GETSYI did not read the SCSSYSTEMID SYSMAN wrote (stores diverged): got '$g_sid'"
[ "$g_alloc" = "$AUTH_ALLOC" ] || fail "F\$GETSYI did not read the ALLOCLASS SYSMAN wrote (stores diverged): got '$g_alloc'"

# --- NUMERIC round-trip: SYSGEN SHOW reads VOTES back from the shared store --
# SYSGEN loads the same OVMXVMSSYS.PAR the sysgen_read_param readers resolve, so
# this proves the numeric value SYSMAN wrote is coherent for the executive.
votesout=$(printf 'USE %s\nSHOW VOTES\nEXIT\n' "$CANON" | "$SYSGEN" 2>&1)
echo "$votesout" | grep -qE "^  VOTES +${AUTH_VOTES} " \
    || fail "SYSGEN SHOW VOTES did not read back the ${AUTH_VOTES} SYSMAN wrote"

# --- MEASURED NEGATIVE CONTROL: the OLD divergent path is dead --------------
# Plant a DIFFERENT SCSNODE at the old SYSPARAMS.DAT target via a literal WRITE.
# (USE CURRENT reloads the canonical store, so this store is otherwise coherent
# -- only SCSNODE differs -- keeping the identity pair whole.)
printf 'PARAMETERS USE CURRENT\nPARAMETERS SET SCSNODE %s\nPARAMETERS WRITE %s\nEXIT\n' \
    "$BAD_NODE" "$OLDPATH" | "$SYSMAN" >/dev/null 2>&1
[ -f "$OLDPATH" ] || fail "negctl setup: SYSMAN did not write the old-path store $OLDPATH"

# Prove the plant really landed at the old path (measurement, not assumption):
# reading THAT file back through SYSMAN echoes the divergent value.
oldshow=$(printf 'PARAMETERS USE %s\nPARAMETERS SHOW SCSNODE\nEXIT\n' "$OLDPATH" | "$SYSMAN" 2>&1)
echo "$oldshow" | grep -qE "^  SCSNODE +\"${BAD_NODE} *\"" \
    || fail "negctl setup: the old-path store does not carry the divergent ${BAD_NODE}"

# THE LOCK: F$GETSYI (reading the canonical store) still returns the authored
# value, NOT the divergent one sitting at the old path. If F$GETSYI read
# SYSPARAMS.DAT, it would report ${BAD_NODE} here.
g_node2=$(getsyi SCSNODE)
echo "after-plant: F\$GETSYI SCSNODE=$g_node2"
[ "$g_node2" = "$AUTH_NODE" ] \
    || fail "F\$GETSYI SCSNODE changed after planting the old path -- coherence lost (got '$g_node2')"
if [ "$g_node2" = "$BAD_NODE" ]; then
    fail "F\$GETSYI read the OLD divergent path (${BAD_NODE}) -- the store-coherence bug is back"
fi

if [ $FAILURES -eq 0 ]; then
    echo "SYSMAN_STORE_COHERENCE_OK"
else
    echo "SYSMAN_STORE_COHERENCE_FAIL ($FAILURES check(s) failed)"
fi
