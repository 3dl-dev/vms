#!/bin/bash
# release_install_inner.sh - the IN-CONTAINER half of the vms-37f R1
# release-acceptance gate (tests/qemu/test_release_install.sh drives it).
#
# This script runs INSIDE the ovmx-boot image (it needs /boot/vmlinuz, the
# bootstrap initramfs, /boot/ovmx-install-media.img and qemu-system-*). The
# ORCHESTRATOR (test_release_install.sh) invokes it in SEPARATE `docker run`
# containers against a single HOST-MOUNTED target image at /work/target.img --
# that host-mounted file surviving between two independent `docker run`s IS the
# container boundary this gate exists to cross (the boundary that bit vms-9b7).
#
# Two modes:
#
#   install   Boot the DISTRIBUTION install media (DKA0:) plus the blank,
#             host-mounted, pre-INITIALIZEd target (DKA100: = /work/target.img)
#             and drive OVMX$INSTALL.COM's full menu over the console exactly as
#             an operator would (PRESERVE / target / label / confirmation gate /
#             real MOUNT / real PRODUCT INSTALL / SYSTEM password prompts /
#             SCSNODE-SCSSYSTEMID / the procedure's own DISMOUNT). The DISMOUNT
#             is what umount(2)s the target and flushes the GUEST's vmsfs writes
#             down to the virtio device; cache=writethrough (below) then carries
#             them from the device to the host file. Both legs are load-bearing
#             for the install to survive into the next container -- see the
#             WRITEBACK-CACHE LESSON block below.
#
#   verify    Boot ONLY the host-mounted target (DKA0: = /work/target.img) with
#             the bootstrap initramfs -- no install media attached, a genuinely
#             separate container -- log in as SYSTEM, and prove the installed OS
#             is really on the target: PRODUCT SHOW PRODUCT lists the OS kit
#             (read from the target's OWN VMS$PRODUCT_DATABASE.DAT, which
#             PRODUCT INSTALL /DESTINATION wrote to the target's rooted [SYSEXE],
#             src/product/product.c) and DIRECTORY SYS$SYSTEM: lists DCL.EXE from
#             the target's rooted structure.
#
# THE WRITEBACK-CACHE LESSON (vms-9b7), KEPT AND ASSERTED, NOT PAPERED OVER:
#   Every drive here is cache=writethrough. vms-9b7 measured that an install
#   done in one container VANISHED in the next when writes were left buffered,
#   and cache=writethrough is the standing fix on the qemu-device->host-file leg
#   that every install/boot e2e now carries. This gate (a) uses cache=writethrough
#   on every drive, (b) is mechanically asserted by the orchestrator to DO so
#   (grep), and (c) proves the SHARED host file is what makes the install cross
#   the boundary at all, via the orchestrator's deterministic negative control:
#   OVMX_NEGCTL_LOCAL_TARGET=1 here runs the SAME fully-successful menu install
#   (real MOUNT, real PRODUCT INSTALL, real DISMOUNT, cache=writethrough) but onto
#   a CONTAINER-LOCAL disk instead of the host-mounted target, so the host file is
#   left untouched; the following verify container then boots the (still-blank)
#   host target and MUST fail. A fully-successful install that lands anywhere but
#   the shared file does not appear in the next container -- which is precisely
#   the boundary property under test, proven deterministically (no dependence on
#   guest writeback timing).
#
# THE SYSTEM PASSWORD -- THE REAL ACCEPTANCE LOOP IS CLOSED:
#   The install menu prompts for a new SYSTEM password and drives AUTHORIZE
#   against the MOUNTed target over the console here. `install` sets a KNOWN,
#   non-default password ($INSTALL_PW) and asserts AUTHORIZE reports
#   %UAF-I-SAVED with no %UAF-E- error -- i.e. the write to the TARGET's own
#   SYSUAF persisted -- and then `verify`, in a SEPARATE container booting the
#   target alone, logs in as SYSTEM with THAT install-set password. Since the
#   kit's default SYSTEM password (MANAGER) is OVERWRITTEN by the install, a
#   successful login with $INSTALL_PW is positive proof the install-set password
#   took (MANAGER would no longer work).
#
#   This is only possible because the two prerequisite gaps vms-dcf originally
#   reported are now FIXED and present in this base: cross-process visibility of
#   a parent-MOUNTed device (vms-8b6, #385 fb5a47e5 -- vmsfs_device_resolve reads
#   the kernel mount table so the exec'd AUTHORIZE sees the DCL parent's mount)
#   and RMS CREATE on a mounted vmsfs volume (vms-581, #378 f09d5812 -- SYSTEM is
#   group<=MAXSYSGROUP). With both in place AUTHORIZE, run against the
#   DEFINE/JOB SYS$SYSTEM-redirected target, opens and rewrites the target's
#   SYSUAF.DAT for real.
#
#   ONE separate, still-open display gap, kept as an informational NOTE (not
#   scored, not owned by vms-37f): OVMX$INSTALL.COM's SET TERMINAL/NOECHO does
#   not suppress the echo of the password typed at the READ prompt (a vms-dcf
#   display gap, distinct from the now-fixed write gaps). It does not affect the
#   write or the login, so this gate captures it for the record rather than
#   scoring it -- see the NOTE below.
#
# Env knobs: BOOT_TIMEOUT (default 90), RUN_TIMEOUT (default 90),
#            OVMX_NEGCTL_LOCAL_TARGET (unset|1). Exit 0 = every assertion passed.

set -uo pipefail

# Never let a write to a DEAD qemu console (a boot that halted/powered off, so
# the FIFO reader is gone) kill this harness with SIGPIPE before it can report
# WHY. Without this, a container-2 boot that halts made the whole inner script
# die silently on the next `send` -- the outer gate then saw a nonzero exit and
# a near-empty console with no dump_and_die diagnostic (the exact way the R1
# container-2 boot failure hid its own root cause). Ignoring SIGPIPE lets send()
# fail harmlessly and the loops below detect the dead qemu and dump the console.
trap '' PIPE

MODE="${1:-}"
case "$MODE" in
    install|verify) ;;
    *) echo "FATAL: usage: $0 install|verify"; exit 2 ;;
esac

BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
RUN_TIMEOUT="${RUN_TIMEOUT:-90}"
LOCAL_TARGET="${OVMX_NEGCTL_LOCAL_TARGET:-}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
INSTALL_MEDIA=/boot/ovmx-install-media.img
TARGET=/work/target.img
INSTALL_PW="INSTALLPW1"     # the SYSTEM password SET during install; verify logs in with THIS
                           # (all-caps so AUTHORIZE-hash and login-hash agree regardless of case folding)
# The SCSNODE/SCSSYSTEMID the install CONFIGURES on the target (vms-597). Shared
# by both phases: `install` drives these into OVMX$INSTALL.COM's SCS step and
# `verify` asserts the booted target reports node name $R1_SCSNODE from its own
# OVMXVMSSYS.PAR. NON-BLANK on purpose -- the old gate answered blank, took the
# SKIP_SCS path, and so never exercised OR proved the node configuration (the
# LARP vms-597 closes). <= 6 chars, distinct from the OVMX kit default.
R1_SCSNODE="OVMXR1"
R1_SCSID="1025"
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    # KVM acceleration (vms-fb8): hardware virt when /dev/kvm is writable,
    # else TCG software emulation (identical behavior, ~10x slower). See
    # distro/boot/run-qemu.sh for the full rationale.
    if [ -w /dev/kvm ]; then
        MACHINE="-accel kvm -cpu host"
    else
        MACHINE="-accel tcg"
    fi
    CONSOLE="console=ttyS0"
fi

need=("$KERNEL" "$INITRD" "$TARGET")
[ "$MODE" = "install" ] && need+=("$INSTALL_MEDIA")
for f in "${need[@]}"; do
    [ -f "$f" ] || { echo "FATAL: $f not found (run inside ovmx-boot with /work bind-mounted; see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR=$(mktemp -d)
LOG="$WORKDIR/console.log"
FIFO="$WORKDIR/console.in"
QPID=""

# kill_boot - kill the `timeout` wrapper AND the qemu-system grandchild it
# spawns. Same reasoning as tests/qemu/test_install_menu.sh's kill_boot: never
# `kill -9` the wrapper alone or qemu is orphaned still holding the target's
# write lock, wedging the next boot.
kill_boot() {
    local pid="$1"
    [ -n "$pid" ] || return 0
    pkill -TERM -P "$pid" 2>/dev/null
    kill "$pid" 2>/dev/null
    sleep 1
    pkill -9 -P "$pid" 2>/dev/null
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}
cleanup() { kill_boot "$QPID"; rm -rf "$WORKDIR"; }
trap cleanup EXIT

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern  limit-seconds  since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL ($MODE): $1 ==="
    echo "--- full console log ---"
    cat "$LOG" 2>/dev/null
    kill_boot "$QPID"; QPID=""
    echo ""
    echo "RESULT ($MODE): $PASS passed, $FAIL failed"
    exit 1
}
# vms-2213: LOGINOUT/DCL wait for the operator's RETURN before a prompt; a lone
# CR at t=0 can be lost while the guest serial driver comes up. Feed one per
# second until the pattern appears. Bounded.
wake_for() {  # pattern
    local pat="$1" w=0
    until grep -qaF "$pat" "$LOG" 2>/dev/null || [ "$w" -ge 120 ]; do
        # Stop the moment qemu is gone: a boot that halted/powered off will
        # never print the pattern, and continuing to send() into a dead FIFO
        # just wastes the whole 120s budget before the caller's wait_for finally
        # dumps the console. Bail immediately so dump_and_die runs with the real
        # halt diagnostic still on the log.
        kill -0 "$QPID" 2>/dev/null || return 1
        send ''; sleep 1; w=$((w + 1))
    done
}

# boot <qemu-arg...>  -- start qemu on $LOG/$FIFO with the given trailing args
# (the -drive specs the caller assembled).
boot() {
    exec 4>&- 2>/dev/null || true
    rm -f "$LOG" "$FIFO"
    mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 12 + 60))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 1 -nic none -nodefaults -serial stdio \
        "$@" \
        -no-reboot <"$FIFO" >"$LOG" 2>&1 &
    QPID=$!
    exec 4>"$FIFO"
}

# =====================================================================
if [ "$MODE" = "install" ]; then
    echo "=== release-install (container 1): drive OVMX\$INSTALL.COM menu onto the host-mounted target ==="
    echo "arch=$ARCH qemu=$QEMU local_target=${LOCAL_TARGET:-0}"

    # DKA0: install media (container-local copy -- writes to it need not
    # persist); DKA100: the HOST-MOUNTED target, written through to the host
    # file. cache=writethrough on BOTH is the kept vms-9b7 lesson.
    #
    # NEGATIVE CONTROL (OVMX_NEGCTL_LOCAL_TARGET=1): install onto a
    # CONTAINER-LOCAL copy of the target instead of the host file, so a
    # fully-successful install cannot cross the container boundary (see header).
    MEDIA_LOCAL="$WORKDIR/media.img"
    cp "$INSTALL_MEDIA" "$MEDIA_LOCAL"
    TARGET_DRIVE="$TARGET"
    if [ -n "$LOCAL_TARGET" ]; then
        echo "  (OVMX_NEGCTL_LOCAL_TARGET=1: installing onto a CONTAINER-LOCAL disk, NOT the shared host file)"
        cp "$TARGET" "$WORKDIR/local-target.img"
        TARGET_DRIVE="$WORKDIR/local-target.img"
    fi
    boot \
        -drive file="$MEDIA_LOCAL",format=raw,if=virtio,cache=writethrough \
        -drive file="$TARGET_DRIVE",format=raw,if=virtio,cache=writethrough

    wake_for 'OVMX$INSTALL Option'
    if wait_for 'OVMX$INSTALL Option' "$BOOT_TIMEOUT" 0; then
        ok "install media boots straight into the OVMX\$INSTALL menu (no login)"
    else
        dump_and_die "install menu never appeared within ${BOOT_TIMEOUT}s"
    fi
    if printf '%s\n' "$(segment_since 0)" | grep -qF 'Username:'; then
        bad "a login prompt preceded the install menu (should be menu-first)"
    else
        ok "no login prompt precedes the install menu"
    fi

    OFF=$(wc -c <"$LOG"); send '1'
    wait_for 'INITIALIZE or to PRESERVE' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "option 1 did not ask INITIALIZE-or-PRESERVE"
    ok "option 1 asks INITIALIZE-or-PRESERVE"

    OFF=$(wc -c <"$LOG"); send 'PRESERVE'
    wait_for 'Enter device name for target disk' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "did not reach the target-device prompt after PRESERVE"
    ok "PRESERVE accepted, asks for the target device"

    OFF=$(wc -c <"$LOG"); send 'DKA100:'
    wait_for 'Enter volume label for target system disk' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "did not reach the volume-label prompt"
    ok "target device accepted, asks for the volume label"

    OFF=$(wc -c <"$LOG"); send 'WORK'
    wait_for 'Is this OK' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "did not reach the confirmation gate"
    ok "volume label accepted, reaches the confirmation gate"

    OFF=$(wc -c <"$LOG"); send 'YES'
    wait_for '%MOUNT-I-MOUNTED' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "MOUNT did not report success from inside the menu"
    ok "real MOUNT of the target succeeds from inside the menu"

    wait_for '%PCSI-I-DONE' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "PRODUCT INSTALL did not reach %PCSI-I-DONE"
    ok "real PRODUCT INSTALL onto the target reports %PCSI-I-DONE"
    if printf '%s\n' "$(segment_since "$OFF")" | grep -qiE '%PCSI-[EF]-|%MOUNT-[EF]-'; then
        bad "install transcript carries a MOUNT/PCSI error despite DONE"
    else
        ok "install transcript carries no MOUNT/PCSI error"
    fi

    # SYSTEM password: set a KNOWN, non-default password ($INSTALL_PW) during
    # install and PROVE it persisted to the TARGET's own SYSUAF (%UAF-I-SAVED,
    # no %UAF-E-). container 2/3 then log in with $INSTALL_PW -- the real
    # acceptance loop (vms-8b6 #385 + vms-581 #378 make the write work; see
    # header).
    if wait_for 'Password for SYSTEM account' "$RUN_TIMEOUT" "$OFF"; then
        ok "install asks for the SYSTEM password"
    else
        dump_and_die "install never asked for the SYSTEM password"
    fi
    send "$INSTALL_PW"
    wait_for 'Reenter password for verification' "$RUN_TIMEOUT" "$OFF" \
        || dump_and_die "did not ask to reenter the password"
    ok "asks for password verification"
    send "$INSTALL_PW"
    if wait_for 'UAF>' "$RUN_TIMEOUT" "$OFF"; then
        ok "drops into AUTHORIZE against the target (SYS\$SYSTEM redirected via DEFINE/JOB)"
    else
        dump_and_die "AUTHORIZE never reached UAF> against the target"
    fi
    # The procedure drives AUTHORIZE NON-INTERACTIVELY via inline SYS$INPUT
    # (vms-963): OVMX$INSTALL.COM feeds "MODIFY SYSTEM/PASSWORD='OVMX_PW1'" +
    # EXIT as data lines after the RUN, and DCL apostrophe-substitutes the
    # entered password ($INSTALL_PW, already answered above) into that line
    # before AUTHORIZE reads it. There is no operator typing at UAF> -- so
    # this driver does NOT send MODIFY/EXIT (a stray send here would sit
    # unconsumed in the PTY and get eaten by the SCSNODE/SCSSYSTEMID prompts
    # below); it asserts the write instead.
    #
    # The write to the TARGET's SYSUAF must PERSIST -- AUTHORIZE saves on EXIT
    # when dirty and prints %UAF-I-SAVED on success, %UAF-E-SAVEFAIL on failure.
    # This is the in-container proof that the install-set password landed on the
    # target (the cross-container login in `verify` is the second, independent
    # proof).
    if wait_for '%UAF-I-SAVED' "$RUN_TIMEOUT" "$OFF"; then
        ok "AUTHORIZE persisted the new SYSTEM password to the TARGET's SYSUAF (%UAF-I-SAVED)"
    else
        dump_and_die "AUTHORIZE did not report %UAF-I-SAVED -- the install-set password did not persist to the target SYSUAF"
    fi
    AUTHSEG=$(segment_since "$OFF")
    if printf '%s\n' "$AUTHSEG" | grep -qiE '%UAF-E-NOSUCHUSER|%UAF-E-WRITEFAIL|%UAF-E-SAVEFAIL'; then
        bad "AUTHORIZE reported a UAF error writing the target SYSUAF"
        printf '%s\n' "$AUTHSEG" | grep -iE '%UAF-E-'
    else
        ok "AUTHORIZE wrote the target SYSUAF with no %UAF-E- error"
    fi
    if wait_for 'Enter SCSNODE' "$RUN_TIMEOUT" "$OFF"; then
        ok "returns from AUTHORIZE and continues (asks for SCSNODE)"
    else
        dump_and_die "did not return from AUTHORIZE / never asked for SCSNODE"
    fi
    # NOT SCORED (a separate, still-open DISPLAY gap): OVMX$INSTALL.COM's
    # SET TERMINAL/NOECHO does not suppress the echo of the password typed at the
    # READ prompt (a vms-dcf display gap, distinct from the now-fixed write
    # gaps). It affects neither the write above nor the login in `verify`, and is
    # outside vms-37f's outcome, so this gate CAPTURES it for the record rather
    # than scoring it.
    if printf '%s\n' "$AUTHSEG" | grep -qF "$INSTALL_PW"; then
        echo "  NOTE (not scored): the SYSTEM password was echoed on the console -- known vms-dcf SET TERMINAL/NOECHO display gap, not owned/fixed by vms-37f"
    else
        echo "  NOTE (not scored): the SYSTEM password was not echoed (vms-dcf NOECHO display gap appears resolved)"
    fi
    # Drive the SCS/SYSGEN step FOR REAL (vms-597): answer SCSNODE/SCSSYSTEMID
    # NON-BLANK so OVMX$INSTALL.COM feeds SYSGEN its inline SYS$INPUT block and
    # GENUINELY configures the node. The old gate answered blank here, took the
    # SKIP_SCS path, and so never exercised OR proved the node configuration --
    # the exact LARP vms-597 closes. Answering the two INQUIRE prompts is the
    # whole operator interaction; the SYSGEN REPL is supplied by the procedure's
    # own inline data block, NOT typed at the console.
    SCS_OFF=$(wc -c <"$LOG")
    send "$R1_SCSNODE"
    wait_for 'Enter SCSSYSTEMID' "$RUN_TIMEOUT" "$SCS_OFF" \
        || dump_and_die "never asked for SCSSYSTEMID"
    ok "asks for SCSSYSTEMID"
    send "$R1_SCSID"
    # ANTI-LARP (vms-dd15/INV-6): anchor on SYSGEN.EXE's runtime BANNER (emitted
    # by the image itself, never by the procedure) -- NOT the bare "SYSGEN>"
    # literal. Under the inline SYS$INPUT feed SYSGEN's stdin is a non-tty
    # tmpfile, and vms_sysgen.c prints this banner unconditionally.
    if wait_for 'OpenVMS System Generation Utility' "$RUN_TIMEOUT" "$SCS_OFF"; then
        ok "SYSGEN.EXE activated against the rooted target via the procedure's inline SYS\$INPUT (runtime banner) -- no %DCL-E-IVIMAGE (vms-597)"
    else
        dump_and_die "SYSGEN.EXE did not start against the target (vms-597: %DCL-E-IVIMAGE / empty inline SYS\$INPUT -- no runtime banner)"
    fi
    # %SYSGEN-I-WRITTEN is printed only after WRITE CURRENT serializes the
    # parameter set to the target's OVMXVMSSYS.PAR -- positive proof the inline
    # feed reached and executed WRITE CURRENT against the target.
    if wait_for '%SYSGEN-I-WRITTEN' "$RUN_TIMEOUT" "$SCS_OFF"; then
        ok "SYSGEN's inline WRITE CURRENT wrote SCSNODE/SCSSYSTEMID to the target's OVMXVMSSYS.PAR (%SYSGEN-I-WRITTEN)"
    else
        dump_and_die "SYSGEN did not report %SYSGEN-I-WRITTEN -- the inline SYS\$INPUT never configured the node"
    fi
    SCS_SEG=$(segment_since "$SCS_OFF")
    if printf '%s\n' "$SCS_SEG" | grep -qE '%DCL-E-IVIMAGE|%SYSGEN-[EF]-'; then
        bad "the SCS/SYSGEN segment carries an IVIMAGE or a SYSGEN error"
        printf '%s\n' "$SCS_SEG" | grep -E '%DCL-E-IVIMAGE|%SYSGEN-[EF]-'
    else
        ok "the SCS/SYSGEN segment carries no IVIMAGE and no SYSGEN error"
    fi

    if wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"; then
        ok "the procedure DISMOUNTs the target (umount(2) flushes guest writes to the device)"
    else
        dump_and_die "did not DISMOUNT the target -- the install would not be durable"
    fi
    if wait_for 'installation is complete' "$RUN_TIMEOUT" "$OFF"; then
        ok "reports the installation complete"
    else
        bad "did not report installation complete"
    fi

    kill_boot "$QPID"; QPID=""
    echo ""
    echo "RESULT (install): $PASS passed, $FAIL failed"
    [ "$FAIL" -eq 0 ] && { echo "INSTALL-CONTAINER CHECKS PASSED"; exit 0; }
    echo "--- full console log ---"; cat "$LOG" 2>/dev/null
    exit 1
fi

# =====================================================================
# verify -- separate container, target only, bootstrap initramfs.
# =====================================================================
echo "=== release-install (verify): boot the installed target ALONE, log in, PRODUCT SHOW PRODUCT ==="
echo "arch=$ARCH qemu=$QEMU"

boot -drive file="$TARGET",format=raw,if=virtio,cache=writethrough

wake_for 'Username:'
if wait_for 'Username:' "$BOOT_TIMEOUT" 0; then
    ok "the installed target boots ALONE (no install media) and reaches login"
else
    dump_and_die "the installed target never reached a login prompt -- install did not cross the container boundary"
fi
if printf '%s\n' "$(segment_since 0)" | grep -qF 'OVMX$INSTALL Option'; then
    bad "the booted target re-ran the install menu (SYSTARTUP variant leaked onto the target)"
else
    ok "the booted target does NOT re-run the install menu"
fi
# END-TO-END vms-597: the SCSNODE the `install` phase wrote to the target's
# OVMXVMSSYS.PAR (via SYSGEN's inline WRITE CURRENT through the rooted
# SYS$SYSTEM redirect) MUST be what this booted target reports as its node
# identity -- ovmx_init prints "%OVMX-I-SCSNODE, node name <X> set from
# SYS$SYSTEM:OVMXVMSSYS.PAR". Seeing $R1_SCSNODE (not the OVMX kit default)
# proves the install's SCS configuration reached and persisted to the target's
# own parameter file across the container boundary. This is the assertion the
# old gate skipped entirely by answering SCSNODE blank.
if printf '%s\n' "$(segment_since 0)" | grep -qF "node name $R1_SCSNODE"; then
    ok "the install-set SCSNODE ($R1_SCSNODE) persisted to the target and the booted target reads it from OVMXVMSSYS.PAR (end-to-end vms-597)"
else
    bad "the install-set SCSNODE ($R1_SCSNODE) did NOT appear on the target's boot -- SYSGEN WRITE CURRENT did not persist to the target's OVMXVMSSYS.PAR"
fi
# First-boot completion is a justified no-op for OVMX (vms-649): the target
# must NOT run any self-install phase on this boot.
if printf '%s\n' "$(segment_since 0)" | grep -qF '%STARTUP-I-INSTALL'; then
    bad "the booted target ran a self-install phase (PID 1 must mount-or-halt, not install)"
else
    ok "the booted target ran no self-install phase (mount-or-halt, vms-2f0/vms-649)"
fi

# Log in with the INSTALL-SET password ($INSTALL_PW), NOT the kit default: the
# install overwrote SYSTEM's password on the target, so a successful login here
# is positive proof the install-set password persisted across the container
# boundary (MANAGER would no longer work). This is the real vms-37f acceptance
# criterion ("login as SYSTEM with the password set during install").
OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 20 "$OFF" && send "$INSTALL_PW"
if wait_for 'Welcome to OpenVMX' 30 "$OFF"; then
    ok "SYSTEM logs in with the INSTALL-SET password from the booted target (not the kit default)"
else
    dump_and_die "SYSTEM login with the install-set password failed -- the install-set password did not persist to the target"
fi
wait_for '$' 20 "$OFF"

# PRODUCT SHOW PRODUCT (default -- reads the TARGET's own VMS$PRODUCT_DATABASE.DAT
# that PRODUCT INSTALL /DESTINATION wrote into the target's rooted [SYSEXE]).
OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qiE 'X86VMS VMS' && printf '%s\n' "$SEG" | grep -qF 'Installed'; then
    ok "PRODUCT SHOW PRODUCT on the booted target lists the OS kit as Installed"
else
    bad "PRODUCT SHOW PRODUCT did not list the installed OS kit"
    echo "$SEG"
fi

# DIRECTORY SYS$SYSTEM: -- DCL.EXE resolves through the target's rooted structure.
OFF=$(wc -c <"$LOG")
send 'DIRECTORY SYS$SYSTEM:DCL.EXE'
wait_for 'Total of' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF 'DCL.EXE;' && printf '%s\n' "$SEG" | grep -qE 'Total of [1-9]'; then
    ok "DIRECTORY SYS\$SYSTEM: on the booted target lists DCL.EXE (rooted SYS\$SYSTEM resolves)"
else
    bad "DIRECTORY SYS\$SYSTEM: did not list DCL.EXE on the booted target"
    echo "$SEG"
fi

kill_boot "$QPID"; QPID=""
echo ""
echo "RESULT (verify): $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && { echo "VERIFY-CONTAINER CHECKS PASSED"; exit 0; }
echo "--- full console log ---"; cat "$LOG" 2>/dev/null
exit 1
