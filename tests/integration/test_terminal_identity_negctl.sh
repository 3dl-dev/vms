#!/bin/sh
#
# test_terminal_identity_negctl.sh - negative controls for the vms-fb9 gate.
#
# WHY THIS EXISTS. test_terminal_identity.sh is a lint, and this repo has
# already shipped two lints that CERTIFIED the exact regression they existed
# to catch (see tests/integration/test_runtime_target_negctl.sh's header for
# the four recorded rounds). A gate nobody has tried to evade is an assertion
# about nothing, so the evasions are checked in and run in CI.
#
# THE RULE THIS FILE ENFORCES ON ITSELF: every property gets its OWN MINIMAL
# mutation, tripping THAT property and no other. A mutation that breaks
# everything at once proves nothing about any single property -- so each case
# below asserts both the reason it expected (the right property fired) and a
# set of reasons it must NOT see (no other property fired).
#
# One property is deliberately checked twice, from both directions: the gate
# asserts SHOW DEVICE does not read /proc/mounts (absence) AND that it does
# call vms_kif_devscan() (presence). Absence alone is satisfied by deleting
# the command, which is why the pair exists; case F is the control that
# proves the presence half is load-bearing.
#
# Usage: test_terminal_identity_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_terminal_identity.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-fb9 gate negative controls: every property must have an evasion that trips it"

ROOT="$WORK/tree"
mkdir -p "$ROOT"
cp -a "$SRC_ROOT/src" "$ROOT/src"

MAIN_C="$ROOT/src/vmsdcl/dcl_main.c"
SHOW_C="$ROOT/src/vmsdcl/dcl_cmd_show.c"
INIT_C="$ROOT/src/ovmx_init/ovmx_init.c"
TERM_C="$ROOT/src/vmsdcl/dcl_terminal.c"
cp "$MAIN_C" "$WORK/dcl_main.c.orig"
cp "$SHOW_C" "$WORK/dcl_cmd_show.c.orig"
cp "$INIT_C" "$WORK/ovmx_init.c.orig"
cp "$TERM_C" "$WORK/dcl_terminal.c.orig"

restore() {
    cp "$WORK/dcl_main.c.orig" "$MAIN_C"
    cp "$WORK/dcl_cmd_show.c.orig" "$SHOW_C"
    cp "$WORK/ovmx_init.c.orig" "$INIT_C"
    cp "$WORK/dcl_terminal.c.orig" "$TERM_C"
}

# Reason fragments, one per gate property. Each is the REQUIRED string for
# its own control and a FORBIDDEN string for every other control.
R_GET_TERM='no code reads VMS_TERMINAL from the environment'
R_GET_TYPE='no code reads VMS_DEVICE_TYPE from the environment'
R_SET_TERM='no code hands VMS_TERMINAL down through the environment'
R_SET_TYPE='no code hands VMS_DEVICE_TYPE down through the environment'
R_POOL='DCL does not allocate its own terminal name from a private pool'
R_DEFAULT='DCL does not seed a default terminal device name'
R_MOUNTS='SHOW DEVICE does not build device rows from /proc/mounts'
R_DEVTAB='SHOW DEVICE does not read the process-local MOUNT table'
R_READER='SHOW DEVICE does not call vms_kif_devscan()'

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. Without this, a negative control could be going red
# because the sandbox itself is broken, and would prove nothing.
# ---------------------------------------------------------------------------
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -eq 0 ]; then
    echo "  PASS: positive control - unmutated sandbox passes the gate"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - unmutated sandbox FAILS the gate, so no"
    echo "        negative control below can attribute a RED to its mutation"
    printf '%s\n' "$out" | sed 's/^/          /'
    failed=$((failed + 1))
    status=1
fi

# expect_red <name> <required-reason> [forbidden-reason ...]
expect_red() {
    name="$1"; need="$2"; shift 2
    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    why=""

    if [ "$rc" -eq 0 ]; then
        ok=0
        why="$why
        gate exited 0 -- the evasion was CERTIFIED, not caught"
    fi
    if ! printf '%s\n' "$out" | grep -qF "FAIL: $need"; then
        ok=0
        why="$why
        expected reason not reported: $need"
    fi
    for bad in "$@"; do
        if printf '%s\n' "$out" | grep -qF "FAIL: $bad"; then
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
        failed=$((failed + 1))
        status=1
    fi
    restore
}

# --- A. DCL reads its terminal name back out of the environment ---------
# The literal regression vms-fb9 deleted, restored as one line.
printf '%s\n' 'static const char *evade(void) { return getenv("VMS_TERMINAL"); }' >> "$MAIN_C"
expect_red "A: getenv(\"VMS_TERMINAL\") reintroduced in DCL" \
    "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_SET_TYPE" "$R_POOL" \
    "$R_DEFAULT" "$R_MOUNTS" "$R_DEVTAB" "$R_READER"

# --- B. ...and the device type with it -----------------------------------
printf '%s\n' 'static const char *evade(void) { return getenv("VMS_DEVICE_TYPE"); }' >> "$MAIN_C"
expect_red "B: getenv(\"VMS_DEVICE_TYPE\") reintroduced in DCL" \
    "$R_GET_TYPE" "$R_GET_TERM" "$R_SET_TERM" "$R_SET_TYPE" "$R_POOL" \
    "$R_DEFAULT" "$R_MOUNTS" "$R_DEVTAB" "$R_READER"

# --- C. PID 1 announces the console terminal to its children -------------
printf '%s\n' 'static void evade(void) { setenv("VMS_TERMINAL", "_OPA0:", 1); }' >> "$INIT_C"
expect_red "C: setenv(\"VMS_TERMINAL\") reintroduced in PID 1" \
    "$R_SET_TERM" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TYPE" "$R_POOL" \
    "$R_DEFAULT" "$R_MOUNTS" "$R_DEVTAB" "$R_READER"

# --- D. ...and the device type with it -----------------------------------
printf '%s\n' 'static void evade(void) { setenv("VMS_DEVICE_TYPE", "VT100", 1); }' >> "$INIT_C"
expect_red "D: setenv(\"VMS_DEVICE_TYPE\") reintroduced in PID 1" \
    "$R_SET_TYPE" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_POOL" \
    "$R_DEFAULT" "$R_MOUNTS" "$R_DEVTAB" "$R_READER"

# --- E. DCL hands itself a name out of the private _FTA pool -------------
# The env vars stay deleted here: this is the OTHER half of the old code,
# the fallback that ran when neither variable was set.
printf '%s\n' 'static const char *evade(void) { return vms_term_allocate("_FTA", getpid(), NULL); }' >> "$MAIN_C"
expect_red "E: private _FTA name pool reintroduced in DCL" \
    "$R_POOL" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_SET_TYPE" \
    "$R_DEFAULT" "$R_MOUNTS" "$R_DEVTAB" "$R_READER"

# --- F. SHOW DEVICE stops reading the executive --------------------------
# The absence checks alone cannot see this: a SHOW DEVICE that prints
# nothing reads no /proc/mounts and no process-local table either. This is
# why the gate carries a positive assertion.
sed -i 's/vms_kif_devscan(/vms_kif_devscan_DISABLED(/g' "$SHOW_C"
expect_red "F: SHOW DEVICE no longer calls \$DEVICE_SCAN on the executive" \
    "$R_READER" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_SET_TYPE" \
    "$R_POOL" "$R_DEFAULT" "$R_MOUNTS" "$R_DEVTAB"

# --- G. SHOW DEVICE regrows a second row source --------------------------
# Kept minimal: the reader stays, /proc/mounts comes back alongside it --
# which is precisely how the old code was structured, and how a "just show
# the disks too" change would arrive.
sed -i 's|^static int cmd_show_device|static const char *evade_src = "/proc/mounts";\nstatic int cmd_show_device|' "$SHOW_C"
expect_red "G: /proc/mounts reintroduced as a device-row source" \
    "$R_MOUNTS" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_SET_TYPE" \
    "$R_POOL" "$R_DEFAULT" "$R_DEVTAB" "$R_READER"

# --- H. ...or the process-local MOUNT table ------------------------------
sed -i 's|^static int cmd_show_device|static int evade_n(void) { return vms_device_table[0].mounted; }\nstatic int cmd_show_device|' "$SHOW_C"
expect_red "H: process-local vms_device_table reintroduced as a row source" \
    "$R_DEVTAB" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_SET_TYPE" \
    "$R_POOL" "$R_DEFAULT" "$R_MOUNTS" "$R_READER"

# --- J. The compiled-in default terminal name comes back -----------------
# The third source, and the reason deleting the other two was not enough:
# with no environment variable and no pool file, vms_terminal_init() seeded
# every DCL process with the same "_FTA0:".
sed -i 's|^    /\* owner is set later from context \*/|    strncpy(term->device_name, "_FTA0:", sizeof(term->device_name) - 1);\n    /* owner is set later from context */|' "$TERM_C"
expect_red "J: compiled-in default terminal name reintroduced" \
    "$R_DEFAULT" "$R_GET_TERM" "$R_GET_TYPE" "$R_SET_TERM" "$R_SET_TYPE" \
    "$R_POOL" "$R_MOUNTS" "$R_DEVTAB" "$R_READER"

# --- I. The evasion the STRIPPER must not fall for -----------------------
# A token inside a comment is prose, not code. If the gate matched raw text
# it would go red on the deletion notes vms-fb9 left at every site -- and
# maintainers would then delete the notes to get green, losing the record of
# why the code is gone. This control asserts the gate stays GREEN here.
printf '%s\n' '/* setenv("VMS_TERMINAL", ...) and getenv("VMS_DEVICE_TYPE") were deleted; see /proc/mounts note */' >> "$MAIN_C"
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -eq 0 ]; then
    echo "  PASS: I: tokens appearing only inside comments do not trip the gate"
    passed=$((passed + 1))
else
    echo "  FAIL: I: the gate went red on tokens that appear only in a comment"
    printf '%s\n' "$out" | sed 's/^/          | /'
    failed=$((failed + 1))
    status=1
fi
restore

echo ""
echo "vms-fb9 negative controls: $passed passed, $failed failed"
exit $status
