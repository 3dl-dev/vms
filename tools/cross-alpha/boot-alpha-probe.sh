#!/usr/bin/env bash
#
# boot-alpha-probe.sh -- diagnostic: boot one of the vms-989 Alpha probe inits
# as PID 1 under qemu-system-alpha (disk on /dev/vda) to localize the PROVISION
# frontier stall.  rd vms-989, rung A5a.
#
# Selectable via PROBE (default: provision):
#   provision  alpha-provision-probe.c      -- attribute the stall to a single
#              (links kif)                      primitive: vmsfs read / write /
#                                               mkdir / lchown / establish_system.
#   contention alpha-contention-probe.c     -- parent HOLDS a /dev/vms
#              (links kif)                      attachment, forked child runs the
#                                               primitives -- tests concurrency.
#   exec       alpha-exec-provision-init.c  -- fork+execl the REAL PROVISION.EXE
#              (no kif)                         off the mounted disk (the
#                                               definitive reproduction).
#
# Depends on build-vmsko-alpha.sh ($VMSKO_WORK) and build-alpha-bootimage.sh
# ($WORK: ovmx-distrib-alpha.img).  Test tooling only (Rule 9); hard timeout.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
VMSKO_WORK="${VMSKO_WORK:-/tmp/ovmx-vmsko-alpha}"
WORK="${WORK:-/tmp/ovmx-alpha-boot}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-160}"
PROBE="${PROBE:-provision}"

case "$PROBE" in
    provision)  PROBE_SRC=alpha-provision-probe.c;     LINK_KIF=1 ;;
    contention) PROBE_SRC=alpha-contention-probe.c;    LINK_KIF=1 ;;
    exec)       PROBE_SRC=alpha-exec-provision-init.c; LINK_KIF=0 ;;
    *) echo "unknown PROBE='$PROBE' (want: provision|contention|exec)"; exit 1 ;;
esac
echo "== PROBE=$PROBE ($PROBE_SRC) =="

docker build -t "$IMG" "$HERE" >/dev/null
docker run --rm --memory=8g --cpus="$(nproc)" \
  -v "$REPO":/repo:ro -v "$HERE":/tools:ro \
  -v "$VMSKO_WORK":/vmsko -v "$WORK":/work "$IMG" bash -euo pipefail -c '
    KV="'"$KV"'"; BT="'"$BOOT_TIMEOUT"'"; SRC="'"$PROBE_SRC"'"; LINK_KIF="'"$LINK_KIF"'"
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
    CC=alpha-linux-gnu-gcc
    echo "== cross-compile the probe init ($SRC) =="
    if [ "$LINK_KIF" = 1 ]; then
        $CC -static -O2 -Wall \
            -I/repo/src/kernel -I/repo/src/libvmssys \
            /tools/$SRC \
            /repo/src/libvmssys/vms_kif.c \
            /repo/src/libvmssys/kif_transport_linux.c \
            /repo/src/libvmssys/vms_string.c \
            /repo/src/libvmssys/arch/alpha/syscall.S \
            -o /work/probe-init
    else
        $CC -static -O2 -Wall /tools/$SRC -o /work/probe-init
    fi
    alpha-linux-gnu-strip /work/probe-init

    cat > /work/probe.list <<L
dir /dev 755 0 0
nod /dev/console 644 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
dir /proc 755 0 0
dir /sys 755 0 0
dir /vms 755 0 0
file /init /work/probe-init 755 0 0
file /vms.ko /vmsko/vms.ko 644 0 0
L
    echo "== bake probe kernel =="
    cd /vmsko/linux-$KV
    # OVMX kernel patches (rd vms-80d): idempotently ensure applied to the
    # (possibly stale-cached) tree before make. `--dry-run --forward` no-ops an
    # already-patched tree.
    for p in /repo/tools/cross-alpha/patches/*.patch; do
        [ -e "$p" ] || continue
        if patch -p1 --dry-run --forward <"$p" >/dev/null 2>&1; then
            echo "   applying kernel patch: $(basename "$p")"
            patch -p1 --forward <"$p"
        fi
    done
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/probe.list \
        --enable VIRTIO --enable VIRTIO_PCI --enable VIRTIO_BLK \
        --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
        --enable DEVTMPFS --enable DEVTMPFS_MOUNT --enable TMPFS --enable BINFMT_ELF
    make ARCH=alpha olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    make ARCH=alpha -j"$(nproc)" vmlinux >/work/kbuild-probe.log 2>&1
    alpha-linux-gnu-strip -o /work/vmlinux-probe vmlinux

    echo "== boot probe (disk on /dev/vda), timeout ${BT}s =="
    cd /work
    rm -f diskP.img; cp ovmx-distrib-alpha.img diskP.img
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
        -kernel vmlinux-probe -append "console=ttyS0 panic=-1" \
        -drive file=diskP.img,format=raw,if=virtio \
        -nographic -no-reboot > /work/probe.raw 2>&1 || true
    grep -avE "TSUNAMI machine check|tsunami_(read|write)" /work/probe.raw > /work/probe.log || true
  '
echo
echo "==================== PROBE RESULT ===================="
grep -a "PROBE:" "$WORK/probe.log" 2>/dev/null | sed 's/^/  /' || echo "  (no PROBE lines -- see $WORK/probe.log)"
echo
echo "The LAST 'PROBE:' line before silence names the stalling primitive."
