#!/bin/bash
# test_distrib_boot.sh - Boot the PRE-INSTALLED distribution disk (vms-8ab).
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the SLIM
# initramfs + /boot/ovmx-distrib.img, the mastered distribution system disk).
#
# WHAT THIS PROVES (the 0.4 headline for this increment,
# docs/design-vms-faithful-install.md §2/§3.3):
#
#   The build's mastering stage produces /boot/ovmx-distrib.img — a bootable
#   VMSFS system disk PRE-POPULATED from distro/rootfs/vms (the
#   [SYS0][SYSCOMMON][SYSEXE][SYSLIB][SYSMGR][SYSUPD] tree, SYSUAF, STARTUP.COM,
#   the .EXE images). Booting the SLIM initramfs (STARTUP.EXE + vms.ko +
#   vmsfs.ko + minimal config — NONE of DCL.EXE/LOGINOUT.EXE/IMGACT.EXE/SYSLIB)
#   against that image reaches a login prompt and SYSTEM/MANAGER logs in, with
#   DCL, LOGINOUT, IMGACT and the SYSLIB shareables ALL resolved FROM THE DISK.
#
# This is the §6 "mount-or-halt" shape: the disk arrives already installed, so
# no self-install runs. The slim initramfs cannot supply a login session on its
# own (proven directly against its cpio listing below), so a real login here is
# functional proof the whole login chain came off the mastered disk.
#
# Distinct from test_persistent_boot.sh Boot 3, which boots a disk that PID 1
# INSTALLED on an earlier fat-initramfs boot: here NOTHING installed the disk —
# vmsfs_master did, at build time. That is exactly what unblocks stripping the
# install machinery out of PID 1 (vms-2f0).
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_distrib_boot.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

TIMEOUT=90
DISTRIB_IMG=/boot/ovmx-distrib.img
DISK="/tmp/test-distrib-sysdisk.img"
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== OVMX Distribution-Disk Boot Test (vms-8ab) ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo "Kernel: $KERNEL"
echo "Slim initrd: $SLIM_INITRD"
echo "Distribution image: $DISTRIB_IMG"
echo ""

# --- Precondition: the mastered image exists and is non-trivial ---
if [ -f "$DISTRIB_IMG" ]; then
    record "Mastered distribution image present in the built image" 0
else
    record "Mastered distribution image present in the built image" 1
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi

# --- SLIM initramfs really is bootstrap-only (guards the proof) ---
# If the slim initramfs already carried DCL/LOGINOUT/SYSLIB, a successful login
# below would NOT prove the mastered disk supplied them. Assert the slim image
# is bootstrap-only, the same guard test_persistent_boot.sh applies to Boot 3.
echo "--- SLIM initramfs content check (static) ---"
SLIM_LISTING=$(zcat "$SLIM_INITRD" | cpio -t 2>/dev/null || true)
slim_absent() {
    local desc="$1" pat="$2"
    TOTAL=$((TOTAL + 1))
    if printf '%s\n' "$SLIM_LISTING" | grep -qF "$pat"; then
        echo "  FAIL: $desc (present but should be absent: $pat)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: $desc (correctly absent)"
        PASS=$((PASS + 1))
    fi
}
slim_absent "Slim initramfs: no DCL.EXE"      "DCL.EXE"
slim_absent "Slim initramfs: no LOGINOUT.EXE" "LOGINOUT.EXE"
slim_absent "Slim initramfs: no IMGACT.EXE"   "IMGACT.EXE"
slim_absent "Slim initramfs: no SYSLIB"       "SYSLIB"
echo ""

# --- Seed a scratch disk from the mastered image (a plain byte copy) ---
# The container owns /boot/ovmx-distrib.img read-only; QEMU needs a writable
# disk, so copy it once. This is exactly what boot.sh --distrib does inside the
# container (DISTRIB=1 seeds the disk from the mastered image on first use).
rm -f "$DISK"
cp "$DISTRIB_IMG" "$DISK"
echo "Seeded scratch disk from mastered image: $(stat -c%s "$DISK") bytes"
echo ""

# --- Boot: SLIM initramfs + pre-installed disk, interactive login ---
echo "--- Boot: SLIM initramfs + pre-installed distribution disk ---"

CONSOLE_LOG="/tmp/test-distrib-console.log"
FIFO="/tmp/test-distrib-console.in"

boot_distrib() {
    local console_log="$1" fifo="$2"
    rm -f "$console_log" "$fifo"
    mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$SLIM_INITRD" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$DISK",format=raw,if=virtio \
        -no-reboot \
        <"$fifo" >"$console_log" 2>&1 &
    QEMU_PID=$!
    exec 4>"$fifo"
}

cleanup() {
    exec 4>&- 2>/dev/null || true
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}

send() { printf '%s\r' "$1" >&4; }

# Non-fatal wait: returns 1 on timeout / guest death so each step reports its
# own PASS/FAIL and the script still reaches the results tally.
wait_for() {
    local pattern="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$CONSOLE_LOG" 2>/dev/null | grep -qF "$pattern"; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    return 1
}

boot_distrib "$CONSOLE_LOG" "$FIFO"
trap cleanup EXIT

OK=1

if wait_for '%OVMX-I-EXEC' 60; then rc=0; else rc=1; OK=0; fi
record "executive attached (slim boot on mastered disk)" "$rc"

if [ "$OK" -eq 1 ]; then
    # The disk mounted and STARTUP found a real system on it (no install ran).
    if wait_for '%STARTUP-I-MOUNTED' 30; then rc=0; else rc=1; fi
    record "system disk DKA0: mounted (pre-installed, no self-install)" "$rc"
    # A mastered disk must NOT trip PID 1's install path: it already has DCL.EXE.
    if grep -qF '%STARTUP-I-INSTALL, installing' "$CONSOLE_LOG"; then rc=1; else rc=0; fi
    record "no install ran (disk arrived pre-installed)" "$rc"
fi

if [ "$OK" -eq 1 ]; then
    if wait_for 'Username:' 60; then rc=0; else rc=1; OK=0; fi
    record "login prompt appears (from the mastered disk)" "$rc"
fi

if [ "$OK" -eq 1 ]; then
    LOGIN_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'SYSTEM'
    if wait_for 'Password:' 30 "$LOGIN_OFFSET"; then rc=0; else rc=1; OK=0; fi
    record "password prompt appears" "$rc"
fi

if [ "$OK" -eq 1 ]; then
    send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFFSET"; then rc=0; else rc=1; OK=0; fi
    record "SYSTEM login succeeds (LOGINOUT.EXE activated from mastered disk)" "$rc"
fi

if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'SHOW TIME'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; OK=0; fi
    record "DCL prompt returns after SHOW TIME (DCL.EXE activated from mastered disk)" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEGMENT=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        CURYEAR=$(date +%Y)
        if printf '%s' "$SEGMENT" | grep -qF "$CURYEAR"; then crc=0; else crc=1; fi
        record "SHOW TIME returns the real date (shareables loaded from disk SYS\$LIBRARY)" "$crc"
    fi
fi

# The rd-item's explicit ground-source: DIRECTORY SYS$SYSTEM: lists the real
# tree FROM THE DISK — DCL.EXE and LOGINOUT.EXE are files on the mounted volume.
if [ "$OK" -eq 1 ]; then
    DIR_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$SYSTEM:'
    if wait_for '$ ' 20 "$DIR_OFFSET"; then rc=0; else rc=1; OK=0; fi
    record "DIRECTORY SYS\$SYSTEM: returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        DIRSEG=$(tail -c "+$((DIR_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$DIRSEG" | grep -qF 'DCL.EXE'; then drc=0; else drc=1; fi
        record "DIRECTORY SYS\$SYSTEM: lists DCL.EXE (from the mastered disk)" "$drc"
        if printf '%s' "$DIRSEG" | grep -qF 'LOGINOUT.EXE'; then lrc=0; else lrc=1; fi
        record "DIRECTORY SYS\$SYSTEM: lists LOGINOUT.EXE (from the mastered disk)" "$lrc"
    fi
fi

cleanup
trap - EXIT
echo ""

# --- Second boot: persistence (the mastered image is a real, re-bootable disk) ---
echo "--- Second boot: same disk, prove it re-boots to login ---"
boot_distrib "$CONSOLE_LOG" "$FIFO"
trap cleanup EXIT

if wait_for 'Username:' 90; then rc=0; else rc=1; fi
record "second boot reaches the login prompt (image survives a re-boot)" "$rc"

cleanup
trap - EXIT
echo ""

echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

[ "$FAIL" -eq 0 ]
