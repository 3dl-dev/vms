#!/usr/bin/env bash
#
# run-syssvc-tests-alpha.sh -- rd vms-341 (epic vms-8954), the RUN-half of the
# Alpha userspace test coverage: boot qemu-system-alpha -M clipper and RUN the
# tests/qemu/test_syssvc_*/test_imgact_* suite (the qemu_syssvc_tests target,
# ~66 tests) in-guest against a real /dev/vms executive + the genuine ODS-2
# fixture volumes, collecting PASS/FAIL. The userspace analog of
# boot-vmsko-qemu-alpha.sh (which does the same for the test_kmod_* tier).
#
# HOW IT MIRRORS THE x86_64 HARNESS (tests/qemu/run_tests.sh + Dockerfile):
#   * cross-builds qemu_syssvc_tests for alpha (CMake alpha-linux toolchain,
#     glibc-static -- the SAME feasibility half proven in #662, 66 EM_ALPHA
#     binaries);
#   * builds the SAME byte-genuine, arch-independent ODS-2 fixtures host-side
#     with plain gcc (src/vmsfs/ods2/* is fixed-width LE) -- real/search/imgact,
#     plus a best-effort sysvol (needs product subject images; see below);
#   * bakes a static C runner-init (test-init-syssvc-alpha.c -- no busybox on
#     alpha, same reason ke-init-alpha.c is C) + the test binaries + vms.ko/
#     vmsfs.ko into the kernel via INITRAMFS_SOURCE (clipper -initrd is
#     unreliable), boots from initramfs so DKA0: is free for /ods2_real.img;
#   * attaches the fixtures as virtio disks in DKA0:/100:/200:/300:/400: order
#     (the executive enumerates virtio-blk into DK units at module init, exactly
#     as test_kmod_disk asserts);
#   * VERDICT via the SAME tests/qemu/lib/harness_verdict.sh grep of the
#     "=== FINAL RESULTS: N suites passed, M suites failed ===" line the runner
#     prints -- 0 failed = green.
#
# Rule 9: build/test tooling only. Every qemu boot is wrapped in timeout(1).
# A facility is proven only when a real second process reaches /dev/vms; the
# runner never fakes or skips -- a missing fixture or nonzero exit is a genuine
# suite failure.
#
# USAGE:
#   tools/cross-alpha/run-syssvc-tests-alpha.sh          # build + run, verdict
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
ROOT="${SYSSVC_ROOT:-$REPO/.boot-cache/alpha-syssvc}"
export VMSKO_WORK="${VMSKO_WORK:-$ROOT/vmsko}"
WORK="${WORK:-$ROOT/work}"
# 66 suites, several fork/exec subject images (DCL/MMK) and live /bin/sh
# subjects (~40-50s each with process create/observe/delete). 2400s reached
# only 48/66; 3600s carries the full run with the bounded-lifetime subjects.
BOOT_TIMEOUT="${BOOT_TIMEOUT:-3600}"
DOCKER_TIMEOUT="${DOCKER_TIMEOUT:-$((BOOT_TIMEOUT + 1800))}"

mkdir -p "$WORK"
log() { echo "[syssvc-alpha] $*"; }
die() { echo "[syssvc-alpha] FATAL: $*" >&2; exit 1; }

# The kernel tree + vms.ko/vmsfs.ko come from build-vmsko-alpha.sh (cached).
if [ ! -f "$VMSKO_WORK/vms.ko" ] || [ ! -f "$VMSKO_WORK/vmsfs.ko" ] || [ ! -d "$VMSKO_WORK/linux-$KV" ]; then
    log "executive modules/kernel not cached -- running build-vmsko-alpha.sh (~15 min)"
    WORK="$VMSKO_WORK" KV="$KV" "$HERE/build-vmsko-alpha.sh"
fi

docker build -t "$IMG" "$HERE" >/dev/null

timeout --kill-after=60 "$DOCKER_TIMEOUT" docker run --rm --memory=8g --cpus="$(nproc)" \
  -v "$REPO:/repo:ro" \
  -v "$HERE:/tools:ro" \
  -v "$VMSKO_WORK:/vmsko" \
  -v "$WORK:/work" "$IMG" bash -euo pipefail -c '
    KV="'"$KV"'"; BT="'"$BOOT_TIMEOUT"'"
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
    export QEMU_LD_PREFIX=/usr/alpha-linux-gnu
    CC=alpha-linux-gnu-gcc
    cd /work
    rm -rf /work/tests; mkdir -p /work/tests

    ####################################################################
    # 1. Cross-build qemu_syssvc_tests for alpha (glibc-static).
    ####################################################################
    echo "== cross-build qemu_syssvc_tests for alpha =="
    cmake -S /repo -B /work/cmake-alpha \
        -DCMAKE_TOOLCHAIN_FILE=/repo/tools/cross-alpha/toolchain-alpha-linux.cmake \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_TOOLS=ON -DOVMX_STATIC=ON \
        >/work/cmake-cfg.log 2>&1 || { echo CONFIG-FAIL; tail -30 /work/cmake-cfg.log; exit 1; }
    cmake --build /work/cmake-alpha --target qemu_syssvc_tests -j"$(nproc)" \
        >/work/cmake-build.log 2>&1 || { echo BUILD-FAIL; grep -nE ": error:" /work/cmake-build.log | head -25; exit 1; }
    n=0
    for t in /work/cmake-alpha/bin/test_syssvc_* /work/cmake-alpha/bin/test_imgact_*; do
        [ -x "$t" ] || continue
        cp "$t" /work/tests/ && n=$((n + 1))
    done
    echo "== staged $n test_syssvc_/test_imgact_ binaries =="
    [ "$n" -ge 1 ] || { echo "FATAL: no syssvc test binaries built"; exit 1; }
    alpha-linux-gnu-strip /work/tests/test_* 2>/dev/null || true

    # SUBJECT IMAGES the suites fork/exec (the pass=0 tier fails its first
    # assertion without these). Best-effort: a self-host target (mmk_native)
    # that does not cross-build for alpha is simply not staged, and its suites
    # fail HONESTLY (never skip). Required targets (DCL/INITIALIZE/AUTHORIZE)
    # cross-build via the same glibc-static toolchain the tests use.
    echo "== build subject images (DCL/INITIALIZE/AUTHORIZE + best-effort MMK/TCC) =="
    for tgt in vmsdcl vms_initialize vms_authorize; do
        cmake --build /work/cmake-alpha --target "$tgt" -j"$(nproc)" >>/work/cmake-subj.log 2>&1 \
            || echo "WARN: subject target $tgt did not build for alpha"
    done
    # mmk_native (self-host MMK) + tcc (self-host compiler) -- the mmk_* suites
    # drive MMK.EXE which shells out to TCC.EXE. Best-effort; a self-host target
    # that does not cross-build leaves its suites to fail honestly.
    for tgt in mmk_native tcc; do
        cmake --build /work/cmake-alpha --target "$tgt" -j"$(nproc)" >>/work/cmake-subj.log 2>&1 \
            || echo "WARN: self-host target $tgt did not cross-build for alpha"
    done
    alpha-linux-gnu-strip /work/cmake-alpha/bin/*.EXE 2>/dev/null || true

    ####################################################################
    # 2. Build the byte-genuine ODS-2 fixtures host-side (arch-independent
    #    writer, src/vmsfs/ods2/*). Recipes mirror tests/qemu/Dockerfile.
    ####################################################################
    echo "== build ODS-2 fixtures (host gcc) =="
    ODS2SRC="/repo/src/vmsfs/ods2"
    ODS2CC="$ODS2SRC/ods2_reader.c $ODS2SRC/ods2_writer.c $ODS2SRC/ods2_edit.c $ODS2SRC/ods2_bdev.c $ODS2SRC/ods2_block_posix.c"
    # DKA0: the real-VAX ODS-2 system disk (committed fixture, verbatim copy).
    cp /repo/tests/ods2/real_vax_ods2.dsk /work/tests/ods2_real.img
    # DKA200: multi-version search volume.
    gcc -O2 -Wall -I/repo/src/vmsfs/include -o /work/mk_search \
        /repo/tests/qemu/mkimage_ods2_search.c $ODS2CC
    /work/mk_search /work/tests/ods2_search.img 1
    # DKA400: an ELF image volume for IMGACT-over-ACP.
    gcc -O2 -Wall -I/repo/src/vmsfs/include -I/repo/tests/qemu -o /work/mk_imgact \
        /repo/tests/qemu/mkimage_ods2_imgact.c $ODS2CC
    /work/mk_imgact /work/tests/ods2_imgact.img 1
    # DKA300: the SYS$SYSTEM sysvol carrying the REAL SYSUAF/RIGHTSLIST. The
    # product subject images (TCC/LIBRARIAN/LINK/DECC$SHR) are x86 build
    # artifacts not present in this cross env; stage the DAT records the login/
    # rights suites actually read, and the alpha IMGACT.EXE if it built. Suites
    # needing the absent EXEs fail honestly (not skip) -- iterated separately.
    gcc -O2 -Wall -I/repo/src/vmsfs/include -o /work/mk_sysvol \
        /repo/tests/qemu/mkimage_ods2_sysvol.c $ODS2CC
    IMGACT_ARG=""
    [ -f /work/cmake-alpha/src/imgact/IMGACT.EXE ] && IMGACT_ARG="SYSEXE:IMGACT.EXE=/work/cmake-alpha/src/imgact/IMGACT.EXE"
    /work/mk_sysvol /work/tests/ods2_sysvol.img 48 \
        /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT \
        /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/RIGHTSLIST.DAT \
        $IMGACT_ARG >/work/mk_sysvol.log 2>&1 \
        || { echo "WARN: ods2_sysvol master failed (sysvol-dependent suites will fail honestly)"; cat /work/mk_sysvol.log; truncate -s 24M /work/tests/ods2_sysvol.img; }
    ls -la /work/tests/ods2_*.img

    ####################################################################
    # 3. Build the static C runner-init + bake the test initramfs.
    ####################################################################
    echo "== build syssvc runner-init + /bin/sh subject stub =="
    $CC -static -O2 -Wall /tools/test-init-syssvc-alpha.c -o /work/syssvc-init
    $CC -static -O2 -Wall /tools/sh-subject-stub.c -o /work/sh-subject
    alpha-linux-gnu-strip /work/syssvc-init /work/sh-subject
    cp /vmsko/vms.ko /work/vms.ko
    cp /vmsko/vmsfs.ko /work/vmsfs.ko

    echo "== assemble test initramfs list =="
    {
      echo "dir /dev 755 0 0"
      echo "nod /dev/console 644 0 0 c 5 1"
      echo "nod /dev/null 666 0 0 c 1 3"
      echo "dir /proc 755 0 0"
      echo "dir /sys 755 0 0"
      echo "dir /tests 755 0 0"
      echo "dir /bin 755 0 0"
      echo "dir /vms 755 0 0"
      echo "dir /vms/SYS0 755 0 0"
      echo "dir /vms/SYS0/SYSCOMMON 755 0 0"
      echo "dir /vms/SYS0/SYSCOMMON/SYSEXE 755 0 0"
      echo "dir /tmp 1777 0 0"
      echo "file /init /work/syssvc-init 755 0 0"
      echo "file /bin/sh /work/sh-subject 755 0 0"   # live-process subject for showproc/procnam/delprc/startup_service
      echo "file /vms.ko /work/vms.ko 644 0 0"
      echo "file /vmsfs.ko /work/vmsfs.ko 644 0 0"
      for f in /work/tests/test_*; do
        echo "file /tests/$(basename "$f") $f 755 0 0"
      done
      # SUBJECT IMAGES at the exact paths the suites exec (guarded by build
      # success; a target absent on alpha leaves its suites to fail honestly).
      B=/work/cmake-alpha/bin
      if [ -f "$B/DCL.EXE" ]; then
        echo "file /bin/DCL.EXE $B/DCL.EXE 755 0 0"          # setprv_dcl/ident/setname/startup_service
        echo "file /tests/DCL.EXE $B/DCL.EXE 755 0 0"        # mbx_dcldrv/showdev
        echo "file /vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE $B/DCL.EXE 755 0 0"
      fi
      [ -f "$B/INITIALIZE.EXE" ] && echo "file /tests/INITIALIZE.EXE $B/INITIALIZE.EXE 755 0 0"
      [ -f "$B/AUTHORIZE.EXE" ]  && echo "file /tests/AUTHORIZE.EXE $B/AUTHORIZE.EXE 755 0 0"
      [ -f "$B/AUTHORIZE.EXE" ]  && echo "file /vms/SYS0/SYSCOMMON/SYSEXE/AUTHORIZE.EXE $B/AUTHORIZE.EXE 755 0 0"
      [ -f "$B/MMK.EXE" ]        && echo "file /vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE $B/MMK.EXE 755 0 0"
      [ -f "$B/TCC.EXE" ]        && echo "file /vms/SYS0/SYSCOMMON/SYSEXE/TCC.EXE $B/TCC.EXE 755 0 0"
    } > /work/syssvc.list

    echo "== rebuild vmlinux with the syssvc-test initramfs baked in =="
    cd /vmsko/linux-$KV
    KDIR="$(pwd)"
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/syssvc.list
    make ARCH=alpha CROSS_COMPILE=alpha-linux-gnu- olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    make ARCH=alpha CROSS_COMPILE=alpha-linux-gnu- -j"$(nproc)" vmlinux >/work/kbuild.log 2>&1 \
        || { echo KBUILD-FAIL; tail -25 /work/kbuild.log; exit 1; }
    alpha-linux-gnu-strip -o /work/vmlinux-syssvc vmlinux
    cd /work

    ####################################################################
    # 4. Boot qemu-system-alpha -M clipper with the 5 ODS-2 fixture disks.
    #    Fixture -> virtio unit -> VMS device (mirrors run_tests.sh vda..vde):
    #    vda DKA0: real | vdb DKA100: blank | vdc DKA200: search |
    #    vdd DKA300: sysvol | vde DKA400: imgact.
    ####################################################################
    echo "== prepare fixture disks =="
    cp /work/tests/ods2_real.img   /work/d0.img
    truncate -s 16M                /work/d1.img
    cp /work/tests/ods2_search.img /work/d2.img
    cp /work/tests/ods2_sysvol.img /work/d3.img
    cp /work/tests/ods2_imgact.img /work/d4.img

    echo "== boot qemu-system-alpha -M clipper, timeout ${BT}s =="
    # NO NIC: on clipper, a virtio-net-pci alongside the 5 virtio-blk fixture
    # disks hangs the boot before /init (6 virtio PCI devices; iter-1 with 5
    # disks + -nic none booted clean). getdvi ETH0: is one suite and fails
    # honestly without a NIC -- revisit with a slimmer disk set or a non-virtio
    # NIC rather than break the whole run for it. -m 2048 gives the larger
    # subject-image initramfs headroom.
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 2048 -vga none -nic none \
        -kernel /work/vmlinux-syssvc -append "console=ttyS0 panic=-1" \
        -nographic -no-reboot \
        -drive file=/work/d0.img,format=raw,if=virtio \
        -drive file=/work/d1.img,format=raw,if=virtio \
        -drive file=/work/d2.img,format=raw,if=virtio \
        -drive file=/work/d3.img,format=raw,if=virtio \
        -drive file=/work/d4.img,format=raw,if=virtio \
        2>&1 | tee /work/syssvc-boot.raw \
        | grep -avE "TSUNAMI machine check|tsunami_(read|write)" || true
    grep -avE "TSUNAMI machine check|tsunami_(read|write)" /work/syssvc-boot.raw > /work/syssvc-boot.log || true
  '

echo
echo "==================== VERDICT ===================="
LOG="$WORK/syssvc-boot.log"
[ -f "$LOG" ] || die "no boot log produced at $LOG"
# Reuse the x86_64 harness verdict logic verbatim.
# shellcheck disable=SC1091
if [ -f "$REPO/tests/qemu/lib/harness_verdict.sh" ]; then
    . "$REPO/tests/qemu/lib/harness_verdict.sh"
    if harness_verdict_zero_failures "$LOG"; then
        grep -aE "FINAL RESULTS|ASSERTIONS" "$LOG" | sed 's/^/  /'
        log "PASS: every syssvc/imgact suite passed on qemu-system-alpha"
        exit 0
    fi
else
    if grep -aqE "FINAL RESULTS:.*[^0-9]0 suites failed" "$LOG"; then
        grep -aE "FINAL RESULTS|ASSERTIONS" "$LOG" | sed 's/^/  /'
        log "PASS: every syssvc/imgact suite passed on qemu-system-alpha"
        exit 0
    fi
fi
echo "-- FINAL RESULTS / failing suites --"
grep -aE "FINAL RESULTS|=== SUITE .* rc=[^0]|  FAIL:" "$LOG" | head -60 | sed 's/^/  /'
die "one or more syssvc suites failed (or no FINAL RESULTS reached) -- see $LOG"
