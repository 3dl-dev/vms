#!/bin/bash
# test_cluster_params_recnx_e2e.sh - the OPERATOR authors the COMPLETE 6-param
# cluster set via SYSGEN.EXE (incl. RECNXINTERVAL, new in vms-c3b), persists it
# to SYS$SYSTEM:OVMXVMSSYS.PAR, and a fresh SCSD adopts every one of them back
# off the real mounted volume (vms-c3b, parent epic vms-098 R1.1).
#
# WHAT THIS PROVES, AND WHY A UNIT TEST CANNOT.
#
# tests/dcl/test_sysgen.sh proves the SYSGEN SET/SHOW round-trip and the
# RECNXINTERVAL range enforcement at the utility layer, against a plain working
# set -- it never touches a real volume, and never proves the value SURVIVES to
# a separate reader process on the real filesystem. The whole point of vms-c3b
# is the AUTHORING->PERSIST->ADOPT round trip: SYSGEN WRITE CURRENT lands a real
# vmsfs version, and a fresh SCSD -- a genuinely separate process that shares no
# memory with the SYSGEN session -- reads the authored RECNXINTERVAL back through
# scsd_recnxinterval() -> sysgen_read_param() on that persisted file. Only a real
# insmod'd vmsfs.ko under a real boot can show that (CLAUDE.md Rule 9 / INV-6:
# no per-process fallback standing in for the executive/filesystem).
#
# THE SEQUENCE:
#   1. Boot the actual shipped image (distro/rootfs seeds OVMXVMSSYS.PAR;1 with
#      factory defaults: SCSNODE=OVMX, RECNXINTERVAL=20).
#   2. A SYSGEN session authors the full 6-param cluster set the VMS way:
#         USE CURRENT
#         SET SCSNODE NODEB        (string form)
#         SET SCSSYSTEMID 1026
#         SET VOTES 1
#         SET EXPECTED_VOTES 2
#         SET RECNXINTERVAL 30     <- the new authored param (default 20 -> 30)
#         SET ALLOCLASS 7
#         SHOW /ALL
#         WRITE CURRENT            <- mints ;2 over the seed's ;1
#         EXIT
#      Asserts %SYSGEN-I-SETPARAM per param (string shape for SCSNODE, numeric
#      for the rest), that SHOW /ALL renders all six with the new Current
#      values, and that WRITE CURRENT prints %SYSGEN-I-WRITTEN ...;2.
#   3. A fresh SCSD --show-identity (foreign command, no raw socket) reads the
#      persisted store back and prints SCSNODE/SCSSYSTEMID/ALLOCLASS/
#      RECNXINTERVAL. If RECNXINTERVAL=30 (and the other five match), the
#      store->boot->scsd adoption path is genuine, not a per-process fake.
#   4. Belt and suspenders: a second, independent SYSGEN USE CURRENT / SHOW
#      RECNXINTERVAL reads 30 back through the same vmsfs highest-version reader.
#
# The paired MEASURED negative control is test_cluster_params_recnx_negctl.sh:
# on the SAME image with NO authoring, SCSD reports RECNXINTERVAL=20 (the seed
# default), proving this test's 30 came from authoring, not a canned value.
#
# WHAT WOULD MAKE THIS FAIL HONESTLY: boot never reaches Username:; SYSGEN.EXE
# or SCSD.EXE missing from SYS$SYSTEM:; a SET prints no %SYSGEN-I-SETPARAM;
# WRITE CURRENT does not mint ;2; SCSD --show-identity reports RECNXINTERVAL!=30
# (the authored value did not persist or was not adopted).
#
# Usage (run INSIDE the bootable image, like test_sysgen_versioning_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_cluster_params_recnx_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for boot to reach Username: (default 180).
#
# Exit 0 = every assertion below passed against the real mounted volume.
# Exit 1 = a real failure (see the printed transcript segment).

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

echo "=== cluster-param authoring e2e: SYSGEN authors 6 params (incl RECNXINTERVAL=30) -> SCSD adopts them (vms-c3b) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

DISK=/tmp/recnx-e2e.img
LOG=/tmp/recnx-e2e-console.log
FIFO=/tmp/recnx-e2e-console.in
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

# --- 1. Boot: pre-installed disk -> login prompt -----------------------------
if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
send ''  # wake OPA0: — LOGINOUT waits for RETURN before Username:
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    ok "boot completes and reaches the login prompt"
else
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi

# --- 2. Log in as SYSTEM -----------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in (LOGINOUT.EXE activated)"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$' 20 "$LOGIN_OFF"

# --- 3. Author the complete 6-param cluster set via SYSGEN -------------------
A_OFF=$(wc -c <"$LOG")
send 'SYSGEN'
wait_for 'SYSGEN>' 20 "$A_OFF"
send 'USE CURRENT'
send 'SET SCSNODE NODEB'
send 'SET SCSSYSTEMID 1026'
send 'SET VOTES 1'
send 'SET EXPECTED_VOTES 2'
send 'SET RECNXINTERVAL 30'
send 'SET ALLOCLASS 7'
send 'SHOW /ALL'
send 'WRITE CURRENT'
send 'EXIT'
if wait_for '%SYSGEN-I-WRITTEN' 30 "$A_OFF"; then
    ok "SYSGEN WRITE CURRENT completed"
else
    dump_and_die "SYSGEN WRITE CURRENT never printed %SYSGEN-I-WRITTEN"
fi
A_SEG=$(segment_since "$A_OFF")
check_a() { if printf '%s\n' "$A_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
# Per-param %SYSGEN-I-SETPARAM: string shape for SCSNODE, numeric for the rest.
check_a '%SYSGEN-I-SETPARAM, SCSNODE changed from OVMX to NODEB' \
    "SET SCSNODE printed the string-form SETPARAM"
check_a '%SYSGEN-I-SETPARAM, SCSSYSTEMID changed from 0 to 1026' \
    "SET SCSSYSTEMID printed the numeric SETPARAM"
check_a '%SYSGEN-I-SETPARAM, VOTES changed from 1 to 1' \
    "SET VOTES printed the numeric SETPARAM"
check_a '%SYSGEN-I-SETPARAM, EXPECTED_VOTES changed from 1 to 2' \
    "SET EXPECTED_VOTES printed the numeric SETPARAM"
check_a '%SYSGEN-I-SETPARAM, RECNXINTERVAL changed from 20 to 30' \
    "SET RECNXINTERVAL printed the numeric SETPARAM (default 20 -> 30)"
check_a '%SYSGEN-I-SETPARAM, ALLOCLASS changed from 0 to 7' \
    "SET ALLOCLASS printed the numeric SETPARAM"
# SHOW /ALL renders all six with the new Current values.
if printf '%s\n' "$A_SEG" | grep -qE 'SCSNODE +"NODEB'; then ok "SHOW /ALL renders SCSNODE Current=NODEB"; else bad "SHOW /ALL renders SCSNODE Current=NODEB"; fi
if printf '%s\n' "$A_SEG" | grep -qE 'SCSSYSTEMID +1026 '; then ok "SHOW /ALL renders SCSSYSTEMID Current=1026"; else bad "SHOW /ALL renders SCSSYSTEMID Current=1026"; fi
if printf '%s\n' "$A_SEG" | grep -qE 'EXPECTED_VOTES +2 '; then ok "SHOW /ALL renders EXPECTED_VOTES Current=2"; else bad "SHOW /ALL renders EXPECTED_VOTES Current=2"; fi
if printf '%s\n' "$A_SEG" | grep -qE 'RECNXINTERVAL +30 +20 +1 +32767'; then ok "SHOW /ALL renders RECNXINTERVAL Current=30 (Default 20, range 1..32767)"; else bad "SHOW /ALL renders RECNXINTERVAL Current=30 (Default 20, range 1..32767)"; fi
if printf '%s\n' "$A_SEG" | grep -qE 'ALLOCLASS +7 '; then ok "SHOW /ALL renders ALLOCLASS Current=7"; else bad "SHOW /ALL renders ALLOCLASS Current=7"; fi
# The version-bump proof: seed already put ;1 on disk, so WRITE CURRENT -> ;2.
check_a '%SYSGEN-I-WRITTEN, 31 parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR;2' \
    "WRITE CURRENT created version ;2 over the seed's ;1 (31 params incl RECNXINTERVAL)"

# --- 4. A FRESH SCSD adopts every authored param back off the volume --------
# SCSD --show-identity opens no socket -- a pure read of the persisted store
# through scsd_recnxinterval()/resolve_* -> sysgen_read_param(). A separate
# process from the SYSGEN session above: if it sees RECNXINTERVAL=30 it read
# the real ;2, not a cached value.
S_OFF=$(wc -c <"$LOG")
send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'
send 'SCSD --show-identity'
if wait_for 'SCSD-I-IDENT' 20 "$S_OFF"; then
    ok "SCSD --show-identity ran (foreign command, no socket)"
else
    dump_and_die "SCSD --show-identity never printed SCSD-I-IDENT"
fi
S_SEG=$(segment_since "$S_OFF")
if printf '%s\n' "$S_SEG" | grep -qF 'SCSD-I-IDENT, SCSNODE=NODEB SCSSYSTEMID=1026 ALLOCLASS=7 RECNXINTERVAL=30'; then
    ok "SCSD adopted ALL six authored params back off the volume (RECNXINTERVAL=30)"
else
    bad "SCSD adopted ALL six authored params back off the volume (RECNXINTERVAL=30)"
    echo "  --- SCSD-I-IDENT line seen: ---"
    printf '%s\n' "$S_SEG" | grep -F 'SCSD-I-IDENT' | sed 's/^/    /'
fi

# --- 5. Independent SYSGEN USE CURRENT reads RECNXINTERVAL=30 back -----------
B_OFF=$(wc -c <"$LOG")
send 'SYSGEN'
wait_for 'SYSGEN>' 20 "$B_OFF"
send 'USE CURRENT'
send 'SHOW RECNXINTERVAL'
send 'EXIT'
wait_for '%SYSGEN-I-LOADED' 20 "$B_OFF"
B_SEG=$(segment_since "$B_OFF")
if printf '%s\n' "$B_SEG" | grep -qE 'RECNXINTERVAL +30 +20 +1 +32767'; then
    ok "fresh SYSGEN USE CURRENT reads RECNXINTERVAL=30 back through vmsfs"
else
    bad "fresh SYSGEN USE CURRENT reads RECNXINTERVAL=30 back through vmsfs"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript: SYSGEN authoring (SET .. / SHOW /ALL / WRITE CURRENT) ==="
printf '%s\n' "$A_SEG" | grep -E '%SYSGEN|RECNXINTERVAL|SCSNODE|SCSSYSTEMID|EXPECTED_VOTES|ALLOCLASS' | sed 's/^/  /'
echo "=== transcript: SCSD --show-identity ==="
printf '%s\n' "$S_SEG" | grep -E 'SCSD-I-IDENT|SCSD-W' | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL CLUSTER-PARAM RECNXINTERVAL E2E CHECKS PASSED -- REAL AUTHORING->PERSIST->ADOPT ROUND TRIP"
    exit 0
fi
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
