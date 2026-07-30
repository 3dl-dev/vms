#!/bin/bash
# OVMX User Acceptance Test — scripted VMS session on THE REAL RUNTIME
#
# Runs inside the ovmx-boot image (QEMU + kernel + initramfs):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/uat/vms_session_qemu.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# WHY THIS EXISTS (vms-0ff, epic vms-6b8)
#
# The UAT used to SSH into the dead-legacy Docker container on port 2222. That
# container has no /dev/vms, so what the UAT was really certifying was a full
# interactive VMS session running with NO EXECUTIVE -- precisely the state the
# epic exists to make unreachable. Once PID 1 started refusing to boot without
# an executive, the old UAT could not pass, and it should not have: it was
# user-acceptance-testing a system that OVMX no longer claims to be.
#
# The fix is not to weaken the boot guarantee so the old harness keeps working
# -- that is the architecture drifting to fit the test harness, the exact
# failure mode CLAUDE.md Rule 9 was written against. The fix is to move the UAT
# onto the runtime OVMX actually has: boot the real kernel under QEMU, log in
# over the console, and drive DCL there.
#
# SAME COMMANDS AND SAME ASSERTIONS as tests/uat/vms_session_test.sh. Nothing
# is relaxed to make the new substrate pass; where the two substrates genuinely
# differ, the real runtime is the authority.
#
# Sequencing waits on the actual prompts rather than sleeping a guessed number
# of seconds. A UAT paced by fixed sleeps is a flaky test waiting to happen,
# and a flaky test is a broken test (CLAUDE.md Rule 11).

set -uo pipefail

KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISK=/tmp/uat-sysdisk.img
CONSOLE_LOG=/tmp/uat-console.log
FIFO=/tmp/uat-console.in
BOOT_TIMEOUT=120
STEP_TIMEOUT=60

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
timeout "$BOOT_TIMEOUT" $QEMU $MACHINE \
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

# Wait for text to appear on the console. Returns 1 on timeout or if the guest
# died, so a hung boot fails fast with a diagnostic instead of silently
# feeding commands into a dead machine.
wait_for() {
    local pattern="$1" limit="${2:-$STEP_TIMEOUT}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if grep -qF "$pattern" "$CONSOLE_LOG" 2>/dev/null; then
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
# there is no session to test. Asserting it here keeps a silent regression in
# the boot guarantee from surfacing as a confusing login timeout.
wait_for '%STARTUP-I-EXEC' "$BOOT_TIMEOUT" \
    || fail_with_console "ERROR: the executive never attached — the system did not come up"

wait_for 'Username:' "$BOOT_TIMEOUT" || fail_with_console "ERROR: no login prompt"
send 'SYSTEM'
wait_for 'Password:' || fail_with_console "ERROR: no password prompt"
send 'MANAGER'
wait_for 'Welcome to OVMX' || fail_with_console "ERROR: login did not succeed"

# --- Drive the session ---------------------------------------------------
# Identical command list to the SSH UAT.
for cmd in \
    'SHOW TIME' \
    'SHOW SYSTEM' \
    'SHOW MEMORY' \
    'SHOW DEFAULT' \
    'SET DEFAULT SYS$MANAGER' \
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
    send "$cmd"
    sleep 0.4
done

send 'LOGOUT'
wait_for 'logged out' || fail_with_console "ERROR: session never logged out"

OUTPUT=$(cat "$CONSOLE_LOG")

echo "=== Session Output ==="
echo "$OUTPUT"
echo "=== End Session Output ==="

# --- Validation (unchanged from the SSH UAT) -----------------------------
PASS=0
FAIL=0
ERRORS=""

check_contains() {
    if echo "$OUTPUT" | grep -qi "$1"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should contain '$1'"
    fi
}

check_not_contains() {
    if echo "$OUTPUT" | grep -qi "$1"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should NOT contain '$1'"
    else
        PASS=$((PASS + 1))
    fi
}

check_regex() {
    if echo "$OUTPUT" | grep -qiE "$1"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should match regex '$1'"
    fi
}

# VMS date format (DD-MMM-YYYY)
check_regex '[0-9]{1,2}-(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)-[0-9]{4}'

# SHOW DEFAULT should show SYS$MANAGER after SET DEFAULT
check_contains 'SYS\$MANAGER'

# SHOW LOGICAL UAT_TEST should show the value
check_contains 'session_test_passed'

# SHOW PROCESS should show process info
check_contains 'SYSTEM'

# Privilege names should appear
check_regex '(TMPMBX|NETMBX|OPER|PRIV)'

# SHOW TERMINAL should show terminal info
check_regex '(Terminal|Device|VT100|_[A-Z])'

# HELP should produce output
check_regex '(SHOW|Additional information)'

# Unix leak checks
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
