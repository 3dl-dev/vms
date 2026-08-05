#!/bin/sh
#
# test_show_symbol_longword.sh - BEHAVIOURAL gate (rd vms-c71): SHOW SYMBOL
# renders an integer's Hex and Octal columns as a LONGWORD, because that is
# what a DCL integer is.
#
# THE ORACLE. Measured side by side 2026-08-05 against OpenVMS VAX V7.3 on lab
# node VAX1, live DCL, same two commands run against OVMX's DCL.EXE:
#
#   real VMS:  IDENT_L = -2147483644   Hex = 80000004   Octal = 20000000004
#              IDENT_D = 8388736       Hex = 00800080   Octal = 00040000200
#
#   OVMX was:  IDENT_L = -2147483644   Hex = FFFFFFFF80000004
#                                      Octal = 1777777777760000000004
#              IDENT_D = 8388736       Hex = 00800080
#                                      Octal = 000040000200
#
# TWO DEFECTS, AND WHY BOTH POPULATIONS ARE IN THIS FILE. The item that filed
# this (rd vms-c71) is explicit that a negative-only or positive-only test is
# not enough, and the reason is that neither defect is caught by both:
#
#   1. SIGN EXTENSION, visible ONLY on a negative value. `v` is a long -- 64
#      bits on this host -- so %lX rendered a negative as 16 hex digits where
#      VMS prints 8. Every positive value renders identically before and
#      after the fix, so a positive-only test cannot see this at all.
#
#   2. OCTAL FIELD WIDTH, visible on EVERY value including positive ones. The
#      format padded to 12 octal digits where a longword is 11. This was wrong
#      for every integer symbol OVMX printed, and it survived unnoticed
#      because the HEX column looks correct for positives -- only the octal
#      column was one digit wide.
#
#   A fix for 1 alone leaves 2 wrong. A test for 2 alone never sees 1. Both
#   are asserted here, and tests/integration/test_show_symbol_longword_negctl.sh
#   proves each assertion is load-bearing by building a DCL that has one defect
#   and not the other.
#
# WHY EVERY CHECK IS PRESENCE-BASED, NOT ABSENCE-BASED. "the wrong string is
# absent" is satisfied by a DCL that prints nothing at all -- a vacuous pass,
# and the failure mode this repo keeps re-finding. Every assertion below
# requires the EXPECTED line to be PRESENT, so a DCL that crashes, prints an
# error, or emits an empty line fails rather than passes.
#
# WHY THE FIELDS ARE END-ANCHORED. `Octal = 00040000200$` does not match
# `Octal = 000040000200` -- the 12-digit rendering has a different prefix after
# "Octal = " and a different length. Without the anchor a wider field would
# pass by containment, which is exactly how a width defect hides.
#
# The DECIMAL column is asserted too. It was already correct against the oracle
# and is deliberately NOT masked by the fix; asserting it means a mutation that
# breaks the value rather than the rendering also reds this gate, and it proves
# the symbol was really assigned and read back rather than the line being
# fabricated.
#
# Usage: test_show_symbol_longword.sh [PATH-TO-DCL.EXE]

set -u

DCL="${1:-${VMSDCL:-}}"
if [ -z "$DCL" ]; then
    for cand in "$(dirname "$0")/../../build/bin/DCL.EXE" \
                "$(dirname "$0")/../../build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

status=0
passed=0
failed=0

if [ -z "$DCL" ] || [ ! -x "$DCL" ]; then
    echo "FAIL: no DCL.EXE to exercise (looked at argv[1], \$VMSDCL, build/bin)"
    echo "  -> this gate is BEHAVIOURAL; with no binary it is reported as"
    echo "     FAILED, never skipped. A skipped test is a failing test."
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-c71: SHOW SYMBOL renders integers as a LONGWORD (oracle: OpenVMS VAX V7.3)"
echo "  DCL under test: $DCL"

# The two oracle values. IDENT_L is the first negative value F$IDENTIFIER can
# return (a general identifier, %X80000004); IDENT_D is DEFAULT's UIC, which is
# positive and is where the octal width defect shows without any sign involved.
printf 'IDENT_L = -2147483644\nSHOW SYMBOL IDENT_L\nIDENT_D = 8388736\nSHOW SYMBOL IDENT_D\n' \
    | "$DCL" >"$WORK/out" 2>"$WORK/err"

# check <name> <extended-regex> <why-this-exists>
check() {
    if grep -qE "$2" "$WORK/out"; then
        echo "  PASS: $1"
        passed=$((passed + 1))
    else
        echo "  FAIL: $1"
        echo "        catches: $3"
        echo "        no line matched: $2"
        echo "        SHOW SYMBOL actually printed:"
        grep -iE 'IDENT_[LD]' "$WORK/out" 2>/dev/null | sed 's/^/          | /' \
            || echo "          | (no IDENT_L/IDENT_D line at all)"
        failed=$((failed + 1))
        status=1
    fi
}

# --- the NEGATIVE value: only this population can see sign extension -------
check "negative: Hex is the 8-digit longword 80000004" \
      'IDENT_L = -2147483644[[:space:]]+Hex = 80000004[[:space:]]' \
      "a negative sign-extended to 16 hex digits (FFFFFFFF80000004)"

check "negative: Octal is the 11-digit longword 20000000004" \
      'IDENT_L = -2147483644.*Octal = 20000000004$' \
      "a negative sign-extended in octal (1777777777760000000004)"

# --- the POSITIVE value: the width defect, invisible in hex ----------------
check "positive: Hex is 00800080" \
      'IDENT_D = 8388736[[:space:]]+Hex = 00800080[[:space:]]' \
      "the hex column drifting; it was already correct and must stay so"

check "positive: Octal is the 11-digit longword 00040000200" \
      'IDENT_D = 8388736.*Octal = 00040000200$' \
      "the 12-digit octal field -- wrong for EVERY integer symbol, and the
                 half a negative-only test cannot see"

echo ""
echo "vms-c71 SHOW SYMBOL longword gate: $passed passed, $failed failed"
exit $status
