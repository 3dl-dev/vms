#!/bin/sh
#
# test_show_symbol_longword_negctl.sh - negative controls for the vms-c71
# SHOW SYMBOL longword gate.
#
# WHAT THIS PROVES, AND WHY "the gate passes on the fixed tree" IS NOT IT.
# tests/integration/test_show_symbol_longword.sh asserts four fields against an
# OpenVMS VAX V7.3 oracle. Four assertions that all happen to hold say nothing
# about whether any of them could ever fail, or about which defect each one is
# actually carrying. This file BUILDS a DCL.EXE with one defect injected and
# RUNS the gate against it, so the claim "this gate catches that defect" is
# measured rather than asserted.
#
# THE PROPERTY EACH CONTROL EXISTS TO ESTABLISH is the item's own warning
# (rd vms-c71): "assert BOTH a negative and a positive. A fix for the sign
# extension alone leaves the octal width wrong, and a positive-only test cannot
# see the sign extension at all." That is a claim about which assertion catches
# which defect, and it is exactly the kind of claim that has been wrong in the
# comforting direction here before. So each case below declares the COMPLETE
# SET of gate checks it expects to redden, and the set is compared for EQUALITY:
#
#   - a listed check that stays green fails the control (the assertion is not
#     carrying the defect it was believed to carry);
#   - an UNLISTED check that reddens also fails the control (the mutation is
#     not minimal, and a mutation that trips everything proves nothing about
#     attribution).
#
# A partial allowlist -- "at least one check went red" -- is satisfiable by
# something other than the property, which is the defect class this repo has
# already paid for twice (see tests/qemu/run_facility_negctl.sh's header).
#
# CASE C IS THE ONE THAT WOULD HAVE CAUGHT THE HISTORICAL MISTAKE. It fixes the
# octal width and leaves the sign extension, and it must redden the NEGATIVE
# checks and ONLY those. If case C ever passes while the negative checks are
# green, the gate has silently become positive-only and the sign-extension half
# is unguarded again -- which is the state the tree was in before vms-2f8 made
# a negative value reachable through F$IDENTIFIER at all.
#
# EVERY MUTANT'S BINARY IS FINGERPRINTED. A previous round of this program
# recorded three "no effect" measurements that were all void because the tool
# was misinvoked and the unmutated artifact was measured each time; the
# landed-check that should have caught it compared against a dirty tree. The
# lesson recorded there is "prove which binary ran -- the md5 of the artifact,
# not the exit status of the tool". So each build's md5 is captured and every
# mutant is required to differ from the pristine one. A mutation that silently
# became a no-op is reported as a BROKEN FIXTURE, not certified as caught.
#
# Usage: test_show_symbol_longword_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_show_symbol_longword.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-c71 negative controls: each defect must turn the SHOW SYMBOL gate RED"

command -v cmake >/dev/null 2>&1 || {
    echo "FAIL: cmake is not available, so no mutant can be built"
    echo "  -> this control cannot be evaluated; reported as FAILED, never skipped"
    exit 1
}
[ -f "$GATE" ] || {
    echo "FAIL: the gate this file controls is missing: $GATE"
    exit 1
}

ROOT="$WORK/tree"
mkdir -p "$ROOT"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp "$SRC_ROOT/CMakeLists.txt" "$ROOT/CMakeLists.txt"

SHOW_C="$ROOT/src/vmsdcl/dcl_cmd_show.c"
cp "$SHOW_C" "$WORK/dcl_cmd_show.c.orig"
restore() { cp "$WORK/dcl_cmd_show.c.orig" "$SHOW_C"; }

# The line under test, as the fix leaves it. Anchored on the format string
# rather than on a line number or a comment, so a reformat makes the mutation
# fail loudly instead of turning into a no-op.
PRISTINE_FMT='Hex = %08lX  Octal = %011lo'
if ! grep -qF "$PRISTINE_FMT" "$WORK/dcl_cmd_show.c.orig"; then
    echo "FAIL: the pristine tree does not contain the format this file mutates"
    echo "      expected: $PRISTINE_FMT"
    echo "  -> BROKEN FIXTURE. Every control below would test nothing."
    exit 1
fi

BUILD="$WORK/build"
build_dcl() {
    cmake -B "$BUILD" -S "$ROOT" \
        -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
        >"$WORK/cmake.log" 2>&1 || return 1
    cmake --build "$BUILD" --target vmsdcl >"$WORK/build.log" 2>&1 || return 1
    [ -x "$BUILD/bin/DCL.EXE" ]
}

md5_of() { md5sum "$BUILD/bin/DCL.EXE" 2>/dev/null | cut -d' ' -f1; }

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. Without it, a red below could be the sandbox being broken
# rather than the mutation being caught -- and it would look identical.
# ---------------------------------------------------------------------------
PRISTINE_MD5=""
if build_dcl && sh "$GATE" "$BUILD/bin/DCL.EXE" >"$WORK/pos.log" 2>&1; then
    PRISTINE_MD5=$(md5_of)
    echo "  PASS: positive control - unmutated sandbox DCL passes the gate (md5 ${PRISTINE_MD5%%??????????????????????}...)"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - the unmutated sandbox does not pass, so no"
    echo "        control below can attribute its RED to a mutation"
    sed 's/^/          | /' "$WORK/pos.log" 2>/dev/null | tail -20
    tail -20 "$WORK/build.log" 2>/dev/null | sed 's/^/          B /'
    failed=$((failed + 1))
    status=1
fi

# expect_red <name> <expected-FAIL-labels, newline separated in a single arg>
#
# Runs the gate against the freshly built mutant and requires the SET of gate
# checks that went red to EQUAL the expected set exactly.
expect_red() {
    name="$1"; expect="$2"
    if ! build_dcl; then
        echo "  FAIL: $name -- mutant did not build"
        tail -20 "$WORK/build.log" | sed 's/^/          | /'
        failed=$((failed + 1)); status=1; restore; return
    fi
    mmd5=$(md5_of)
    if [ -n "$PRISTINE_MD5" ] && [ "$mmd5" = "$PRISTINE_MD5" ]; then
        echo "  FAIL: $name -- the mutant binary is byte-identical to the"
        echo "        pristine one, so the mutation did not reach the artifact."
        echo "  -> BROKEN FIXTURE, not a caught defect."
        failed=$((failed + 1)); status=1; restore; return
    fi

    out=$(sh "$GATE" "$BUILD/bin/DCL.EXE" 2>&1)
    rc=$?
    got=$(printf '%s\n' "$out" | sed -n 's/^  FAIL: \(.*\)$/\1/p' | sort)
    want=$(printf '%s\n' "$expect" | grep -v '^$' | sort)

    if [ "$rc" -eq 0 ]; then
        echo "  FAIL: $name -- the gate exited 0; the defect was CERTIFIED, not caught"
        printf '%s\n' "$out" | sed 's/^/          | /'
        failed=$((failed + 1)); status=1; restore; return
    fi
    if [ "$got" = "$want" ]; then
        echo "  PASS: $name"
        echo "        red set is EXACTLY the checks named, and no others:"
        printf '%s\n' "$got" | sed 's/^/          - /'
        passed=$((passed + 1))
    else
        echo "  FAIL: $name -- the red set is not the one this control declares"
        echo "        EXPECTED red:"; printf '%s\n' "$want" | sed 's/^/          - /'
        echo "        ACTUALLY red:"; printf '%s\n' "$got" | sed 's/^/          - /'
        echo "        (a listed check staying green means that assertion does not"
        echo "         carry this defect; an unlisted check reddening means the"
        echo "         mutation is not minimal and attributes nothing.)"
        failed=$((failed + 1)); status=1
    fi
    restore
}

NEG_HEX='negative: Hex is the 8-digit longword 80000004'
NEG_OCT='negative: Octal is the 11-digit longword 20000000004'
POS_HEX='positive: Hex is 00800080'
POS_OCT='positive: Octal is the 11-digit longword 00040000200'

# --- A. THE DEFECT AS IT SHIPPED, verbatim --------------------------------
# The pre-fix line: no mask, 12-digit octal. This is the exact rendering
# measured against the oracle on 2026-08-05 and recorded in rd vms-c71.
# It must redden three of the four checks -- and NOT the positive hex one,
# because 00800080 rendered identically before and after the fix. That green
# check is the whole reason the defect survived: the hex column looked right.
sed 's/Hex = %08lX  Octal = %011lo\\n",$/Hex = %08lX  Octal = %012lo\\n",/; s/^                       upper_name, v, lw, lw);$/                       upper_name, v, v, v);/' \
    "$WORK/dcl_cmd_show.c.orig" > "$SHOW_C"
if grep -qF 'Octal = %012lo' "$SHOW_C" && grep -qF 'upper_name, v, v, v);' "$SHOW_C"; then
    expect_red "A: the defect as it shipped (64-bit render, 12-digit octal)" \
"$NEG_HEX
$NEG_OCT
$POS_OCT"
else
    echo "  FAIL: A -- mutation could not be applied; BROKEN FIXTURE"
    failed=$((failed + 1)); status=1; restore
fi

# --- B. THE HALF-FIX THE ITEM WARNS ABOUT ---------------------------------
# Sign extension fixed, octal width still 12. This is what "a fix for the sign
# extension alone" produces, and it must redden BOTH octal checks and neither
# hex check. Two reds rather than one is declared, not hidden: the width defect
# is genuinely visible on both the negative and the positive value, so a
# control claiming only one would be understating what it trips.
sed 's/Octal = %011lo/Octal = %012lo/' "$WORK/dcl_cmd_show.c.orig" > "$SHOW_C"
if grep -qF 'Octal = %012lo' "$SHOW_C"; then
    expect_red "B: sign extension fixed, octal still 12 digits (the half-fix)" \
"$NEG_OCT
$POS_OCT"
else
    echo "  FAIL: B -- mutation could not be applied; BROKEN FIXTURE"
    failed=$((failed + 1)); status=1; restore
fi

# --- C. THE CONTROL THAT KEEPS THE GATE FROM GOING POSITIVE-ONLY ----------
# Octal width fixed, sign extension left in place: only the HEX argument goes
# back to the unmasked 64-bit value. It must redden the negative hex check and
# ONLY it -- both positive checks stay green, which is the item's claim that "a
# positive-only test cannot see the sign extension" turned into a measurement.
sed 's/^                       upper_name, v, lw, lw);$/                       upper_name, v, v, lw);/' \
    "$WORK/dcl_cmd_show.c.orig" > "$SHOW_C"
if grep -qF 'upper_name, v, v, lw);' "$SHOW_C"; then
    expect_red "C: octal fixed, sign extension left (only a NEGATIVE can see it)" \
"$NEG_HEX"
else
    echo "  FAIL: C -- mutation could not be applied; BROKEN FIXTURE"
    failed=$((failed + 1)); status=1; restore
fi

echo ""
echo "vms-c71 negative controls: $passed passed, $failed failed"
exit $status
