#!/bin/sh
# test_run_status_e2e.sh - DCL RUN-path acceptance gate (vms-707).
#
# Boots the REAL runtime (the pre-mastered ODS-2 distribution disk baked into the
# ovmx-boot image), logs in SYSTEM/MANAGER, and RUNs SYS$SYSTEM:RC3.EXE -- an
# image that prints one line to SYS$OUTPUT and exits successfully.
#
# The RUN fork path was reworked for vms-707 (waitid(WNOWAIT) peek -> executive
# $STATUS readback by Linux pid -> reap). This is the end-to-end smoke test that
# the reworked path still, on the real runtime:
#   1. OUTPUT ROUTING: routes the activated image's stdout to the console.
#   2. $STATUS: reports the image's completion status through the new readback
#      path -- SS$_NORMAL (%X00000001) for this clean exit -- never a hang or a
#      wrong/absent status.
#
# The FAITHFUL-ENCODING half (a bit<0>-set condition C$_EXIT1+(N-1)*8 surviving
# to $STATUS instead of collapsing to %X00000001) is an Alpha GCC-port property
# (IMGACT VMS-standard activation, which x86_64 lacks -- imgact.c stub); it is
# proven at the executive level by tests/qemu/test_kmod_exit.c (Part 5, real
# /dev/vms) and end-to-end by the Alpha crtl_rms re-run this change unblocks.
#
# Run INSIDE the ovmx-boot image (distro/Dockerfile.bootable), which supplies
# qemu-system-* AND the pre-mastered disk with RC3.EXE in SYS$SYSTEM:.
#
# Exit 0 = RUN activates, routes output, and reports the right status. Exit 1 =
# the reworked RUN path regressed (see the printed transcript).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
CMD_TIMEOUT="${CMD_TIMEOUT:-30}"

DISTRIB_IMG=/boot/ovmx-distrib.img
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
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
    [ -f "$f" ] || { echo "FATAL: $f not found - run this INSIDE the ovmx-boot image"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== OVMX DCL RUN \$STATUS-propagation e2e (vms-707): boot, login, RUN RC3.EXE, SHOW SYMBOL \$STATUS ==="
echo "arch=$ARCH qemu=$QEMU"

DISK=/tmp/run-status-e2e.img
LOG=/tmp/run-status-e2e-console.log
FIFO=/tmp/run-status-e2e-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

WALL=$((BOOT_TIMEOUT + CMD_TIMEOUT * 6 + 120))
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

send() { printf '%s\r' "$1" >&4; }
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
    echo ""; echo "=== FATAL: $1 ==="; echo "--- full console log ---"; cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; exit 1
}
SEG=""
run_cmd() {
    local cmd="$1" off
    off=$(wc -c <"$LOG")
    send "$cmd"
    wait_for '$ ' "$CMD_TIMEOUT" "$off"
    sleep 1
    SEG=$(segment_since "$off")
}

# --- Boot to login ----------------------------------------------------------
if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
w=0
until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge "$BOOT_TIMEOUT" ]; do
    send ''; sleep 1; w=$((w + 1))
done
wait_for 'Username:' 5 || dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
ok "runtime boots to the login prompt"

LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF" || dump_and_die "SYSTEM login failed"
ok "SYSTEM logs in (LOGINOUT.EXE -> DCL.EXE off the mounted ODS-2 disk)"
wait_for '$ ' 20 "$LOGIN_OFF"

# --- RUN the image and read its $STATUS -------------------------------------
run_cmd 'RUN SYS$SYSTEM:RC3.EXE'
RUN_SEG="$SEG"
echo "----- verbatim: RUN SYS\$SYSTEM:RC3.EXE -----"
printf '%s\n' "$RUN_SEG"
echo "--------------------------------------------"

# 1. OUTPUT ROUTING: the activated image's stdout reached the console.
if printf '%s\n' "$RUN_SEG" | grep -qF 'RC3: image output reached SYS$OUTPUT'; then
    ok "RUN routes the activated image's stdout to the console (vms-707 output half)"
else
    bad "RUN did NOT route the image's stdout to the console (the RC3 line is absent)"
fi

run_cmd 'SHOW SYMBOL $STATUS'
STATUS_SEG="$SEG"
echo "----- verbatim: SHOW SYMBOL \$STATUS -----"
printf '%s\n' "$STATUS_SEG"
echo "------------------------------------------"

# 2. $STATUS: the reworked readback path reports the image's completion status.
#    RC3 exits cleanly, so the faithful status is SS$_NORMAL (%X00000001) -- read
#    back through the new waitid-peek/getexit path, proving it runs end-to-end on
#    the real runtime and reports the right value (never a hang or wrong status).
if printf '%s\n' "$STATUS_SEG" | grep -qiE '\$STATUS[^0-9A-Fa-f]*"?%X00000001'; then
    ok "SHOW SYMBOL \$STATUS reports SS\$_NORMAL (%X00000001) for the clean exit, via the reworked readback path"
else
    bad "SHOW SYMBOL \$STATUS is not SS\$_NORMAL for a successful RUN -- the reworked RUN path regressed"
fi
# The RUN must not have reported an error for an image that exited cleanly.
if printf '%s\n' "$RUN_SEG" | grep -qiE '%DCL-.-ABORT|error status'; then
    bad "RUN reported an error for RC3.EXE which exited cleanly (spurious failure)"
else
    ok "RUN reported no error for the clean exit (no spurious %DCL-*-ABORT)"
fi

echo ""
echo "=== RUN \$STATUS e2e: $PASS passed, $FAIL failed ==="
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
