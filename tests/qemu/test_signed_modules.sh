#!/bin/bash
# test_signed_modules.sh - the VMS modules are SIGNED (vms-ff5, parent vms-19e
#                          "owns-kernel"): modinfo shows a signature, and loading
#                          them does NOT set TAINT_UNSIGNED_MODULE (bit 13 / 'E').
#
# Runs inside the ovmx-boot Docker image (has QEMU + the from-source OVMX kernel
# + the boot initramfs + /boot/ovmx-distrib.img, and `modinfo` from kmod).
#
# WHAT THIS PROVES (vms-ff5 acceptance)
#
# vms-934 built vms.ko/vmsfs.ko IN-TREE (no TAINT_OUT_OF_TREE, bit 12). They were
# still UNSIGNED, so with module-signature verification off the kernel did not
# complain -- but the moment verification is enabled an unsigned module sets
# TAINT_UNSIGNED_MODULE (bit 13 / 'E') and the kernel logs "module verification
# failed: signature and/or required key missing - tainting kernel". vms-ff5
# enables CONFIG_MODULE_SIG=y (module verification ON) and signs both modules
# with a COMMITTED OVMX key whose certificate is in the kernel's built-in
# trusted keyring, so finit_module() VERIFIES the signature at load and does NOT
# set bit 13. CONFIG_MODULE_SIG_FORCE is deliberately NOT set (deferred), so an
# unverifiable module would taint-but-load, never brick the boot -- which is
# exactly the condition this test discriminates.
#
# Two independent, complementary proofs (mirroring test_intree_modules.sh):
#
#   STATIC (the exact shipped artifacts): extract vms.ko + vmsfs.ko FROM the boot
#     initramfs that actually boots (/boot/initramfs-ovmx.cpio.gz) and assert
#     each carries a signature -- `modinfo -F signer` is non-empty and
#     `modinfo -F sig_id` names a PKCS#7 signature. An UNSIGNED module has no
#     signer/sig_id field, so this reddens on any regression that ships an
#     unsigned module (the exact bit-13 condition). This is acceptance
#     criterion "modinfo shows a sig", on the bytes that boot.
#
#   BOOT-TIME (bit 13 is not set): boot the real runtime at a RAISED loglevel
#     (the normal boot uses `loglevel=3 quiet`, which would suppress the
#     KERN_NOTICE taint line regardless -- so a naive absence check on the
#     normal boot proves nothing). At loglevel=7 the "module verification
#     failed ... tainting kernel" line WOULD reach the console if either module
#     were unverifiable. Assert the modules demonstrably load (%OVMX-I-EXEC =
#     vms.ko attached; %OVMX-I-MOUNTED = vmsfs.ko mounted the ODS-2 disk) AND
#     that no "module verification failed" / "signature and/or required key
#     missing" line appears. Modules loaded + verification enabled (the shipped
#     kernel is built CONFIG_MODULE_SIG=y, gated in the Dockerfile) + high
#     loglevel + no verification-failure line = bit 13 not set by these modules.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_signed_modules.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

TIMEOUT=90
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qaiF "$pattern"; then
        if [ "$expect" = "present" ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
        else echo "  FAIL: $desc (found but should be absent: $pattern)"; FAIL=$((FAIL + 1)); fi
    else
        if [ "$expect" = "absent" ]; then echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else echo "  FAIL: $desc (not found: $pattern)"; FAIL=$((FAIL + 1)); fi
    fi
}

echo "=== OVMX signed VMS modules gate (vms-ff5) ==="
echo "Architecture: $ARCH   Kernel: $KERNEL"
echo ""

# --- STATIC: modinfo signature on the SHIPPED modules ----------------------
# Extract the two .ko from the boot initramfs -- the exact artifacts that boot,
# not a side copy -- and prove each is signed.
echo "--- STATIC: modinfo signature on the shipped vms.ko / vmsfs.ko ---"
WORK=$(mktemp -d)
if gzip -dc "$INITRD" | ( cd "$WORK" && cpio -idm 2>/dev/null ); then
    record "boot initramfs unpacked" 0
else
    record "boot initramfs unpacked" 1
    echo "FATAL: could not unpack $INITRD"; exit 1
fi

for mod in vms vmsfs; do
    ko=$(find "$WORK" -name "${mod}.ko" | head -1)
    if [ -z "$ko" ]; then
        record "${mod}.ko present in boot initramfs" 1
        continue
    fi
    record "${mod}.ko present in boot initramfs" 0
    signer=$(modinfo -F signer "$ko" 2>/dev/null)
    echo "    modinfo -F signer ${mod}.ko  => '${signer:-<empty>}'"
    [ -n "$signer" ]; record "${mod}.ko is SIGNED (modinfo signer present -- no unsigned-module taint)" $?
    sigid=$(modinfo -F sig_id "$ko" 2>/dev/null)
    echo "    modinfo -F sig_id ${mod}.ko  => '${sigid:-<empty>}'"
    echo "$sigid" | grep -qi 'PKCS#7'; record "${mod}.ko signature is a PKCS#7 appended sig" $?
    sighash=$(modinfo -F sig_hashalgo "$ko" 2>/dev/null)
    echo "    modinfo -F sig_hashalgo ${mod}.ko => '${sighash:-<empty>}'"
    [ -n "$sighash" ]; record "${mod}.ko signature names a hash algorithm" $?
done
rm -rf "$WORK"
echo ""

# --- BOOT-TIME: no unsigned-module taint at raised loglevel ----------------
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64; MACHINE=""; CONSOLE="console=ttyS0"
fi

# Boot to a captured console log, feeding CR to reach login (OPA0: waits for
# the operator's RETURN). loglevel=7 (no `quiet`) so a would-be unsigned-module
# taint notice reaches the console; the normal boot's `loglevel=3 quiet` would
# hide it and make an absence check meaningless.
run_qemu() {
    local disk="$1" log fifo qp w=0
    log=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" -nographic \
        -append "$CONSOLE loglevel=7" \
        -m 256M -smp 1 -nic none -nodefaults -serial stdio \
        -drive file="$disk",format=raw,if=virtio -no-reboot \
        <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 6>"$fifo"; trap '' PIPE
    while kill -0 "$qp" 2>/dev/null; do
        grep -qaF 'Username:' "$log" 2>/dev/null && break
        printf '\r' >&6 2>/dev/null
        sleep 1; w=$((w + 1)); [ "$w" -ge "$TIMEOUT" ] && break
    done
    exec 6>&-; kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null
    cat "$log"; rm -f "$log" "$fifo"
}

echo "--- BOOT-TIME: boot at loglevel=7, assert no unsigned-module taint ---"
if [ ! -f "$DISTRIB_IMG" ]; then
    record "distribution image present" 1
    echo "FATAL: $DISTRIB_IMG missing"; exit 1
fi
DISK=$(mktemp -u).img
cp "$DISTRIB_IMG" "$DISK"
OUT=$(run_qemu "$DISK")
rm -f "$DISK"
echo "$OUT" | tail -30
echo "[... console truncated ...]"
echo ""

# Positive: the modules actually loaded (so 'no taint line' is meaningful, not
# just 'nothing loaded').
check "vms.ko loaded (executive attached)"     "$OUT" "%OVMX-I-EXEC" present
check "vmsfs.ko loaded (ODS-2 disk mounted)"   "$OUT" "%OVMX-I-MOUNTED" present
# The bit-13 taint notice an unsigned/unverifiable module produces must be gone.
check "no 'module verification failed' notice"        "$OUT" "module verification failed" absent
check "no 'signature and/or required key missing'"    "$OUT" "signature and/or required key missing" absent

echo ""
echo "=== $PASS/$TOTAL passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
