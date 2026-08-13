#!/bin/bash
# test_startup_phase_driver.sh - STARTUP.COM is a real STDRV phase driver
# over a real SYS$STARTUP search list, and OVMX.CONF is gone (vms-21a).
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the SLIM
# initramfs + /boot/ovmx-distrib.img, the mastered distribution system disk).
#
# WHAT THIS PROVES (docs/design-boot-faithful.md §3.2/§3.3, target shape #4):
#
#   Boots the pre-installed distribution disk (same shape as
#   test_distrib_boot.sh) and, once logged in, drives real DCL commands
#   against the REAL executive/vmsfs (not a build-time inspection) to show:
#
#     - SHOW LOGICAL SYS$STARTUP reports the measured multi-value search
#       list (two equivalence strings, the second literally "SYS$MANAGER") --
#       proof DEFINE/SYSTEM's vms-420 multi-value path is what actually ran
#       at boot, not just testable in isolation.
#     - DIRECTORY SYS$STARTUP:*.DAT lists VMS$PHASES.DAT and VMS$VMS.DAT --
#       the phase driver's data files are really on the booted system disk.
#     - DIRECTORY SYS$MANAGER:*.COM lists SYCONFIG.COM and SYLOGICALS.COM --
#       the new site files STARTUP.COM's phase driver invokes exist.
#     - DIRECTORY SYS$MANAGER:OVMX.CONF reports zero files -- the deleted
#       config file is not merely absent from the source tree, it is absent
#       from the booted, mounted volume a session actually sees.
#
#   The boot console log (captured from the same boot, before login) is also
#   checked for the LPMAIN-phase site announcement line and for the absence
#   of any %RMS-E-FNF / %DCL-E-OPENIN error, which would mean the phase
#   driver's own OPEN calls failed silently on this disk.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_startup_phase_driver.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

TIMEOUT=90
DISTRIB_IMG=/boot/ovmx-distrib.img
DISK="/tmp/test-phase-driver-sysdisk.img"
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

echo "=== OVMX STARTUP.COM Phase Driver Test (vms-21a) ==="
echo "Architecture: $ARCH   QEMU: $QEMU"
echo "Kernel: $KERNEL   Slim initrd: $SLIM_INITRD   Distribution image: $DISTRIB_IMG"
echo ""

if [ -f "$DISTRIB_IMG" ]; then
    record "Mastered distribution image present in the built image" 0
else
    record "Mastered distribution image present in the built image" 1
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi

rm -f "$DISK"
cp "$DISTRIB_IMG" "$DISK"

CONSOLE_LOG="/tmp/test-phase-driver-console.log"
FIFO="/tmp/test-phase-driver-console.in"

boot() {
    rm -f "$CONSOLE_LOG" "$FIFO"
    mkfifo "$FIFO"
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
        <"$FIFO" >"$CONSOLE_LOG" 2>&1 &
    QEMU_PID=$!
    exec 4>"$FIFO"
}

cleanup() {
    exec 4>&- 2>/dev/null || true
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}

send() { printf '%s\r' "$1" >&4; }

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

boot
trap cleanup EXIT

OK=1

if wait_for '%OVMX-I-EXEC' 60; then rc=0; else rc=1; OK=0; fi
record "executive attached" "$rc"

if [ "$OK" -eq 1 ]; then
    send ''  # vms-2213: wake OPA0: — LOGINOUT waits for RETURN before Username:
    if wait_for 'Username:' 60; then rc=0; else rc=1; OK=0; fi
    record "reaches the login prompt (phase driver ran to completion)" "$rc"
fi

# Snapshot the BOOT-ONLY portion of the console (everything up to the first
# login prompt) before any interactive command runs below -- a later
# DIRECTORY of a deliberately-absent file is expected to print "0 files" and
# must never be mistaken for a boot-time OPEN failure.
BOOT_LOG=""
if [ "$OK" -eq 1 ]; then
    BOOT_LOG=$(cat "$CONSOLE_LOG")
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
    record "SYSTEM login succeeds" "$rc"
fi

# --- SHOW LOGICAL SYS$STARTUP: the measured multi-value search list --------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'SHOW LOGICAL SYS$STARTUP'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "SHOW LOGICAL SYS\$STARTUP returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$SEG" | grep -qF 'SYS$STARTUP'; then r1=0; else r1=1; fi
        record "SHOW LOGICAL SYS\$STARTUP names the logical" "$r1"
        if printf '%s' "$SEG" | grep -qF 'SYS$MANAGER'; then r2=0; else r2=1; fi
        record "SYS\$STARTUP search list carries SYS\$MANAGER as an element (vms-420 multi-value)" "$r2"
    fi
fi

# --- DIRECTORY SYS$STARTUP:*.DAT: the phase driver's data files ------------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$STARTUP:*.DAT'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "DIRECTORY SYS\$STARTUP:*.DAT returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$SEG" | grep -qF 'VMS$PHASES.DAT'; then r1=0; else r1=1; fi
        record "SYS\$STARTUP: carries VMS\$PHASES.DAT on the booted disk" "$r1"
        if printf '%s' "$SEG" | grep -qF 'VMS$VMS.DAT'; then r2=0; else r2=1; fi
        record "SYS\$STARTUP: carries VMS\$VMS.DAT on the booted disk" "$r2"
    fi
fi

# --- DIRECTORY SYS$MANAGER:*.COM: the new site files ------------------------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$MANAGER:*.COM'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "DIRECTORY SYS\$MANAGER:*.COM returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$SEG" | grep -qF 'SYCONFIG.COM'; then r1=0; else r1=1; fi
        record "SYS\$MANAGER: carries SYCONFIG.COM" "$r1"
        if printf '%s' "$SEG" | grep -qF 'SYLOGICALS.COM'; then r2=0; else r2=1; fi
        record "SYS\$MANAGER: carries SYLOGICALS.COM" "$r2"
    fi
fi

# --- DIRECTORY SYS$MANAGER:OVMX.CONF: must find nothing ---------------------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$MANAGER:OVMX.CONF'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "DIRECTORY SYS\$MANAGER:OVMX.CONF returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        # Authentic VMS for a zero-match exact filespec is
        # "%DIRECT-W-NOFILES, no files found" (VSI OpenVMS DCL Dictionary,
        # DIRECTORY; landed in vms-1c6 #461) -- NOT an empty "Total of 0 files."
        # trailer. Either is proof the file is absent; a PRESENT OVMX.CONF would
        # instead list the name with "Total of 1 file.", so this stays
        # discriminating.
        if printf '%s' "$SEG" | grep -qE '%DIRECT-W-NOFILES|Total of 0 file'; then r1=0; else r1=1; fi
        record "OVMX.CONF does not exist on the booted, mounted system disk" "$r1"
    fi
fi

cleanup
trap - EXIT
echo ""

# --- Boot console: the phase driver actually ran, and did not fail an OPEN --
# Checked against BOOT_LOG (captured above, before any interactive DIRECTORY
# command that legitimately expects a "0 files" no-match) so a later,
# intentional absence-check can never be mistaken for a boot-time failure.
echo "--- Boot console checks (from the same boot, pre-login) ---"
if printf '%s' "$BOOT_LOG" | grep -qF 'The OVMX system is now executing the site-specific startup commands.'; then
    rc=0
else
    rc=1
fi
record "LPMAIN-phase site announcement appears (SYSTARTUP_VMS.COM ran from inside the phase driver)" "$rc"

if printf '%s' "$BOOT_LOG" | grep -qF '%RMS-E-FNF'; then rc=1; else rc=0; fi
record "no %RMS-E-FNF during boot (phase driver's OPENs all found their files)" "$rc"

if printf '%s' "$BOOT_LOG" | grep -qF '%DCL-E-OPENIN'; then rc=1; else rc=0; fi
record "no %DCL-E-OPENIN during boot" "$rc"

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "--- Full console log ---"
    cat "$CONSOLE_LOG"
fi

[ "$FAIL" -eq 0 ]
