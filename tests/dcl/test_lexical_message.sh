#!/bin/bash
# TEST: F$MESSAGE names the status codes the product actually returns
# EXPECT: contains:MSG_ROUNDTRIP_OK
# EXPECT_NOT: contains:MSG_ROUNDTRIP_FAIL
#
# WHY THIS EXISTS (vms-9fc). SS$_ILLIOFUNC was 580 throughout the tree. The
# reference lab (OpenVMS VAX V7.3, node VAX1) says it is 244, and that 580 is
# a DIFFERENT condition -- SS$_VASFULL. Correcting the C constant alone was a
# HALF-APPLIED correction: DCL's F$MESSAGE table is a number->message table
# and still hard-coded 580/ILLIOFUNC, so OVMX could not name the status
# sys$qio returns for an unimplemented function code, and rendered
# "illegal I/O function" for address-space exhaustion.
#
# ORACLE (re-run 2026-07-30 on the reference lab, not recalled):
#   LIBRARY/EXTRACT=$SSDEF/OUTPUT=... SYS$LIBRARY:STARLET.MLB + SEARCH
#       $EQU  SS$_ILLIOFUNC   244
#       $EQU  SS$_VASFULL     580
#   F$MESSAGE round-trip
#       244 -> %SYSTEM-F-ILLIOFUNC, illegal I/O function code
#       580 -> %SYSTEM-F-VASFULL, virtual address space is full
#
# The C side of the round trip -- that 244 is still the value sys$qio
# returns -- is a _Static_assert in src/vmsdcl/dcl_lexical.c, so a future
# change to SS$_ILLIOFUNC breaks the build rather than desynchronising this
# table again.
VMSDCL="${VMSDCL:-vmsdcl}"
FAILURES=0

# Assignment + SHOW SYMBOL is the form the other F$ tests use here
# (test_lexical_getjpi.sh and friends).
render() {
    printf 'X = %s\nSHOW SYMBOL X\n' "$1" | $VMSDCL 2>&1
}

check() {
    local desc="$1" expr="$2" want="$3"
    local out
    out=$(render "$expr")
    if echo "$out" | grep -qF "$want"; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc -- wanted '$want', got: $out"
        FAILURES=$((FAILURES + 1))
    fi
}

check_not() {
    local desc="$1" expr="$2" unwanted="$3"
    local out
    out=$(render "$expr")
    if echo "$out" | grep -qF "$unwanted"; then
        echo "FAIL: $desc -- must not contain '$unwanted', got: $out"
        FAILURES=$((FAILURES + 1))
    else
        echo "PASS: $desc"
    fi
}

# The status sys$qio returns for an unimplemented function code must be
# nameable by F$MESSAGE, with the oracle's text and severity.
check "F\$MESSAGE(244) is ILLIOFUNC" 'F$MESSAGE(244)' \
      "%SYSTEM-F-ILLIOFUNC, illegal I/O function code"

# ... and the value it used to hold is a different condition entirely.
check "F\$MESSAGE(580) is VASFULL" 'F$MESSAGE(580)' \
      "%SYSTEM-F-VASFULL, virtual address space is full"

check_not "F\$MESSAGE(580) no longer claims to be ILLIOFUNC" 'F$MESSAGE(580)' \
      "ILLIOFUNC"

# Regression guard for the two rows vms-8019 pinned in the same table, so a
# future edit to this table cannot quietly undo them either.
check "F\$MESSAGE(148) is DUPLNAM" 'F$MESSAGE(148)' "%SYSTEM-F-DUPLNAM"
check "F\$MESSAGE(596) is VOLINV" 'F$MESSAGE(596)' "%SYSTEM-F-VOLINV"

if [ $FAILURES -eq 0 ]; then
    echo "MSG_ROUNDTRIP_OK"
else
    echo "MSG_ROUNDTRIP_FAIL ($FAILURES)"
fi
