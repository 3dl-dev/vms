#!/usr/bin/env bash
#
# boot-alpha-image.sh -- boot the assembled OVMX/Alpha system image under
# qemu-system-alpha -M clipper (the DS10 compute stack) and drive PID 1 as far
# as the current executive/ACP frontier allows.  rd vms-989, rung A5a -- the
# dispatchable front-half of boot-to-DCL.
#
# Requires build-alpha-bootimage.sh to have populated $WORK
# (vmlinux-boot + ovmx-distrib-alpha.img + imgact-proof/).
#
# It runs TWO boots, each under a HARD timeout (Rule 9: a hung qemu-system-alpha
# can run for hours):
#
#   BOOT A -- the REAL image: /init = STARTUP.EXE (ovmx_init), mounting the
#     ODS-2 system disk on /dev/vda, exactly mirroring the x86_64 QEMU bootable
#     path (distro/Dockerfile.bootable).  This finds the true Alpha frontier:
#     PID 1 -> vms.ko/dev/vms Files-11 ACP -> mount /dev/vda -> exec PROVISION ->
#     ... -> stop.  Whatever milestone Alpha reaches is reported honestly; no
#     userspace fake is used to "get further" (INV-6).
#
#   BOOT B -- the IMGACT capability proof: /init = alpha-imgact-init, which
#     activates a REAL VMS-native Alpha image (PT_INTERP=IMGACT.EXE) UNDER THE
#     BOOTED kernel.  Alpha's real login chain is static (no LINK.EXE graph for
#     alpha yet), so IMGACT is not in the static boot chain; this proves the
#     A2 activator works in the booted-kernel context, on a live /dev/vms.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
VMSKO_WORK="${VMSKO_WORK:-/tmp/ovmx-vmsko-alpha}"
WORK="${WORK:-/tmp/ovmx-alpha-boot}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-200}"

for f in vmlinux-boot ovmx-distrib-alpha.img imgact-proof/IMGACT.EXE; do
    [ -e "$WORK/$f" ] || { echo "FATAL: $WORK/$f missing -- run build-alpha-bootimage.sh first"; exit 1; }
done

docker build -t "$IMG" "$HERE" >/dev/null

docker run --rm --memory=8g --cpus="$(nproc)" \
  -v "$HERE":/tools:ro \
  -v "$VMSKO_WORK":/vmsko \
  -v "$WORK":/work "$IMG" bash -euo pipefail -c '
    KV="'"$KV"'"; BT="'"$BOOT_TIMEOUT"'"
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
    cd /work

    #########################################################################
    # BOOT A -- the real OVMX/Alpha boot (init=STARTUP.EXE), disk on /dev/vda.
    #
    # CONSOLE WAKE (vms-3f6). On OPA0: LOGINOUT waits for the operator RETURN
    # before it presents "Username:" -- the "press RETURN to log in" console
    # behaviour (vms-2213, tools/vms_login.c console_login()). An UNATTENDED
    # boot must therefore DRIVE the console, exactly as the x86_64 boot gate
    # tests/qemu/test_persistent_boot.sh does: background QEMU on a FIFO stdin
    # and feed a CR every couple of seconds until "Username:" appears in the log
    # (or the guest exits). Without this the Alpha login chain runs correctly
    # -- JOB_CONTROL forks LOGINOUT.EXE, which execs and reaches the wake wait
    # -- but blocks there unseen and "Username:" never reaches the log. This is
    # a HARNESS driver, not a boot change: the same disk booted under a human at
    # the console reaches the prompt on the operator hitting RETURN.
    #########################################################################
    echo "======================================================================"
    echo "== BOOT A: real OVMX/Alpha image -- init=STARTUP.EXE, VMSFS on /dev/vda"
    echo "======================================================================"
    rm -f diskA.img; cp ovmx-distrib-alpha.img diskA.img
    FIFO=/work/bootA.fifo; rm -f "$FIFO"; mkfifo "$FIFO"
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
        -kernel vmlinux-boot -append "console=ttyS0 panic=-1" \
        -drive file=diskA.img,format=raw,if=virtio \
        -nographic -no-reboot <"$FIFO" > /work/bootA.raw 2>&1 &
    QP=$!
    exec 6>"$FIFO"
    trap "" PIPE   # a CR fed just as the guest exits must not kill this shell
    W=0
    while kill -0 "$QP" 2>/dev/null; do
        grep -qaF "Username:" /work/bootA.raw 2>/dev/null && break
        printf "\r" >&6 2>/dev/null || true
        sleep 2; W=$((W + 2))
        [ "$W" -ge "$BT" ] && break
    done
    exec 6>&-
    sleep 2                       # let LOGINOUT flush the prompt after the wake CR
    kill "$QP" 2>/dev/null || true
    wait "$QP" 2>/dev/null || true
    rm -f "$FIFO"
    grep -avE "TSUNAMI machine check|tsunami_(read|write)" /work/bootA.raw > /work/bootA.log || true
    tail -80 /work/bootA.log

    #########################################################################
    # BOOT B -- IMGACT-under-booted-kernel proof (init=alpha-imgact-init).
    # Rebuild the proof program with the IN-GUEST interpreter path, bake a
    # proof initramfs into a second vmlinux, boot it.
    #########################################################################
    echo
    echo "======================================================================"
    echo "== BOOT B: IMGACT activates a REAL VMS-native Alpha image (booted kernel)"
    echo "======================================================================"
    CC=alpha-linux-gnu-gcc
    PROOF=/work/imgact-proof
    JOINT=/work/joint
    # PROOF IMAGE (vms-157 confirm run): the REAL alpha-dec-vms GCC-port image
    # joint_e2e.exe, prebuilt by tools/cross-alpha-vms/joint-e2e/build-joint-image.sh
    # (real port crt0 + joint_main, linked strict/zero-deferred against the genuine
    # alpha DECC$SHR). It is NOT rebuilt here -- it is the finished VMS-native image.
    #   readelf: EM_ALPHA ET_DYN, PT_INTERP=/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE
    #   .vms$imp producers: DECC$SHR.EXE (decc$main/decc$malloc/decc$tprintf) ->
    #     LIBOTS_SHR.EXE (OTS$ int-divide/block-move), resolved by IMGACT from
    #     SYS$SHARE == IMGACT_FALLBACK_SYSLIB == /vms/SYS0/SYSCOMMON/SYSLIB.
    # So stage: IMGACT.EXE at its baked interp path (SYSEXE); joint_e2e.exe as the
    # image the init execs (/imgact-proof/test_prog_alpha); BOTH producers in SYSLIB
    # (the port musl needs LIBOTS_SHR.EXE AND DECC$SHR.EXE staged in SYS$SHARE).
    $CC -static -O2 -Wall /tools/alpha-imgact-init.c -o /work/imgact-init
    alpha-linux-gnu-strip /work/imgact-init

    test -f "$JOINT/joint_e2e.exe"   || { echo "FATAL: $JOINT/joint_e2e.exe missing -- copy build-joint-image.sh output into WORK/joint"; exit 1; }
    test -f "$JOINT/DECC\$SHR.EXE"    || { echo "FATAL: $JOINT/DECC\$SHR.EXE missing";   exit 1; }
    test -f "$JOINT/LIBOTS_SHR.EXE"  || { echo "FATAL: $JOINT/LIBOTS_SHR.EXE missing"; exit 1; }

    cat > /work/imgact-proof.list <<L
dir /dev 755 0 0
nod /dev/console 644 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
dir /proc 755 0 0
dir /sys 755 0 0
dir /imgact-proof 755 0 0
dir /vms 755 0 0
dir /vms/SYS0 755 0 0
dir /vms/SYS0/SYSCOMMON 755 0 0
dir /vms/SYS0/SYSCOMMON/SYSEXE 755 0 0
dir /vms/SYS0/SYSCOMMON/SYSLIB 755 0 0
file /init /work/imgact-init 755 0 0
file /vms.ko /vmsko/vms.ko 644 0 0
file /vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE $PROOF/IMGACT.EXE 755 0 0
file /imgact-proof/test_prog_alpha $JOINT/joint_e2e.exe 755 0 0
file /vms/SYS0/SYSCOMMON/SYSLIB/DECC\$SHR.EXE $JOINT/DECC\$SHR.EXE 644 0 0
file /vms/SYS0/SYSCOMMON/SYSLIB/LIBOTS_SHR.EXE $JOINT/LIBOTS_SHR.EXE 644 0 0
L
    cd /vmsko/linux-$KV
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/imgact-proof.list
    make ARCH=alpha olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    make ARCH=alpha -j"$(nproc)" vmlinux >/work/kbuild-imgact.log 2>&1
    alpha-linux-gnu-strip -o /work/vmlinux-imgact vmlinux
    cd /work
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
        -kernel vmlinux-imgact -append "console=ttyS0 panic=-1" \
        -nographic -no-reboot > /work/bootB.raw 2>&1 || true
    grep -avE "TSUNAMI machine check|tsunami_(read|write)" /work/bootB.raw > /work/bootB.log || true
    grep -a "OVMX-ALPHA-IMGACT:\|IMGACT-TEST:\|%IMGACT" /work/bootB.log | sed "s/^/  /" || true
  '

echo
echo "==================== FRONTIER VERDICT ===================="
echo "-- BOOT A milestones (real OVMX/Alpha boot) --"
grep -aE "OVMX/Linux|OVMX-I-EXEC|executive attached|vmsfs|STDRV-I-STARTUP|PROVISION|Username:|%OVMX|%RMS|%SYSTEM|halt|HALT|DEVNOTMOUNT|SS\\\$_" "$WORK/bootA.log" 2>/dev/null | sed 's/^/  /' | tail -40 || echo "  (no milestone lines)"
echo
if grep -qaF "Username:" "$WORK/bootA.log" 2>/dev/null; then
    echo "  PASS: Alpha booted to the interactive DCL login prompt (Username:) under qemu-system-alpha."
else
    echo "  NOT REACHED: Username: prompt absent -- see $WORK/bootA.log"
fi
echo
echo "-- BOOT B (IMGACT capability) --"
if grep -aq "OVMX-ALPHA-IMGACT: ALL-PROVEN" "$WORK/bootB.log" 2>/dev/null; then
    echo "  PASS: IMGACT.EXE activated a REAL VMS-native Alpha image under the booted kernel."
    grep -a "OVMX-ALPHA-IMGACT:\|IMGACT-TEST:" "$WORK/bootB.log" | sed 's/^/    /'
else
    echo "  NOT PROVEN -- see $WORK/bootB.log"
    grep -a "OVMX-ALPHA-IMGACT:\|IMGACT-TEST:\|%IMGACT" "$WORK/bootB.log" 2>/dev/null | sed 's/^/    /' || true
fi
echo
echo "Full transcripts: $WORK/bootA.log , $WORK/bootB.log"
