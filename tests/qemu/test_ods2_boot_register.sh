#!/bin/bash
# test_ods2_boot_register.sh - PID 1 registers the boot device DKA0: into the
# userspace ODS-2 MOUNTED-VOLUME table at boot, ADDITIVELY (vms-351, epic
# vms-5eb "B2", docs/design-ods2-runtime-flip.md §4).
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the SLIM
# initramfs + /boot/ovmx-distrib.img, the mastered distribution disk) -- the
# SAME image tests/qemu/test_boot_conformance.sh / test_persistent_boot.sh run
# against, and this file reuses that harness's CR-wake run_qemu() verbatim.
#
# WHAT THIS PROVES, AND WHY IT IS A "LIGHTER" ASSERTION.
#
# The vms-351 done-condition -- a cached ods2_bdev_t handle resolving a real
# system file over the block device -- is proven fully by the host ctest
# tests/ods2/test_ods2_volume.c, which drives the SAME vmsfs_volume_register()
# call PID 1 makes, over a genuine ODS-2 loop image. A full boot-time RESOLVE
# is not yet feasible on the REAL boot disk: the mastered /boot/ovmx-distrib.img
# is still the bespoke VMFS format (the R6 boot-master flip to genuine ODS-2 has
# not landed), so ods2_bdev_open(/dev/vda) honestly reports "not a DECFILE11B
# volume" and registers nothing. What this QEMU test adds, that the host ctest
# cannot, is that PID 1 (the REAL init) actually RUNS the registration against
# the REAL block device at boot, AND that adding it broke nothing -- boot still
# reaches the login prompt through the untouched /vms passthrough (atomic-safe).
#
# The registration is SILENT on the normal console (the boot facility+ident
# sequence is pinned to the oracle -- test_boot_conformance.sh -- and must not
# gain a line); its outcome is surfaced ONLY under the ovmx.ods2reg boot flag,
# exactly as report_kernel_taint() gates its readout behind ovmx.taintreport. So
# this test boots WITH ovmx.ods2reg and asserts:
#
#   1. PID 1 emitted the "%OVMX-I-ODS2VOL, DKA0: ..." registration readout --
#      proving register_system_volume() (src/ovmx_init/ovmx_init.c) ran against
#      the real /dev/vda at boot.
#   2. On today's bespoke-VMFS boot disk that readout is the FAIL-HONEST variant
#      ("not a genuine ODS-2 volume ...; handle not registered") -- proving
#      INV-6 / Rule 9: no per-process fake when the device is not (yet) ODS-2.
#      When R6 masters the boot disk as genuine ODS-2, the SAME test will see
#      the "registered as genuine ODS-2 volume handle" variant instead; both
#      count as "registration happened".
#   3. Boot still reached the "Username:" login prompt -- the additive change
#      flipped no live path (the /vms POSIX passthrough is untouched).
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_ods2_boot_register.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

TIMEOUT=90
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

FAIL=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; FAIL=1; }

# Boot to a captured console log, up to TIMEOUT, feeding a CR every second so
# LOGINOUT presents "Username:" on OPA0: (the "press RETURN to log in" console
# behaviour). REUSED VERBATIM from test_boot_conformance.sh (vms-2213/#358),
# with ONE difference: the -append carries the ovmx.ods2reg boot flag so PID 1
# emits its %OVMX-I-ODS2VOL registration readout (default boots keep it silent).
run_qemu() {
    local initrd="$1" disk="$2" log fifo qp w=0
    log=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$initrd" \
        -nographic \
        -append "$CONSOLE loglevel=3 ovmx.ods2reg" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$disk",format=raw,if=virtio \
        -no-reboot \
        <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 6>"$fifo"
    trap '' PIPE   # a CR fed just as the guest exits must not kill this subshell
    while kill -0 "$qp" 2>/dev/null; do
        grep -qaF 'Username:' "$log" 2>/dev/null && break
        printf '\r' >&6 2>/dev/null
        sleep 1; w=$((w + 1))
        [ "$w" -ge "$TIMEOUT" ] && break
    done
    exec 6>&-
    kill "$qp" 2>/dev/null
    wait "$qp" 2>/dev/null
    cat "$log"
    rm -f "$log" "$fifo"
}

echo "=== OVMX PID-1 ODS-2 mounted-volume registration (vms-351) ==="
echo "Architecture: $ARCH   QEMU: $QEMU"
echo "Kernel: $KERNEL   Distribution image: $DISTRIB_IMG"
echo ""

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi

DISK="/tmp/ods2-boot-register.img"
rm -f "$DISK"
cp "$DISTRIB_IMG" "$DISK"
OUT=$(run_qemu "$SLIM_INITRD" "$DISK")
echo "$OUT" | head -80
echo "[... see full transcript below on failure ...]"
echo ""

CLEAN=$(printf '%s' "$OUT" | tr -d '\r')

# --- (1) PID 1 ran the registration against the real /dev/vda ----------------
if printf '%s\n' "$CLEAN" | grep -qE '%OVMX-I-ODS2VOL, DKA0:'; then
    pass "PID 1 emitted the %OVMX-I-ODS2VOL registration readout (register_system_volume ran)"
else
    bad "no %OVMX-I-ODS2VOL registration readout on the console under ovmx.ods2reg"
fi

# --- (2) fail-honest on today's bespoke-VMFS disk (INV-6 / Rule 9) -----------
# The mastered boot disk is not yet genuine ODS-2, so the honest outcome is
# "not registered", never a fabricated success. Accept EITHER wording so this
# test keeps passing when R6 flips the boot master to genuine ODS-2 (then the
# "registered as genuine ODS-2 volume handle" variant fires instead).
if printf '%s\n' "$CLEAN" | grep -qE '%OVMX-I-ODS2VOL, DKA0: not a genuine ODS-2 volume .*handle not registered'; then
    pass "fail-honest: DKA0: reported not-yet-ODS-2, handle not registered (INV-6, no per-process fake)"
elif printf '%s\n' "$CLEAN" | grep -qE '%OVMX-I-ODS2VOL, DKA0: registered as genuine ODS-2 volume handle'; then
    pass "DKA0: registered as a genuine ODS-2 volume handle (R6 boot master has flipped)"
else
    bad "the %OVMX-I-ODS2VOL readout is neither the fail-honest nor the registered variant"
fi

# --- (3) atomic-safe: boot still reaches login through the /vms passthrough ---
if printf '%s\n' "$CLEAN" | grep -qF 'Username:'; then
    pass "boot still reached the Username: login prompt (additive; no live path flipped)"
else
    bad "boot did not reach Username: — the additive registration broke boot-to-login"
fi

echo ""
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL ODS-2 BOOT-REGISTER CHECKS PASSED"
    echo "=========================================="
    exit 0
else
    echo "  ODS-2 BOOT-REGISTER CHECKS FAILED"
    echo "=========================================="
    echo ""
    echo "--- full console transcript ---"
    echo "$OUT"
    exit 1
fi
