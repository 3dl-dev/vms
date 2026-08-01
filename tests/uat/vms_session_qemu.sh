#!/bin/bash
# OVMX User Acceptance Test — scripted VMS session on THE REAL RUNTIME
#
# Runs inside the ovmx-boot image (QEMU + kernel + initramfs):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/uat/vms_session_qemu.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# WHY THIS EXISTS (vms-71a, epic vms-6b8)
#
# The UAT used to SSH into the dead-legacy Docker container on port 2222. That
# container has no /dev/vms, so what the UAT was really certifying was a full
# interactive VMS session running with no executive backing -- not what the
# real runtime is. CLAUDE.md Rule 9 (operator ruling 2026-07-28) retired that
# container as a runtime target, so the UAT moves onto the runtime OVMX
# actually has: boot the real kernel under QEMU, log in over the console, and
# drive DCL there.
#
# PROVENANCE (disclosed per vms-71a re-dispatch ruling, completed by the
# vms-a35 PR #6 rebase): this script started as a near-verbatim copy of
# origin/vms-0ff-executive-fatal:tests/uat/vms_session_qemu.sh, with the
# `wait_for '%STARTUP-I-EXEC'` assertion that branch has deliberately NOT
# ported (main's src/ovmx_init/ovmx_init.c did not print that string yet, so
# waiting for it would have hung forever). vms-0ff has since merged, so
# ovmx_init.c prints an executive-attached line once the kernel executive
# attaches, and the wait is ported below (see "Boot and log in"). Everything
# else in the harness (assertion anchoring, prompt sync, timeout split) is
# unique to this file and was never present on vms-0ff-executive-fatal.
#
# vms-a35 round 2: the string itself moved from `%STARTUP-I-EXEC` to
# `%OVMX-I-EXEC` (see src/ovmx_init/ovmx_init.c, executive_attach) --
# a message that names a Linux device node (/dev/vms) is an OVMX event,
# not a VMS one, and may not wear the STARTUP facility (Rule 10). This
# wait was updated to match.
#
# NEARLY THE SAME COMMANDS as tests/uat/vms_session_test.sh (the retired SSH
# UAT), with ONE deliberate change: `SET DEFAULT SYS$MANAGER` (no trailing
# colon) is sent here as `SET DEFAULT SYS$MANAGER:`. Empirically verified on
# this runtime (2026-07-30): the bare form fails --
# `%DCL-E-DIRECT, invalid directory - \SYS$MANAGER\` -- for EVERY bare
# logical name tried, including SYS$LOGIN, while the same name with a
# trailing colon (`SYS$MANAGER:`) succeeds and SHOW DEFAULT correctly
# reflects it. Whether OpenVMS itself requires the colon here, or OVMX's
# SET DEFAULT has a real gap handling a bare logical name (src/vmsdcl/
# dcl_cmd_set.c:cmd_set_default), is a VMS-semantics question this
# CI-migration item is not scoped to answer -- filed separately (see the item
# this script's commit references). This script uses the syntax proven to
# work so the assertion is exercising real DCL behavior instead of masking
# an unrelated, undiagnosed gap.
#
# The ASSERTIONS are NOT a verbatim carryover from the SSH UAT either: this
# harness anchors each echo-sensitive check to the specific command's own
# response segment (see "Assertion anchoring" below) rather than grepping
# the whole console log, because the whole log also contains the guest tty's
# echo of every command this script sends -- a naive `grep` for e.g. 'SYSTEM'
# would be satisfied by the echo of `send 'SYSTEM'` at the login prompt
# regardless of what any real command printed. Where the two substrates
# genuinely differ, the real runtime is the authority.
#
# Sequencing waits on the actual DCL prompt rather than sleeping a guessed
# number of seconds. A UAT paced by fixed sleeps is a flaky test waiting to
# happen, and a flaky test is a broken test (CLAUDE.md Rule 11).

set -uo pipefail

KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISK=/tmp/uat-sysdisk.img
CONSOLE_LOG=/tmp/uat-console.log
FIFO=/tmp/uat-console.in

# Timeouts are split so a slow boot cannot silently eat the budget for the
# rest of the session (or vice versa) and surface as an opaque mid-session
# guest death instead of a diagnosable timeout at the right stage.
BOOT_TIMEOUT=120     # budget for EACH of the two boot-phase waits below
                      # (executive attach, then the login prompt) -- see
                      # SESSION_TIMEOUT's comment for why it is counted twice
STEP_TIMEOUT=60      # budget for each named login-flow wait (password/welcome/logout)
COMMAND_TIMEOUT=10   # budget for the DCL prompt to return after one command
                      # (observed on this host: whole 16-command session completes
                      # in well under 1s per command under TCG -- 10s is a >10x margin)
# Overall wall-clock kill switch for the whole QEMU process: two boot-phase
# waits (executive attach, then login prompt -- each at BOOT_TIMEOUT) + 3
# named steps + up to 16 commands, each at its own budget, plus slack. This
# is a safety net (the guest should return far faster than this in the normal
# case, observed 11-14s end to end on this host) so a genuinely wedged QEMU
# process cannot run forever -- it is NOT the pacing budget for any single
# stage (that would let a slow boot silently eat the rest of the session's
# time, CLAUDE.md Rule 11 / vms-71a re-dispatch finding). Kept comfortably
# under the CI step's `timeout-minutes: 10` (600s) in ci.yml's uat-session
# job so THIS timeout fires first with a diagnosis, instead of GH Actions
# SIGKILLing the job with no console log captured.
SESSION_TIMEOUT=$((BOOT_TIMEOUT * 2 + STEP_TIMEOUT * 3 + COMMAND_TIMEOUT * 16))

ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

rm -f "$DISK" "$CONSOLE_LOG" "$FIFO"
truncate -s 64M "$DISK"
mkfifo "$FIFO"

echo "=== OVMX UAT — scripted VMS session (real kernel, QEMU) ==="
echo "Architecture: $ARCH / QEMU: $QEMU"
echo ""

# shellcheck disable=SC2086
timeout "$SESSION_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE loglevel=3 quiet" \
    -m 256M \
    -smp 1 \
    -nic none \
    -nodefaults \
    -serial stdio \
    -drive file="$DISK",format=raw,if=virtio \
    -no-reboot \
    <"$FIFO" >"$CONSOLE_LOG" 2>&1 &
QEMU_PID=$!

# Hold the FIFO open for the life of the session: if every writer closes, the
# guest console sees EOF and the login loop tears down mid-test.
exec 3>"$FIFO"

cleanup() {
    exec 3>&- 2>/dev/null || true
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT

send() { printf '%s\r' "$1" >&3; }

# Wait for text to appear in $CONSOLE_LOG. By default scans the whole file
# (byte offset 0); callers driving the command loop pass $since (a byte
# offset captured before send()) so they synchronise on output APPENDED
# after that point rather than on anything anywhere in the log -- which
# matters because the log also contains the tty echo of what was just sent,
# and (before login) the boot log. Returns 1 on timeout or if the guest
# died, so a hung boot or a wedged command fails fast with a diagnostic
# instead of silently feeding more input into a dead or stuck machine.
wait_for() {
    local pattern="$1" limit="${2:-$STEP_TIMEOUT}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$CONSOLE_LOG" 2>/dev/null | grep -qF "$pattern"; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "ERROR: guest exited while waiting for '$pattern'" >&2
            return 1
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    echo "ERROR: timed out after ${limit}s waiting for '$pattern'" >&2
    return 1
}

fail_with_console() {
    echo "$1" >&2
    echo "--- console output so far ---" >&2
    tail -60 "$CONSOLE_LOG" >&2
    exit 1
}

# --- Boot and log in -----------------------------------------------------
# The executive must come up first; if it does not, PID 1 aborts the boot and
# there is no session to test. Asserting it here (vms-0ff, ported by the
# vms-a35 PR #6 rebase) keeps a silent regression in the boot guarantee from
# surfacing as a confusing login timeout instead of a clear one.
wait_for '%OVMX-I-EXEC' "$BOOT_TIMEOUT" \
    || fail_with_console "ERROR: the executive never attached — the system did not come up"

wait_for 'Username:' "$BOOT_TIMEOUT" || fail_with_console "ERROR: no login prompt"
send 'SYSTEM'
wait_for 'Password:' || fail_with_console "ERROR: no password prompt"
send 'MANAGER'
wait_for 'Welcome to OVMX' || fail_with_console "ERROR: login did not succeed"

# --- Drive the session -----------------------------------------------------
# Same command list as the SSH UAT, except SET DEFAULT SYS$MANAGER: (see the
# header note above for why the trailing colon was added).
#
# Assertion anchoring: for each command, record the console log's byte
# offset immediately before sending it, then wait for the DCL prompt ('$ ')
# to reappear IN THE BYTES APPENDED SINCE THAT OFFSET. That segment is
# [echoed input line][command's real output][next prompt]. The first line is
# always the guest tty's echo of what we typed (readline echoes each
# character as it's read, so the whole line lands before any output does);
# stripping it leaves only the command's own response, which is what
# CMD_OUTPUT holds. This is what lets check_response() below assert against
# real DCL output instead of being satisfiable by the fact that we typed the
# string we're checking for.
declare -A CMD_OUTPUT

run_cmd() {
    local cmd="$1" offset segment
    offset=$(wc -c <"$CONSOLE_LOG")
    send "$cmd"
    wait_for '$ ' "$COMMAND_TIMEOUT" "$offset" \
        || fail_with_console "ERROR: no DCL prompt after '$cmd'"
    segment=$(tail -c "+$((offset + 1))" "$CONSOLE_LOG" | tr -d '\r')
    CMD_OUTPUT["$cmd"]=$(printf '%s\n' "$segment" | tail -n +2)
}

for cmd in \
    'SHOW TIME' \
    'SHOW SYSTEM' \
    'SHOW MEMORY' \
    'SHOW DEFAULT' \
    'SET DEFAULT SYS$MANAGER:' \
    'SHOW DEFAULT' \
    'DIRECTORY' \
    'SHOW PROCESS' \
    'SHOW PROCESS /PRIVILEGES' \
    'SHOW LOGICAL SYS$LOGIN' \
    'DEFINE UAT_TEST "session_test_passed"' \
    'SHOW LOGICAL UAT_TEST' \
    'DEASSIGN UAT_TEST' \
    'SHOW USERS' \
    'SHOW TERMINAL' \
    'HELP SHOW'
do
    run_cmd "$cmd"
done

LOGIN_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'LOGOUT'
wait_for 'logged out' "$STEP_TIMEOUT" "$LOGIN_OFFSET" || fail_with_console "ERROR: session never logged out"

# Whole-session output, for the human-readable transcript below and for the
# negative Unix-leak checks (which are safe to scan broadly -- none of the
# strings they look for are ones this script ever sends).
OUTPUT=$(cat "$CONSOLE_LOG")

# Post-login output only: excludes the boot log (kernel/init messages this
# harness never validated before, so scanning it for the negative checks
# would be new, unvalidated surface) and excludes the login-prompt echo of
# the username/password we sent.
WELCOME_OFFSET=0
if grep -qF 'Welcome to OVMX' "$CONSOLE_LOG"; then
    WELCOME_OFFSET=$(grep -aboF 'Welcome to OVMX' "$CONSOLE_LOG" | head -1 | cut -d: -f1)
fi
SESSION_OUTPUT=$(tail -c "+$((WELCOME_OFFSET + 1))" "$CONSOLE_LOG")

echo "=== Session Output ==="
echo "$OUTPUT"
echo "=== End Session Output ==="

# --- Validation --------------------------------------------------------
PASS=0
FAIL=0
ERRORS=""

# Whole-log substring check. Used ONLY for the negative Unix-leak checks
# below, whose patterns cannot be satisfied by something this script itself
# typed (see check_response() below for the positive assertions, which can
# be echo-satisfied and so are anchored to one command's own response
# instead of scanning the whole log). Round-3 mutation testing found and
# removed the two whole-log positive-assertion helpers this file used to
# have (check_contains/check_regex): every positive check that used them
# was provably satisfiable by a command's own echo or by unrelated output
# elsewhere in the log (e.g. LOGOUT's timestamp satisfying a "SHOW TIME
# printed a date" check even when SHOW TIME itself was rejected by DCL).
check_not_contains() {
    if echo "$OUTPUT" | grep -qi "$1"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should NOT contain '$1'"
    else
        PASS=$((PASS + 1))
    fi
}

# Anchored check: asserts against ONE command's own captured response
# (CMD_OUTPUT[cmd]), not the whole log -- so it cannot be satisfied by the
# echo of any command's input, including its own.
check_response() {
    local cmd="$1" pattern="$2"
    if printf '%s' "${CMD_OUTPUT[$cmd]}" | grep -qiE "$pattern"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: response to '$cmd' should contain '$pattern' (got: $(printf '%s' "${CMD_OUTPUT[$cmd]}" | tr '\n' ' '))"
    fi
}

# SHOW TIME should print a VMS date (DD-MMM-YYYY) in its own response.
# Anchored, not a whole-log scan: LOGOUT's own message ("logged out at
# 1-JAN-1970 00:00:09.95") independently matches this exact date-format
# regex, so a whole-log check_regex here would still pass even if SHOW TIME
# were rejected outright and printed no date at all. Verified by mutation:
# prefixing SHOW TIME with a bogus verb (%DCL-E-IVVERB, no date printed by
# that command) makes this assertion fail only once it is anchored to SHOW
# TIME's own response; the unmutated run still passes.
check_response 'SHOW TIME' '[0-9]{1,2}-(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)-[0-9]{4}'

# SHOW DEFAULT should show SYS$MANAGER after SET DEFAULT. Anchored to the
# SECOND 'SHOW DEFAULT' response (associative array key => last write wins,
# i.e. the one issued after SET DEFAULT), not the whole log -- the whole log
# also contains the echo of `SET DEFAULT SYS$MANAGER` itself, which contains
# the literal substring 'SYS$MANAGER' regardless of whether SET DEFAULT
# actually took effect.
check_response 'SHOW DEFAULT' 'SYS\$MANAGER'

# SHOW LOGICAL UAT_TEST should show the value. Anchored to that command's own
# response, not the whole log -- the whole log also contains the echo of
# `DEFINE UAT_TEST "session_test_passed"`.
check_response 'SHOW LOGICAL UAT_TEST' 'session_test_passed'

# SHOW PROCESS should show process info. Anchored to SHOW PROCESS's own
# response -- the whole log also contains the echo of `send 'SYSTEM'` at the
# login-username prompt, which would satisfy a plain substring scan on its
# own regardless of what SHOW PROCESS printed.
#
# THE ASSERTION CHANGED FROM 'SYSTEM' TO THE PROCESS ID, AND IT IS STRONGER,
# NOT WEAKER (vms-6a7). READ THIS BEFORE CHANGING IT BACK.
#
# 'SYSTEM' matched because SHOW PROCESS printed the STRING LITERAL "SYSTEM":
#     const char *upper_user = ctx->username[0] ? ctx->username : "SYSTEM";
# a hardcoded fallback in src/vmsdcl/dcl_cmd_show.c. The command now reads the
# target's row out of the executive through $GETJPI and prints what that row
# holds, and on this runtime the row holds NO user name at all -- so the old
# assertion was satisfied by a constant in the source and nothing else. It
# would have passed on a machine with no executive, no login and no process.
#
# WHAT THIS RUN ACTUALLY REVEALED, verbatim, once the literal was gone:
#     1-JAN-1970 00:00:06.94   User:                  Process ID:   10000001
#                              Node: OVMX             Process name: ""
#     User Identifier:    [000,000]
#     Default file spec:  SYS$MANAGER:
# The console DCL session has no authenticated identity stamped on its
# executive row (no user name), and no process name -- because nothing on the
# console boot path calls $SETPRN or the SYSUAF authentication that stamps one
# (vms-2b8's vms_kif_setident). Those are REAL GAPS the fallback was hiding;
# they are reported by vms-6a7 rather than papered over again, and they are not
# vms-6a7's to fix (identity: vms-2b8/vms-d0b; the missing default process
# name: vms-d0e).
#
# The Process ID: field is the honest assertion available today: it is an
# EXECUTIVE-ASSIGNED value (src/kernel/vms_module.c assign_vms_pid), not
# getpid() and not a literal, so it cannot be produced without a working
# /dev/vms round trip -- which is exactly what this UAT exists to certify.
#
# THE END ANCHOR IS LOAD-BEARING, measured not assumed. Without it the pattern
# was 'Process ID:   [0-9A-F]{8}', and a mutation printing the pid as decimal
# (%u instead of %08X) did NOT redden it -- 268435457 contains eight
# consecutive hex-legal characters immediately after the label, so the
# assertion could not see its own defect. It was rebuilt and rerun on this
# runtime with the anchor in place: the decimal form is rejected and the
# unmutated %08X still passes.
check_response 'SHOW PROCESS' 'Process ID:   [0-9A-F]{8}[[:space:]]*$'
check_response 'SHOW PROCESS' 'Node: +OVMX'

# Privilege names should appear in SHOW PROCESS /PRIVILEGES's own response.
# Anchored, not a whole-log scan: the whole log also contains the echo of
# the command itself, and 'PRIV' is a literal substring of 'SHOW PROCESS
# /PRIVILEGES' -- a check_regex('...|PRIV') on SESSION_OUTPUT would pass on
# the echo alone even if DCL rejected the command outright. Verified by
# mutation: prefixing the command with a bogus verb so DCL returns
# %DCL-E-IVVERB and no privilege list is ever printed makes this assertion
# fail; the unmutated run still passes.
check_response 'SHOW PROCESS /PRIVILEGES' '(TMPMBX|NETMBX|OPER)'

# SHOW TERMINAL should show terminal info in its own response. Anchored, not
# a whole-log scan: grep is case-insensitive, so 'Terminal' matches the echo
# of the command 'SHOW TERMINAL' itself, and '_[A-Z]' matches the echoes of
# 'DEFINE UAT_TEST ...' / 'SHOW LOGICAL UAT_TEST' / 'DEASSIGN UAT_TEST' (the
# '_T') as well as '_OPA0:' in SHOW PROCESS's output -- neither alternative
# needs SHOW TERMINAL to have run at all. Verified by mutation: prefixing
# the command with a bogus verb (%DCL-E-IVVERB, no terminal info printed)
# makes this assertion fail; the unmutated run still passes.
check_response 'SHOW TERMINAL' '(Terminal|Device|VT100)'

# HELP should produce output. Anchored to HELP SHOW's own response (not the
# whole log) because the command text itself contains 'SHOW' -- an unanchored
# scan would pass on the echo alone regardless of whether HELP.EXE printed
# anything real.
check_response 'HELP SHOW' '(SHOW|Additional information)'

# Unix leak checks -- scanned against the WHOLE log (including the boot log
# and command echoes), which is a strictly BROADER surface than before.
# Safe: none of these strings are ones this script ever sends, so widening
# the scan can only catch more real leaks, never manufacture a false one
# from our own input.
check_not_contains '/bin/bash'
check_not_contains 'Permission denied'
check_not_contains 'No such file or directory'
check_not_contains 'Segmentation fault'
check_not_contains '/home/'
check_not_contains 'errno'

echo ""
echo "=== UAT Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
if [ -n "$ERRORS" ]; then
    echo -e "Errors:$ERRORS"
fi

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo "All checks passed!"
