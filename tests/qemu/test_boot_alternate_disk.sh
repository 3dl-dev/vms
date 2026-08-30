#!/bin/bash
# test_boot_alternate_disk.sh - Boot from a NON-DEFAULT system disk (vms-9f5).
#
# Runs inside the ovmx-boot Docker image (QEMU + kernel + SLIM initramfs +
# /boot/ovmx-distrib.img, the mastered distribution system disk).
#
# WHAT THIS PROVES (the vms-9f5 DONE-condition, "boot from a NON-DEFAULT/second
# disk", currently a latent-bug regression that is impossible on pristine main):
#
#   Device-native naming (vms-9f5) makes the system disk DISCOVERED, not
#   compile-frozen to vda/VDA0:. PID 1 selects WHICH unit to $MOUNT as
#   SYS$SYSDEVICE from a runtime selector -- an "ovmx.sysdev=<unit>" token on the
#   kernel command line (or OVMX_SYSDEVICE in the environment) -- defaulting to
#   the substrate's SYSDISK_DEVICE (VDA0: on virtio). So a system that lives on
#   the SECOND virtio disk (vdb -> VDA100:) is reachable by naming it, where
#   before the boot chain always mounted vda and a second-disk system was dead.
#
# THREE cases, and why each is here:
#   A (default sanity): the real system disk as the SOLE disk (vda -> VDA0:), no
#     selector -> reaches login. Proves the discovery default did not break the
#     ordinary single-disk boot. Passes on pristine main too (it is not the
#     regression, just a guard that this change is zero-regression for the norm).
#   B (THE REGRESSION): a BLANK decoy as vda + the real system as vdb, with
#     ovmx.sysdev=VDA100: -> PID 1 mounts vdb and reaches login. On PRISTINE MAIN
#     this FAILS: main ignores the selector, mounts vda (the blank decoy), finds
#     no installed volume, and halts -- no login. This case failing on main is
#     the required "fails on pristine main" property.
#   C (anti-LARP control): the SAME two disks (blank vda + real vdb) but NO
#     selector -> must NOT reach login. Proves the decoy really is on vda (so B's
#     success is due to the selector, not QEMU putting the real disk on vda by
#     chance). Expected to pass on main and on this branch alike -- it is a
#     control, not the regression.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_boot_alternate_disk.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail
# A boot that HALTS (Case C, by design; or a failing discovery) makes QEMU exit,
# after which the wake_login CRs write to a closed console FIFO. Ignore SIGPIPE
# so that broken-pipe write fails quietly (EPIPE) instead of killing the script
# with exit 141 -- the case must be REPORTED (PASS/FAIL), never crash the harness.
trap '' PIPE

TIMEOUT=90
DISTRIB_IMG=/boot/ovmx-distrib.img
REAL="/tmp/test-altdisk-real.img"      # the genuine mastered system disk
DECOY="/tmp/test-altdisk-decoy.img"    # a blank disk that is NOT a system volume
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
        echo "  PASS: $desc"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"; FAIL=$((FAIL + 1))
    fi
}

echo "=== OVMX Alternate-Disk Boot Test (vms-9f5) ==="
echo "Architecture: $ARCH"
echo "QEMU: $QEMU"
echo ""

# --- Precondition: the mastered image exists ---
if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi

# The real system disk is a writable byte copy of the mastered image; the decoy
# is a blank raw disk of the same size (a plausible attached disk that is NOT an
# installed system volume, so mounting it as the system disk fails honestly).
rm -f "$REAL" "$DECOY"
cp "$DISTRIB_IMG" "$REAL"
SZ=$(stat -c%s "$REAL")
# Sparse blank of the same size — never a valid ODS-2 system volume.
truncate -s "$SZ" "$DECOY"
echo "Real system disk: $SZ bytes; blank decoy: $(stat -c%s "$DECOY") bytes"
echo ""

CONSOLE_LOG="/tmp/test-altdisk-console.log"
FIFO="/tmp/test-altdisk-console.in"
QEMU_PID=""

# boot_case <extra-append> <drive-args...>
# Drives are passed in vda,vdb,... order (QEMU assigns virtio-blk by -drive order).
boot_case() {
    local extra_append="$1"; shift
    rm -f "$CONSOLE_LOG" "$FIFO"
    mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$SLIM_INITRD" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet $extra_append" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        "$@" \
        -no-reboot \
        <"$FIFO" >"$CONSOLE_LOG" 2>&1 &
    QEMU_PID=$!
    exec 4>"$FIFO"
}

cleanup() {
    exec 4>&- 2>/dev/null || true
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}

send() { printf '%s\r' "$1" >&4 2>/dev/null || true; }

wake_login() {
    local logf="$1" w=0
    # Stop as soon as the prompt appears, the budget is spent, OR QEMU has
    # exited (a halted boot -- Case C by design, or a failed discovery): keep
    # writing CRs into a dead console's FIFO otherwise, which is the SIGPIPE the
    # top-of-file `trap '' PIPE` now also neutralises.
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 100 ] \
          || ! kill -0 "$QEMU_PID" 2>/dev/null; do
        send ''; sleep 1; w=$((w + 1))
    done
}

# Dump the tail of the boot console for a case that did not go as expected, so a
# CI failure is DIAGNOSABLE (which unit PID 1 selected/mounted, where it halted).
dump_console() {
    echo "  --- boot console (tail) for: $1 ---"
    tail -n 40 "$CONSOLE_LOG" 2>/dev/null | sed 's/\r$//; s/^/    | /' || true
    echo "  --- end console ---"
}

wait_for() {
    local pattern="$1" limit="${2:-30}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if grep -qaF "$pattern" "$CONSOLE_LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            grep -qaF "$pattern" "$CONSOLE_LOG" 2>/dev/null && return 0
            return 1
        fi
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}

reaches_login() {   # 0 if 'Username:' appears within the window, else 1
    wake_login "$CONSOLE_LOG"
    wait_for 'Username:' 60
}

VDA_REAL=(-drive file="$REAL",format=raw,if=virtio)
# vda=decoy (first), vdb=real (second) -> the real system is VDA100:
VDB_LAYOUT=(-drive file="$DECOY",format=raw,if=virtio \
            -drive file="$REAL",format=raw,if=virtio)

# --- Case A: default single-disk boot (real disk as vda), no selector --------
echo "--- Case A: default boot, real system disk as VDA0: (no selector) ---"
boot_case "" "${VDA_REAL[@]}"
trap cleanup EXIT
if reaches_login; then rc=0; else rc=1; fi
record "Case A: default boot reaches login (VDA0:, zero-regression)" "$rc"
[ "$rc" -ne 0 ] && dump_console "Case A"
cleanup; trap - EXIT
echo ""

# --- Case B: alternate-disk boot, real system on vdb, selected by cmdline -----
echo "--- Case B: real system on vdb, ovmx.sysdev=VDA100: (THE regression) ---"
boot_case "ovmx.sysdev=VDA100:" "${VDB_LAYOUT[@]}"
trap cleanup EXIT
if reaches_login; then rc=0; else rc=1; fi
record "Case B: boots the SECOND disk (VDA100:) to login — fails on pristine main" "$rc"
# Corroborate: the mount narration names the selected unit, not VDA0:.
if grep -qaF 'system disk VDA100: mounted' "$CONSOLE_LOG"; then mrc=0; else mrc=1; fi
record "Case B: %OVMX-I-MOUNTED names the discovered unit VDA100:" "$mrc"
{ [ "$rc" -ne 0 ] || [ "$mrc" -ne 0 ]; } && dump_console "Case B"
cleanup; trap - EXIT
echo ""

# --- Case C: anti-LARP control — same disks, NO selector -> must NOT log in ---
echo "--- Case C: blank vda + real vdb, NO selector -> must NOT reach login ---"
boot_case "" "${VDB_LAYOUT[@]}"
trap cleanup EXIT
if reaches_login; then rc=1; else rc=0; fi   # inverted: login here would mean vda!=decoy
record "Case C: default picks the blank vda and halts (no login) — proves the decoy is on vda" "$rc"
[ "$rc" -ne 0 ] && dump_console "Case C"
cleanup; trap - EXIT
echo ""

echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

[ "$FAIL" -eq 0 ]
