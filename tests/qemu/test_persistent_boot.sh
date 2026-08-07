#!/bin/bash
# test_persistent_boot.sh - Verify persistent system disk across QEMU reboots
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + initramfs).
# Tests the install-once / boot-many model:
#   Boot 1: blank disk → STARTUP.EXE initializes, formats, installs
#   Boot 2: same disk  → STARTUP.EXE detects installed system, skips install
#   Boot 3: same disk, SLIM initramfs → STARTUP.EXE (bootstrap only) boots to
#           a login prompt, DCL runs, shareable images load from the
#           installed system disk's SYS$LIBRARY (vms-913.10)
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_persistent_boot.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures

set -euo pipefail

TIMEOUT=90
DISK="/tmp/test-sysdisk.img"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

# Select QEMU binary based on architecture
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qF "$pattern"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern found but should be absent: $pattern)"
            FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern not found: $pattern)"
            FAIL=$((FAIL + 1))
        fi
    fi
}

run_qemu() {
    timeout "$TIMEOUT" $QEMU $MACHINE \
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
        </dev/null 2>&1 || true
}

echo "=== OVMX Persistent Boot Smoke Test ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo "Kernel: $KERNEL"
echo "Initrd: $INITRD"
echo ""

# --- Boot 1: Fresh install (blank disk) ---
echo "--- Creating blank 64M system disk ---"
truncate -s 64M "$DISK"
echo ""

echo "--- Boot 1: Fresh install (blank disk) ---"
OUTPUT1=$(run_qemu)

# Show first 40 lines for diagnostics
echo "$OUTPUT1" | head -40
echo "[... truncated ...]"
echo ""

check "Boot 1: system disk detected"      "$OUTPUT1" "%STARTUP-I-SYSDISK"
check "Boot 1: blank disk initialized"    "$OUTPUT1" "%STARTUP-I-INIT, initializing"
check "Boot 1: disk mounted"              "$OUTPUT1" "%STARTUP-I-MOUNTED"
check "Boot 1: install started"           "$OUTPUT1" "%STARTUP-I-INSTALL, installing"
check "Boot 1: install completed"         "$OUTPUT1" "%STARTUP-I-INSTALLED"
check "Boot 1: boot banner"              "$OUTPUT1" "OpenVMS-compatible"
# The boot chain reaches SITE startup. STARTUP.COM's last act is
# @SYS$MANAGER:SYSTARTUP_VMS.COM, which is where services are started
# (vms-47b) -- so a boot that stops before it starts nothing at all.
# This asserts the procedure RAN by matching a line only that procedure
# prints; "no error appeared" is not evidence that it was invoked.
check "Boot 1: site startup ran"          "$OUTPUT1" \
      "The OVMX system is now executing the site-specific startup commands."
echo ""

# Verify disk is not empty (persistence proof — data was written)
DISK_BYTES=$(stat -c%s "$DISK" 2>/dev/null || stat -f%z "$DISK" 2>/dev/null)
echo "Disk size after boot 1: $DISK_BYTES bytes"
echo ""

# --- Boot 2: Persistent reboot (same disk) ---
echo "--- Boot 2: Persistent reboot (same disk) ---"
OUTPUT2=$(run_qemu)

echo "$OUTPUT2" | head -40
echo "[... truncated ...]"
echo ""

check "Boot 2: system disk detected"      "$OUTPUT2" "%STARTUP-I-SYSDISK"
check "Boot 2: disk mounted"              "$OUTPUT2" "%STARTUP-I-MOUNTED"
check "Boot 2: install skipped"           "$OUTPUT2" "%STARTUP-I-SYSBOOT"
check "Boot 2: no disk initialization"    "$OUTPUT2" "%STARTUP-I-INIT, initializing" "absent"
check "Boot 2: no install"                "$OUTPUT2" "%STARTUP-I-INSTALL, installing" "absent"
check "Boot 2: boot banner"              "$OUTPUT2" "OpenVMS-compatible"
# Also on the SECOND boot, from the INSTALLED system disk rather than
# from the initramfs copy: the procedure has to have been provisioned to
# the disk, not just shipped in the image.
check "Boot 2: site startup ran"          "$OUTPUT2" \
      "The OVMX system is now executing the site-specific startup commands."
echo ""

# --- SLIM initramfs content check (static) ---
# The slim initramfs is a DISTINCT artifact from the fat one (Dockerfile.bootable,
# "SLIM initramfs" stage): STARTUP.EXE (/init) + INITIALIZE.EXE + kernel modules
# + config only. DCL.EXE, LOGINOUT.EXE, IMGACT.EXE and the SYSLIB shareables are
# deliberately NOT packed into it -- they must come from the installed system
# disk. Checked directly against the cpio listing rather than assumed from the
# Dockerfile recipe, so a future recipe change that accidentally re-fattens the
# slim image is caught here, not just read off comments.
echo "--- SLIM initramfs content check (static) ---"
SLIM_LISTING=$(zcat "$SLIM_INITRD" | cpio -t 2>/dev/null || true)
# GNU cpio's listing strips the leading "./" that `find .` produced when the
# archive was packed (verified locally: an entry packed as "./init" lists as
# bare "init") -- matched here with a WHOLE-LINE (-x) check, not check()'s
# substring match, so this cannot be satisfied by some other path that merely
# contains the four letters "init".
TOTAL=$((TOTAL + 1))
if printf '%s\n' "$SLIM_LISTING" | grep -qxF "init"; then
    echo "  PASS: Slim initramfs: has /init (STARTUP.EXE)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: Slim initramfs: has /init (STARTUP.EXE) (bare 'init' entry not found)"
    FAIL=$((FAIL + 1))
fi
check "Slim initramfs: no DCL.EXE"                  "$SLIM_LISTING" "DCL.EXE"      "absent"
check "Slim initramfs: no LOGINOUT.EXE"             "$SLIM_LISTING" "LOGINOUT.EXE" "absent"
check "Slim initramfs: no IMGACT.EXE"               "$SLIM_LISTING" "IMGACT.EXE"   "absent"
check "Slim initramfs: no SYSLIB directory"         "$SLIM_LISTING" "SYSLIB"       "absent"
echo ""

# --- Boot 3: SLIM initramfs, subsequent boot, login + DCL from disk ---
# Reuses the SAME $DISK Boot 1/2 already installed. The slim initramfs (just
# proven above to carry none of DCL.EXE/LOGINOUT.EXE/IMGACT.EXE/SYSLIB) cannot
# supply a working login session on its own -- so a real login reaching a real
# DCL prompt here is direct, functional proof that LOGINOUT.EXE, IMGACT.EXE and
# the SYSLIB shareables (LIBVMS$SHR etc.) are all resolved from the mounted
# system disk's SYS$SYSTEM:/SYS$LIBRARY:, not from the initramfs (vms-913.10
# done condition). This drives a real login (SYSTEM/MANAGER, the same real
# SHA256-backed credential tests/uat/vms_session_qemu.sh uses -- vms-72c) over
# the QEMU serial console rather than merely booting and reading log lines, the
# same reason that harness exists: a banner alone is not proof DCL runs.
echo "--- Boot 3: SLIM initramfs (subsequent boot, login + DCL from disk) ---"

SLIM_CONSOLE_LOG="/tmp/test-console-slim.log"
SLIM_FIFO="/tmp/test-console-slim.in"
rm -f "$SLIM_CONSOLE_LOG" "$SLIM_FIFO"
mkfifo "$SLIM_FIFO"

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
    <"$SLIM_FIFO" >"$SLIM_CONSOLE_LOG" 2>&1 &
SLIM_QEMU_PID=$!
exec 4>"$SLIM_FIFO"

slim_cleanup() {
    exec 4>&- 2>/dev/null || true
    kill "$SLIM_QEMU_PID" 2>/dev/null || true
    wait "$SLIM_QEMU_PID" 2>/dev/null || true
    rm -f "$SLIM_FIFO"
}
trap slim_cleanup EXIT

slim_send() { printf '%s\r' "$1" >&4; }

# Non-fatal wait: returns 1 on timeout/guest-death instead of exiting, so
# every step below can be recorded as its own PASS/FAIL and the script still
# reaches "--- Results ---" with a diagnosable tally, matching how Boot 1/2
# above report (rather than aborting the whole test on the first mishap).
slim_wait_for() {
    local pattern="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$SLIM_CONSOLE_LOG" 2>/dev/null | grep -qF "$pattern"; then
            return 0
        fi
        if ! kill -0 "$SLIM_QEMU_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    return 1
}

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

# Each step is gated on the previous one succeeding: once the guest is dead
# or wedged, sending more input into it cannot produce a meaningful result,
# so the remaining steps are skipped (still counted FAIL below) rather than
# hanging out their own timeouts one after another.
SLIM_OK=1

if slim_wait_for '%OVMX-I-EXEC' 60; then rc=0; else rc=1; SLIM_OK=0; fi
record "Boot 3: executive attached (slim boot)" "$rc"

if [ "$SLIM_OK" -eq 1 ]; then
    if slim_wait_for 'Username:' 60; then rc=0; else rc=1; SLIM_OK=0; fi
    record "Boot 3: login prompt appears (slim boot)" "$rc"
fi

if [ "$SLIM_OK" -eq 1 ]; then
    SLIM_LOGIN_OFFSET=$(wc -c <"$SLIM_CONSOLE_LOG")
    slim_send 'SYSTEM'
    if slim_wait_for 'Password:' 30 "$SLIM_LOGIN_OFFSET"; then rc=0; else rc=1; SLIM_OK=0; fi
    record "Boot 3: password prompt appears" "$rc"
fi

if [ "$SLIM_OK" -eq 1 ]; then
    slim_send 'MANAGER'
    if slim_wait_for 'Welcome to OVMX' 30 "$SLIM_LOGIN_OFFSET"; then rc=0; else rc=1; SLIM_OK=0; fi
    record "Boot 3: SYSTEM login succeeds (LOGINOUT.EXE activated from disk)" "$rc"
fi

if [ "$SLIM_OK" -eq 1 ]; then
    SLIM_CMD_OFFSET=$(wc -c <"$SLIM_CONSOLE_LOG")
    slim_send 'SHOW TIME'
    if slim_wait_for '$ ' 15 "$SLIM_CMD_OFFSET"; then rc=0; else rc=1; SLIM_OK=0; fi
    record "Boot 3: DCL prompt returns after SHOW TIME (DCL.EXE activated from disk)" "$rc"

    if [ "$rc" -eq 0 ]; then
        SLIM_SEGMENT=$(tail -c "+$((SLIM_CMD_OFFSET + 1))" "$SLIM_CONSOLE_LOG" | tr -d '\r')
        CURYEAR=$(date +%Y)
        if printf '%s' "$SLIM_SEGMENT" | grep -qF "$CURYEAR"; then
            crc=0
        else
            crc=1
        fi
        record "Boot 3: SHOW TIME returns the real date (shareables loaded from disk SYS\$LIBRARY)" "$crc"
    fi
fi

slim_cleanup
trap - EXIT
echo ""

# --- Results ---
echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL PERSISTENT BOOT CHECKS PASSED"
    exit 0
else
    echo "  PERSISTENT BOOT CHECKS FAILED"
    echo ""
    echo "--- Boot 1 full output ---"
    echo "$OUTPUT1"
    echo ""
    echo "--- Boot 2 full output ---"
    echo "$OUTPUT2"
    echo ""
    echo "--- Boot 3 (slim) full console log ---"
    cat "$SLIM_CONSOLE_LOG" 2>/dev/null || echo "(no slim console log captured)"
    exit 1
fi
