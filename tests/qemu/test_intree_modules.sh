#!/bin/bash
# test_intree_modules.sh - the VMS modules are built IN-TREE (vms-934, parent
#                          vms-19e "owns-kernel"): modinfo intree=Y, and loading
#                          them does NOT set TAINT_OUT_OF_TREE.
#
# Runs inside the ovmx-boot Docker image (has QEMU + the from-source OVMX kernel
# + the boot initramfs + /boot/ovmx-distrib.img, and `modinfo` from kmod).
#
# WHAT THIS PROVES (vms-934 acceptance)
#
# Before this item, vms.ko/vmsfs.ko were built OUT-OF-TREE (standalone Makefiles
# under src/kernel/) and loading them set TAINT_OUT_OF_TREE (bit 12, 0x1000) --
# the kernel logged "<mod>: loading out-of-tree module taints kernel". vms-934
# builds them IN-TREE under drivers/ovmx/ of OUR from-source kernel (vms-448), so
# modpost stamps modinfo intree=Y; loading an intree=Y module does NOT set
# TAINT_OUT_OF_TREE. ("In-tree" = OUR tree; no mainline/Linus acceptance.)
#
# Two independent, complementary proofs:
#
#   STATIC (the exact shipped artifacts): extract vms.ko + vmsfs.ko FROM the boot
#     initramfs that actually boots (/boot/initramfs-ovmx.cpio.gz) and assert
#     `modinfo -F intree` == "Y" for each. This is the acceptance criterion #1
#     verbatim, on the bytes that boot. modpost setting intree=Y is exactly the
#     kernel-side condition under which module_enable_taint()/add_taint_module()
#     skips TAINT_OUT_OF_TREE, so it stands in for acceptance criterion #2 as
#     well. This check is discriminating on its own: an out-of-tree build stamps
#     intree "N" (or omits it), reddening here.
#
#   BOOT-TIME (the taint message is gone): boot the real runtime at a RAISED
#     loglevel (the normal boot uses `loglevel=3 quiet`, which would suppress the
#     KERN_WARNING taint line regardless -- so a naive absence check on the
#     normal boot proves nothing). At loglevel=7 a would-be taint warning WOULD
#     reach the console. Assert the modules demonstrably load (%OVMX-I-EXEC =
#     vms.ko attached; %OVMX-I-MOUNTED = vmsfs.ko mounted the ODS-2 disk) AND
#     that no "taints kernel" / "out-of-tree module" line appears. Modules
#     loaded + high loglevel + no taint line = not tainted by these modules.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_intree_modules.sh:/test.sh:ro \
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

echo "=== OVMX in-tree VMS modules gate (vms-934) ==="
echo "Architecture: $ARCH   Kernel: $KERNEL"
echo ""

# --- STATIC: modinfo intree=Y on the SHIPPED modules -----------------------
# Extract the two .ko from the boot initramfs -- the exact artifacts that boot,
# not a side copy -- and modinfo them.
echo "--- STATIC: modinfo intree on the shipped vms.ko / vmsfs.ko ---"
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
    intree=$(modinfo -F intree "$ko" 2>/dev/null)
    echo "    modinfo -F intree ${mod}.ko => '${intree:-<empty>}'"
    [ "$intree" = "Y" ]; record "${mod}.ko modinfo intree=Y (built in OUR tree, no out-of-tree taint)" $?
    vermagic=$(modinfo -F vermagic "$ko" 2>/dev/null)
    echo "    modinfo -F vermagic ${mod}.ko => '${vermagic}'"
    echo "$vermagic" | grep -q -- '-ovmx'; record "${mod}.ko vermagic is the OVMX from-source kernel" $?
done
rm -rf "$WORK"
echo ""

# --- BOOT-TIME: no 'taints kernel' at raised loglevel ----------------------
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64; MACHINE=""; CONSOLE="console=ttyS0"
fi

# Boot to a captured console log, feeding CR to reach login (OPA0: waits for
# the operator's RETURN). loglevel=7 (no `quiet`) so a would-be out-of-tree
# taint warning reaches the console; the normal boot's `loglevel=3 quiet` would
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

echo "--- BOOT-TIME: boot at loglevel=7, assert no out-of-tree taint ---"
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
# The taint line the out-of-tree build produced must be gone.
check "no 'loading out-of-tree module taints kernel'" "$OUT" "loading out-of-tree module" absent
check "no generic 'taints kernel' line"        "$OUT" "taints kernel" absent

echo ""
echo "=== $PASS/$TOTAL passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
