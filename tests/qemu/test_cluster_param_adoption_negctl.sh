#!/bin/bash
# test_cluster_param_adoption_negctl.sh - THE MEASURED NEGATIVE CONTROL for
# tests/qemu/test_cluster_param_adoption.sh (vms-495, epic vms-098 R1.3).
#
# WHY THIS FILE EXISTS.
#
# The positive across-reboot proof asserts that after authoring
# SCSNODE=NODEB/SCSSYSTEMID=1026/VOTES=1/EXPECTED_VOTES=2 (CASE 1) or
# NODEC/1027/VOTES=2/EXPECTED_VOTES=3 (CASE 2 control) and rebooting, a fresh
# SCSD reads those values back. That is only worth something if those values
# could NOT have come from anywhere but the authored store -- if SCSD hardcoded
# them, or read a mock/wrong file, the positives would pass for the wrong
# reason. This control boots the SAME shipped image and does the OPPOSITE of the
# positive: it authors NOTHING. On the factory-seeded store (SCSNODE=OVMX,
# SCSSYSTEMID=0, VOTES=1, EXPECTED_VOTES=1 -- the documented SYSGEN defaults, VSI
# OpenVMS System Management Utilities Reference Manual), a fresh SCSD
# --show-identity MUST report those DEFAULTS and NEVER either positive's
# authored identity.
#
# This is MEASURED, not asserted in prose: it boots a real QEMU image, runs the
# real SCSD against the real unauthored volume, and checks what it actually
# printed. If SCSD ever reported NODEB/1026/EXPECTED_VOTES=2 (or the control's
# NODEC/1027/EXPECTED_VOTES=3) here -- with no authoring -- the positives' reads
# would be exposed as canned/wrong-store rather than adopted, and all three
# gates would (correctly) be worthless.
#
# WHAT WOULD MAKE THIS FAIL HONESTLY (i.e. catch the defect it guards): boot
# never reaches Username:; SCSD.EXE missing; SCSD --show-identity reports any of
# the positives' authored values (or anything other than the seed defaults)
# despite no authoring -- which would mean the value is not really read from the
# store.
#
# Usage (run INSIDE the bootable image):
#   docker run --rm -v $PWD/tests/qemu/test_cluster_param_adoption_negctl.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for boot to reach Username: (default 180).
#
# Exit 0 = the control behaved as designed (defaults read back, never authored).
# Exit 1 = the control is broken (see the printed transcript).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
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

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== NEGCTL: unauthored store -> SCSD reads the seed DEFAULTS, never the authored identity (vms-495) ==="
echo "arch=$ARCH qemu=$QEMU"

DISK=/tmp/clu-adopt-negctl.img
LOG=/tmp/clu-adopt-negctl-console.log
FIFO=/tmp/clu-adopt-negctl-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + 180))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
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
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}

# --- 1. Boot to login --------------------------------------------------------
if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
send ''   # wake OPA0: — LOGINOUT waits for RETURN before Username: (vms-2213)
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    ok "boot reaches the login prompt"
else
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi

# --- 2. Log in as SYSTEM -----------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$' 20 "$LOGIN_OFF"

# --- 3. NO authoring. A fresh SCSD reads the SEEDED defaults straight back ----
S_OFF=$(wc -c <"$LOG")
send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'
send 'SCSD --show-identity'
if wait_for 'SCSD-I-IDENT' 20 "$S_OFF"; then
    ok "SCSD --show-identity ran against the unauthored store"
else
    dump_and_die "SCSD --show-identity never printed SCSD-I-IDENT"
fi
S_SEG=$(segment_since "$S_OFF")
IDENT_LINE=$(printf '%s\n' "$S_SEG" | grep -F 'SCSD-I-IDENT' | head -1)

# THE CONTROL: the seed defaults, and NONE of the positives' authored values.
# On the unauthored seed store SCSNODE is the default OVMX and the stored
# SCSSYSTEMID is 0; resolve_scssystemid() reports its documented OVMX default
# (1030) for a 0/absent stored value, so the identity line reads SCSNODE=OVMX
# SCSSYSTEMID=1030 -- neither of the positives' authored 1026/1027. We key on
# the default node name (the discriminating part) and separately forbid the
# authored system ids below.
if printf '%s\n' "$IDENT_LINE" | grep -qF 'SCSNODE=OVMX '; then
    ok "unauthored store reads the default node name SCSNODE=OVMX"
else
    bad "unauthored store reads the default node name SCSNODE=OVMX"
    echo "  --- SCSD-I-IDENT line seen: $IDENT_LINE"
fi
if printf '%s\n' "$IDENT_LINE" | grep -qF 'VOTES=1 EXPECTED_VOTES=1'; then
    ok "unauthored store reads the default quorum VOTES=1 EXPECTED_VOTES=1"
else
    bad "unauthored store reads the default quorum VOTES=1 EXPECTED_VOTES=1"
    echo "  --- SCSD-I-IDENT line seen: $IDENT_LINE"
fi

# The positives' authored values must NEVER appear on an unauthored store.
neg_absent() {  # pattern  human
    if printf '%s\n' "$IDENT_LINE" | grep -qF "$1"; then
        bad "CONTROL BROKEN: SCSD reported '$1' with NO authoring -- $2 is not really read from the store"
    else
        ok "SCSD did NOT report '$1' on an unauthored store ($2 in the positives is authored, not canned)"
    fi
}
neg_absent 'SCSNODE=NODEB'      "CASE 1's node name"
neg_absent 'SCSNODE=NODEC'      "CASE 2's node name"
neg_absent 'SCSSYSTEMID=1026'   "CASE 1's system id"
neg_absent 'SCSSYSTEMID=1027'   "CASE 2's system id"
neg_absent 'EXPECTED_VOTES=2'   "CASE 1's expected votes"
neg_absent 'EXPECTED_VOTES=3'   "CASE 2's expected votes"

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript: SCSD --show-identity (unauthored) ==="
printf '%s\n' "$S_SEG" | grep -E 'SCSD-I-IDENT|SCSD-W' | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "NEGCTL OK -- seed defaults read back, never the authored identity; the positive proofs cannot pass on a stale/mock/wrong store"
    exit 0
fi
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
