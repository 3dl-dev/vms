#!/bin/bash
# test_dcl_acceptance_e2e.sh - boot-and-RUN-COMMANDS DCL/SHOW acceptance gate.
#
# THE HOLE THIS CLOSES (the systemic fix the operator demanded):
#   V0.5-2 shipped with SHOW USERS empty, SHOW DEVICE not reporting Mounted,
#   SHOW QUOTA fabricating a UIC, and WRITE F$xxx() printing the literal --
#   NONE of it caught, because tests/qemu/test_release_acceptance_e2e.sh boots,
#   logs in, and then checks only `PRODUCT SHOW PRODUCT`'s version string. It
#   never runs the commands a user actually types. A release can go out with
#   every basic SHOW command broken and still pass the acceptance gate.
#
#   This gate boots the REAL runtime (the pre-mastered ODS-2 ovmx-distrib.img,
#   same disk test_distrib_boot.sh boots), logs in SYSTEM/MANAGER, runs a
#   battery of the most basic DCL/SHOW commands a user types on first login,
#   and ASSERTS THE OUTPUT IS VMS-FAITHFUL -- grounded in real OpenVMS SHOW
#   output (project Rule 1), not merely "non-empty".
#
# EACH ASSERTION CARRIES A NEGATIVE CONTROL (negctl below): every command
# segment is run through a search that must FIND a token known to be present
# (the echoed command) and REJECT a random sentinel known to be absent. That
# proves the search mechanism over THAT segment can actually report both
# present and absent -- so a green "the bug marker is absent" line is real
# evidence, not a vacuous pass over an empty/unsearchable segment.
#
# THIS GATE IS EXPECTED TO BE RED until the in-flight cluster of fixes lands.
# Each assertion names the bug it guards:
#   - SHOW USERS empty / "0 users"          -> vms-01f / vms-72c
#   - SHOW DEVICE not "Mounted" (bare Online) -> vms-e6f
#   - SHOW DEVICES (plural) %DCL-*-IVKEYW    -> vms-9344 (surface)
#   - WRITE F$GETSYI() prints the literal    -> vms-65f
#   - SHOW QUOTA fabricated "[200,1]"        -> vms-73c4
# A red line here means that command is still broken; do NOT weaken the
# assertion to make it pass. It goes green only when the runtime is fixed.
#
# Usage (run INSIDE the ovmx-boot image, which supplies qemu-system-* AND the
# pre-mastered /boot/{vmlinuz,initramfs-ovmx-slim.cpio.gz,ovmx-distrib.img}):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm \
#       -e EXPECTED_BOOT_BANNER='OpenVMX V0.5-2' \
#       -e EXPECTED_COMPAT_VERSION='V9.2-3' \
#       -v $PWD/tests/qemu/test_dcl_acceptance_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
# (tests/qemu/run_dcl_acceptance_e2e.sh derives the two EXPECTED_* values from
# src/libvms/include/ovmx_identity.h -- the INV-1 single source -- and invokes
# this the same way.)
#
# Env knobs:
#   EXPECTED_BOOT_BANNER    the brand+version the boot must print (e.g.
#                           "OpenVMX V0.5-2"). Default derived from a copy of
#                           ovmx_identity.h if present, else "OpenVMX".
#   EXPECTED_COMPAT_VERSION the VMS-compat version F$GETSYI("VERSION") must
#                           report (e.g. "V9.2-3"). Default "V9.2-3".
#   BOOT_TIMEOUT            seconds to reach the login prompt (default 180).
#   CMD_TIMEOUT            seconds for a single command to return the prompt
#                           (default 30).
#
# Exit 0 = every basic command produced VMS-faithful output. Exit 1 = at least
# one command is broken (see the printed transcript). RED-UNTIL-FIXED is the
# CORRECT state of this gate today.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
CMD_TIMEOUT="${CMD_TIMEOUT:-30}"
EXPECTED_BOOT_BANNER="${EXPECTED_BOOT_BANNER:-OpenVMX}"
EXPECTED_COMPAT_VERSION="${EXPECTED_COMPAT_VERSION:-V9.2-3}"
# Ground-source constants, from distro/Dockerfile.bootable's master step and
# src/kernel/vms_internal.h (VMS_SYSTEM_UIC == [1,4]):
VOLUME_LABEL="OVMXSYS"    # vmsfs_master --ods2 ... OVMXSYS (Dockerfile.bootable)
SYSTEM_UIC='[1,4]'        # SYSTEM's UIC; the fabricated bug value is [200,1]

DISTRIB_IMG=/boot/ovmx-distrib.img
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
    # rd vms-76c3: F$GETSYI("ARCH_NAME") on an __aarch64__ build -> ovmx_hw_arch() = "AARCH64".
    EXPECTED_ARCH_NAME="AARCH64"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
    # rd vms-76c3: F$GETSYI("ARCH_NAME") on an __x86_64__ build -> ovmx_hw_arch() = "X86_64".
    EXPECTED_ARCH_NAME="X86_64"
fi

for f in "$KERNEL" "$SLIM_INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this INSIDE the ovmx-boot image (distro/Dockerfile.bootable), which bakes in the pre-mastered distribution disk"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
# The assertion helpers (ok/bad/must_have/must_match/must_not_have/negctl) AND
# the login+command battery (run_dcl_acceptance_battery) live in the SHARED,
# arch-independent battery that x86_64 and Alpha both drive, so the two release
# co-arches can never diverge in what "DCL/SHOW acceptance" asserts. This script
# supplies the x86_64/aarch64 console primitives (send/wait_for/run_cmd/SEG) and
# the EXPECTED_* vars; the shared file supplies the assertions. See the contract
# in tests/qemu/lib/dcl_acceptance_battery.sh.
BATTERY_LIB="${OVMX_BATTERY_LIB:-$(dirname "$0")/lib/dcl_acceptance_battery.sh}"
[ -f "$BATTERY_LIB" ] || { echo "FATAL: shared DCL acceptance battery not found at '$BATTERY_LIB' (set OVMX_BATTERY_LIB or mount tests/qemu/lib/dcl_acceptance_battery.sh)"; exit 1; }
# shellcheck source=lib/dcl_acceptance_battery.sh
. "$BATTERY_LIB"

echo "=== OVMX DCL/SHOW acceptance e2e: boot the runtime, log in, RUN THE COMMANDS A USER TYPES, assert VMS-faithful output ==="
echo "arch=$ARCH qemu=$QEMU"
echo "expected boot banner   : $EXPECTED_BOOT_BANNER"
echo "expected compat version: $EXPECTED_COMPAT_VERSION (F\$GETSYI VERSION)"
echo "volume label           : $VOLUME_LABEL ; SYSTEM UIC: $SYSTEM_UIC"
echo ""

DISK=/tmp/dcl-acceptance-e2e.img
LOG=/tmp/dcl-acceptance-e2e-console.log
FIFO=/tmp/dcl-acceptance-e2e-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

# Whole-VM hard cap: boot + a settle + ~13 commands, each bounded by CMD_TIMEOUT.
WALL=$((BOOT_TIMEOUT + CMD_TIMEOUT * 16 + 120))

cleanup() { exec 4>&- 2>/dev/null || true; [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -f "$FIFO"; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$WALL" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern limit-seconds since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qaF -- "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}
# run_cmd CMD  ->  echoes CMD, waits for the returned DCL prompt, sets SEG to
# everything the command produced (its echo + output + trailing prompt).
SEG=""
run_cmd() {
    local cmd="$1" off
    off=$(wc -c <"$LOG")
    send "$cmd"
    # Wait for the returned "$ " prompt after the command completes. Bounded;
    # if it never returns, SEG still captures whatever appeared (the caller's
    # assertions then go red honestly rather than the script hanging).
    wait_for '$ ' "$CMD_TIMEOUT" "$off"
    sleep 1   # settle: let the last line flush after the prompt is seen
    SEG=$(segment_since "$off")
}

# --- Drive the runtime: boot to login, log in, run the battery, assert -------
# The shared battery does the executive-attach check, the OPA0: CR-feed to the
# Username: prompt, the boot-banner assertion, SYSTEM/MANAGER login, and the full
# basic-command battery -- byte-identical to what Alpha runs. It returns nonzero
# ONLY on a hard boot/login failure (never reached Username: / login rejected),
# which we surface with the same console dump dump_and_die would.
if ! run_dcl_acceptance_battery; then
    dump_and_die "runtime never reached an authenticated DCL prompt (boot or SYSTEM login failed)"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL DCL/SHOW ACCEPTANCE CHECKS PASSED"
    exit 0
fi
echo ""
echo "NOTE: red lines above name the bug each guards. This gate is EXPECTED to"
echo "be RED until the in-flight command fixes land -- it must NOT be weakened"
echo "to go green. It goes green only when the runtime produces VMS-faithful"
echo "output for these basic commands."
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
