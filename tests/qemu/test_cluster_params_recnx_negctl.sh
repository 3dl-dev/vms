#!/bin/bash
# test_cluster_params_recnx_negctl.sh - THE MEASURED NEGATIVE CONTROL for
# tests/qemu/test_cluster_params_recnx_e2e.sh (vms-c3b).
#
# WHY THIS FILE EXISTS.
#
# The positive e2e proves that after authoring RECNXINTERVAL=30 via SYSGEN and
# WRITE CURRENT, a fresh SCSD reads RECNXINTERVAL=30 back. That assertion is
# only worth something if 30 could NOT have come from anywhere but the authored
# store -- if SCSD hardcoded 30, or read a mock file, the positive would pass
# for the wrong reason. This control boots the SAME shipped image and does the
# OPPOSITE of the positive: it authors NOTHING. On the factory-seeded store
# (RECNXINTERVAL=20, the documented OpenVMS default -- OpenVMS System Management
# Utilities Reference Manual), a fresh SCSD --show-identity MUST report
# RECNXINTERVAL=20, NEVER the positive's 30.
#
# This is MEASURED, not asserted in prose: it boots a real QEMU image, runs the
# real SCSD against the real unauthored volume, and checks what it actually
# printed. If SCSD ever reported 30 here, the positive's 30 would be exposed as
# canned rather than adopted, and BOTH gates would (correctly) be worthless.
#
# WHAT WOULD MAKE THIS FAIL HONESTLY (i.e. catch the defect it guards): boot
# never reaches Username:; SCSD.EXE missing; SCSD --show-identity reports
# RECNXINTERVAL=30 (or anything other than the seed default 20) despite no
# authoring having happened -- which would mean the value is not really being
# read from the store.
#
# Usage (run INSIDE the bootable image):
#   docker run --rm -v $PWD/tests/qemu/test_cluster_params_recnx_negctl.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for boot to reach Username: (default 180).
#
# Exit 0 = the control behaved as designed (default 20 read back, never 30).
# Exit 1 = the control is broken (see the printed transcript).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
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

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== NEGCTL: unauthored store -> SCSD reads the DEFAULT RECNXINTERVAL=20, never the positive's 30 (vms-c3b) ==="
echo "arch=$ARCH qemu=$QEMU"

DISK=/tmp/recnx-negctl.img
LOG=/tmp/recnx-negctl-console.log
FIFO=/tmp/recnx-negctl-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + 180))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern  limit-seconds  since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}

# --- 1. Boot to login --------------------------------------------------------
send ''
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    ok "boot reaches the login prompt"
else
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi

# --- 2. Log in as SYSTEM -----------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$' 20 "$LOGIN_OFF"

# --- 3. NO authoring. A fresh SCSD reads the SEEDED default straight back ----
S_OFF=$(wc -c <"$LOG")
send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'
send 'SCSD --show-identity'
if wait_for 'SCSD-I-IDENT' 20 "$S_OFF"; then
    ok "SCSD --show-identity ran against the unauthored store"
else
    dump_and_die "SCSD --show-identity never printed SCSD-I-IDENT"
fi
S_SEG=$(segment_since "$S_OFF")
IDENT_LINE=$(printf '%s\n' "$S_SEG" | grep -F 'SCSD-I-IDENT' | head -1)

# THE CONTROL: the seed default is 20, and the positive's authored 30 must NOT
# appear on an unauthored store.
if printf '%s\n' "$IDENT_LINE" | grep -qF 'RECNXINTERVAL=20'; then
    ok "unauthored store reads the documented default RECNXINTERVAL=20"
else
    bad "unauthored store reads the documented default RECNXINTERVAL=20"
    echo "  --- SCSD-I-IDENT line seen: $IDENT_LINE"
fi
if printf '%s\n' "$IDENT_LINE" | grep -qF 'RECNXINTERVAL=30'; then
    bad "CONTROL BROKEN: SCSD reported the positive's authored 30 with NO authoring -- the value is not really read from the store"
else
    ok "SCSD did NOT report the positive's 30 on an unauthored store (the 30 in the positive is authored, not canned)"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript: SCSD --show-identity (unauthored) ==="
printf '%s\n' "$S_SEG" | grep -E 'SCSD-I-IDENT|SCSD-W' | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "NEGCTL OK -- default 20 read back, never 30; the positive proof cannot pass on a stale/mock store"
    exit 0
fi
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
