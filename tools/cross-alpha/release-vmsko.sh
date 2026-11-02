#!/bin/sh
# release-vmsko.sh - runs INSIDE the ovmx-cross-alpha container, driven by
# tools/cut-release-alpha.sh (rd vms-ec1, epic vms-f10). Cross-builds the
# Linux/Alpha 6.6.52 kernel tree and the OVMX executive module vms.ko from the
# archived source tree, DETERMINISTICALLY, and writes /out/vms.ko.
#
# Reproducibility (proven by cut-release-reproducible, which cuts twice with
# --no-cache and cmp's alpha/vms.ko): the kernel tarball is sha256-pinned; the
# kernel config is the SAME one tools/cross-alpha/build-vmsko-alpha.sh uses;
# the build clock/user/host are pinned from SOURCE_DATE_EPOCH; the module is
# built from a copy of src at a FIXED path (no build-dir bytes vary); the .ko
# is debug-stripped. The Linux/Alpha kernel config does not enable MODULE_SIG,
# so the module is UNSIGNED -- there is no signature to make deterministic
# (the x86_64/aarch64 in-tree builds sign; Alpha does not, by construction).
#
# Env: SDE (required, nonnegative integer). Mounts: /src (ro), /out (rw).
set -eu
KV=6.6.52
KSHA=1591ab348399d4aa53121158525056a69c8cf0fe0e90935b0095e9a58e37b4b8
: "${SDE:?SDE (SOURCE_DATE_EPOCH) required}"
export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
export SOURCE_DATE_EPOCH="$SDE" KBUILD_BUILD_USER=ovmx KBUILD_BUILD_HOST=ovmx
export KBUILD_BUILD_TIMESTAMP="$(date -u -d "@$SDE" '+%a %b %e %H:%M:%S UTC %Y')"
export TZ=UTC LC_ALL=C

W=/work; mkdir -p "$W"; cd "$W"
wget -q "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KV.tar.xz"
echo "$KSHA  linux-$KV.tar.xz" | sha256sum -c -
tar xf "linux-$KV.tar.xz"
cd "linux-$KV"
for p in /src/tools/cross-alpha/patches/*.patch; do
    [ -e "$p" ] || continue
    patch -p1 --forward <"$p"
done
make defconfig >/dev/null 2>&1
./scripts/config \
    --enable MODULES --enable MODULE_UNLOAD --disable MODVERSIONS \
    --enable DEVTMPFS --enable DEVTMPFS_MOUNT --enable BLK_DEV_INITRD \
    --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
    --enable TMPFS --enable PROC_FS --enable SYSFS \
    --enable BLK_DEV --enable BLK_DEV_LOOP \
    --enable VIRTIO --enable VIRTIO_PCI --enable VIRTIO_BLK --enable VIRTIO_NET
make olddefconfig >/dev/null 2>&1
make -j"$(nproc)" vmlinux modules >"$W/kernel-build.log" 2>&1 \
    || { tail -40 "$W/kernel-build.log"; echo "FATAL: Linux/Alpha kernel build failed"; exit 1; }
[ -f Module.symvers ] || { echo "FATAL: Module.symvers not generated"; exit 1; }

# Module source at a fixed path (kbuild writes objects beside sources).
cp -a /src/src /work/src
make -C "$W/linux-$KV" M=/work/src/kernel modules >"$W/vms-build.log" 2>&1 \
    || { tail -40 "$W/vms-build.log"; echo "FATAL: vms.ko cross-build failed"; exit 1; }
[ -f /work/src/kernel/vms.ko ] || { echo "FATAL: vms.ko not produced"; exit 1; }
alpha-linux-gnu-strip --strip-debug -o /out/vms.ko /work/src/kernel/vms.ko
alpha-linux-gnu-objdump -f /out/vms.ko | grep -qi 'alpha' || { echo "FATAL: vms.ko is not an Alpha object"; exit 1; }
echo "release-vmsko: vms.ko $(sha256sum /out/vms.ko)"
