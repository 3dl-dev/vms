#!/bin/bash
# test_install_boot_e2e.sh - PRODUCT INSTALL /DESTINATION -> boot THAT target
# AS ITS OWN SYSTEM DISK -> login (vms-96ec, parent vms-718, gates vms-37f).
#
# WHY THIS EXISTS -- THE COVERAGE GAP IT CLOSES. vms-df9's
# test_product_install_e2e.sh proves a /DESTINATION install lands runnable
# bytes on a mounted DATA volume (it MOUNTs the target and runs an image
# from it). It never BOOTS that target as a system disk. Every CI e2e that
# boots (test_distrib_boot.sh, test_persistent_boot.sh, the release
# acceptance gates) boots the PRE-MASTERED /boot/ovmx-distrib.img -- already
# rooted+concealed by vmsfs_master -- never a freshly /DESTINATION-installed
# target. So the on-disk SHAPE a /DESTINATION install produces was never
# booted, and vms-649 found it was FLAT (<mount>/SYSEXE/...) instead of the
# rooted, concealed [SYS0.SYSCOMMON.SYSEXE] layout a bootable OpenVMS system
# disk requires. A flat target halts at boot with %OVMX-F-SYSINIT: STARTUP
# resolves SYS$SYSROOT:[SYSEXE]DCL.EXE through the rooted structure
# (ovmx_layout.h VMS_SYSTEM_DIR = /vms/SYS0/SYSCOMMON/SYSEXE) and finds
# nothing. vms-96ec makes PRODUCT INSTALL lay the rooted layout; THIS test
# is the ground-source proof of it -- red on the unfixed tree (boot 2 never
# reaches Username:), green after.
#
# WHAT IT PROVES (nothing earlier does):
#   BOOT 1 (fat initramfs, distrib VDA0: + a blank INITIALIZEd VDA100:):
#     log in, MOUNT VDA100:, PRODUCT INSTALL the real OS kit onto it with
#     /DESTINATION=VDA100:, then INDEPENDENTLY confirm the login chain
#     (DCL.EXE + LOGINOUT.EXE) landed at the ROOTED path
#     VDA100:[SYS0.SYSCOMMON.SYSEXE], and that the OLD FLAT path
#     VDA100:[SYSEXE] is EMPTY (the regression this bead fixes would have
#     put the files there). DISMOUNT so the volume flushes cleanly.
#   BOOT 2 (SLIM bootstrap initramfs, the JUST-INSTALLED disk as the SOLE
#     VDA0: system disk): the slim initramfs carries NO DCL/LOGINOUT/SYSLIB
#     (asserted from its cpio listing, same guard as test_distrib_boot.sh),
#     so reaching a login prompt and logging SYSTEM in is functional proof
#     the whole login chain came off the /DESTINATION-INSTALLED disk. This
#     is the exact boot a flat install cannot survive.
#
# Runs INSIDE the ovmx-boot image (QEMU + /boot/vmlinuz + the fat and slim
# initramfs images + /boot/ovmx-distrib.img). The blank target disk is
# formatted on the host by run_install_boot_e2e.sh and bind-mounted at
# /work/dka100.img -- same convention as run_product_install_e2e.sh.
#
# Env knobs: BOOT_TIMEOUT (default 90), RUN_TIMEOUT (default 90).
# Exit 0 = every assertion above passed.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
RUN_TIMEOUT="${RUN_TIMEOUT:-90}"
KERNEL=/boot/vmlinuz
FAT_INITRD=/boot/initramfs-ovmx.cpio.gz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
VDA100_SRC=/work/dka100.img
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$FAT_INITRD" "$SLIM_INITRD" "$DISTRIB_IMG" "$VDA100_SRC"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image with /work bind-mounted (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== PRODUCT INSTALL -> boot the installed target as a system disk (vms-96ec) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL"

WORKDIR=$(mktemp -d)
DISK0="$WORKDIR/dka0.img"       # distrib boot disk for BOOT 1
TARGET="$WORKDIR/target.img"    # blank -> installed -> booted as VDA0: in BOOT 2
cp "$DISTRIB_IMG" "$DISK0"
cp "$VDA100_SRC" "$TARGET"

QPID=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

# --- SLIM initramfs really is bootstrap-only (guards BOOT 2's proof) ---
# If the slim image already carried DCL/LOGINOUT/SYSLIB, a login in BOOT 2
# would not prove the INSTALLED disk supplied them. Same guard as
# test_distrib_boot.sh.
SLIM_LISTING=$(zcat "$SLIM_INITRD" | cpio -t 2>/dev/null || true)
slim_absent() {
    if printf '%s\n' "$SLIM_LISTING" | grep -qF "$2"; then
        bad "slim initramfs unexpectedly carries $2 (would invalidate the BOOT 2 proof)"
    else
        ok "slim initramfs is bootstrap-only: no $2"
    fi
}
slim_absent "" "DCL.EXE"
slim_absent "" "LOGINOUT.EXE"
slim_absent "" "SYSLIB"

send() { printf '%s\r' "$1" >&4; }

# vms-2213: OPA0: LOGINOUT waits for the operator's RETURN before "Username:".
# Feed a CR each second until the prompt appears. Bounded.
wake_login() {
    local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
}
wait_for() {  # pattern  limit-seconds  since-byte  log-file
    local pat="$1" limit="${2:-30}" since="${3:-0}" log="${4:-$LOG}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$log" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "${2:-$LOG}" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ($LOG) ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""
    echo ""
    echo "RESULT: $PASS passed, $FAIL failed"
    exit 1
}

login() {  # login <log-file>  -- SYSTEM/MANAGER, leaves a live DCL session
    local log="$1"
    wake_login "$log"
    if wait_for 'Username:' "$BOOT_TIMEOUT" 0 "$log"; then
        ok "boot reaches the login prompt ($(basename "$log"))"
    else
        dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
    fi
    local off; off=$(wc -c <"$log")
    send 'SYSTEM'
    wait_for 'Password:' 20 "$off" "$log" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 20 "$off" "$log"; then
        ok "SYSTEM logs in ($(basename "$log"))"
    else
        dump_and_die "SYSTEM login failed"
    fi
    wait_for '$' 20 "$off" "$log"
}

# =====================================================================
# BOOT 1 -- fat initramfs, distrib VDA0: + blank VDA100:: MOUNT + INSTALL
# =====================================================================
LOG="$WORKDIR/boot1.log"
FIFO="$WORKDIR/boot1.in"
rm -f "$LOG" "$FIFO"; mkfifo "$FIFO"
# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 8 + 60))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$FAT_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$DISK0",format=raw,if=virtio,cache=writethrough \
    -drive file="$TARGET",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

login "$LOG"

# --- MOUNT the blank target ------------------------------------------
OFF=$(wc -c <"$LOG")
send 'MOUNT VDA100: WORK'
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _VDA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT VDA100: (blank install target) succeeds"
else
    dump_and_die "MOUNT VDA100: did not report success within ${RUN_TIMEOUT}s"
fi

# --- PRODUCT INSTALL the real OS kit onto the blank target -----------
OFF=$(wc -c <"$LOG")
send 'PRODUCT INSTALL VMS /SOURCE=SYS$UPDATE:OVMX-OS.KIT /DESTINATION=VDA100:'
if wait_for '%PCSI-I-DONE' "$RUN_TIMEOUT" "$OFF"; then
    ok "PRODUCT INSTALL /DESTINATION=VDA100: reports %PCSI-I-DONE"
else
    dump_and_die "PRODUCT INSTALL did not reach %PCSI-I-DONE within ${RUN_TIMEOUT}s"
fi
INSTALL_SEG=$(segment_since "$OFF")
if printf '%s\n' "$INSTALL_SEG" | grep -qiE '%PCSI-[EF]-'; then
    bad "PRODUCT INSTALL transcript carries a PCSI error despite DONE"
    echo "$INSTALL_SEG"
else
    ok "PRODUCT INSTALL transcript carries no %PCSI-E-/%PCSI-F- error"
fi

# --- The kit landed at the ROOTED path (not the old flat one) --------
# Match the LISTING (DCL.EXE;<version>), not the echoed command -- the
# command text itself contains "DCL.EXE" without a version semicolon, so a
# bare "DCL.EXE" grep would false-pass off the echo. "Total of N" only
# prints after a real successful listing. (Neither this command nor the
# flat one below contains a '$', so wait_for '$'/'Total of' waits for the
# command to COMPLETE, not the leftover prompt before it.)
OFF=$(wc -c <"$LOG")
send 'DIRECTORY VDA100:[SYS0.SYSCOMMON.SYSEXE]DCL.EXE'
wait_for 'Total of' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF 'DCL.EXE;' \
    && printf '%s\n' "$SEG" | grep -qE 'Total of [1-9]'; then
    ok "install landed DCL.EXE at rooted VDA100:[SYS0.SYSCOMMON.SYSEXE]"
else
    bad "rooted VDA100:[SYS0.SYSCOMMON.SYSEXE]DCL.EXE not listed"
    echo "$SEG"
fi

# NEGATIVE: the OLD flat path must be EMPTY -- if files landed there, the
# regression this bead fixes is back and the disk is not bootable. A missing
# rooted directory reports %RMS-E-DNF (directory not found); a present file
# would instead print a "Total of N" listing.
OFF=$(wc -c <"$LOG")
send 'DIRECTORY VDA100:[SYSEXE]DCL.EXE'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qE '%RMS-E-DNF|%RMS-E-FNF|no such file|not found'; then
    ok "old flat VDA100:[SYSEXE]DCL.EXE is correctly ABSENT (rooted, not flat)"
elif printf '%s\n' "$SEG" | grep -qE 'Total of [1-9]'; then
    bad "flat VDA100:[SYSEXE]DCL.EXE resolves -- install wrote the flat layout"
    echo "$SEG"
else
    bad "flat VDA100:[SYSEXE]DCL.EXE check inconclusive (no listing, no RMS error)"
    echo "$SEG"
fi

# --- DISMOUNT so the volume flushes before the kill ------------------
OFF=$(wc -c <"$LOG")
send 'DISMOUNT VDA100:'
wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"

exec 4>&- 2>/dev/null || true
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

# =====================================================================
# BOOT 2 -- THE PROOF: boot the JUST-INSTALLED disk as the SOLE system
# disk, with the bootstrap-only SLIM initramfs. A flat install halts here
# (%OVMX-F-SYSINIT, DCL.EXE absent); a rooted install boots to login.
# =====================================================================
LOG="$WORKDIR/boot2.log"
FIFO="$WORKDIR/boot2.in"
rm -f "$LOG" "$FIFO"; mkfifo "$FIFO"
# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 4 + 60))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$TARGET",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

# The discriminating assertion: the installed target reaches login. On the
# unfixed (flat) tree this never fires -- STARTUP halts %OVMX-F-SYSINIT.
if wait_for '%OVMX-F-SYSINIT' 60 0 "$LOG"; then
    dump_and_die "installed target halted %OVMX-F-SYSINIT -- the layout is not bootable (flat, not rooted)"
fi
login "$LOG"

# Confirm DCL really activated FROM the installed disk (slim initramfs has none).
OFF=$(wc -c <"$LOG")
send 'SHOW TIME'
if wait_for '$' 20 "$OFF" "$LOG"; then
    SEG=$(segment_since "$OFF")
    CURYEAR=$(date +%Y)
    if printf '%s' "$SEG" | grep -qF "$CURYEAR"; then
        ok "DCL.EXE activated from the INSTALLED disk (SHOW TIME returns the real date)"
    else
        bad "SHOW TIME did not return the current year from the installed disk"
        echo "$SEG"
    fi
else
    bad "DCL prompt did not return after SHOW TIME on the installed disk"
fi

# DIRECTORY SYS$SYSTEM: -- the login chain is really on the mounted volume,
# and SYS$SYSTEM: resolves into the rooted structure on the booted target.
# The command contains a '$' (SYS$SYSTEM), so wait on 'Total of' (which only
# prints after a successful listing), never on '$' -- the echoed '$' would
# match instantly. Match the versioned listing (DCL.EXE;N), not the echo.
OFF=$(wc -c <"$LOG")
send 'DIRECTORY SYS$SYSTEM:DCL.EXE'
wait_for 'Total of' 20 "$OFF" "$LOG"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF 'DCL.EXE;' && printf '%s\n' "$SEG" | grep -qE 'Total of [1-9]'; then
    ok "SYS\$SYSTEM: on the booted installed disk lists DCL.EXE (rooted SYS\$SYSTEM resolves)"
else
    bad "SYS\$SYSTEM: on the booted installed disk does not list the login chain"
    echo "$SEG"
fi

exec 4>&- 2>/dev/null || true
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL INSTALL->BOOT E2E CHECKS PASSED (a /DESTINATION-installed target boots as its own system disk)"
    exit 0
fi
echo ""
echo "--- full console log (boot1) ---"; cat "$WORKDIR/boot1.log" 2>/dev/null
echo "--- full console log (boot2) ---"; cat "$WORKDIR/boot2.log" 2>/dev/null
exit 1
