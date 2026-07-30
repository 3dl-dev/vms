#!/bin/sh
#
# test_show_device_rows.sh - BEHAVIOURAL gate (vms-fb9): SHOW DEVICE emits no
# row it did not read from /dev/vms, and never answers in VMS's voice for a
# question the executive did not answer.
#
# WHY THIS EXISTS, AND WHY THE SOURCE-SCAN GATE WAS NOT ENOUGH.
# tests/integration/test_terminal_identity.sh checks that specific TOKENS are
# absent: "/proc/mounts", "vms_device_table", "_FTA0:". Those are the
# vocabulary of the fabrication that was deleted. On 2026-07-30 an adversary
# reintroduced the identical DEFECT -- a hardcoded row emitted when the
# executive returns nothing -- written in the NEW oracle format instead
# (a memset'd struct vms_devinfo named "OPA0:" handed to show_device_row()).
# Every one of those token checks passed, all twelve negative controls
# passed, and full ctest was green. A gate that recognises the OLD SPELLING
# of a lie does not stop the lie.
#
# So this file asserts the PROPERTY instead, and does it by RUNNING DCL:
#
#   With no executive present, `SHOW DEVICE` must put NOTHING on stdout.
#
# Not "nothing that looks like the old format" -- nothing at all. The probe
# script appends one WRITE SYS$OUTPUT, so the whole of stdout must be exactly
# that one line. Any row, in ANY format anyone might invent, adds bytes and
# turns this red. It is also not satisfiable by DCL failing to start, because
# the marker line has to be there.
#
# The second property is Rule 10's: when the executive did not answer, DCL
# must not print the oracle-pinned "%SYSTEM-W-NOSUCHDEV, no such device
# available". That message was MEASURED for exactly one condition -- a named
# device that the executive says does not exist (docs/oracle/
# vax73-terminal-device.md section 6). Using it for "the executive rejected
# us" is a false statement wearing an oracle citation.
#
# stdout and stderr are captured SEPARATELY and never merged: rows go to
# stdout via printf() and diagnostics to stderr via dcl_error(), and the two
# streams are buffered differently, so a merged capture would interleave
# non-deterministically. A test paced by luck is a flaky test.
#
# tests/integration/test_show_device_rows_negctl.sh builds mutant DCLs --
# including the exact adversary mutant above -- and requires each to drive
# this file RED.
#
# Usage: test_show_device_rows.sh [PATH_TO_DCL.EXE]

set -u

DCL="${1:-${VMSDCL:-}}"
if [ -z "$DCL" ]; then
    for cand in "$(dirname "$0")/../../build/bin/DCL.EXE" \
                "$(dirname "$0")/../../build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

status=0
MARK='OVMX-PROBE-ALIVE'
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-fb9 behavioural gate: SHOW DEVICE prints only rows it read from /dev/vms"

if [ ! -x "$DCL" ]; then
    echo "FAIL: DCL.EXE not found (looked at '$DCL')"
    echo "  -> this gate must RUN the command; it cannot be evaluated by reading"
    exit 1
fi

# run_show <command-line>  -> $WORK/out (stdout), $WORK/err (stderr)
run_show() {
    printf '%s\nWRITE SYS$OUTPUT "%s"\n' "$1" "$MARK" \
        | "$DCL" >"$WORK/out" 2>"$WORK/err"
}

# The executive is reached through /dev/vms and nowhere else, so its presence
# is what decides which half of the property is checkable here. ctest never
# runs anywhere that has one (CLAUDE.md Rule 9); the QEMU runtime always does.
if [ -c /dev/vms ]; then
    HAVE_EXEC=1
    echo "  (executive present: /dev/vms is a character device)"
else
    HAVE_EXEC=0
    echo "  (no executive: /dev/vms absent)"
fi

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

# --- Property 1: with no executive, not one byte of a device row ----------
check_no_rows() {
    label="$1"; cmdline="$2"
    run_show "$cmdline"
    actual=$(cat "$WORK/out")
    if [ "$actual" = "$MARK" ]; then
        echo "  OK: $label emitted no device row"
    else
        fail "$label put something other than the liveness marker on stdout" \
             "with no executive there is no row to print, so stdout must be" \
             "exactly '$MARK'. It was:" "$(sed 's/^/       | /' "$WORK/out")"
    fi
}

# --- Property 2: no VMS device verdict for a question VMS never answered --
check_no_vms_verdict() {
    label="$1"; cmdline="$2"
    run_show "$cmdline"
    if grep -q 'NOSUCHDEV\|IVDEVNAM' "$WORK/err"; then
        fail "$label answered with a VMS device verdict the executive never gave" \
             "the executive did not answer at all, so NOSUCHDEV / IVDEVNAM --" \
             "both measured on the oracle for conditions the executive DID" \
             "answer -- are false statements in VMS's own voice (Rule 10)." \
             "stderr was:" "$(sed 's/^/       | /' "$WORK/err")"
    else
        echo "  OK: $label did not borrow a VMS device verdict"
    fi
    # ...and it must still say something. Silence would pass the check above
    # while telling the user nothing, so the refusal is required too.
    if [ ! -s "$WORK/err" ]; then
        fail "$label reported nothing at all" \
             "an unanswered read must be reported, not swallowed"
    fi
}

# --- Property 3: with an executive, the console IS listed ----------------
check_lists_console() {
    label="$1"; cmdline="$2"
    run_show "$cmdline"
    if grep -q '^OPA0: ' "$WORK/out"; then
        echo "  OK: $label listed the executive's console device"
    else
        fail "$label did not list OPA0: from the executive device table" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    fi
}

if [ "$HAVE_EXEC" -eq 0 ]; then
    check_no_rows "bare SHOW DEVICE" 'SHOW DEVICE'
    check_no_rows "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
    check_no_rows "SHOW DEVICE ZZA0:" 'SHOW DEVICE ZZA0:'
    check_no_rows "SHOW DEVICE DUA0:" 'SHOW DEVICE DUA0:'
    check_no_vms_verdict "bare SHOW DEVICE" 'SHOW DEVICE'
    check_no_vms_verdict "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
else
    check_lists_console "bare SHOW DEVICE" 'SHOW DEVICE'
    check_lists_console "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
    # A device the executive does not have must still be refused with the
    # oracle's own message -- this half of Rule 10 is "match VMS".
    run_show 'SHOW DEVICE ZZA0:'
    if grep -q 'NOSUCHDEV, no such device available' "$WORK/err"; then
        echo "  OK: an absent device gets the oracle's NOSUCHDEV"
    else
        fail "SHOW DEVICE ZZA0: did not report the oracle's NOSUCHDEV" \
             "stderr was:" "$(sed 's/^/       | /' "$WORK/err")"
    fi
    if [ "$(cat "$WORK/out")" != "$MARK" ]; then
        fail "SHOW DEVICE ZZA0: printed a row for a device that does not exist" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "vms-fb9 behavioural gate: PASS"
else
    echo "vms-fb9 behavioural gate: FAIL"
fi
exit $status
