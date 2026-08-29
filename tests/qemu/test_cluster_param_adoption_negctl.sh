#!/bin/bash
# test_cluster_param_adoption_negctl.sh - THE MEASURED NEGATIVE CONTROL for
# tests/qemu/test_cluster_param_adoption.sh (vms-495, epic vms-098 R1.3).
#
# WHY THIS FILE EXISTS.
#
# The positive proves that after authoring SCSNODE=NODEB/SCSSYSTEMID=1026 on
# BOOT 1 and power-cycling, BOOT 2's fresh executive reports NODEB/1026. That
# assertion is only worth something if NODEB/1026 could NOT have come from
# anywhere but the persisted authored store -- if SCSD hardcoded NODEB, read a
# mock file, or the test grep matched a canned string, the positive would pass
# for the wrong reason. This control does the OPPOSITE of the positive on the
# SAME across-reboot shape: it boots a persistent disk, authors NOTHING, and
# reboots it. On the factory-seeded store (SCSNODE=OVMX, SCSSYSTEMID=0), the
# rebooted SCSD --show-identity MUST report the seed identity OVMX/0 and MUST
# NEVER report the positive's NODEB/1026.
#
# This is MEASURED, not asserted in prose: it boots real QEMU twice on a real
# persistent disk and checks what SCSD actually printed on the second boot. If
# SCSD ever reported NODEB/1026 here -- with no authoring having happened -- the
# positive's NODEB/1026 would be exposed as canned rather than adopted, and BOTH
# gates would (correctly) be worthless.
#
# WHAT WOULD MAKE THIS FAIL HONESTLY (the defect it guards): boot never reaches
# Username:; SCSD.EXE missing; SCSD --show-identity reports NODEB or 1026 (or any
# non-seed identity) on the rebooted unauthored disk -- which would mean the
# identity is not really being read from the persisted store.
#
# Usage (run INSIDE the bootable image):
#   docker run --rm -v $PWD/tests/qemu/test_cluster_param_adoption_negctl.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for a boot to reach Username: (default 180).
#
# Exit 0 = the control behaved as designed (seed default read back, never NODEB).
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

echo "=== NEGCTL: unauthored persistent disk rebooted -> SCSD reads the SEED identity OVMX/0, never NODEB/1026 (vms-495) ==="
echo "arch=$ARCH qemu=$QEMU"

QPID=""; LOG=""; FIFO=""

qemu_launch() {
    local disk="$1"
    LOG="$2"; FIFO="$3"
    rm -f "$LOG" "$FIFO"
    mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + 120))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nic none -nodefaults -serial stdio \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$FIFO" >"$LOG" 2>&1 &
    QPID=$!
    exec 4>"$FIFO"
}
qemu_halt() {
    exec 4>&- 2>/dev/null || true
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
    rm -f "$FIFO"
    QPID=""
}
cleanup() { [ -n "${QPID:-}" ] && { kill "$QPID" 2>/dev/null; rm -f "$FIFO"; }; }
trap cleanup EXIT

send() { printf '%s\r' "$1" >&4; }
wait_for() {
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }

login_system() {
    if wait_for '%OVMX-I-EXEC' 60; then ok "$1: executive attached (real vms.ko)"; else bad "$1: executive never attached"; fi
    send ''
    if wait_for 'Username:' "$BOOT_TIMEOUT"; then
        ok "$1: boot reaches the login prompt"
    else
        echo "=== FATAL: $1 boot never reached Username: ==="; cat "$LOG"; qemu_halt; exit 1
    fi
    local off; off=$(wc -c <"$LOG")
    send 'SYSTEM'
    wait_for 'Password:' 30 "$off" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 30 "$off"; then
        ok "$1: SYSTEM logs in"
    else
        echo "=== FATAL: $1 SYSTEM login failed ==="; cat "$LOG"; qemu_halt; exit 1
    fi
    wait_for '$' 20 "$off"
}

DISK=/tmp/adopt-negctl.img
rm -f "$DISK"; cp "$DISTRIB_IMG" "$DISK"

echo ""
echo "--- BOOT 1: log in, author NOTHING, halt (the disk keeps its factory-seeded params) ---"
qemu_launch "$DISK" /tmp/adopt-negctl-b1.log /tmp/adopt-negctl-b1.in
login_system "negctl boot1"
# Deliberately no SYSGEN authoring here -- that is the whole point of the control.
qemu_halt

echo ""
echo "--- BOOT 2: fresh boot of the SAME unauthored disk -- SCSD must show the SEED identity ---"
qemu_launch "$DISK" /tmp/adopt-negctl-b2.log /tmp/adopt-negctl-b2.in
login_system "negctl boot2"
S_OFF=$(wc -c <"$LOG")
send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'
send 'SCSD --show-identity'
wait_for 'SCSD-I-IDENT' 20 "$S_OFF"
S_SEG=$(segment_since "$S_OFF")

IDENT_LINE=$(printf '%s\n' "$S_SEG" | grep -F 'SCSD-I-IDENT' | head -1)
echo "  --- SCSD-I-IDENT line seen: ${IDENT_LINE:-<none>} ---"

# THE control assertions: the unauthored rebooted disk must NEVER carry the
# positive's authored identity.
if printf '%s\n' "$IDENT_LINE" | grep -qF 'SCSNODE=NODEB'; then
    bad "unauthored disk wrongly reports SCSNODE=NODEB (positive's NODEB is canned, not adopted)"
else
    ok "unauthored disk does NOT report SCSNODE=NODEB"
fi
if printf '%s\n' "$IDENT_LINE" | grep -qF 'SCSSYSTEMID=1026'; then
    bad "unauthored disk wrongly reports SCSSYSTEMID=1026 (positive's 1026 is canned, not adopted)"
else
    ok "unauthored disk does NOT report SCSSYSTEMID=1026"
fi
# And it positively reads back the factory-seeded NODE identity. The seed node
# name is OVMX (the documented OVMX default -- vms_sysgen.c str_default), which
# is the load-bearing contrast with the positive's authored NODEB: the rebooted
# unauthored disk yields the SEED node, not the authored one. (The seed's
# numeric SCSSYSTEMID is a mastered-image property, not asserted to an exact
# value here so the control does not go brittle across image revisions -- the
# "never 1026" assertion above is what pins the numeric identity to the seed.)
if printf '%s\n' "$IDENT_LINE" | grep -qF 'SCSNODE=OVMX'; then
    ok "unauthored disk reads back the factory-seeded node identity SCSNODE=OVMX (the seed, not the authored NODEB)"
else
    bad "unauthored disk reads back the factory-seeded node identity SCSNODE=OVMX (the seed, not the authored NODEB)"
fi
qemu_halt

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "NEGCTL PASSED -- unauthored rebooted disk reads the seed identity, never the authored NODEB/1026 (vms-495)"
    exit 0
fi
echo ""
echo "--- boot 2 console log ---"
cat "$LOG" 2>/dev/null || true
exit 1
