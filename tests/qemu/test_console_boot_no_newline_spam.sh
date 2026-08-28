#!/bin/bash
# ===========================================================================
# OVMX console boot: NO echoed-RETURN "newline spam" before Username: (vms-dec)
# ===========================================================================
#
# THE BUG (operator-reported, on BOTH x86_64 and VAX): the boot-to-login
# console shows obnoxious blank-line "newline spam" scattered through the boot
# output. Root cause is NOT the software emitting newlines -- the boot output
# is clean. It is the OPERATOR'S OWN RETURN KEYSTROKES ECHOING.
#
# tools/vms_login.c console_login() has an OPA0: "wake-on-RETURN" (vms-2213):
# it prints nothing and blocks until the operator presses RETURN before it
# shows "Username:". During the slow boot no prompt is visible, so an operator
# naturally hammers RETURN several times. The console tty is in cooked/ECHO
# mode, so the kernel echoes each RETURN at type-time as a BLANK LINE, mixed
# into the boot output. The tcflush() in console_login() discards the leftover
# keystrokes but CANNOT un-print the blank lines the tty already echoed -- that
# residual spam is the defect. The fix (vms-dec) clears ECHO for the duration
# of the wake window and restores it just before "Username:".
#
# WHY CI NEVER CAUGHT THIS: every existing boot harness waits for the
# "Username:" string and only THEN feeds input -- it never types DURING the
# boot, so it never provokes the echo. This test types RETURNs DURING the
# pre-login boot window (as a real operator does) and asserts the console has
# no run of echoed blank lines before "Username:", while login still works.
# THIS is the coverage CI lacked; it is what makes the vms-dec fix durable.
#
# Runs INSIDE the ovmx-boot image (distro/Dockerfile.bootable), which bakes in
# the pre-mastered distribution disk. Invoke exactly like the DCL acceptance
# e2e:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm --device /dev/kvm \
#       -v $PWD/tests/qemu/test_console_boot_no_newline_spam.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT          seconds to reach the login prompt (default 180).
#   HAMMER_RETURNS        how many RETURNs the "operator" mashes during boot
#                         (default 15).
#   MAX_BLANK_RUN         max tolerated run of consecutive blank lines in the
#                         pre-Username: boot segment (default 3). The spam bug
#                         produces a run ~= HAMMER_RETURNS; the fix produces 0.
#
# Exit 0 = no echoed-RETURN spam AND login still works. Exit 1 = spam present
# or login broke (see the printed transcript + the measured longest blank run).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
HAMMER_RETURNS="${HAMMER_RETURNS:-15}"
MAX_BLANK_RUN="${MAX_BLANK_RUN:-3}"

KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
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

for f in "$KERNEL" "$SLIM_INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this INSIDE the ovmx-boot image (distro/Dockerfile.bootable)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== OVMX console boot: no echoed-RETURN newline spam before Username: (vms-dec) ==="
echo "arch=$ARCH qemu=$QEMU  hammer=$HAMMER_RETURNS returns  max-blank-run=$MAX_BLANK_RUN"
echo ""

DISK=/tmp/console-spam-e2e.img
LOG=/tmp/console-spam-console.log
FIFO=/tmp/console-spam-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

WALL=$((BOOT_TIMEOUT + 180))
cleanup() { exec 4>&- 2>/dev/null || true; [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -f "$FIFO"; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$WALL" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send_cr() { printf '\r' >&4; }
send()    { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern limit-seconds since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qaF -- "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""; echo "=== FATAL: $1 ==="
    echo "--- full console log (\\r stripped) ---"
    segment_since 0
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}
# longest run of consecutive blank (whitespace-only) lines in stdin
longest_blank_run() {
    awk '
        /^[[:space:]]*$/ { run++; if (run > max) max = run; next }
        { run = 0 }
        END { print max + 0 }'
}

# --- Boot the real runtime; the OPERATOR MASHES RETURN DURING THE BOOT -------
# This is the whole point: we type RETURNs BEFORE "Username:" is visible, the
# way an operator does while staring at a slow boot with no prompt yet.
if wait_for '%OVMX-I-EXEC' "$BOOT_TIMEOUT"; then
    ok "executive attached (real vms.ko) -- boot is underway, no login prompt yet"
else
    dump_and_die "executive never attached (%OVMX-I-EXEC) within ${BOOT_TIMEOUT}s"
fi

# Record where the pre-login boot segment begins (right after the exec line),
# then hammer RETURN. Everything the tty echoes for these keystrokes lands in
# this segment -- that is the spam we are hunting.
HAMMER_OFF=$(wc -c <"$LOG")
echo "  ... operator mashes $HAMMER_RETURNS RETURNs during the boot ..."
i=0
while [ "$i" -lt "$HAMMER_RETURNS" ]; do
    # stop the instant the prompt appears so post-prompt keystrokes (which are
    # empty-username reprompts, a different concern) do not pollute the segment
    grep -qaF 'Username:' "$LOG" 2>/dev/null && break
    send_cr
    sleep 0.2
    i=$((i + 1))
done

# Keep nudging (one CR at a time, as an operator would) until the prompt shows.
w=0
until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge "$BOOT_TIMEOUT" ]; do
    send_cr; sleep 1; w=$((w + 1))
done
# Byte offset of the FIRST "Username:" so the measured segment ends exactly
# where the prompt begins.
if ! wait_for 'Username:' 5; then
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi
ok "runtime boots to the login prompt (Username:) despite the RETURN hammering"

# --- ASSERTION: no run of echoed blank lines in the pre-Username: segment ----
# Segment = from just after %OVMX-I-EXEC up to (not including) the first
# "Username:" line. In the buggy build every mashed RETURN was echoed here as a
# blank line, so the longest run ~= HAMMER_RETURNS. The fix clears ECHO for the
# wake window, so the operator's keystrokes are not echoed and the run is ~0.
FULL=$(segment_since 0)
# Cut the segment at the first Username: line.
PRE=$(printf '%s\n' "$FULL" | awk '/Username:/{exit} {print}')
# And restrict to what came after the exec attach (drop the linux/boot noise
# before it, which is not what this test is about).
PRE=$(printf '%s\n' "$PRE" | awk 'seen{print} /%OVMX-I-EXEC/{seen=1}')
RUN=$(printf '%s\n' "$PRE" | longest_blank_run)
TOTAL_BLANK=$(printf '%s\n' "$PRE" | grep -c '^[[:space:]]*$')
echo "  measured: longest consecutive blank-line run in pre-Username: boot segment = $RUN (total blank lines = $TOTAL_BLANK)"

# NEGATIVE CONTROL: prove the measurement is not vacuous -- the segment must be
# non-empty and must actually contain the prompt-approach content.
if [ -z "$(printf '%s' "$PRE" | tr -d '[:space:]')" ]; then
    bad "NEGCTL: pre-Username: segment is empty -- the blank-run measurement is vacuous and cannot be trusted"
else
    ok "NEGCTL: pre-Username: segment is non-empty ($(printf '%s\n' "$PRE" | wc -l) lines) -- the blank-run measurement is real"
fi

if [ "$RUN" -le "$MAX_BLANK_RUN" ]; then
    ok "NO newline spam: longest echoed blank-line run ($RUN) <= allowed ($MAX_BLANK_RUN) -- wake keystrokes are not echoed (vms-dec)"
else
    bad "NEWLINE SPAM PRESENT: longest blank-line run ($RUN) > allowed ($MAX_BLANK_RUN) -- the operator's RETURNs are echoing as blank lines during boot (vms-dec regression)"
fi

# --- Login must still work (ECHO restored at the real prompt) ---------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in after the fix (ECHO restored, username prompt works)"
else
    dump_and_die "SYSTEM login failed after the RETURN hammering -- the fix must not break login"
fi
# The username the operator types at the REAL prompt must still echo (ECHO was
# restored). Confirm SYSTEM appears on the console after the login offset.
if wait_for 'SYSTEM' 5 "$LOGIN_OFF"; then
    ok "typed username echoes at the real Username: prompt (ECHO restored, boot output not suppressed)"
else
    bad "typed username did NOT echo at the prompt -- ECHO was not restored (fix over-reached)"
fi

echo ""
echo "=== RESULT: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] || exit 1
exit 0
