#!/usr/bin/env bash
#
# build-vmsko-alpha.sh -- cross-compile the OVMX executive kernel modules
# (vms.ko + vmsfs.ko) for the Linux/Alpha (AXP) kernel.  rd vms-89dd, epic
# vms-89dd (OVMX-on-Alpha), executive rung A4.
#
# WHY: on the OVMX/Linux-Alpha substrate (operator ruling 2026-08-16) the VMS
# executive on Alpha is `vms.ko` recompiled for the Linux/Alpha kernel and
# reached via /dev/vms -- NOT a NetBSD SYSKRNL port (that is the VAX path).  The
# kernel-core is substrate-agnostic C; this proves it, and the ACP-bearing
# vmsfs.ko codec, cross-compile clean for the alpha target.
#
# This is BUILD/TEST tooling (Rule 9): it never runs OVMX, it builds the module
# that IS the executive and boots it under qemu-system-alpha in a sibling
# script (boot-vmsko-qemu-alpha.sh).
#
# Everything runs in the ovmx-cross-alpha container (tools/cross-alpha/Dockerfile:
# alpha-linux-gnu cross toolchain + qemu-system-alpha + a kernel build chain).
#
# Runtime: ~15-25 min first run (Linux/Alpha kernel cross-build); the kernel
# tree is cached in $WORK so re-runs only rebuild the modules (seconds).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
WORK="${WORK:-/tmp/ovmx-vmsko-alpha}"

echo "== building cross/emulation image ($IMG) =="
docker build -t "$IMG" "$HERE" >/dev/null

mkdir -p "$WORK"
echo "== cross-build Linux/Alpha kernel + vms.ko + vmsfs.ko =="
# Mount the repo src read-only; the module build needs a WRITABLE src tree
# (kbuild writes .o beside .c for external modules), so we rsync it into /work.
docker run --rm --memory=8g --cpus="$(nproc)" \
  -v "$REPO:/repo:ro" \
  -v "$WORK:/work" "$IMG" bash -euo pipefail -c '
    KV="'"$KV"'"
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
    cd /work

    # ---- 1. Linux/Alpha kernel tree (cached) ----
    [ -f linux-$KV.tar.xz ] || wget -q https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KV.tar.xz
    [ -d linux-$KV ] || tar xf linux-$KV.tar.xz
    cd linux-$KV

    if [ ! -f .ovmx-kernel-built ]; then
        make defconfig >/dev/null 2>&1
        # Modules + a bootable-enough config for the sibling boot script:
        # loadable modules, devtmpfs (so /dev/vms auto-appears), initramfs,
        # 8250 serial console, tmpfs.
        ./scripts/config \
            --enable MODULES --enable MODULE_UNLOAD \
            --disable MODVERSIONS \
            --enable DEVTMPFS --enable DEVTMPFS_MOUNT \
            --enable BLK_DEV_INITRD \
            --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
            --enable TMPFS --enable PROC_FS --enable SYSFS \
            --enable BLK_DEV --enable BLK_DEV_LOOP \
            --enable VIRTIO --enable VIRTIO_PCI --enable VIRTIO_BLK --enable VIRTIO_NET
        make olddefconfig >/dev/null 2>&1
        echo "-- building vmlinux + modules (the long part) --"
        # `make modules` is REQUIRED, not just `vmlinux modules_prepare`: modpost
        # of an EXTERNAL module resolves the vmlinux-exported symbols (filp_open,
        # iget_locked, misc_deregister, _copy_from_user, memmove, ...) against
        # Module.symvers, and Module.symvers is generated only by the modules
        # phase (modpost over vmlinux.o). Without it modpost errors out with
        # "Module.symvers is missing" + a wall of undefined symbols even though
        # every .o compiled clean. (Learned building vms.ko/vmsfs.ko for alpha.)
        make -j"$(nproc)" vmlinux modules >/dev/null 2>&1
        test -f Module.symvers || { echo "FATAL: Module.symvers not generated"; exit 1; }
        alpha-linux-gnu-strip -o /work/vmlinux-alpha vmlinux
        touch .ovmx-kernel-built
        echo "-- kernel built --"
    else
        echo "-- kernel already built (cached) --"
    fi
    KDIR=/work/linux-$KV

    # ---- 2. writable copy of the module source tree ----
    rm -rf /work/src
    cp -a /repo/src /work/src

    echo
    echo "############ CROSS-COMPILE vms.ko (executive) ############"
    set +e
    make -C "$KDIR" M=/work/src/kernel ARCH=alpha CROSS_COMPILE=alpha-linux-gnu- modules \
        > /work/build-vms.log 2>&1
    RC_VMS=$?
    set -e
    tail -40 /work/build-vms.log
    if [ $RC_VMS -eq 0 ] && [ -f /work/src/kernel/vms.ko ]; then
        echo "RESULT vms.ko: BUILT"
        alpha-linux-gnu-objdump -f /work/src/kernel/vms.ko | grep -i "architecture" || true
        cp /work/src/kernel/vms.ko /work/vms.ko
    else
        echo "RESULT vms.ko: FAILED (rc=$RC_VMS) -- see /work/build-vms.log"
    fi

    echo
    echo "############ CROSS-COMPILE vmsfs.ko (ACP-bearing ODS-2 codec) ############"
    set +e
    make -C "$KDIR" M=/work/src/kernel/vmsfs ARCH=alpha CROSS_COMPILE=alpha-linux-gnu- modules \
        > /work/build-vmsfs.log 2>&1
    RC_VMSFS=$?
    set -e
    tail -60 /work/build-vmsfs.log
    if [ $RC_VMSFS -eq 0 ] && [ -f /work/src/kernel/vmsfs/vmsfs.ko ]; then
        echo "RESULT vmsfs.ko: BUILT"
        alpha-linux-gnu-objdump -f /work/src/kernel/vmsfs/vmsfs.ko | grep -i "architecture" || true
        cp /work/src/kernel/vmsfs/vmsfs.ko /work/vmsfs.ko
    else
        echo "RESULT vmsfs.ko: FAILED (rc=$RC_VMSFS) -- see /work/build-vmsfs.log"
    fi

    echo
    echo "############ SUMMARY ############"
    ls -la /work/*.ko 2>/dev/null || echo "(no .ko produced)"
    file /work/vms.ko /work/vmsfs.ko 2>/dev/null || true
  '
echo "== done; artifacts + logs in $WORK =="
