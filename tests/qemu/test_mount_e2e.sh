#!/bin/bash
# test_mount_e2e.sh - MOUNT/DISMOUNT are real mount(2)/umount(2), ground-
# sourced against a real vms.ko + vmsfs.ko (vms-651, docs/design-vms-
# faithful-install.md sec 3.1/3.3).
#
# WHAT THIS PROVES, AND WHY NOTHING EARLIER PROVES IT. The facade this item
# deleted (src/vmsdcl/dcl_cmd_misc.c's old cmd_mount/cmd_dismount) never
# called mount(2)/umount(2) at all -- it wrote a per-process userspace
# device table, used getcwd() as the "mount path", and printed
# %MOUNT-I-MOUNTED unconditionally. tests/dcl/test_mount.sh (ctest, no
# executive) can only prove the facade is GONE -- an honest failure with no
# /dev/vms is the one thing checkable there. This is the paired POSITIVE:
# a real vms.ko, a real SECOND virtio disk, a real vmsfs mount, a real file
# on it that survives DISMOUNT/re-MOUNT AND a full QEMU process restart --
# proof it hit the disk, not a tmpfs or a per-process fake sharing nothing
# (CLAUDE.md Rule 9 / INV-6).
#
# THE SECOND DISK is formatted VMSFS on the HOST before this script ever
# runs (run_mount_e2e.sh, with the host's own INITIALIZE.EXE) and handed in
# at /work/dka100.img -- a blank, unformatted disk fails to mount(2) as
# vmsfs, the same rule STARTUP.EXE's own system-disk mount enforces. The
# executive enumerates it as DKA100: (the second virtio-blk device, vms-3e8:
# DKA0: from vda, DKA100: from vdb).
#
# GROUND-SOURCE SHAPE (all of it against the REAL system, no mocks):
#   1. MOUNT DKA100:, CREATE a file on it via DCL (a real COPY, not a
#      fabricated row), DISMOUNT, re-MOUNT -> file still there.
#   2. /proc/mounts (via `$ TYPE "/proc/mounts"` -- dcl_resolve_path treats
#      a leading "/" as a literal Linux path, so this is a real read of the
#      kernel's own mount table, not a VMS filespec; quoted because DCL's
#      parser reads a bare leading "/" as a qualifier, not a parameter)
#      shows the vmsfs mount
#      while mounted, and does NOT after DISMOUNT.
#   3. QEMU is KILLED and a FRESH qemu-system-x86_64 process is started
#      against the SAME disk FILE -- proof this hit the disk, not guest
#      RAM/tmpfs. The file is still there after the restart.
#   4. NEGATIVE CONTROL: MOUNT of a unit that does not exist (only two
#      virtio disks are attached, so DKA200: names no unit) fails with a
#      real error and never prints %MOUNT-I-MOUNTED.
#   5. SET VOLUME (vms-309) against DKA100: while it is genuinely mounted:
#      a bogus qualifier draws %DCL-W-IVQUAL, and /LABEL= draws the honest
#      %SET-W-NOTIMPL refusal (vmsfs has no volume-label write-back path)
#      instead of the old unconditional success -- this is the one branch
#      of cmd_set_volume() that needs a REAL mount to reach at all;
#      tests/dcl/test_set_volume_veracity.sh covers the rest on host ctest.
#
# Usage (run INSIDE the bootable image, like test_parts_demo_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_mount_e2e.sh:/test.sh:ro \
#       -v /path/to/formatted/dka100/dir:/work \
#       --entrypoint bash ovmx-boot /test.sh
# (see tests/qemu/run_mount_e2e.sh, which does the host-side formatting and
# invokes this the same way.)
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for each boot to reach Username: (default
#                  90 -- the distribution disk is pre-installed, so this is
#                  NOT the blank-disk install budget test_parts_demo_e2e.sh
#                  needs).
#   RUN_TIMEOUT    seconds to wait for each DCL command's expected output
#                  (default 60).
#
# Exit 0 = every ground-source assertion above passed. Exit 1 = a real
# failure (see the printed transcript segment).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
DKA100_SRC=/work/dka100.img
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

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG" "$DKA100_SRC"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image with /work bind-mounted (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== MOUNT/DISMOUNT e2e: real mount(2)/umount(2) through the executive (vms-651) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

WORKDIR=$(mktemp -d)
DISK0="$WORKDIR/dka0.img"
DISK1="$WORKDIR/dka100.img"
cp "$DISTRIB_IMG" "$DISK0"
cp "$DKA100_SRC" "$DISK1"

QPID=""
FIFO=""
LOG=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

boot_qemu() {  # boot_qemu <log-file> <fifo-path>
    local log="$1" fifo="$2"
    rm -f "$log" "$fifo"
    mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 12 + 60))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 1 -nic none -nodefaults -serial stdio \
        -drive file="$DISK0",format=raw,if=virtio,cache=writethrough \
        -drive file="$DISK1",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    QPID=$!
    exec 4>"$fifo"
}

send() { printf '%s\r' "$1" >&4; }
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
    exit 1
}

login() {  # login <log-file>  -- logs in as SYSTEM, returns once at the $ prompt
    local log="$1"
    if wait_for 'Username:' "$BOOT_TIMEOUT" 0 "$log"; then
        ok "boot reaches the login prompt ($log)"
    else
        dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
    fi
    local off; off=$(wc -c <"$log")
    send 'SYSTEM'
    wait_for 'Password:' 20 "$off" "$log" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 20 "$off" "$log"; then
        ok "SYSTEM logs in"
    else
        dump_and_die "SYSTEM login failed"
    fi
    wait_for '$' 20 "$off" "$log"
}

# =====================================================================
# BOOT 1 -- mount, create, dismount, re-mount, and the negative control
# =====================================================================
LOG="$WORKDIR/boot1.log"
boot_qemu "$LOG" "$WORKDIR/boot1.in"
login "$LOG"

# --- 1. MOUNT DKA100: -------------------------------------------------
OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'
if wait_for '%MOUNT-I-MOUNTED' "$RUN_TIMEOUT" "$OFF"; then
    SEG=$(segment_since "$OFF")
    if printf '%s\n' "$SEG" | grep -qF '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:'; then
        ok "MOUNT DKA100: reports mounted (real mount(2) via the executive)"
    else
        bad "MOUNT DKA100: printed an unexpected message: $SEG"
    fi
else
    dump_and_die "MOUNT DKA100: did not reach %MOUNT-I-MOUNTED within ${RUN_TIMEOUT}s"
fi

# --- 1b. SET VOLUME (vms-309) against a GENUINELY mounted volume --------
# tests/dcl/test_set_volume_veracity.sh (ctest, no /dev/vms) can only
# prove SET VOLUME's "device not mounted" branch, because nothing is ever
# mounted in that environment. This is the paired POSITIVE: DKA100: really
# is mounted right now, so the per-qualifier honest-refusal branch below
# the mount check (see cmd_set_volume()'s header comment,
# src/vmsdcl/dcl_cmd_set.c) is what actually runs -- including /LABEL,
# which needs a real mounted volume to reach at all.
OFF=$(wc -c <"$LOG")
send 'SET VOLUME DKA100:/BOGUS'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF '%DCL-W-IVQUAL'; then
    ok "SET VOLUME DKA100:/BOGUS rejects an unknown qualifier with IVQUAL, mounted or not"
else
    bad "SET VOLUME DKA100:/BOGUS did not report IVQUAL: $SEG"
fi

OFF=$(wc -c <"$LOG")
send 'SET VOLUME DKA100:/LABEL=NEWLABEL'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF '%SET-W-NOTIMPL, SET VOLUME/LABEL is not implemented in OVMX'; then
    ok "SET VOLUME DKA100:/LABEL= on a genuinely mounted volume honestly refuses (no vmsfs write-back path) instead of faking success"
else
    bad "SET VOLUME DKA100:/LABEL= did not honestly refuse: $SEG"
fi
if printf '%s\n' "$SEG" | grep -qF '%SET-I-NOTIMPL'; then
    bad "SET VOLUME DKA100:/LABEL= printed the OLD facade's success-toned NOTIMPL"
else
    ok "SET VOLUME DKA100:/LABEL= prints no success-toned message"
fi

OFF=$(wc -c <"$LOG")
send 'SET VOLUME DKA100:'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qiE '%SET-[EWF]-|%DCL-[EWF]-'; then
    bad "bare SET VOLUME DKA100: (mounted, no qualifier) reported an unexpected error: $SEG"
else
    ok "bare SET VOLUME DKA100: (mounted, no qualifier) is a real no-op: verified mounted, changed nothing, no error"
fi

# --- 2. /proc/mounts shows the vmsfs mount while mounted ---------------
OFF=$(wc -c <"$LOG")
send 'TYPE "/proc/mounts"'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qE '/mnt/dka100 +vmsfs'; then
    ok "/proc/mounts shows the vmsfs mount at /mnt/dka100 while mounted"
else
    bad "/proc/mounts does not show the vmsfs mount while mounted"
    echo "$SEG"
fi

# --- 3. CREATE a real file on it (COPY, not fabricated) -----------------
OFF=$(wc -c <"$LOG")
send 'COPY SYS$MANAGER:SYSTARTUP_VMS.COM DKA100:[000000]MOUNTTST.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qiE '%COPY-[EF]-|%RMS-[EF]-|%DCL-[EF]-'; then
    dump_and_die "COPY to DKA100: reported an error: $SEG"
else
    ok "COPY to DKA100:[000000]MOUNTTST.TXT reported no error"
fi

OFF=$(wc -c <"$LOG")
send 'DIRECTORY DKA100:[000000]MOUNTTST.TXT'
wait_for 'Total of' "$RUN_TIMEOUT" "$OFF"
DIR_CMD='DIRECTORY DKA100:[000000]MOUNTTST.TXT'
SEG=$(segment_since "$OFF" | grep -vF "$DIR_CMD")
if printf '%s\n' "$SEG" | grep -qE 'Total of [1-9][0-9]* files?, [0-9]+ blocks' \
    && printf '%s\n' "$SEG" | grep -qF 'MOUNTTST.TXT'; then
    ok "DIRECTORY independently confirms MOUNTTST.TXT exists on DKA100:"
else
    bad "DIRECTORY does not confirm MOUNTTST.TXT exists on DKA100:"
    echo "$SEG"
fi

OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[000000]MOUNTTST.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
TYPE_CMD='TYPE DKA100:[000000]MOUNTTST.TXT'
CONTENT_BEFORE=$(segment_since "$OFF" | grep -vF "$TYPE_CMD")
if [ -n "$(printf '%s' "$CONTENT_BEFORE" | tr -d '[:space:]')" ]; then
    ok "TYPE reads real content back from DKA100:"
else
    bad "TYPE read nothing back from DKA100: (expected SYSTARTUP_VMS.COM's content)"
fi

# --- 4. DISMOUNT, and /proc/mounts stops showing it ---------------------
OFF=$(wc -c <"$LOG")
send 'DISMOUNT DKA100:'
if wait_for '%DISMOUNT-I-DISMOUNTED, _DKA100: dismounted' "$RUN_TIMEOUT" "$OFF"; then
    ok "DISMOUNT DKA100: reports dismounted (real umount(2))"
else
    dump_and_die "DISMOUNT DKA100: did not report success within ${RUN_TIMEOUT}s"
fi

OFF=$(wc -c <"$LOG")
send 'TYPE "/proc/mounts"'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qE '/mnt/dka100 +vmsfs'; then
    bad "/proc/mounts still shows /mnt/dka100 after DISMOUNT"
    echo "$SEG"
else
    ok "/proc/mounts no longer shows /mnt/dka100 after DISMOUNT"
fi

# --- 5. re-MOUNT: the file is still there -------------------------------
OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "re-MOUNT DKA100: succeeds"
else
    dump_and_die "re-MOUNT DKA100: did not report success within ${RUN_TIMEOUT}s"
fi

OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[000000]MOUNTTST.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
CONTENT_REMOUNT=$(segment_since "$OFF" | grep -vF "$TYPE_CMD")
if [ "$(printf '%s' "$CONTENT_REMOUNT" | tr -d '[:space:]')" = "$(printf '%s' "$CONTENT_BEFORE" | tr -d '[:space:]')" ] \
    && [ -n "$(printf '%s' "$CONTENT_REMOUNT" | tr -d '[:space:]')" ]; then
    ok "the file survives DISMOUNT/re-MOUNT within the same boot, byte-identical"
else
    bad "the file did not survive DISMOUNT/re-MOUNT identically"
fi

# --- 6. NEGATIVE CONTROL: a unit that does not exist ---------------------
OFF=$(wc -c <"$LOG")
send 'MOUNT DKA200: NOPE'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF '%MOUNT-I-MOUNTED'; then
    bad "MOUNT DKA200: (no such unit) printed %MOUNT-I-MOUNTED anyway"
else
    ok "MOUNT DKA200: (no such unit, only two virtio disks attached) prints no %MOUNT-I-MOUNTED"
fi
if printf '%s\n' "$SEG" | grep -qiE '%SYSTEM-[WF]-NOSUCHDEV|%SYSTEM-[WF]-IVDEVNAM'; then
    ok "MOUNT DKA200: fails with a real error (no such device)"
else
    bad "MOUNT DKA200: did not report a real error"
    echo "$SEG"
fi

# DISMOUNT before killing QEMU so umount(2) flushes the volume cleanly --
# the same reason test_docker_persistent_disk.sh's fix exists (a killed
# guest can lose dirty page-cache writes that were never fsynced/unmounted).
OFF=$(wc -c <"$LOG")
send 'DISMOUNT DKA100:'
wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

# =====================================================================
# BOOT 2 -- a FRESH qemu-system process, SAME disk files: real persistence
# =====================================================================
LOG="$WORKDIR/boot2.log"
boot_qemu "$LOG" "$WORKDIR/boot2.in"
login "$LOG"

OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT DKA100: succeeds again after a full QEMU restart"
else
    dump_and_die "MOUNT DKA100: did not report success on the restarted boot"
fi

OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[000000]MOUNTTST.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
CONTENT_AFTER_RESTART=$(segment_since "$OFF" | grep -vF "$TYPE_CMD")
if [ "$(printf '%s' "$CONTENT_AFTER_RESTART" | tr -d '[:space:]')" = "$(printf '%s' "$CONTENT_BEFORE" | tr -d '[:space:]')" ] \
    && [ -n "$(printf '%s' "$CONTENT_AFTER_RESTART" | tr -d '[:space:]')" ]; then
    ok "MOUNTTST.TXT survives a full QEMU restart, byte-identical -- proves the disk, not a tmpfs"
else
    bad "MOUNTTST.TXT did not survive the QEMU restart identically"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL MOUNT/DISMOUNT E2E CHECKS PASSED"
    exit 0
fi
echo ""
echo "--- full console log (boot1) ---"
cat "$WORKDIR/boot1.log" 2>/dev/null
echo "--- full console log (boot2) ---"
cat "$WORKDIR/boot2.log" 2>/dev/null
exit 1
