#!/usr/bin/env bash
#
# boot-vmsko-qemu-alpha.sh -- prove the cross-compiled OVMX executive (vms.ko)
# loads under qemu-system-alpha and serves REAL cross-process executive
# facilities via /dev/vms. rd vms-89dd, executive rung A4.
#
# Depends on build-vmsko-alpha.sh having populated $WORK (the Linux/Alpha kernel
# tree + Module.symvers + vms.ko/vmsfs.ko). This script:
#   1. cross-compiles the SAME cross-process suites the x86_64 Kernel Executive
#      CI job runs (test_kmod_eflag_mproc, test_kmod_lock_mproc) for alpha,
#   2. embeds them + ke-init-alpha (PID 1) + the modules as a built-in
#      initramfs (the proven path from boot-ovmx-qemu.sh -- qemu-system-alpha's
#      -initrd on clipper is unreliable, so we bake it into vmlinux),
#   3. boots qemu-system-alpha -M clipper under a HARD timeout, and
#   4. asserts the executive proved the facilities cross-process.
#
# Rule 9: build/test tooling only. Every qemu boot is wrapped in timeout(1) --
# a hung qemu-system-alpha can run for hours.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
WORK="${WORK:-/tmp/ovmx-vmsko-alpha}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-900}"

if [ ! -f "$WORK/vms.ko" ] || [ ! -d "$WORK/linux-$KV" ]; then
    echo "FATAL: run build-vmsko-alpha.sh first ($WORK/vms.ko + kernel tree missing)"
    exit 1
fi

docker build -t "$IMG" "$HERE" >/dev/null

docker run --rm --memory=8g --cpus="$(nproc)" \
  -v "$REPO:/repo:ro" \
  -v "$HERE:/tools:ro" \
  -v "$WORK:/work" "$IMG" bash -euo pipefail -c '
    KV="'"$KV"'"; BT="'"$BOOT_TIMEOUT"'"
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
    CC=alpha-linux-gnu-gcc
    cd /work

    echo "== cross-compile the executive suites + PID-1 init for alpha =="
    # The SAME source the x86_64 KE job builds; -static so the initramfs needs no
    # libc. Include paths: src/kernel (vms_ioctl.h), src/libvmssys (vms_kif.h).
    $CC -static -O2 -Wall \
        -I/repo/src/kernel -I/repo/src/libvmssys \
        /repo/tests/qemu/test_kmod_eflag_mproc.c -o /work/test_eflag
    # test_kmod_lock_mproc includes vms_kif.h and calls the kernel-interface
    # helpers (vms_kif_getlki / vms_kif_deliverast), so -- exactly as the
    # x86_64 KE Dockerfile does for every "#include vms_kif.h" suite -- link it
    # against the real kif implementation in src/libvmssys.
    $CC -static -O2 -Wall \
        -I/repo/src/kernel -I/repo/src/libvmssys \
        /repo/tests/qemu/test_kmod_lock_mproc.c \
        /repo/src/libvmssys/vms_kif.c \
        /repo/src/libvmssys/kif_transport_linux.c \
        /repo/src/libvmssys/vms_string.c \
        /repo/src/libvmssys/arch/alpha/syscall.S \
        -o /work/test_lock
    $CC -static -O2 -Wall /tools/ke-init-alpha.c -o /work/ke-init
    alpha-linux-gnu-strip /work/test_eflag /work/test_lock /work/ke-init
    file /work/ke-init

    echo "== assemble built-in initramfs list =="
    cat > /work/ovmx-ke.list <<L
dir /dev 755 0 0
nod /dev/console 644 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
dir /proc 755 0 0
dir /sys 755 0 0
file /init /work/ke-init 755 0 0
file /vms.ko /work/vms.ko 755 0 0
file /vmsfs.ko /work/vmsfs.ko 755 0 0
file /test_eflag /work/test_eflag 755 0 0
file /test_lock /work/test_lock 755 0 0
L

    echo "== rebuild vmlinux with the executive-proof initramfs baked in =="
    cd /work/linux-$KV
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/ovmx-ke.list
    make olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    make -j"$(nproc)" vmlinux >/dev/null 2>&1
    alpha-linux-gnu-strip -o /work/vmlinux-ke vmlinux

    echo "== boot qemu-system-alpha -M clipper (DS10 compute stack), timeout ${BT}s =="
    cd /work
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
        -kernel vmlinux-ke -append "console=ttyS0 panic=-1" -nographic -no-reboot \
        2>&1 | tee /work/ke-boot.log \
        | grep -avE "TSUNAMI machine check|tsunami_(read|write)" | tail -80 || true
  '

echo
echo "==================== VERDICT ===================="
if grep -aq "OVMX-ALPHA-KE: ALL-PROVEN" "$WORK/ke-boot.log" 2>/dev/null; then
    echo "PASS: vms.ko loaded on qemu-system-alpha; /dev/vms served the"
    echo "      eflag + lock facilities CROSS-PROCESS (A-writes/B-reads)."
    grep -a "OVMX-ALPHA-KE:" "$WORK/ke-boot.log" | sed 's/^/  /'
    exit 0
else
    echo "NOT PROVEN. Executive verdict lines seen (if any):"
    grep -a "OVMX-ALPHA-KE:" "$WORK/ke-boot.log" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
    echo "  --- see $WORK/ke-boot.log for the full boot transcript ---"
    exit 1
fi
