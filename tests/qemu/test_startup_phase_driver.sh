#!/bin/bash
# test_startup_phase_driver.sh - STARTUP.COM is a real STDRV phase driver
# over a real SYS$STARTUP search list, and OVMX.CONF is gone (vms-21a).
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the SLIM
# initramfs + /boot/ovmx-distrib.img, the mastered distribution system disk).
#
# WHAT THIS PROVES (docs/design-boot-faithful.md §3.2/§3.3, target shape #4):
#
#   Boots the pre-installed distribution disk (same shape as
#   test_distrib_boot.sh) and, once logged in, drives real DCL commands
#   against the REAL executive/vmsfs (not a build-time inspection) to show:
#
#     - SHOW LOGICAL SYS$STARTUP reports the measured multi-value search
#       list (two equivalence strings, the second literally "SYS$MANAGER") --
#       proof DEFINE/SYSTEM's vms-420 multi-value path is what actually ran
#       at boot, not just testable in isolation.
#     - DIRECTORY SYS$STARTUP:*.DAT lists VMS$PHASES.DAT and VMS$VMS.DAT --
#       the phase driver's data files are really on the booted system disk.
#     - DIRECTORY SYS$MANAGER:*.COM lists SYCONFIG.COM and SYLOGICALS.COM --
#       the new site files STARTUP.COM's phase driver invokes exist.
#     - DIRECTORY SYS$MANAGER:OVMX.CONF reports zero files -- the deleted
#       config file is not merely absent from the source tree, it is absent
#       from the booted, mounted volume a session actually sees.
#
#   The boot console log (captured from the same boot, before login) is also
#   checked for the LPMAIN-phase site announcement line and for the absence
#   of any %RMS-E-FNF / %DCL-E-OPENIN error, which would mean the phase
#   driver's own OPEN calls failed silently on this disk.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_startup_phase_driver.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

TIMEOUT=90
DISTRIB_IMG=/boot/ovmx-distrib.img
DISK="/tmp/test-phase-driver-sysdisk.img"
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== OVMX STARTUP.COM Phase Driver Test (vms-21a) ==="
echo "Architecture: $ARCH   QEMU: $QEMU"
echo "Kernel: $KERNEL   Slim initrd: $SLIM_INITRD   Distribution image: $DISTRIB_IMG"
echo ""

if [ -f "$DISTRIB_IMG" ]; then
    record "Mastered distribution image present in the built image" 0
else
    record "Mastered distribution image present in the built image" 1
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi

rm -f "$DISK"
cp "$DISTRIB_IMG" "$DISK"

CONSOLE_LOG="/tmp/test-phase-driver-console.log"
FIFO="/tmp/test-phase-driver-console.in"

boot() {
    rm -f "$CONSOLE_LOG" "$FIFO"
    mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$SLIM_INITRD" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$DISK",format=raw,if=virtio \
        -no-reboot \
        <"$FIFO" >"$CONSOLE_LOG" 2>&1 &
    QEMU_PID=$!
    exec 4>"$FIFO"
}

cleanup() {
    exec 4>&- 2>/dev/null || true
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}

send() { printf '%s\r' "$1" >&4; }

wait_for() {
    local pattern="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$CONSOLE_LOG" 2>/dev/null | grep >/dev/null -F "$pattern"; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    return 1
}

boot
trap cleanup EXIT

OK=1

if wait_for ' - OpenVMS-compatible' 60; then rc=0; else rc=1; OK=0; fi
record "executive attached" "$rc"

if [ "$OK" -eq 1 ]; then
    send ''  # vms-2213: wake OPA0: — LOGINOUT waits for RETURN before Username:
    if wait_for 'Username:' 60; then rc=0; else rc=1; OK=0; fi
    record "reaches the login prompt (phase driver ran to completion)" "$rc"
fi

# Snapshot the BOOT-ONLY portion of the console (everything up to the first
# login prompt) before any interactive command runs below -- a later
# DIRECTORY of a deliberately-absent file is expected to print "0 files" and
# must never be mistaken for a boot-time OPEN failure.
BOOT_LOG=""
if [ "$OK" -eq 1 ]; then
    BOOT_LOG=$(cat "$CONSOLE_LOG")
fi

if [ "$OK" -eq 1 ]; then
    LOGIN_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'SYSTEM'
    if wait_for 'Password:' 30 "$LOGIN_OFFSET"; then rc=0; else rc=1; OK=0; fi
    record "password prompt appears" "$rc"
fi

if [ "$OK" -eq 1 ]; then
    send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFFSET" && wait_for '$ ' 90 "$LOGIN_OFFSET"; then rc=0; else rc=1; OK=0; fi
    record "SYSTEM login succeeds" "$rc"
fi

# --- SHOW LOGICAL SYS$STARTUP: the measured multi-value search list --------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'SHOW LOGICAL SYS$STARTUP'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "SHOW LOGICAL SYS\$STARTUP returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$SEG" | grep >/dev/null -F 'SYS$STARTUP'; then r1=0; else r1=1; fi
        record "SHOW LOGICAL SYS\$STARTUP names the logical" "$r1"
        if printf '%s' "$SEG" | grep >/dev/null -F 'SYS$MANAGER'; then r2=0; else r2=1; fi
        record "SYS\$STARTUP search list carries SYS\$MANAGER as an element (vms-420 multi-value)" "$r2"
    fi
fi

# --- DIRECTORY SYS$STARTUP:*.DAT: the phase driver's data files ------------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$STARTUP:*.DAT'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "DIRECTORY SYS\$STARTUP:*.DAT returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$SEG" | grep >/dev/null -F 'VMS$PHASES.DAT'; then r1=0; else r1=1; fi
        record "SYS\$STARTUP: carries VMS\$PHASES.DAT on the booted disk" "$r1"
        if printf '%s' "$SEG" | grep >/dev/null -F 'VMS$VMS.DAT'; then r2=0; else r2=1; fi
        record "SYS\$STARTUP: carries VMS\$VMS.DAT on the booted disk" "$r2"
    fi
fi

# --- DIRECTORY SYS$MANAGER:*.COM: the new site files ------------------------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$MANAGER:*.COM'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "DIRECTORY SYS\$MANAGER:*.COM returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        if printf '%s' "$SEG" | grep >/dev/null -F 'SYCONFIG.COM'; then r1=0; else r1=1; fi
        record "SYS\$MANAGER: carries SYCONFIG.COM" "$r1"
        if printf '%s' "$SEG" | grep >/dev/null -F 'SYLOGICALS.COM'; then r2=0; else r2=1; fi
        record "SYS\$MANAGER: carries SYLOGICALS.COM" "$r2"
        # STARTNET.COM (rd vms-a70): the DECnet Phase IV startup procedure the
        # LPBETA VMS$VMS.DAT component runs is really on the booted disk.
        if printf '%s' "$SEG" | grep >/dev/null -F 'STARTNET.COM'; then r3=0; else r3=1; fi
        record "SYS\$MANAGER: carries STARTNET.COM (DECnet Phase IV startup)" "$r3"
    fi
fi

# --- VMS$VMS.DAT REGISTERS STARTNET at LPBETA (rd vms-a70) -------------------
# Because STARTNET self-gates SILENT on an unconfigured node (above), its LPBETA
# component leaves no console trace on this boot -- so prove the registration
# itself is present in the phase table the driver reads. This is the positive
# proof that STARTNET IS wired into the boot at LPBETA (a dropped registration
# line would be caught here, where the silent-no-op runtime check cannot see it).
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'TYPE SYS$STARTUP:VMS$VMS.DAT'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "TYPE SYS\$STARTUP:VMS\$VMS.DAT returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        # A phase line naming both the LPBETA phase and STARTNET.COM.
        if printf '%s' "$SEG" | grep >/dev/null -iE 'LPBETA[[:space:]].*STARTNET'; then r1=0; else r1=1; fi
        record "VMS\$VMS.DAT registers @SYS\$MANAGER:STARTNET at the LPBETA phase" "$r1"
    fi
fi

# --- DIRECTORY SYS$MANAGER:OVMX.CONF: must find nothing ---------------------
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'DIRECTORY SYS$MANAGER:OVMX.CONF'
    if wait_for '$ ' 15 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "DIRECTORY SYS\$MANAGER:OVMX.CONF returns" "$rc"

    if [ "$rc" -eq 0 ]; then
        SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
        # Authentic VMS for a zero-match exact filespec is
        # "%DIRECT-W-NOFILES, no files found" (VSI OpenVMS DCL Dictionary,
        # DIRECTORY; landed in vms-1c6 #461) -- NOT an empty "Total of 0 files."
        # trailer. Either is proof the file is absent; a PRESENT OVMX.CONF would
        # instead list the name with "Total of 1 file.", so this stays
        # discriminating.
        if printf '%s' "$SEG" | grep >/dev/null -E '%DIRECT-W-NOFILES|Total of 0 file'; then r1=0; else r1=1; fi
        record "OVMX.CONF does not exist on the booted, mounted system disk" "$r1"
    fi
fi

# --- rd vms-158d: OVMX's boot narration is in OPERATOR.LOG, not on OPA0: ----
# A real VMS boot console prints no "executive attached", "mounting system
# disk", "system disk mounted", "node name set" or "system identity" line, so
# PID 1 and PROVISION file them as operator-log records. They must be THERE --
# re-routed, never dropped -- and NOT on the boot console.
if [ "$OK" -eq 1 ]; then
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'TYPE SYS$MANAGER:OPERATOR.LOG'
    if wait_for '$ ' 30 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "TYPE SYS\$MANAGER:OPERATOR.LOG returns" "$rc"
    OPLOG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
    for want in '%OVMX-I-EXEC, VMS executive attached on /dev/vms' \
                '%OVMX-I-SYSDISK, mounting system disk VDA0:' \
                '%OVMX-I-MOUNTED, system disk VDA0: mounted' \
                '%OVMX-I-SCSNODE, node name ' \
                '%OVMX-I-EXEC, system identity SYSTEM [1,4] established by the executive'; do
        if printf '%s' "$OPLOG" | grep >/dev/null -F "$want"; then r1=0; else r1=1; fi
        record "OPERATOR.LOG carries the boot record '$want'" "$r1"
        if printf '%s' "$BOOT_LOG" | grep >/dev/null -F "$want"; then r1=1; else r1=0; fi
        record "the boot console does NOT carry '$want' (no VMS console prints it)" "$r1"
    done

    # Unconfigured DECnet: STARTNET launched no NETACP (it prints no launch
    # line of its own any more, so ask the executive's process table).
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'SHOW SYSTEM'
    if wait_for '$ ' 30 "$CMD_OFFSET"; then rc=0; else rc=1; fi
    record "SHOW SYSTEM returns" "$rc"
    SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
    if printf '%s' "$SEG" | grep >/dev/null -E '^[0-9A-Fa-f]{8} +NETACP'; then r1=1; else r1=0; fi
    record "STARTNET launched NO NETACP on an unconfigured boot (SHOW SYSTEM lists none)" "$r1"
    # rd vms-798: OPCOM, the operator communication process, runs -- created by
    # PID 1 before STARTUP.COM, DETACHED, never root -- and enabled OPA0: as
    # the operator terminal, saying so on it the way a real VAX does.
    if printf '%s' "$SEG" | grep >/dev/null -E '^[0-9A-Fa-f]{8} +OPCOM'; then r1=0; else r1=1; fi
    record "SHOW SYSTEM lists the OPCOM process" "$r1"
    if grep >/dev/null -aE 'Operator _[A-Z0-9$_]+\$OPA0: has been enabled, username SYSTEM' "$CONSOLE_LOG"; then r1=0; else r1=1; fi
    record "OPCOM enabled OPA0: as an operator terminal and said so on the console" "$r1"
    if grep >/dev/null -aE '^%%%%%%%%%%%  OPCOM +[ 0-9]{2}-[A-Z]{3}-[0-9]{4} [0-9:.]+  %%%%%%%%%%%' "$CONSOLE_LOG"; then r1=0; else r1=1; fi
    record "the OPCOM banner on OPA0: is VMS's (eleven %, two spaces, OPCOM, the time)" "$r1"
    if grep >/dev/null -aF '%OVMX-W-OPCOM' "$CONSOLE_LOG"; then r1=1; else r1=0; fi
    record "OPCOM was created (no %OVMX-W-OPCOM on the console)" "$r1"

    # $SNDOPR goes THROUGH OPCOM: a REQUEST is broadcast on OPA0: in OPCOM's
    # format, naming the requester from the executive, and filed in the log.
    CMD_OFFSET=$(wc -c <"$CONSOLE_LOG")
    send 'REQUEST "VMS-798 OPCOM PROBE"'
    sleep 4
    SEG=$(tail -c "+$((CMD_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')
    if printf '%s' "$SEG" | grep >/dev/null -E '^Message from user SYSTEM on [A-Z0-9$_]+$'; then r1=0; else r1=1; fi
    record "REQUEST reaches OPA0: through OPCOM ('Message from user SYSTEM on <node>')" "$r1"
    if printf '%s' "$SEG" | grep >/dev/null -F 'VMS-798 OPCOM PROBE'; then r1=0; else r1=1; fi
    record "... carrying the request's text" "$r1"
fi

cleanup
trap - EXIT
echo ""

# --- Boot console: the phase driver actually ran, and did not fail an OPEN --
# Checked against BOOT_LOG (captured above, before any interactive DIRECTORY
# command that legitimately expects a "0 files" no-match) so a later,
# intentional absence-check can never be mistaken for a boot-time failure.
echo "--- Boot console checks (from the same boot, pre-login) ---"
if printf '%s' "$BOOT_LOG" | grep >/dev/null -F 'The OVMX system is now executing the site-specific startup commands.'; then
    rc=0
else
    rc=1
fi
record "LPMAIN-phase site announcement appears (SYSTARTUP_VMS.COM ran from inside the phase driver)" "$rc"

if printf '%s' "$BOOT_LOG" | grep >/dev/null -F '%RMS-E-FNF'; then rc=1; else rc=0; fi
record "no %RMS-E-FNF during boot (phase driver's OPENs all found their files)" "$rc"

if printf '%s' "$BOOT_LOG" | grep >/dev/null -F '%DCL-E-OPENIN'; then rc=1; else rc=0; fi
record "no %DCL-E-OPENIN during boot" "$rc"

# --- DECnet STARTNET at LPBETA is a faithful SILENT no-op when UNCONFIGURED ----
# (rd vms-a70 direction B). The LPBETA VMS$VMS.DAT component @SYS$MANAGER:STARTNET
# runs during boot, but real OpenVMS runs NO DECnet on an unconfigured node --
# STARTNET is invoked only when the Phase IV databases exist. The mastered disk
# carries NO DECnet configuration (no SYS$SYSTEM:NETNODE_LOCAL.DAT), so on this
# boot STARTNET must gate itself OFF with a pure DCL F$SEARCH and stay SILENT:
# launch no NETACP and print NOTHING to the pristine boot console the vms-1fb
# oracle pins. (The daemon serve-decision matrix is proven at the unit level by
# tests/vmsdecnet/test_decnetd_startnet_gate.sh; the configured -> serve path by
# the live inbound-SET-HOST bracket -- neither belongs on this unconfigured boot.)
#
# What we assert here is the LPBETA component ran AND self-gated cleanly: no
# %DECNET-I-STARTNET announcement, no NETACP %RUN-S-PROC_ID, no %DCL abort, and
# no %DECNETD-E-NOADDRESS. A foreground DECNETD probe whose nonzero exit DCL would
# render as %DCL-E-ABORT is exactly the pollution this guards against (vms-a70).
if printf '%s' "$BOOT_LOG" | grep >/dev/null -F 'DECnet Phase IV -- Startup (STARTNET)'; then rc=1; else rc=0; fi
record "STARTNET is SILENT on an unconfigured boot (no DECnet startup announcement -- faithful: real VMS runs no STARTNET unconfigured)" "$rc"

if printf '%s' "$BOOT_LOG" | grep >/dev/null -F '%DECNET-I-STARTNET'; then rc=1; else rc=0; fi
record "no %DECNET-I-STARTNET on the console (STARTNET prints no launch line of its own, rd vms-158d)" "$rc"

if printf '%s' "$BOOT_LOG" | grep >/dev/null -E '%DCL-[EF]-ABORT'; then rc=1; else rc=0; fi
record "STARTNET leaves NO %DCL-E-ABORT on the boot console (config check is a DCL F\$SEARCH, not a foreground probe)" "$rc"

if printf '%s' "$BOOT_LOG" | grep >/dev/null -F 'DECNETD-E-NOADDRESS'; then rc=1; else rc=0; fi
record "STARTNET leaves NO %DECNETD-E-NOADDRESS on the boot console (unconfigured = clean no-op, not an error)" "$rc"

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "--- Full console log ---"
    cat "$CONSOLE_LOG"
fi

[ "$FAIL" -eq 0 ]
