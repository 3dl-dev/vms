#!/bin/bash
# TEST: R1.2 store-coherence proof (vms-7c3) -- SYSMAN PARAMETERS SET/WRITE and
# the executive's own readers target ONE canonical parameter store, so a value
# an operator authors with SYSMAN is exactly what SCSD (the cluster daemon) and
# SYSGEN read back. This LOCKS the store-coherence fix: SYSMAN used to write a
# DIVERGENT SYS$MANAGER:SYSPARAMS.DAT that nothing in the executive ever read,
# while SCSD/F$GETSYI read SYS$SYSTEM:OVMXVMSSYS.PAR -- so SET SCSNODE could
# never reach the node-identity resolver. SYSMAN is now converged onto the SAME
# store (sysgen_params.h: sysgen_commit_working -> OVMXVMSSYS.PAR; the readers
# sysgen_read_string / sysgen_read_param resolve the identical path).
#
# The proof runs REAL, SEPARATELY-LINKED images (SYSMAN.EXE, SYSGEN.EXE,
# SCSD.EXE) that share ONLY the on-disk store -- there is no per-process fake
# (Rule 9 / INV-6). It covers BOTH value types:
#   - STRING  SCSNODE  : SET via SYSMAN -> read by SCSD's resolve_node_identity
#                        (sysgen_read_string), echoed by SYSMAN SHOW.
#   - NUMERIC SCSSYSTEMID / ALLOCLASS : SET via SYSMAN -> read by SCSD's
#                        resolve_scssystemid / resolve_alloclass, which call
#                        sysgen_read_param. VOTES round-trips through the shared
#                        store and is read back by SYSGEN SHOW (which loads the
#                        same OVMXVMSSYS.PAR the sysgen_read_param readers do).
#
# MEASURED NEGATIVE CONTROL (the lock): a DIFFERENT SCSNODE written to the OLD
# divergent path (SYSPARAMS.DAT) is proven present there (SYSMAN USE <old> SHOW
# echoes it) yet is NOT what SCSD reads -- SCSD still returns the canonical
# value. If the coherence fix regressed (SYSMAN writing, or SCSD reading, the
# old path), SCSD would report the divergent value and this test would FAIL.
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
SCSD="${SCSD:-$BINDIR/SCSD.EXE}"

if [ ! -x "$SYSMAN" ] || [ ! -x "$SYSGEN" ] || [ ! -x "$SCSD" ]; then
    echo "SYSMAN_STORE_COHERENCE_SKIPPED: need SYSMAN.EXE + SYSGEN.EXE + SCSD.EXE"
    echo "  next to VMSDCL (BUILD_TOOLS=ON builds them into the same bin/). If any"
    echo "  is genuinely absent this is an honest skip, not a fabricated pass."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# One private, versioned canonical store -- every reader/writer honors
# OVMX_SYSGEN_PATH exactly as they do on a real boot, where PID 1 exports the
# same env to SYSMAN, SYSGEN, SCSD and DCL. This IS the SYS$SYSTEM:OVMXVMSSYS.PAR
# store in production.
CANON="$TMPDIR/OVMXVMSSYS.PAR"
export OVMX_SYSGEN_PATH="$CANON"

# The pre-fix divergent target. In the bug, SYSMAN wrote SYS$MANAGER:SYSPARAMS.DAT
# and the executive never read it. Nothing consults it now, so a value planted
# here must be invisible to SCSD. (A literal stand-in for VMS_MANAGER_DIR/
# SYSPARAMS.DAT; the discriminating measurement is "SCSD ignores this file".)
OLDPATH="$TMPDIR/SYSPARAMS.DAT"

FAILURES=0
fail() { echo "  FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# The authored identity -- chosen distinct from every default so a stale/default
# read cannot masquerade as a pass. SCSNODE max 6 chars (SYSGEN_STRVAL_LEN 8).
AUTH_NODE="NODEB"
AUTH_SID="1042"
AUTH_ALLOC="7"
AUTH_VOTES="5"
# The divergent value planted at the OLD path -- must never reach SCSD.
BAD_NODE="BADXYZ"

scsd_ident() { "$SCSD" --show-identity 2>/dev/null | grep '^SCSD-I-IDENT'; }

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

# --- SCSD reads the authored identity from the IDENTICAL store ---------------
# STRING SCSNODE via sysgen_read_string; NUMERIC SCSSYSTEMID/ALLOCLASS via
# sysgen_read_param. This is the coherence claim: SYSMAN wrote it, SCSD reads it.
ident=$(scsd_ident)
echo "authored: $ident"
echo "$ident" | grep -q "SCSNODE=${AUTH_NODE} SCSSYSTEMID=${AUTH_SID} ALLOCLASS=${AUTH_ALLOC}" \
    || fail "SCSD did not read the identity SYSMAN wrote (stores diverged): $ident"

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

# THE LOCK: SCSD (reading the canonical store) still returns the authored value,
# NOT the divergent one sitting at the old path. If SCSD read SYSPARAMS.DAT, it
# would report ${BAD_NODE} here.
ident2=$(scsd_ident)
echo "after-plant: $ident2"
echo "$ident2" | grep -q "SCSNODE=${AUTH_NODE} " \
    || fail "SCSD identity changed after planting the old path -- coherence lost"
if echo "$ident2" | grep -q "SCSNODE=${BAD_NODE}"; then
    fail "SCSD read the OLD divergent path (${BAD_NODE}) -- the store-coherence bug is back"
fi

if [ $FAILURES -eq 0 ]; then
    echo "SYSMAN_STORE_COHERENCE_OK"
else
    echo "SYSMAN_STORE_COHERENCE_FAIL ($FAILURES check(s) failed)"
fi
