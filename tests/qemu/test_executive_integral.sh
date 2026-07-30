#!/bin/bash
# test_executive_integral.sh - OVMX does not come up without its executive
#
# Runs inside the ovmx-boot image (has QEMU + kernel + initramfs variants).
#
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_executive_integral.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# WHAT THIS PROVES (epic vms-6b8 / item vms-0ff)
#
# The executive is INTEGRAL, not optional. On OpenVMS, SYSBOOT loads the
# executive before any process exists -- VMS is NEVER in the state where a
# running system has no executive. So OVMX does not get to have that state
# either, and the deliverable is a GUARANTEE, not an error path:
#
#   PID 1 refuses to bring the system up unless vms.ko loads and /dev/vms
#   opens (src/ovmx_init/ovmx_init.c, executive_attach).
#
# That guarantee is what licenses deleting the per-call "what if the executive
# isn't there?" fallbacks from sys_lock.c and vms_kif.c. If it silently
# regressed, those deletions would turn into real bugs and nothing else in the
# suite would notice -- a system booting happily WITHOUT an executive looks
# exactly like a system booting happily WITH one, right up until a system
# service is called. Hence this test.
#
# Boot A (executive present) is the positive case. Boot B (vms.ko removed from
# the initramfs) is the negative control: it proves the check can actually
# fail, and fail for the right reason. A gate that cannot go red is not a gate
# -- that lesson is why the kernel-executive CI job ships with its own
# negative control too.
#
# Exit code 0 = all checks pass, 1 = failures

set -uo pipefail

TIMEOUT=90
KERNEL=/boot/vmlinuz
INITRD_OK=/boot/initramfs-ovmx.cpio.gz
INITRD_NOEXEC=/boot/initramfs-ovmx-noexec.cpio.gz
ARCH=$(uname -m)

PASS=0
FAIL=0

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
    if echo "$output" | grep -qF "$pattern"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (found but must be absent: $pattern)"; FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (not found: $pattern)"; FAIL=$((FAIL + 1))
        fi
    fi
}

run_qemu() {
    local initrd="$1" disk="$2"
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

echo "=== OVMX Executive-Integral Test ==="
echo "Architecture: $ARCH / QEMU: $QEMU"
echo ""

if [ ! -f "$INITRD_NOEXEC" ]; then
    echo "FAIL: $INITRD_NOEXEC is missing — the negative control cannot run."
    echo "  -> distro/Dockerfile.bootable must build the NOEXEC initramfs."
    exit 1
fi

# --- Boot A: executive present → the system comes up --------------------
echo "--- Boot A: executive present ---"
DISK_A=/tmp/exec-integral-a.img
rm -f "$DISK_A"; truncate -s 64M "$DISK_A"
OUT_A=$(run_qemu "$INITRD_OK" "$DISK_A")

check "Boot A: executive attached" "$OUT_A" '%STARTUP-I-EXEC'
check "Boot A: system came up (boot banner)" "$OUT_A" 'OVMX V0.1'
check "Boot A: no fatal executive error" "$OUT_A" '%STARTUP-F-NOEXEC' absent
check "Boot A: startup not aborted" "$OUT_A" '%STARTUP-F-NOBOOT' absent
echo ""

# --- Boot B (NEGATIVE CONTROL): no executive → the system must NOT come up ---
# Same kernel, same disk layout, same STARTUP.EXE. The ONLY difference is that
# /lib/modules/vms.ko is absent from the initramfs. If OVMX still reaches a
# login prompt here, the guarantee is gone.
echo "--- Boot B (negative control): executive absent ---"
DISK_B=/tmp/exec-integral-b.img
rm -f "$DISK_B"; truncate -s 64M "$DISK_B"
OUT_B=$(run_qemu "$INITRD_NOEXEC" "$DISK_B")

check "Boot B: reports the executive could not be loaded" "$OUT_B" '%STARTUP-F-NOEXEC'
check "Boot B: startup aborted"                           "$OUT_B" '%STARTUP-F-NOBOOT'
# The load-bearing assertions: the system genuinely did NOT come up. Before
# vms-0ff, this boot printed a severity-W warning and proceeded to a full DCL
# session with no executive at all.
check "Boot B: NO boot banner (system did not come up)" "$OUT_B" 'OVMX V0.1'  absent
check "Boot B: NO login prompt"                         "$OUT_B" 'Username:'  absent
check "Boot B: startup did not complete"                "$OUT_B" '%STDRV-I-STARTUP, OVMX startup completed' absent
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "=== RESULTS: $PASS checks passed, 0 failed ==="
    echo "ALL EXECUTIVE-INTEGRAL CHECKS PASSED"
    exit 0
fi

echo "=== RESULTS: $PASS passed, $FAIL FAILED ==="
echo ""
echo "--- Boot B console output (executive absent) ---"
echo "$OUT_B" | tail -40
exit 1
