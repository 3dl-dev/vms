#!/bin/bash
#
# run_tests.sh - container entrypoint for the OVMX aarch64 system-emulation
# boot proof (rd vms-f66). The aarch64 sibling of tests/qemu/run_tests.sh
# (Linux vms.ko) and tests/netbsd/run_tests.sh (NetBSD/amd64): it boots a REAL
# arm64 Linux kernel under *system-mode* qemu-system-aarch64 and runs a genuine
# OVMX freestanding arm64 artifact as PID 1.
#
# Positive proof (BLOCKING):
#   Boot the aarch64 initramfs (/init == the EM_AARCH64 OVMX freestanding
#   proof) under qemu-system-aarch64 -machine virt. The guest asserts
#   getpid()==1, uname().machine=="aarch64", read(bad fd)==-EBADF, write(), and
#   prints OVMX_AARCH64_SYSMODE_PASS. PASS requires that marker on the console.
#
# Negative control (BLOCKING -- guards against the "secretly ran x86_64 and
#   printed aarch64 PASS" defect):
#   Boot the SAME arm64 kernel with the x86_64 build of the same /init. A real
#   arm64 kernel cannot exec an EM_X86_64 binary as init, so the PASS marker
#   MUST NOT appear. If it does, the "aarch64" run was not really aarch64 and
#   the whole proof is void -- the harness fails.
#
# HARD TIMEOUT: every qemu invocation is wrapped in `timeout` (a prior QEMU
# proof in this repo once ran unbounded for 1h43m).

set -uo pipefail

KERNEL="${KERNEL:-/boot/vmlinuz-arm64}"
INITRD_AARCH64="${INITRD_AARCH64:-/initramfs-aarch64.cpio.gz}"
INITRD_X86="${INITRD_X86:-/initramfs-x86-negctl.cpio.gz}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-15}"
# The marker the harness keys its verdict on. Overridable ONLY so an outer
# negative control (CI) can point it at a string the guest never prints and
# confirm the grep has teeth -- production runs leave the default.
PASS="${PASS:-OVMX_AARCH64_SYSMODE_PASS}"

boot() {
    # boot <initrd> -- echo the serial transcript, always return 0 (the caller
    # decides the verdict from the transcript, never from qemu's exit code:
    # a clean PSCI poweroff, a panic, and a timeout are all "qemu stopped").
    local initrd="$1"
    timeout --kill-after="$TIMEOUT_GRACE" "$BOOT_TIMEOUT" \
        qemu-system-aarch64 \
            -machine virt -cpu cortex-a57 -m 256M -smp 1 \
            -kernel "$KERNEL" -initrd "$initrd" \
            -append "console=ttyAMA0 earlycon panic=-1 rdinit=/init" \
            -nographic -no-reboot -nic none -nodefaults \
            -serial mon:stdio 2>&1
    return 0
}

echo "======================================================================"
echo "  OVMX aarch64 system-emulation boot proof (rd vms-f66)"
echo "  real arm64 Linux kernel under qemu-system-aarch64 -machine virt"
echo "  PID 1 == OVMX freestanding EM_AARCH64 init"
echo "======================================================================"
qemu-system-aarch64 --version | head -1 || true
echo "kernel: $KERNEL"
file "$KERNEL" 2>/dev/null | sed 's/^/  /'
echo ""

# ---- Positive proof --------------------------------------------------------
echo "== [1/2] POSITIVE: boot aarch64 /init =="
POS="$(boot "$INITRD_AARCH64")"
echo "$POS"
echo ""
if echo "$POS" | grep -q "$PASS"; then
    echo "  -> positive proof: PASS marker present"
    POS_OK=1
else
    echo "  -> positive proof: PASS marker ABSENT (FAIL)"
    POS_OK=0
fi
echo ""

# ---- Negative control ------------------------------------------------------
echo "== [2/2] NEGATIVE CONTROL: boot x86_64 /init on the arm64 kernel =="
echo "   (the arm64 kernel must REFUSE to exec an EM_X86_64 init; no PASS marker)"
NEG="$(boot "$INITRD_X86")"
echo "$NEG"
echo ""
if echo "$NEG" | grep -q "$PASS"; then
    echo "  -> negative control: PASS marker PRESENT -- INVALID (arm64 boot ran an x86_64 binary?!)"
    NEG_OK=0
else
    echo "  -> negative control: PASS marker correctly absent"
    NEG_OK=1
fi
echo ""

echo "======================================================================"
if [ "$POS_OK" = 1 ] && [ "$NEG_OK" = 1 ]; then
    echo "  aarch64 SYSTEM-EMULATION BOOT PROOF: PASSED"
    echo "  (real qemu-system-aarch64 booted a real arm64 kernel and ran the"
    echo "   OVMX freestanding EM_AARCH64 layer as PID 1; x86_64 control rejected)"
    echo "======================================================================"
    exit 0
fi
echo "  aarch64 SYSTEM-EMULATION BOOT PROOF: FAILED"
echo "  positive_marker=$POS_OK  negative_control_ok=$NEG_OK"
echo "======================================================================"
exit 1
