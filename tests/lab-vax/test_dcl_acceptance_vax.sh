#!/bin/bash
# test_dcl_acceptance_vax.sh -- VAX half of co-release DCL/SHOW acceptance parity
# (rd vms-f2c, epic vms-8954). Runs INSIDE the LAB_IMAGE container (which carries
# anita + SIMH + pexpect + python3), driving the SAME shared, arch-independent
# DCL/SHOW acceptance battery x86_64 and Alpha run:
#     tests/qemu/lib/dcl_acceptance_battery.sh
# -- boot the real OVMX/NetBSD-vax runtime (the slim SINGLE disk sysboot-single
# builds), log in SYSTEM/MANAGER, run the basic DCL/SHOW commands a user types on
# first login, and ASSERT VMS-faithful output for each with a negative control.
# ONE battery, three arches, zero assertion drift.
#
# WHY A PYTHON BRIDGE (unlike Alpha/x86, which launch qemu-system-* directly with
# a stdio console + FIFO): the VAX runtime is OVMX/NetBSD-vax under SIMH, driven
# by anita's pexpect child over a pty -- not a bash-launchable stdio console. So
# `drive_boot_vax.py acceptance' (do_acceptance) boots the single disk through
# anita EXACTLY as sysboot-single does, then BRIDGES: it tees every console byte
# into $RAW (pexpect logfile_read = the battery's LOG) and forwards bytes written
# to $FIFO into the console (child.send = the battery's `send'). This script then
# provides the SAME send/wait_for/run_cmd/SEG the shared battery's caller contract
# requires -- identical to the ones in tools/cross-alpha/run-boot-alpha.sh and
# tests/qemu/test_dcl_acceptance_e2e.sh -- so the battery cannot tell it is on
# SIMH rather than QEMU.
#
# EXPECTED VALUES ARE SINGLE-SOURCED (INV-1) from src/libvms/include/ovmx_identity.h,
# never a literal here -- the SAME idval sed the x86_64/Alpha drivers use:
#   BANNER  = OVMX_PRODUCT_NAME + " " + OVMX_PRODUCT_VERSION
#   COMPAT  = the token F$GETSYI("VERSION") reports, ovmx_compat_version(): rd
#             vms-10e added a __vax__ VMS-lineage branch, so on a VAX build this
#             returns the real OpenVMS VAX version OVMX_VMS_COMPAT_VERSION_VAX
#             ("V7.3", the final VAX release) -- NOT the product version. We
#             single-source it from that same per-arch constant, so the F$GETSYI
#             VERSION assertion stays INV-1-consistent with the runtime (both read
#             ovmx_identity.h). Alpha gained the same treatment (V8.4).
#   VOLUME_LABEL = OVMXSYS (run-boot.sh master_system_volume: vmsfs_master --ods2
#                  ... OVMXSYS); verified against the source below.
#
# A RED assertion here is a RESULT: a real VAX faithful-output gap to fix, each
# naming the bug it guards. It must NOT be weakened to go green -- the gate's job
# is to RUN the shared battery on VAX and assert. Exit 0 IFF every command is
# VMS-faithful AND the battery reached an authenticated prompt.
#
# Env knobs (all optional; sensible defaults):
#   OVMX_ACCEPT_RAW / OVMX_ACCEPT_FIFO   console log + input FIFO (default /cache).
#   NETBSD_WORKDIR                        single-disk workdir (default /cache/single-work).
#   OVMX_SINGLE_RQ0_TYPE                  SIMH rq0 sizing (default RAUSER=340).
#   BOOT_TIMEOUT / CMD_TIMEOUT / ACCEPT_TIMEOUT   battery + whole-run bounds.
#   EXPECT_HOST_YEAR                      1: SHOW TIME must show the host's year and
#                                         today's date (rd vms-b06; always 1 here).
set -uo pipefail

log() { echo "[vax-accept] $*"; }
die() { echo "[vax-accept] FATAL: $*" >&2; exit 2; }

REPO=/src
IDENTITY="$REPO/src/libvms/include/ovmx_identity.h"
BATTERY="$REPO/tests/qemu/lib/dcl_acceptance_battery.sh"
DRIVER=/lab-vax/drive_boot_vax.py
[ -f "$IDENTITY" ] || die "ovmx_identity.h not found at $IDENTITY (mount the repo at /src) -- cannot derive EXPECTED_* (INV-1)"
[ -f "$BATTERY" ]  || die "shared DCL acceptance battery not found at $BATTERY"
[ -f "$DRIVER" ]   || die "VAX boot driver not found at $DRIVER (mount tests/lab-vax at /lab-vax)"

# --- INV-1 single source: derive the expected banner + compat version ---------
idval() { sed -n "s/^#define[[:space:]]\+$1[[:space:]]\+\"\([^\"]*\)\".*/\1/p" "$IDENTITY" | head -1; }
PNAME=$(idval OVMX_PRODUCT_NAME)
PVER=$(idval OVMX_PRODUCT_VERSION)
[ -n "$PNAME" ] && [ -n "$PVER" ] || die "could not read OVMX_PRODUCT_NAME/VERSION from $IDENTITY"
export EXPECTED_BOOT_BANNER="$PNAME $PVER"
# rd vms-10e: VAX now has a VMS-lineage branch, so ovmx_compat_version() returns
# the real OpenVMS VAX version (V7.3), NOT the product version -- single-source
# it from the SAME per-arch constant the runtime reads (OVMX_VMS_COMPAT_VERSION_VAX
# in ovmx_identity.h), so the F$GETSYI VERSION assertion stays INV-1-consistent.
CVER=$(idval OVMX_VMS_COMPAT_VERSION_VAX)
[ -n "$CVER" ] || die "could not read OVMX_VMS_COMPAT_VERSION_VAX from $IDENTITY (rd vms-10e)"
export EXPECTED_COMPAT_VERSION="$CVER"
# rd vms-76c3: F$GETSYI("ARCH_NAME") on a __vax__ build -> ovmx_hw_arch() = "VAX".
export EXPECTED_ARCH_NAME="VAX"

# VOLUME_LABEL tracks master_system_volume's vmsfs_master --ods2 label; verify.
export VOLUME_LABEL="OVMXSYS"
if ! grep -qE "ods2 master .* $VOLUME_LABEL " "$REPO/tests/lab-vax/run-boot.sh" 2>/dev/null; then
    log "WARNING: run-boot.sh no longer masters the VAX system volume as '$VOLUME_LABEL' -- update VOLUME_LABEL"
fi

# SYSDEV: OVMX/NetBSD-vax names its MSCP system disk DUA0: -- the authentic
# VMS-VAX MSCP/RQ device name (vms-9f5), not the virtio VDA0: the other arches
# discover. The shared battery defaults to VDA0:; the VAX driver overrides it so
# SHOW DEVICE / F$GETDVI / SHOW DEVICES assert the faithful VAX name. Verify the
# device model still emits DUA0: as PRIMARY so this doesn't silently drift.
export SYSDEV="DUA0:"
if ! grep -qE 'DUA0.* is PRIMARY' "$REPO/src/kernel-netbsd/vms_blockdev_netbsd.c" 2>/dev/null; then
    log "WARNING: vms_blockdev_netbsd.c no longer names the VAX system disk '$SYSDEV' as PRIMARY -- update SYSDEV"
fi

export CMD_TIMEOUT="${CMD_TIMEOUT:-30}"
export BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
# rd vms-b06: the VAX guest clock IS the host clock. SIMH loads the KA655 TODR
# with the host's time of year at power on; the year comes from the root
# filesystem, which the build session now writes with today's date
# (drive_boot_vax.py start_single_user). The 1-JAN-2010 this rail used to show
# was NetBSD's built-in default leaking through a 2010-stamped root FFS, not
# SIMH -- an OVMX/VAX bug, now fixed, so the battery pins the host year AND
# today's date here exactly as on x86_64/aarch64.
export EXPECT_HOST_YEAR=1
# --- The shared battery's CONSOLE LOGIN-SEQUENCE gate (vms-3e9 a/b/c) ---------
# console_login_acceptance() in the shared battery proves, on THIS rail, that:
#   (a) the console prints the system-identification line before "Username:";
#   (b) OPA0: waits for the operator's RETURN before it prompts at all -- the
#       assertion that was RED here, and green on x86_64/aarch64, while the wake
#       was gated on isatty() (FALSE on the SIMH serial console, TRUE on QEMU's
#       virtio console); and
#   (c) an idle login prompt is disconnected and replaced after the LGI-style
#       deadline (tools/login_input.h LOGIN_INPUT_TIMEOUT_SEC, 20s).
# THIS IS THE VAX CONSOLE LEG. The only other gate that types at a booting
# console (tests/qemu/test_console_boot_no_newline_spam.sh) branches on
# `uname -m' over aarch64/x86_64 and launches qemu-system-* directly, so it
# cannot run on a SIMH/anita rail at all; the shared battery is the one place
# all three arches meet, which is why the sequence is asserted there.
# The two knobs below are the SIMH allowance: the emulated VAX runs a ~1980s CPU
# at a fraction of real speed, so the console needs longer to settle and the
# guest's own 20-second deadline can take substantially longer than 20 seconds
# of HOST wall-clock to elapse. They lengthen the WAIT, never the assertion.
export WAKE_QUIET_SECS="${WAKE_QUIET_SECS:-8}"
export LOGIN_IDLE_WAIT="${LOGIN_IDLE_WAIT:-120}"
# The single-disk boot is ~1980s VAX under SIMH: give the WHOLE run (boot + the
# operator-CR-feed to Username: + login + ~10 commands) a generous bound. The
# per-command wait stays CMD_TIMEOUT; the CR-feed loop stays BOOT_TIMEOUT.
# vms-3e9 raised this from 3000: the battery now opens with the console
# login-sequence gate, whose idle-at-the-prompt probe (LOGIN_IDLE_WAIT, 120s on
# this rail) plus the quiet-settle is ~150s of additional WAIT.
ACCEPT_TIMEOUT="${ACCEPT_TIMEOUT:-3300}"

WORKDIR="${NETBSD_WORKDIR:-/cache/single-work}"
SINGLE_RQ0_TYPE="${OVMX_SINGLE_RQ0_TYPE:-RAUSER=340}"
[ -f "$WORKDIR/wd0.img" ] || die "single-disk image $WORKDIR/wd0.img missing -- run-boot.sh must build sysboot-single before the acceptance run"

RAW="${OVMX_ACCEPT_RAW:-/cache/vax-acceptance-console.raw}"
FIFO="${OVMX_ACCEPT_FIFO:-/cache/vax-acceptance-console.fifo}"
rm -f "$RAW" "$FIFO"
: > "$RAW"
mkfifo "$FIFO" || die "mkfifo $FIFO failed"

log "expected boot banner (ovmx_identity.h): $EXPECTED_BOOT_BANNER"
log "expected compat version (ovmx_compat_version, VAX VMS lineage V7.3): $EXPECTED_COMPAT_VERSION"
log "volume label: $VOLUME_LABEL ; workdir: $WORKDIR ; rq0: $SINGLE_RQ0_TYPE"
log "console RAW=$RAW FIFO=$FIFO  (whole-run cap ${ACCEPT_TIMEOUT}s)"

# --- Launch the SIMH console bridge (do_acceptance) in the background ---------
# It boots the single disk and bridges console<->RAW/FIFO. Bounded so nothing
# hangs; if it dies the battery's wait_for sees the guest die and reds honestly.
OVMX_MODE=acceptance \
OVMX_ACCEPT_RAW="$RAW" OVMX_ACCEPT_FIFO="$FIFO" \
NETBSD_WORKDIR="$WORKDIR" OVMX_SINGLE_RQ0_TYPE="$SINGLE_RQ0_TYPE" \
NETBSD_BOOT_DEADLINE="$ACCEPT_TIMEOUT" \
    timeout --kill-after=30 "$ACCEPT_TIMEOUT" python3 "$DRIVER" &
QP=$!

# LOG is what the shared battery reads directly (boot banner + Username: feed).
export LOG="$RAW"

# Opening the FIFO write end blocks until do_acceptance opens its read end (it
# does so before booting), so this also serves as a readiness barrier.
exec 6>"$FIFO"
trap "" PIPE   # a CR fed just as the guest exits must not kill this shell

# --- caller-provided console primitives the shared battery drives ------------
# IDENTICAL semantics to tools/cross-alpha/run-boot-alpha.sh's in-container
# primitives -- the whole point of the shared battery is that these are the only
# arch-specific glue.
send()    { printf "%s\r" "$1" >&6 2>/dev/null || true; }
# wait_for <pat> <secs> <since-byte> -- fixed-string, bounded, dies if guest dies.
wait_for() {
    local pat="$1" lim="${2:-30}" since="${3:-0}" w=0
    while [ "$w" -lt "$lim" ]; do
        tail -c "+$((since + 1))" "$RAW" 2>/dev/null | grep >/dev/null -aF -- "$pat" && return 0
        kill -0 "$QP" 2>/dev/null || return 1
        sleep 1; w=$((w + 1))
    done
    return 1
}
# run_cmd <cmd> -- send, wait for the returned DCL prompt, set SEG (CR-stripped).
SEG=""
run_cmd() {
    local cmd="$1" off
    off=$(wc -c < "$RAW")
    send "$cmd"
    wait_for '$ ' "$CMD_TIMEOUT" "$off"
    sleep 1
    SEG=$(tail -c "+$((off + 1))" "$RAW" 2>/dev/null | tr -d "\r")
}

# --- run the SHARED battery (no errexit: it relies on grep exit codes) --------
PASS=0
FAIL=0
FLOOR_KB="${OVMX_VAX_FREE_FLOOR_KB:-}"
# The floor leg boots a deliberately small VAX (16M): the full battery does not
# fit it, so that leg stops after login and measures there (rd vms-83d).
if [ -n "$FLOOR_KB" ] && [ -n "${OVMX_VAX_MEMORY:-}" ]; then
    export OVMX_BATTERY_STOP_AFTER_LOGIN=1
fi
set +e
# shellcheck source=../qemu/lib/dcl_acceptance_battery.sh
. "$BATTERY"
run_dcl_acceptance_battery
BRC=$?

# rd vms-83d: FREE-MEMORY FLOOR. The 16 MB VAX demo node (Node B) starved at
# boot -- free memory ~20 KB, tmpfs refusing writes ('No space left on device'),
# a console that never offered Username: -- and no gate noticed until the
# visitor matrix. OVMX_VAX_FREE_FLOOR_KB (set by the 16 MB CI leg together with
# OVMX_VAX_MEMORY=16M) asserts that, after boot and login (the 16M leg stops
# there), SHOW MEMORY (the executive's own uvm figures) still reports at least that
# much Free. A footprint regression now reds here, not in the visitor gate.
if [ -n "$FLOOR_KB" ]; then
    run_cmd 'SHOW MEMORY'
    FREE_PAGES=$(printf '%s\n' "$SEG" | awk '/Main Memory \(/ { for (i = 1; i <= NF; i++) if ($i ~ /^\(.*Mb\)$/) { print $(i+2); exit } }')
    TOTAL_PAGES=$(printf '%s\n' "$SEG" | awk '/Main Memory \(/ { for (i = 1; i <= NF; i++) if ($i ~ /^\(.*Mb\)$/) { print $(i+1); exit } }')
    if [ -z "$FREE_PAGES" ] || [ -z "$TOTAL_PAGES" ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: free-memory floor: could not read Main Memory Total/Free from SHOW MEMORY"
        printf '%s\n' "$SEG" | tail -12
    else
        FREE_KB=$((FREE_PAGES / 2))
        TOTAL_KB=$((TOTAL_PAGES / 2))
        log "SHOW MEMORY after the battery: total ${TOTAL_KB} KB, free ${FREE_KB} KB (floor ${FLOOR_KB} KB)"
        if [ -n "${OVMX_VAX_MEMORY:-}" ]; then
            want=$(( ${OVMX_VAX_MEMORY%[Mm]} * 1024 ))
            # The kernel image and reserved pages come off the top; the total
            # must still show the node was really booted this small.
            if [ "$TOTAL_KB" -gt "$want" ]; then
                FAIL=$((FAIL + 1))
                echo "  FAIL: free-memory floor: guest reports ${TOTAL_KB} KB total, more than the ${want} KB it was booted with"
            fi
        fi
        if [ "$FREE_KB" -ge "$FLOOR_KB" ]; then
            PASS=$((PASS + 1))
            echo "  PASS: free-memory floor: ${FREE_KB} KB free >= ${FLOOR_KB} KB"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: free-memory floor: ${FREE_KB} KB free < ${FLOOR_KB} KB on a ${TOTAL_KB} KB node (rd vms-83d)"
        fi
    fi
fi

exec 6>&-
sleep 2
kill "$QP" 2>/dev/null || true
wait "$QP" 2>/dev/null || true
rm -f "$FIFO"

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ] && [ "$BRC" -ne 1 ]; then
    echo "ALL DCL/SHOW ACCEPTANCE CHECKS PASSED (OVMX/NetBSD-vax)"
    exit 0
fi
echo ""
echo "NOTE: red lines above name the bug each guards. On VAX this gate is"
echo "EXPECTED to be RED until the VAX faithful-output fixes land -- it must NOT"
echo "be weakened to go green. It goes green only when the OVMX/NetBSD-vax runtime"
echo "produces VMS-faithful output for these basic commands."
echo ""
echo "--- console log (last 400 lines) ---"
tail -400 "$RAW" 2>/dev/null | tr -d '\r'
exit 1
