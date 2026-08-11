#!/bin/bash
# OVMX regression test — VMS usernames are case-insensitive at login (vms-962)
#
# Runs inside the ovmx-boot image (QEMU + kernel + initramfs), same harness
# shape as tests/uat/vms_session_qemu.sh:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/uat/test_login_case_insensitive.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# WHY THIS EXISTS (vms-962). OpenVMS folds usernames to uppercase; SYSUAF
# lookup is case-insensitive on the real product. The bug report claimed
# OVMX's console login treats 'alice' / 'Alice' / 'ALICE' differently for
# the same SYSUAF account. Investigation (vms-962) traced every account-
# matching and path-derivation site reachable from console login
# (sysuaf_scan()'s case-fold, tools/vms_login.c's str_upcase() before
# lookup, vmsfs_to_linux_path()'s case-insensitive fallback resolution)
# and could not reproduce a divergence: this script is the empirical proof,
# run against the real runtime rather than argued from source reading.
#
# SCOPE: console login only (tools/vms_login.c), the only login interface
# this runtime currently exercises. SSH (src/vmsssh/vmssshd.c) has no CI
# coverage at all -- a disclosed, pre-existing gap (see
# tests/uat/vms_session_qemu.sh's own header comment and vms-02d, which the
# operator cancelled) -- not something this item's scope changes. Reading
# vmssshd.c's password callback shows the identical shape (str_upcase()
# before sysuaf_lookup(), the canonical upcased name carried through PCB/
# env/executive-identity) as the console path this script proves, but that
# is a source-reading claim, not an execution one, and is reported as such.
# vmssshd.c also implements NO pubkey authentication at all (only
# SSH_AUTH_METHOD_PASSWORD is handled in its auth loop) -- so a pubkey-auth
# case-sensitivity question does not apply to this codebase; there is no
# such code path to be case-sensitive.
#
# WHAT THIS PROVES, for ONE pre-existing SYSUAF account (GUEST, which ships
# with a real password hash unlike USER1/USER2/OPERATOR/DEFAULT):
#   1. Login succeeds identically for 'guest', 'Guest', and 'GUEST'.
#   2. Every session's SHOW PROCESS reports the canonical uppercase identity
#      (User: GUEST) regardless of what case was typed at the prompt.
#   3. The three sessions share the SAME home directory: a file written in
#      the lowercase-typed session is visible and readable in the
#      canonical-typed session (vmsfs_to_linux_path()'s directory
#      resolution does not fork by typed case).
#
# MUTATION-TESTED (vms-962): with sysuaf_scan()'s search-key str_upcase()
# (src/libvms/rtl/sysuaf.c) disabled, this script's PASS 1/2 below correctly
# fail for 'guest'/'Guest' while 'GUEST' still succeeds -- i.e. this harness
# does catch the defect class the bug report describes, it is not merely
# vacuously green. See the vms-962 PR description for the before/after run.

set -uo pipefail

KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
DISK=/tmp/case-uat-sysdisk.img
CONSOLE_LOG=/tmp/case-uat-console.log
FIFO=/tmp/case-uat-console.in

BOOT_TIMEOUT=120
STEP_TIMEOUT=30
COMMAND_TIMEOUT=15

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
if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG not found — run this inside the ovmx-boot image."
    exit 1
fi
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

echo "=== OVMX regression — case-insensitive console login (vms-962) ==="
echo "Architecture: $ARCH / QEMU: $QEMU"
echo ""

# shellcheck disable=SC2086
timeout 300 $QEMU $MACHINE \
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

exec 3>"$FIFO"

cleanup() {
    exec 3>&- 2>/dev/null || true
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT

send() { printf '%s\r' "$1" >&3; }

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
    echo "--- console output so far ---"
    cat "$CONSOLE_LOG"
    exit 1
}

PASS=0
FAIL=0
ERRORS=""

wait_for '%OVMX-I-EXEC' "$BOOT_TIMEOUT" \
    || fail_with_console "ERROR: the executive never attached — the system did not come up"
wait_for 'Username:' "$BOOT_TIMEOUT" || fail_with_console "ERROR: no login prompt"

# run_cmd(): send a DCL command, wait for the next prompt, capture ONLY the
# command's own response (strip the tty echo of the input line, same
# anchoring rationale as vms_session_qemu.sh's run_cmd()).
run_cmd() {
    local cmd="$1" offset segment
    offset=$(wc -c <"$CONSOLE_LOG")
    send "$cmd"
    wait_for '$ ' "$COMMAND_TIMEOUT" "$offset" || { echo "WARN: no DCL prompt after '$cmd'"; return 1; }
    segment=$(tail -c "+$((offset + 1))" "$CONSOLE_LOG" | tr -d '\r')
    printf '%s\n' "$segment" | tail -n +2
}

# attempt_login(): type $1/$2 at Username:/Password:, assert the session
# reaches a DCL prompt AND SHOW PROCESS reports the canonical uppercase
# username. Returns 1 (and counts a FAIL) on any divergence.
declare -A LAST_USER_LINE
attempt_login() {
    local typed="$1" pass="$2" label="$3" off
    off=$(wc -c <"$CONSOLE_LOG")
    send "$typed"
    if ! wait_for 'Password:' "$STEP_TIMEOUT" "$off"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: [$label] no Password: prompt after typing '$typed'"
        return 1
    fi
    send "$pass"
    if ! wait_for '$ ' "$STEP_TIMEOUT" "$off"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: [$label] login as '$typed' did not reach a DCL prompt (rejected)"
        return 1
    fi
    PASS=$((PASS + 1))

    local proc_out
    proc_out=$(run_cmd 'SHOW PROCESS')
    LAST_USER_LINE["$label"]=$(printf '%s' "$proc_out" | grep -F 'User:' || true)
    if printf '%s' "${LAST_USER_LINE[$label]}" | grep -qE 'User: +GUEST\b'; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: [$label] SHOW PROCESS after typing '$typed' did not report canonical 'User: GUEST' (got: ${LAST_USER_LINE[$label]})"
    fi
    return 0
}

# --- Round 1: 'guest' (all lowercase) logs in, and writes a marker file ---
if attempt_login 'guest' 'GUEST' 'lowercase'; then
    run_cmd 'COPY SYS$MANAGER:LOGIN.COM CASE962.TXT' >/dev/null
    dir_out=$(run_cmd 'DIRECTORY')
    if printf '%s' "$dir_out" | grep -qF 'CASE962.TXT'; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: [lowercase] marker file CASE962.TXT not visible in its own session's DIRECTORY"
    fi
fi
off=$(wc -c <"$CONSOLE_LOG")
send 'LOGOUT'
wait_for 'Username:' "$STEP_TIMEOUT" "$off" || echo "WARN: no reprompt after lowercase session LOGOUT"

# --- Round 2: 'Guest' (title case) logs in, sees the SAME home directory --
if attempt_login 'Guest' 'GUEST' 'titlecase'; then
    dir_out=$(run_cmd 'DIRECTORY')
    if printf '%s' "$dir_out" | grep -qF 'CASE962.TXT'; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: [titlecase] marker file written by the lowercase session is NOT visible here — home directory diverged by typed case"
    fi
fi
off=$(wc -c <"$CONSOLE_LOG")
send 'LOGOUT'
wait_for 'Username:' "$STEP_TIMEOUT" "$off" || echo "WARN: no reprompt after titlecase session LOGOUT"

# --- Round 3: 'GUEST' (canonical) logs in, cleans up ------------------------
if attempt_login 'GUEST' 'GUEST' 'canonical'; then
    run_cmd 'DELETE CASE962.TXT;1' >/dev/null
fi
off=$(wc -c <"$CONSOLE_LOG")
send 'LOGOUT'
wait_for 'Username:' "$STEP_TIMEOUT" "$off" || echo "WARN: no reprompt after canonical session LOGOUT"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
if [ "$FAIL" -gt 0 ]; then
    echo -e "$ERRORS"
    echo "--- full console log ---"
    cat "$CONSOLE_LOG"
    exit 1
fi
echo "PASS: console login is case-insensitive (typed guest/Guest/GUEST all" \
     "authenticate as SYSUAF account GUEST, report canonical identity, and" \
     "share one home directory)."
exit 0
