#!/bin/bash
# test_persistent_boot.sh - PID 1 is bootstrap-only: mount the installed disk,
#                           or HALT (vms-2f0)
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the FAT and SLIM
# initramfs variants + /boot/ovmx-distrib.img, the mastered distribution disk).
#
# WHAT THIS PROVES (operator ruling 2026-08-10, docs/design-init-scope.md §6
# "STRIP ALL OF IT")
#
# PID 1 no longer installs, initializes, seeds or provisions ANYTHING. A booting
# VMS system finds its system disk installed or does not boot (§1). This test
# therefore no longer proves "blank disk -> install" (that path is deleted); it
# proves the two halves of the mount-or-halt shape the strip leaves behind:
#
#   POSITIVE: the SLIM initramfs boots the PRE-INSTALLED distribution disk
#             (/boot/ovmx-distrib.img, mastered at BUILD time by vms-8ab, NOT by
#             PID 1) and reaches a SYSTEM login prompt. No install / initialize /
#             overlay code path runs, and %STDRV-I-STARTUP begun PRECEDES the
#             login (the F1 STDRV-bracket fix), with NO "completed" counterpart.
#
#   NEGATIVE (halt control): a BLANK, uninitialized disk -> PID 1 HALTS honestly
#             with the OVMX-facility SYSINIT mount failure. NO boot banner, NO
#             login prompt, NO install message, NO %STARTUP-W-MOUNTFAIL, NO
#             overlay. The disk is NOT initialized and NOT installed. This is run
#             with the FAT initramfs on purpose: the initramfs that used to carry
#             the self-install path now halts instead, proving the path is gone.
#
# A gate that cannot go red is decoration; the negative control is what proves
# the halt discriminates (same reasoning as test_executive_integral.sh Boot
# B/C). The full positive login + DCL-from-disk proof lives in
# test_distrib_boot.sh; this file focuses on the halt and the STDRV ordering.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_persistent_boot.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures

set -uo pipefail

TIMEOUT=90
DISTRIB_IMG=/boot/ovmx-distrib.img
KERNEL=/boot/vmlinuz
FAT_INITRD=/boot/initramfs-ovmx.cpio.gz
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

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qF "$pattern"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern found but should be absent: $pattern)"; FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern not found: $pattern)"; FAIL=$((FAIL + 1))
        fi
    fi
}

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}

# Boot to a captured console log, up to TIMEOUT. No interactive input is needed:
# both the login prompt (positive) and the halt diagnostic (negative) are
# printed by the boot itself before any prompt exists.
run_qemu() {
    local initrd="$1" disk="$2"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$initrd" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$disk",format=raw,if=virtio \
        -no-reboot \
        </dev/null 2>&1 || true
}

echo "=== OVMX mount-or-halt gate (vms-2f0) ==="
echo "Architecture: $ARCH   QEMU: $QEMU"
echo "Kernel: $KERNEL   Distribution image: $DISTRIB_IMG"
echo ""

# --- Precondition: the mastered image exists -------------------------------
if [ -f "$DISTRIB_IMG" ]; then
    record "Mastered distribution image present in the built image" 0
else
    record "Mastered distribution image present in the built image" 1
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi
echo ""

# --- POSITIVE: SLIM initramfs boots the pre-installed disk to login --------
echo "--- POSITIVE: SLIM initramfs + pre-installed distribution disk ---"
POS_DISK="/tmp/mount-or-halt-installed.img"
rm -f "$POS_DISK"
cp "$DISTRIB_IMG" "$POS_DISK"
OUT_POS=$(run_qemu "$SLIM_INITRD" "$POS_DISK")
echo "$OUT_POS" | head -40
echo "[... truncated ...]"
echo ""

check "positive: executive attached"          "$OUT_POS" "%OVMX-I-EXEC"
check "positive: system disk DKA0: mounted"    "$OUT_POS" "%STARTUP-I-MOUNTED"
check "positive: STDRV begun printed"          "$OUT_POS" "%STDRV-I-STARTUP, OpenVMX startup begun"
check "positive: reaches the login prompt"     "$OUT_POS" "Username:"
# The strip: none of the install/initialize/overlay lines may appear.
check "positive: NO install ran"               "$OUT_POS" "%STARTUP-I-INSTALL"    absent
check "positive: NO blank-disk initialize"     "$OUT_POS" "%STARTUP-I-INIT"       absent
check "positive: NO overlay mount-fail warning" "$OUT_POS" "%STARTUP-W-MOUNTFAIL" absent
check "positive: NO honest halt on a good disk" "$OUT_POS" "%OVMX-F-SYSINIT"      absent
# F1: there is exactly ONE STDRV line and it is the "begun" one — the invented
# "completed" line is deleted, not repositioned.
check "positive: NO invented STDRV completed line" "$OUT_POS" "%STDRV-I-STARTUP, OpenVMX startup completed" absent

# F1 ordering: the STDRV "begun" bracket precedes the login prompt. On the
# collapsed pre-fix code they printed together, after startup; here "begun"
# comes first. Compare the byte offsets in the console stream.
BEGUN_POS=$(printf '%s' "$OUT_POS" | grep -aboF "%STDRV-I-STARTUP, OpenVMX startup begun" | head -1 | cut -d: -f1)
USER_POS=$(printf '%s' "$OUT_POS" | grep -aboF "Username:" | head -1 | cut -d: -f1)
if [ -n "$BEGUN_POS" ] && [ -n "$USER_POS" ] && [ "$BEGUN_POS" -lt "$USER_POS" ]; then rc=0; else rc=1; fi
record "positive: %STDRV-I-STARTUP begun precedes the login prompt (F1 bracket)" "$rc"
echo ""

# --- NEGATIVE (halt control): blank disk -> honest halt --------------------
echo "--- NEGATIVE: blank/uninitialized disk + FAT initramfs -> honest halt ---"
NEG_DISK="/tmp/mount-or-halt-blank.img"
rm -f "$NEG_DISK"
truncate -s 64M "$NEG_DISK"
OUT_NEG=$(run_qemu "$FAT_INITRD" "$NEG_DISK")
echo "$OUT_NEG" | head -40
echo "[... truncated ...]"
echo ""

check "negative: executive attached (halt is AFTER the executive)" "$OUT_NEG" "%OVMX-I-EXEC"
check "negative: honest OVMX-facility SYSINIT halt"                 "$OUT_NEG" "%OVMX-F-SYSINIT"
# Either honest halt is acceptable: the blank disk fails to mount as vmsfs, or
# (were an empty volume to mount) it carries no SYS$SYSTEM:DCL.EXE. Both name
# the system disk DKA0: and both are ovmx_sysinit_halt — neither installs it.
check "negative: halt names the system disk DKA0:"                 "$OUT_NEG" "system disk DKA0:"
# The strip: the blank disk must NOT be initialized, installed, or overlaid,
# and the boot must NOT reach a banner or a login prompt.
check "negative: NO blank-disk initialize"       "$OUT_NEG" "%STARTUP-I-INIT"       absent
check "negative: NO install ran"                 "$OUT_NEG" "%STARTUP-I-INSTALL"    absent
check "negative: NO overlay mount-fail warning"  "$OUT_NEG" "%STARTUP-W-MOUNTFAIL"  absent
check "negative: NO boot banner (system did not come up)" "$OUT_NEG" "OpenVMS-compatible" absent
check "negative: NO login prompt on an uninstalled disk"  "$OUT_NEG" "Username:"          absent
check "negative: NO STDRV begun (startup never ran)"      "$OUT_NEG" "%STDRV-I-STARTUP"   absent
echo ""

# --- Results ---
echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL MOUNT-OR-HALT CHECKS PASSED"
    exit 0
else
    echo "  MOUNT-OR-HALT CHECKS FAILED"
    echo ""
    echo "--- POSITIVE full output ---"
    echo "$OUT_POS"
    echo ""
    echo "--- NEGATIVE full output ---"
    echo "$OUT_NEG"
    exit 1
fi
