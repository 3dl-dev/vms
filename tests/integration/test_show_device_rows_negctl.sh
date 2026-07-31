#!/bin/sh
#
# test_show_device_rows_negctl.sh - negative controls for the BEHAVIOURAL
# SHOW DEVICE gate (vms-fb9 round 2).
#
# WHY THIS EXISTS. The source-scan gate this repo already had was defeated by
# writing the SAME defect in a DIFFERENT SPELLING: an adversary replaced the
# zero-rows refusal in cmd_show_device() with a memset'd struct vms_devinfo
# named "OPA0:" fed to show_device_row(), and every token check, every one of
# the twelve source-level negative controls, tests/dcl/test_show_device.sh and
# a 42/42 ctest all certified it clean while `SHOW DEVICE ZZA0:` answered
# "OPA0:  Online  0" for a device that does not exist.
#
# That mutant is case A below, checked in verbatim so the property can never
# again be enforced only against its own history. Unlike the source-scan
# controls, these BUILD a mutant DCL.EXE in a sandbox and RUN it: the gate
# they exercise is behavioural, so proving it red-able has to be behavioural
# too. A sandbox configure + build of the vmsdcl target takes about a second.
#
# THE RULE THIS FILE ENFORCES ON ITSELF: every mutation is minimal and trips
# the property it targets. Case A and B target "no row without a /dev/vms
# read" and must NOT trip the Rule 10 verdict check; case C targets the Rule
# 10 verdict check and must NOT print a row.
#
# CASE D, ADDED vms-fb9 r6: a SECOND adversary found that round 5's own fix
# for case A/B/C ($STATUS = 676, property 2b in the gate) was ITSELF
# vacuous -- a bare `return SS$_BUGCHECK;` before any executive read also
# sets that status and prints nothing, so it passed every check A/B/C
# defend against. Case D is that mutant, verbatim, and it can only be
# caught by property 4 (a real openat("/dev/vms", ...) syscall,
# strace-observed) -- see the gate script's own header.
#
# Usage: test_show_device_rows_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_show_device_rows.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-fb9 behavioural negative controls: a fabricated row must turn the gate RED"

command -v cmake >/dev/null 2>&1 || {
    echo "FAIL: cmake is not available, so no mutant can be built"
    echo "  -> this control cannot be evaluated; reported as FAILED, never skipped"
    exit 1
}

ROOT="$WORK/tree"
mkdir -p "$ROOT"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp "$SRC_ROOT/CMakeLists.txt" "$ROOT/CMakeLists.txt"

SHOW_C="$ROOT/src/vmsdcl/dcl_cmd_show.c"
cp "$SHOW_C" "$WORK/dcl_cmd_show.c.orig"
restore() { cp "$WORK/dcl_cmd_show.c.orig" "$SHOW_C"; }

# Insert one statement block immediately after cmd_show_device()'s opening
# brace. Anchored on the function signature rather than on any comment or
# nearby line, so a mutation does not silently become a no-op if the file is
# reformatted -- a no-op mutation would make this control pass for the wrong
# reason.
#
# The injected text must contain no backslash escapes: awk -v expands them,
# so the verification grep below would not find what it wrote. Use puts()
# rather than printf("...\n") when a mutant needs to emit a line.
inject() {
    awk -v code="$1" '
        /^static int cmd_show_device\(struct dcl_command \*cmd\)$/ { sig = 1 }
        { print }
        sig && /^\{$/ { print code; sig = 0 }
    ' "$WORK/dcl_cmd_show.c.orig" > "$SHOW_C"
    if ! grep -qF "$1" "$SHOW_C"; then
        echo "  FAIL: mutation could not be applied -- cmd_show_device()'s"
        echo "        signature moved, so this control tested NOTHING"
        failed=$((failed + 1))
        status=1
        restore
        return 1
    fi
    return 0
}

BUILD="$WORK/build"
build_dcl() {
    cmake -B "$BUILD" -S "$ROOT" \
        -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
        >"$WORK/cmake.log" 2>&1 || return 1
    cmake --build "$BUILD" --target vmsdcl >"$WORK/build.log" 2>&1 || return 1
    [ -x "$BUILD/bin/DCL.EXE" ]
}

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. Without it a red below could be the sandbox being broken
# rather than the mutation being caught, and would prove nothing.
# ---------------------------------------------------------------------------
if build_dcl && sh "$GATE" "$BUILD/bin/DCL.EXE" >"$WORK/pos.log" 2>&1; then
    echo "  PASS: positive control - unmutated sandbox DCL passes the gate"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - the unmutated sandbox does not pass, so no"
    echo "        control below can attribute its RED to a mutation"
    sed 's/^/          | /' "$WORK/pos.log" 2>/dev/null | tail -30
    tail -20 "$WORK/build.log" 2>/dev/null | sed 's/^/          B /'
    failed=$((failed + 1))
    status=1
fi

# expect_red <name> <required-substring-of-gate-output> [forbidden-substring...]
expect_red() {
    name="$1"; need="$2"; shift 2
    if ! build_dcl; then
        echo "  FAIL: $name -- mutant did not build"
        tail -20 "$WORK/build.log" | sed 's/^/          | /'
        failed=$((failed + 1)); status=1; restore; return
    fi
    out=$(sh "$GATE" "$BUILD/bin/DCL.EXE" 2>&1)
    rc=$?
    ok=1; why=""
    if [ "$rc" -eq 0 ]; then
        ok=0
        why="$why
        gate exited 0 -- the fabrication was CERTIFIED, not caught"
    fi
    if ! printf '%s\n' "$out" | grep -qF "$need"; then
        ok=0
        why="$why
        expected reason not reported: $need"
    fi
    for bad in "$@"; do
        if printf '%s\n' "$out" | grep -qF "$bad"; then
            ok=0
            why="$why
        NOT minimal -- also tripped an unrelated property: $bad"
        fi
    done
    if [ "$ok" -eq 1 ]; then
        echo "  PASS: $name"
        passed=$((passed + 1))
    else
        echo "  FAIL: $name$why"
        printf '%s\n' "$out" | sed 's/^/          | /'
        failed=$((failed + 1)); status=1
    fi
    restore
}

R_ROW='put something other than the liveness marker on stdout'
R_VERDICT='answered with a VMS device verdict the executive never gave'
R_NOREAD='no openat("/dev/vms", ...) syscall was observed by strace'

# --- A. THE ADVERSARY MUTANT, verbatim -----------------------------------
# A hardcoded stub row emitted when the executive returns nothing, written
# in the CURRENT oracle format so that every token the source-scan gate
# knows about stays absent. This is the exact shape that defeated the old
# gate; it must not defeat this one.
inject '{ struct vms_devinfo fab; int fr = 0; memset(&fab, 0, sizeof(fab)); strcpy(fab.devnam, "OPA0:"); show_device_row(&fab, &fr); return SS$_NORMAL; }' \
    && expect_red "A: stub row fabricated in the NEW oracle format" \
        "$R_ROW" "$R_VERDICT"

# --- B. ...and in a format nobody has used yet ---------------------------
# The point of a property gate is that it does not need to have seen the
# format before. This row shares no token, no column geometry and no helper
# with anything in the file.
inject 'puts("console device: opa0 [up]");' \
    && expect_red "B: a row in a format the gate has never seen" \
        "$R_ROW" "$R_VERDICT"

# --- C. Rule 10: the executive's silence answered in VMS's voice ---------
# The regression the second round of this item fixed: every executive
# failure reported as the oracle-pinned NOSUCHDEV, so "the executive
# rejected us" was indistinguishable from "that device does not exist".
# Prints no row, so it must trip ONLY the verdict property.
inject 'dcl_error("SYSTEM", 0, "NOSUCHDEV", "no such device available"); return SS$_NOSUCHDEV;' \
    && expect_red "C: an unanswered read reported as %SYSTEM-W-NOSUCHDEV" \
        "$R_VERDICT" "$R_ROW"

# --- D. THE VACUITY MUTANT (vms-fb9 r6 adversary finding, verbatim) -------
# Round 5 added property 2b ($STATUS = 676, SS$_BUGCHECK) to defeat A/B/C.
# An adversary then found it was ITSELF satisfiable by something other than
# the behaviour under test: this exact statement, dropped at the top of
# cmd_show_device() before vms_kif_open() or any ioctl, ALSO sets
# $STATUS = 676 and prints nothing -- so cases A/B/C's own anchors (R_ROW,
# R_VERDICT) and the $STATUS check all stayed green. Property 4
# (check_executive_read_attempted, strace-observed) is the ONLY one that
# can catch this, so this control requires THAT property and no other --
# not R_ROW, not R_VERDICT. If this ever passes with $R_NOREAD absent from
# the gate's output, property 4 has been weakened back into vacuity.
inject 'return SS$_BUGCHECK;' \
    && expect_red "D: \$STATUS fabricated with no executive read at all (M3)" \
        "$R_NOREAD" "$R_ROW" "$R_VERDICT"

echo ""
echo "vms-fb9 behavioural negative controls: $passed passed, $failed failed"
exit $status
