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
    # OVMX_IMGACT_SEAM=1 (kernel cmdline -> init env -> inherited by execv down
    # the boot chain to DCL and its RUN fork child IMGACT): opt-in vms-f60d
    # activation-seam diagnostic. IMGACT prints one line per activated image
    # carrying the EXECUTIVE-RECORDED completion $STATUS it read back via
    # GETEXIT(SEL_SELF) -- "OVMX-SEAM: image=<name> ... $STATUS=0x<cond>". This is
    # how the returned VALUE (the sentinel 3 that main returns -> VMS condition %X...03) is
    # surfaced: the executive owns it; the DCL-RUN fork path collapses the POSIX
    # exit to SS$_NORMAL, but the executive $STATUS the seam prints is the truth.
    timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
        -kernel vmlinux-boot -append "console=ttyS0 panic=-1 OVMX_IMGACT_SEAM=1" \
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
    #   readelf: EM_ALPHA ET_DYN, PT_INTERP=/run/ovmx-boot/IMGACT.EXE (vms-430:
    #     build-joint-image.sh now bakes the ACP-flipped runtime interp, matching
    #     the ODS-2/BOOT-A staging; the retired /vms interp is dead).
    #   .vms$imp producers: DECC$SHR.EXE (decc$main/decc$malloc/decc$tprintf) ->
    #     LIBOTS_SHR.EXE (OTS$ int-divide/block-move), resolved by IMGACT from
    #     SYS$SHARE == IMGACT_FALLBACK_SYSLIB == /vms/SYS0/SYSCOMMON/SYSLIB.
    # So stage: IMGACT.EXE at its baked interp path (/run/ovmx-boot, where PID 1
    # stages it on the real runtime); joint_e2e.exe as the image the init execs
    # (/imgact-proof/test_prog_alpha); BOTH producers in SYSLIB (the port musl
    # needs LIBOTS_SHR.EXE AND DECC$SHR.EXE staged in SYS$SHARE, the IMGACT
    # compiled-in fallback path /vms/SYS0/SYSCOMMON/SYSLIB).
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
dir /run 755 0 0
dir /run/ovmx-boot 755 0 0
file /init /work/imgact-init 755 0 0
file /vms.ko /vmsko/vms.ko 644 0 0
file /run/ovmx-boot/IMGACT.EXE $PROOF/IMGACT.EXE 755 0 0
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
echo "-- JOINT-E2E (vms-157): GCC-port image RUN at DCL over the mounted ODS-2 ACP --"
if [ -f "$WORK/bootA.log" ]; then
    grep -aE "JOINT-E2E-PROOF:|OVMX crt0 join|exited with error status" "$WORK/bootA.log" 2>/dev/null | sed 's/^/    /' || true
    echo
    # MILESTONE requires ALL THREE, read from the console (no crt0-join-alone claim):
    #  (a) crt0-join line (crt0 ran), (b) argc=1 FROM main (main entered),
    #  (c) the returned VALUE 3 (main ran its body to `return 3`) -- DECODED, below.
    _crt0=$(grep -qaE "OVMX crt0 join: activated, argc=1" "$WORK/bootA.log" 2>/dev/null && echo 1 || echo 0)
    # (c) the returned VALUE: read the EXECUTIVE-recorded completion $STATUS from
    # IMGACT's opt-in seam line (OVMX_IMGACT_SEAM=1) for each image. The value is
    # NOT the raw integer -- crt0.s emits the DEC C main-return mapping:
    #   return 0    -> SS$_NORMAL (0x00000001)
    #   return N>=2 -> C$_EXIT1 + (N-1)*8     (C$_EXIT1 = the OVMX bootstrap-surface
    #                                          globalvalue LINK folds; build.log:
    #                                          "C$_EXIT1 folded to absolute 0x35a009")
    # So DECODE the milestone status back to the sentinel and assert it is 3, with
    # the control (return 0 -> 0x1) as the value-sensitivity anchor (teeth: a fixed
    # constant or a failed activation cannot both satisfy the decode AND the anchor).
    _cexit1=$((0x35a009))
    _seam_mile=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E\.EXE[^\n]*STATUS=0x[0-9A-Fa-f]+" "$WORK/bootA.log" 2>/dev/null | tail -1)
    _seam_ctl=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E_OK\.EXE[^\n]*STATUS=0x[0-9A-Fa-f]+" "$WORK/bootA.log" 2>/dev/null | tail -1)
    _mile_hex=$(printf '%s' "$_seam_mile" | grep -oiE '0x[0-9a-f]+' | tail -1)
    _ctl_hex=$(printf '%s' "$_seam_ctl"  | grep -oiE '0x[0-9a-f]+' | tail -1)
    _val3=0; _sentinel="?"
    if [ -n "$_mile_hex" ]; then
        _mile_dec=$(( _mile_hex ))
        if [ "$_mile_dec" -ge "$_cexit1" ] && [ $(( (_mile_dec - _cexit1) % 8 )) -eq 0 ]; then
            _sentinel=$(( (_mile_dec - _cexit1) / 8 + 1 ))
            [ "$_sentinel" -eq 3 ] && _val3=1
        fi
    fi
    # value-sensitivity anchor: the control (return 0) MUST read SS$_NORMAL (0x1).
    _ctl_ok=0; [ -n "$_ctl_hex" ] && [ "$(( _ctl_hex ))" -eq 1 ] && _ctl_ok=1
    _ctl_ran=$(grep -qaE "OVMX crt0 join OK-CONTROL: activated, argc=1" "$WORK/bootA.log" 2>/dev/null && echo 1 || echo 0)

    echo "  Control  (main returns 0): crt0-join+argc=1=$_ctl_ran  executive seam: $_seam_ctl  (SS\$_NORMAL anchor=$_ctl_ok)"
    echo "  Milestone(main returns 3): crt0-join+argc=1=$_crt0     executive seam: $_seam_mile"
    echo "           DECODE: (${_mile_hex:-<none>} - C\$_EXIT1 0x35a009)/8 + 1 = $_sentinel  (want 3; match=$_val3)"
    if [ "$_crt0" = 1 ] && [ "$_val3" = 1 ] && [ "$_ctl_ok" = 1 ]; then
        echo "  PASS (MILESTONE): the real alpha-dec-vms GCC-port image joint_e2e.exe ACTIVATED via IMGACT"
        echo "        over the MOUNTED ODS-2 volume; crt0 -> decc\$main -> main ENTERED (argc=1) and ran its"
        echo "        body to return the sentinel 3. The EXECUTIVE-recorded completion \$STATUS decodes via the"
        echo "        faithful DEC C mapping C\$_EXIT1+(N-1)*8 to N=3, and the control (return 0 -> SS\$_NORMAL)"
        echo "        anchors value-sensitivity. NOTE: DCL RUN's fork path collapses the POSIX exit to"
        echo "        SS\$_NORMAL and does NOT propagate the executive \$STATUS -- a separate DCL-fidelity gap;"
        echo "        the executive seam value is the truth."
    else
        echo "  NOT PROVEN -- need crt0-join+argc=1 ($_crt0), decoded-sentinel-3 ($_val3), control-anchor ($_ctl_ok). Signatures:"
        grep -aE "%IMGACT|%RUN-|%DCL-|IMGNOTFND|no such|NOSUCHFILE|DEVNOTMOUNT|SS\\\$_" "$WORK/bootA.log" 2>/dev/null | sed 's/^/    /' | tail -20 || echo "    (none captured)"
    fi
else
    echo "  NOT PROVEN -- no bootA.log"
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
