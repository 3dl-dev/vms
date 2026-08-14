#!/bin/bash
# TEST: SYSMAN PARAMETERS SHOW/SET operate on NAMED, TYPE-AWARE SYSGEN
# parameters (VOTES, EXPECTED_VOTES, the string-typed SCSNODE) -- not numeric
# indices -- and share SYSGEN.EXE's parameter store, so a SET+WRITE here is
# visible to SYSGEN and to F$GETSYI (vms-8da; unblocks vms-098 R1 cluster-param
# authoring). Grounded in the VSI OpenVMS System Management Utilities Reference
# Manual (SYSMAN PARAMETERS SET/SHOW/USE/WRITE {ACTIVE|CURRENT|file}) and VSI
# OpenVMS System Manager's Manual Vol. 2 ("Managing System Parameters with
# SYSMAN"): SHOW displays the work area's current/default/min/max; WRITE
# commits it.
# EXPECT: regex:(SYSMAN_PARAMS_OK|SYSMAN_PARAMS_SKIPPED)
# EXPECT_NOT: contains:SYSMAN_PARAMS_FAIL
# EXPECT_NOT: contains:Segmentation
VMSDCL="${VMSDCL:-vmsdcl}"
SYSMAN="${SYSMAN:-$(dirname "$VMSDCL")/SYSMAN.EXE}"
SYSGEN="${SYSGEN:-$(dirname "$VMSDCL")/SYSGEN.EXE}"

if [ ! -x "$SYSMAN" ] || [ ! -x "$SYSGEN" ]; then
    echo "SYSMAN_PARAMS_SKIPPED: SYSMAN.EXE/SYSGEN.EXE not found next to VMSDCL"
    echo "  (BUILD_TOOLS=ON builds them into the same bin/ as VMSDCL; if they are"
    echo "  genuinely absent here this is an honest skip, not a fabricated pass)."
    exit 0
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
# OVMX_SYSGEN_PATH points both tools at a private, unversioned literal store
# (no /vms mount needed) -- the same override the F$GETSYI SCSNODE test uses.
export OVMX_SYSGEN_PATH="$TMPDIR/OVMXVMSSYS.PAR"

# Seed the store with the factory defaults via SYSGEN (the store SYSMAN reads).
printf 'USE DEFAULT\nWRITE %s\nEXIT\n' "$OVMX_SYSGEN_PATH" | "$SYSGEN" >/dev/null 2>&1

FAILURES=0

# --- 1. PARAMETERS SHOW <name> resolves a NAMED numeric param (VOTES) ------
out=$(printf 'PARAMETERS SHOW VOTES\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$out"
if ! echo "$out" | grep -qE '^  VOTES +[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+'; then
    echo "  FAIL: PARAMETERS SHOW VOTES did not display the named numeric parameter"
    FAILURES=$((FAILURES + 1))
fi

# --- 2. PARAMETERS SHOW <name> is TYPE-AWARE for string params (SCSNODE) ---
#     A numeric-only SHOW would print SCSNODE with bogus numeric columns; the
#     type-aware SHOW quotes the string value and dashes min/max.
out=$(printf 'PARAMETERS SHOW SCSNODE\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$out"
if ! echo "$out" | grep -qE '^  SCSNODE +"[^"]*" +"[^"]*" +- +-'; then
    echo "  FAIL: PARAMETERS SHOW SCSNODE was not type-aware (expected quoted string, dashed min/max)"
    FAILURES=$((FAILURES + 1))
fi

# --- 3. SET a numeric param + WRITE CURRENT, then re-read in a FRESH SYSMAN
#     invocation: proves the change persisted to the shared store, not just an
#     in-memory printout. ------------------------------------------------------
setout=$(printf 'PARAMETERS SET EXPECTED_VOTES 3\nPARAMETERS WRITE CURRENT\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$setout"
if ! echo "$setout" | grep -q '%SYSMAN-I-SETPARAM, EXPECTED_VOTES changed from 1 to 3'; then
    echo "  FAIL: SET EXPECTED_VOTES 3 did not report the real change"
    FAILURES=$((FAILURES + 1))
fi
reread=$(printf 'PARAMETERS SHOW EXPECTED_VOTES\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$reread"
if ! echo "$reread" | grep -qE '^  EXPECTED_VOTES +3 +1 '; then
    echo "  FAIL: re-read after WRITE CURRENT did not show Current=3 (Default stays 1)"
    FAILURES=$((FAILURES + 1))
fi

# --- 4. The change is visible to SYSGEN too (shared store, not a private copy)
sysgenview=$(printf 'USE %s\nSHOW EXPECTED_VOTES\nEXIT\n' "$OVMX_SYSGEN_PATH" | "$SYSGEN" 2>&1)
if ! echo "$sysgenview" | grep -qE '^  EXPECTED_VOTES +3 '; then
    echo "  FAIL: SYSGEN did not see the value SYSMAN wrote (stores diverged)"
    FAILURES=$((FAILURES + 1))
fi

# --- 5. SET a STRING param (SCSNODE) via SYSMAN -- type-aware write path -----
scsout=$(printf 'PARAMETERS SET SCSNODE cluby\nPARAMETERS SHOW SCSNODE\nEXIT\n' | "$SYSMAN" 2>&1)
if ! echo "$scsout" | grep -q '%SYSMAN-I-SETPARAM, SCSNODE changed from OVMX to CLUBY'; then
    echo "  FAIL: SET SCSNODE (string param) did not report the real change"
    FAILURES=$((FAILURES + 1))
fi

# --- 6. Unknown parameter errors AUTHENTICALLY (no fake success) ------------
badout=$(printf 'PARAMETERS SHOW BOGUSPARAM\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$badout"
if ! echo "$badout" | grep -q '%SYSMAN-E-NOSUCHP, BOGUSPARAM is not a valid parameter name'; then
    echo "  FAIL: unknown parameter did not produce the real NOSUCHP error"
    FAILURES=$((FAILURES + 1))
fi

# --- 7. Out-of-range SET is rejected AND the value does NOT change ----------
rangeout=$(printf 'PARAMETERS SET EXPECTED_VOTES 99999\nPARAMETERS SHOW EXPECTED_VOTES\nEXIT\n' | "$SYSMAN" 2>&1)
echo "$rangeout"
if ! echo "$rangeout" | grep -q '%SYSMAN-E-TOOLARGE, value 99999 exceeds maximum 32767 for EXPECTED_VOTES'; then
    echo "  FAIL: out-of-range SET did not produce the real TOOLARGE rejection"
    FAILURES=$((FAILURES + 1))
fi
last_show=$(echo "$rangeout" | grep -E '^  EXPECTED_VOTES' | tail -1)
if ! echo "$last_show" | grep -qE 'EXPECTED_VOTES +3 '; then
    echo "  FAIL: value changed despite the out-of-range SET being rejected (last: $last_show)"
    FAILURES=$((FAILURES + 1))
fi

if [ $FAILURES -eq 0 ]; then
    echo "SYSMAN_PARAMS_OK"
else
    echo "SYSMAN_PARAMS_FAIL ($FAILURES check(s) failed)"
fi
