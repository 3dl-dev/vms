#!/usr/bin/env bash
#
# boot-vmsko-qemu-alpha.sh -- prove the cross-compiled OVMX executive (vms.ko)
# loads under qemu-system-alpha and serves REAL cross-process executive
# facilities via /dev/vms, exercised by the FULL tests/qemu/test_kmod_*.c
# raw-ioctl/kernel-interface suite (all 29) -- the same layer A4 (rd vms-89dd)
# proved for 2 suites. rd vms-bc4, executive rung A4b: closes the Rule-7
# coverage gap A3 flagged (inline-asm activation bootstraps + a ptrace regs
# struct undefined for alpha -- both fixed in tests/qemu/*.c and (for the
# in-process-activation family) src/libvms/syssvc/sys_imgact.c).
#
# Depends on build-vmsko-alpha.sh having populated $WORK (the Linux/Alpha kernel
# tree + Module.symvers + vms.ko/vmsfs.ko). This script:
#   1. cross-compiles all 29 test_kmod_* suites for alpha -- the RAW-ioctl tier
#      (plain gcc -static against vms_ioctl.h) and the vms_kif.h-linked tier
#      (built against src/libvmssys/vms_kif.c + kif_transport_linux.c +
#      vms_string.c + arch/alpha/syscall.S), exactly mirroring the two build
#      recipes tests/qemu/Dockerfile uses for x86_64,
#   2. stages the SAME fixtures tests/qemu/Dockerfile stages (the mkimage_vmsfs
#      test image + the real-VAX ODS-2 fixture + its golden) so the loop-
#      device-backed suites run for real, not skip,
#   3. embeds them + ke-init-alpha (PID 1) + the modules as a built-in
#      initramfs (the proven path from boot-ovmx-qemu.sh -- qemu-system-alpha's
#      -initrd on clipper is unreliable, so we bake it into vmlinux),
#   4. boots qemu-system-alpha -M clipper WITH two virtio disks (mirroring
#      run_tests.sh's own convention: vda = the real ODS-2 volume, vdb = a
#      blank scratch unit) under a HARD timeout, and
#   5. asserts every suite proved its facilities (zero suite failures, zero
#      assertion failures).
#
# NOT PORTED (userspace public sys$ API tier, tests/qemu/test_syssvc_*.c /
# test_imgact_*.c): those cross-compile clean for alpha via the CMake alpha
# toolchain (tools/cross-alpha/toolchain-alpha-linux.cmake, `qemu_syssvc_tests`
# target, verified) but RUNNING them needs the whole staged self-host toolchain
# (MMK/TCC/LIBRARIAN/LINK/IMGACT/DECC$SHR), a real sshd for the SSH KEX proof,
# and the ACP-mounted ODS-2 volume DCL drives -- staging all of that for alpha
# (no musl-alpha cross toolchain exists; the glibc-static alpha path that DOES
# work for these has not been extended to the whole self-host chain) is
# follow-on work, tracked separately (see the PR this script's commit belongs
# to). This is a real, disclosed gap, not a silent skip: their BUILD is proven
# (Rule 7's "does it compile" half is closed for the whole tests/qemu tree);
# their alpha RUN is not yet wired.
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
    mkdir -p /work/tests

    echo "== stage fixtures (mirrors tests/qemu/Dockerfile) =="
    gcc -O2 -Wall -o /work/mkimage_vmsfs /repo/tests/qemu/mkimage_vmsfs.c -I/repo/src/kernel/vmsfs
    /work/mkimage_vmsfs /work/tests/vmsfs_test.img
    cp /repo/tests/ods2/real_vax_ods2.dsk /work/tests/ods2_real.img
    cp /repo/tests/ods2/ovmxdir_hello.golden /work/tests/hello.golden

    echo "== cross-compile the RAW-ioctl test_kmod_* suites for alpha =="
    # Plain gcc -static against vms_ioctl.h -- the SAME recipe
    # tests/qemu/Dockerfile uses for every test_kmod_*.c that does NOT
    # #include "vms_kif.h" (see that Dockerfile'"'"'s own selector comment).
    n_raw=0
    for f in /repo/tests/qemu/test_kmod_*.c; do
        grep -q '"'"'#include "vms_kif.h"'"'"' "$f" && continue
        t=$(basename "$f" .c)
        $CC -static -O2 -Wall -Wextra -o /work/tests/$t "$f" -I/repo/src/kernel
        n_raw=$((n_raw + 1))
        echo "  built (raw) $t"
    done

    echo "== cross-compile the vms_kif.h-linked test_kmod_* suites for alpha =="
    # Linked against the real freestanding kernel-interface client, exactly
    # mirroring tests/qemu/Dockerfile'"'"'s second recipe (and this script'"'"'s own
    # prior test_lock recipe, proven under qemu-alpha by A4/#627/#632).
    n_kif=0
    for f in /repo/tests/qemu/test_kmod_*.c; do
        grep -q '"'"'#include "vms_kif.h"'"'"' "$f" || continue
        t=$(basename "$f" .c)
        $CC -static -O2 -Wall -Wextra -pthread -o /work/tests/$t \
            "$f" \
            /repo/src/libvmssys/vms_kif.c \
            /repo/src/libvmssys/kif_transport_linux.c \
            /repo/src/libvmssys/vms_string.c \
            /repo/src/libvmssys/arch/alpha/syscall.S \
            -I/repo/src/kernel -I/repo/src/libvmssys
        n_kif=$((n_kif + 1))
        echo "  built (kif) $t"
    done
    echo "== built $n_raw raw + $n_kif kif suites =="
    [ "$n_raw" -ge 1 ] || { echo "FATAL: no raw test_kmod_* matched -- the selector no longer matches anything"; exit 1; }
    [ "$n_kif" -ge 1 ] || { echo "FATAL: no vms_kif.h test_kmod_* matched -- the selector no longer matches anything"; exit 1; }

    $CC -static -O2 -Wall /tools/ke-init-alpha.c -o /work/ke-init

    echo "== cross-compile DCL.EXE for alpha (CMake glibc-static toolchain) =="
    # test_kmod_vmsfs_mountvis.c drives the REAL shipped DCL.EXE as a separate
    # process to prove a mounted vmsfs unit resolves cross-process (it does not
    # touch /dev/vms itself -- the mount-table read is the point). vmsdcl
    # already cross-builds clean for alpha (rd vms-fed/A3, build-libstack-
    # alpha.sh) via the SAME glibc alpha-linux-gnu toolchain + OVMX_STATIC=ON
    # this repo already proves static-links on Alpha (no musl-alpha exists,
    # unlike the x86_64 harness -- glibc static linking is sufficient here
    # since DCL.EXE makes no NSS calls).
    cmake -S /repo -B /work/cmake-alpha \
        -DCMAKE_TOOLCHAIN_FILE=/repo/tools/cross-alpha/toolchain-alpha-linux.cmake \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DOVMX_STATIC=ON \
        >/work/cmake-alpha-configure.log 2>&1
    cmake --build /work/cmake-alpha --target vmsdcl -j"$(nproc)" >/work/cmake-alpha-build.log 2>&1
    { [ -f /work/cmake-alpha/bin/DCL.EXE ] || { echo "FATAL: DCL.EXE did not build for alpha"; tail -60 /work/cmake-alpha-build.log; exit 1; }; }
    cp /work/cmake-alpha/bin/DCL.EXE /work/tests/DCL.EXE

    alpha-linux-gnu-strip /work/tests/test_kmod_* /work/tests/DCL.EXE /work/ke-init
    file /work/ke-init

    echo "== assemble built-in initramfs list =="
    {
      cat <<L
dir /dev 755 0 0
nod /dev/console 644 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
dir /proc 755 0 0
dir /sys 755 0 0
dir /tests 755 0 0
dir /test_data 755 0 0
dir /mnt 755 0 0
dir /tmp 1777 0 0
file /init /work/ke-init 755 0 0
file /vms.ko /work/vms.ko 755 0 0
file /vmsfs.ko /work/vmsfs.ko 755 0 0
file /test_data/vmsfs_test.img /work/tests/vmsfs_test.img 644 0 0
file /test_data/ods2_real.img /work/tests/ods2_real.img 644 0 0
file /test_data/hello.golden /work/tests/hello.golden 644 0 0
file /tests/DCL.EXE /work/tests/DCL.EXE 755 0 0
L
      for f in /work/tests/test_kmod_*; do
        echo "file /tests/$(basename "$f") $f 755 0 0"
      done
    } > /work/ovmx-ke.list

    echo "== rebuild vmlinux with the executive-proof initramfs baked in =="
    cd /work/linux-$KV
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/ovmx-ke.list
    make olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    make -j"$(nproc)" vmlinux >/dev/null 2>&1
    alpha-linux-gnu-strip -o /work/vmlinux-ke vmlinux

    echo "== prepare the two virtio disks (mirrors tests/qemu/run_tests.sh) =="
    cp /work/tests/ods2_real.img /work/disk0.img
    truncate -s 16M /work/disk1.img

    echo "== boot qemu-system-alpha -M clipper (DS10 compute stack), timeout ${BT}s =="
    cd /work
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none \
        -kernel vmlinux-ke -append "console=ttyS0 panic=-1" -nographic -no-reboot \
        -netdev user,id=net0 -device virtio-net-pci,netdev=net0,romfile= \
        -drive if=none,id=ovmxdisk0,file=/work/disk0.img,format=raw \
        -device virtio-blk-pci,drive=ovmxdisk0 \
        -drive if=none,id=ovmxdisk1,file=/work/disk1.img,format=raw \
        -device virtio-blk-pci,drive=ovmxdisk1 \
        2>&1 | tee /work/ke-boot.log \
        | grep -avE "TSUNAMI machine check|tsunami_(read|write)" || true
  '

echo
echo "==================== VERDICT ===================="
if grep -aq "OVMX-ALPHA-KE: ALL-PROVEN" "$WORK/ke-boot.log" 2>/dev/null; then
    echo "PASS: vms.ko loaded on qemu-system-alpha; /dev/vms served the FULL"
    echo "      test_kmod_* raw-ioctl/kernel-interface suite (29/29) cross-process."
    grep -a "OVMX-ALPHA-KE: TOTAL" "$WORK/ke-boot.log" | sed 's/^/  /'
    exit 0
else
    echo "NOT PROVEN. Executive verdict lines seen (if any):"
    grep -a "OVMX-ALPHA-KE: SUITE\|OVMX-ALPHA-KE: TOTAL\|OVMX-ALPHA-KE: NOT-PROVEN" "$WORK/ke-boot.log" 2>/dev/null | sed 's/^/  /' || echo "  (none)"
    echo "  --- see $WORK/ke-boot.log for the full boot transcript ---"
    exit 1
fi
