#!/bin/bash
# test_job_control_negctl.sh - the negative half of vms-2a9: remove
# JOB_CONTROL's STDRV component registration and it is never created.
#
# WHAT THIS PROVES, END TO END, AGAINST A REAL (MUTATED) MASTERED IMAGE.
#
# tests/integration/test_job_control_component_registration.sh (the cheap,
# always-on half of this item) proves the SOURCE registers JOB_CONTROL as a
# CONFIG-phase STDRV component and no longer hardcodes the call anywhere
# else. tests/qemu/test_job_control_console.sh (vms-8d2) proves that with
# the registration PRESENT, a real boot creates a real, named, detached
# JOB_CONTROL process. Neither proves the other direction: that the
# registration is what actually GATES creation, not just one path among
# several that happens to work.
#
# This test boots /boot/ovmx-distrib-negctl.img -- a SECOND mastered disk,
# built by distro/Dockerfile.bootable from the identical staged system tree
# as the normal /boot/ovmx-distrib.img, with exactly one line removed:
# SYS$STARTUP:VMS$VMS.DAT's `CONFIG SYS$STARTUP:JOB_CONTROL_STARTUP.COM`
# registration (docs/design-boot-faithful.md §3.8). Nothing else differs.
#
# THE A-WRITES/B-READS SHAPE THIS TEST CANNOT USE, AND WHY THAT IS THE
# POINT. tests/qemu/test_job_control_console.sh proves JOB_CONTROL is real
# by having a DIFFERENT process (a DCL session logged in through it) read
# SHOW SYSTEM. On OVMX, JOB_CONTROL is the ONLY thing that creates the
# console login loop (vms-8d2, docs/design-init-scope.md §2/§5) -- so with
# its registration removed, NOTHING can ever log in to run SHOW SYSTEM in
# the first place. That is not a gap in this test's method; it IS the
# proof: the absence of JOB_CONTROL is observable as the boot silently
# running out the clock at a login prompt that never appears, because
# nothing else on OVMX stands in for the console session it owns.
#
# WHAT WOULD MAKE THIS TEST FAIL HONESTLY:
#   - the negctl image never reaches %STDRV-I-STARTUP / the site-specific
#     startup line (the mutation broke the boot generally, not just
#     JOB_CONTROL -- this test would then be proving nothing about
#     JOB_CONTROL specifically)
#   - %RUN-S-PROC_ID appears anywhere in the boot console (something is
#     still RUN/DETACHED-ing during this boot -- the component removal did
#     not actually stop JOB_CONTROL's creation)
#   - Username: appears (the console login loop exists despite the
#     registration's removal -- JOB_CONTROL is not really gating it)
#
# Usage (run INSIDE the bootable image, like test_job_control_console.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_job_control_negctl.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for the STDRV/site-startup landmarks
#                  (default 60). Also the bound on the "Username: never
#                  appears" negative wait, below.
#
# Exit 0 = all checks pass (the negative control held). Exit 1 = a real
# failure (see the printed transcript segment).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
NEGCTL_IMG=/boot/ovmx-distrib-negctl.img
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

for f in "$KERNEL" "$SLIM_INITRD" "$NEGCTL_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== JOB_CONTROL component-removal negative control (vms-2a9) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$SLIM_INITRD image=$NEGCTL_IMG"

DISK=/tmp/job-control-negctl.img
LOG=/tmp/job-control-negctl-console.log
FIFO=/tmp/job-control-negctl-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$NEGCTL_IMG" "$DISK"
mkfifo "$FIFO"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT * 2 + 60))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 384M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

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
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}

# --- 1. The boot still runs to completion: STDRV begins, and STARTUP.COM's
#        phase driver still reaches its site-specific-startup landmark. If
#        this fails, the mutation broke boot generally and the checks below
#        would prove nothing specific to JOB_CONTROL. -----------------------
if wait_for '%OVMX-I-EXEC' 30; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
if wait_for '%STDRV-I-STARTUP, OpenVMX startup begun' "$BOOT_TIMEOUT"; then
    ok "STDRV begun (STARTUP.COM ran despite the missing component)"
else
    dump_and_die "STDRV begun never printed within ${BOOT_TIMEOUT}s"
fi
STARTUP_OFF=$(wc -c <"$LOG")
if wait_for 'The OVMX system is now executing the site-specific startup commands.' "$BOOT_TIMEOUT" "$STARTUP_OFF"; then
    ok "STARTUP.COM's phase driver reached LPMAIN and ran SYSTARTUP_VMS.COM (boot did not merely crash early)"
else
    dump_and_die "site-specific startup line never printed within ${BOOT_TIMEOUT}s -- boot did not run to completion, this negative control is inconclusive"
fi

# --- 2. Give the boot the SAME window test_job_control_console.sh gives a
#        healthy boot to reach Username:, then confirm it never does. -------
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    bad "Username: appeared -- a console login loop exists despite JOB_CONTROL's registration being removed"
else
    ok "Username: never appeared within ${BOOT_TIMEOUT}s -- no console login loop was created"
fi

# --- 3. %RUN-S-PROC_ID (RUN/DETACHED's own creation announcement, the same
#        line the positive test asserts DOES appear) must never appear
#        anywhere in this boot: JOB_CONTROL was the only RUN/DETACHED call
#        this boot would ever make. --------------------------------------
FULL_LOG=$(tr -d '\r' <"$LOG")
if printf '%s' "$FULL_LOG" | grep -qF '%RUN-S-PROC_ID'; then
    bad "%RUN-S-PROC_ID appeared -- something was still RUN/DETACHED'd during this boot"
else
    ok "no %RUN-S-PROC_ID anywhere in the boot console -- JOB_CONTROL (or anything else) was never created"
fi

# --- Results ---
echo ""
echo "=========================================="
echo "  RESULTS: $PASS passed, $FAIL failed"
echo "=========================================="

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL JOB_CONTROL NEGATIVE-CONTROL CHECKS PASSED"
    exit 0
else
    echo "  JOB_CONTROL NEGATIVE-CONTROL CHECKS FAILED"
    echo ""
    echo "--- full console log ---"
    cat "$LOG"
    exit 1
fi
