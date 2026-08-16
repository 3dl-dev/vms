#!/usr/bin/env bash
#
# boot-ovmx-qemu.sh -- prove OVMX's freestanding Alpha layer boots as PID 1 on
# qemu-system-alpha.
#
# The machine "clipper" that qemu-system-alpha emulates is the 21264/EV6-EV67
# CPU + Tsunami (21272) core logic + OSF/1 PALcode -- i.e. the DS10's COMPUTE
# stack -- and it boots a real Linux/Alpha kernel.  This is the INTERIM proving
# ground for OVMX-on-Alpha (operator ruling 2026-08-10): we lean on qemu here
# until we have a dual-boot faithful emulator (a patched AXPbox, or Charon-AXP)
# or the DS10 itself.  See README.md for why AXPbox is NOT this (it is VMS-
# faithful but its Linux/OSF-1 path is incomplete).
#
# What it does: builds the OVMX self-test init (freestanding: OVMX's own
# crt0.S + syscall.S), embeds it as the initramfs of a minimal Linux/Alpha
# kernel, boots that under qemu-system-alpha, and asserts the OVMX banner.
#
# Runtime: ~15-20 min the first time (kernel cross-build); the kernel source
# tarball is cached in $WORK.  Everything runs in the ovmx-cross-alpha
# container (Rule 9: build/test tooling only, never a runtime).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
WORK="${WORK:-/tmp/ovmx-alpha-qemu}"

echo "== building cross/emulation image ($IMG) =="
docker build -t "$IMG" "$HERE" >/dev/null

mkdir -p "$WORK"
echo "== build OVMX-embedded Linux/Alpha kernel + boot on qemu-system-alpha =="
docker run --rm --memory=6g --cpus="$(nproc)" \
  -v "$REPO/src/libvmssys/arch/alpha:/asm:ro" \
  -v "$HERE/ovmx-alpha-selftest.c:/selftest.c:ro" \
  -v "$WORK:/work" "$IMG" bash -euo pipefail -c "
    cd /work
    KV='$KV'

    # 1. OVMX self-test init, built on OVMX's own freestanding backend.
    alpha-linux-gnu-gcc -static -nostdlib -ffreestanding -fno-builtin -Os \
        /asm/crt0.S /asm/syscall.S /selftest.c -o /work/init
    alpha-linux-gnu-strip /work/init

    # 2. kernel source (cached).
    [ -f linux-\$KV.tar.xz ] || wget -q https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-\$KV.tar.xz
    [ -d linux-\$KV ] || tar xf linux-\$KV.tar.xz
    cd linux-\$KV
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-

    # 3. embed the OVMX init as the built-in initramfs (with /dev/console).
    cat > /work/ovmx.list <<L
dir /dev 755 0 0
nod /dev/console 644 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
file /init /work/init 755 0 0
L
    make defconfig >/dev/null 2>&1
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/ovmx.list \
        --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE
    make olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    echo '-- building vmlinux (this is the long part) --'
    make -j\"\$(nproc)\" vmlinux >/dev/null 2>&1
    alpha-linux-gnu-strip -o /work/vmlinux-ovmx vmlinux

    # 4. boot under qemu-system-alpha (clipper = DS10 compute stack).
    cd /work
    echo '== booting OVMX on qemu-system-alpha -M clipper =='
    timeout 120 qemu-system-alpha -M clipper -smp 1 -m 512 -vga none -nic none \
        -kernel vmlinux-ovmx -append 'console=ttyS0 panic=1' -nographic -no-reboot \
        2>&1 | tee /work/boot.log | grep -avE 'TSUNAMI machine check|tsunami_(read|write)' | tail -40 || true
  "

echo
if grep -aq 'OVMX syscall trampoline + crt0: WORKING on Alpha' "$WORK/boot.log" 2>/dev/null; then
    echo "PASS: OVMX booted as PID 1 on qemu-system-alpha (DS10 compute stack)."
    exit 0
else
    echo "FAIL: OVMX banner not found in $WORK/boot.log"
    exit 1
fi
