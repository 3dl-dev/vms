#!/bin/bash
# test_sysgen_versioning_e2e.sh - SYSGEN's parameter file lives on the
# system disk and versions like the real thing (vms-d34,
# docs/design-boot-faithful.md sec 3.4).
#
# WHAT THIS PROVES, AND WHY A UNIT TEST CANNOT.
#
# tests/libvms/test_sysgen_identity.c and the sysgen_params.h readers'
# OVMX_SYSGEN_PATH override prove the READ side against a plain temp file --
# deliberately bypassing vmsfs, because that is what makes them runnable
# without a mounted volume. What they cannot prove is the thing this item is
# actually about: that WRITE CURRENT creates a REAL vmsfs file version
# (";2" over ";1" on the actual filesystem, not an in-memory counter or a
# renamed single file), and that USE CURRENT reads the file back at its
# HIGHEST version through the SAME vmsfs translation DCL and scsd use. Only
# a real, insmod'd vmsfs.ko under a real boot can show that -- see CLAUDE.md
# Rule 9's INV-6 (no per-process fallback standing in for the executive/
# filesystem) and the "anti-LARP proof" language in vms-d34 itself.
#
# THE SEQUENCE, AND WHY EACH STEP IS A-WRITES/B-READS ACROSS SEPARATE
# PROCESSES (the same discipline test_syssvc_setname.c's header explains):
#   1. Boot the actual shipped image. distro/rootfs seeds SYS$SYSTEM:
#      OVMXVMSSYS.PAR;1 with factory defaults (SCSNODE=OVMX) -- so this run
#      starts with a REAL ;1 already on disk, not a clean slate.
#   2. A FIRST, independent `$ SYSGEN` session (process A) does USE DEFAULT
#      (the compiled-in factory defaults, not the seeded file -- proves the
#      two are distinct sources), SET SCSNODE to a value that does NOT match
#      the seed, then WRITE CURRENT. Because ;1 already exists, this MUST
#      produce ;2, not silently overwrite ;1 -- the seed is what turns "a
#      single write" into an actual version-bump proof.
#   3. A SECOND, independent `$ SYSGEN` session (process B) does USE CURRENT
#      and SHOW SCSNODE. If it reads the value process A set, USE CURRENT
#      read the HIGHEST version (;2) through vmsfs -- not a cached in-memory
#      value from process A (there is none; B is a fresh process) and not a
#      stale ;1 (which still carries the seed's SCSNODE=OVMX).
#   4. `$ DIRECTORY SYS$SYSTEM:*.PAR` from a THIRD, independent DCL command
#      independently corroborates both file versions exist on the real
#      volume -- not just SYSGEN's own stdout claims.
#
# WHAT WOULD MAKE THIS TEST FAIL HONESTLY: boot never reaches Username:;
# SYSGEN.EXE is missing from SYS$SYSTEM: (%DCL-W-... or %SYSGEN-F-NOIMG);
# WRITE CURRENT reports version 1 instead of 2 (proves it overwrote the
# seed instead of versioning); process B's SHOW SCSNODE returns the SEED
# value instead of what process A set (proves USE CURRENT read ;1, not the
# highest version); or DIRECTORY does not show both OVMXVMSSYS.PAR;1 and
# OVMXVMSSYS.PAR;2.
#
# Usage (run INSIDE the bootable image, like test_parts_demo_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_sysgen_versioning_e2e.sh:/test.sh:ro \
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
# PRE-INSTALLED distribution disk (vms-8ab, same convention as
# test_parts_demo_e2e.sh): PID 1 no longer installs on a blank disk
# (vms-2f0), so this test seeds its disk from the mastered image, which is
# where distro/rootfs's seeded OVMXVMSSYS.PAR;1 actually lands.
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

echo "=== SYSGEN parameter file versioning e2e: seed ;1 -> WRITE CURRENT -> ;2 -> USE CURRENT reads ;2 (vms-d34) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

DISK=/tmp/sysgen-e2e.img
LOG=/tmp/sysgen-e2e-console.log
FIFO=/tmp/sysgen-e2e-console.in
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
send ''  # vms-2213: wake OPA0: — LOGINOUT waits for RETURN before Username:
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

# --- 3. Process A: USE DEFAULT, SET SCSNODE, WRITE CURRENT -------------------
# The seed on disk (distro/rootfs) carries SCSNODE=OVMX. TESTND is chosen
# specifically to NOT match the seed, so step 4 cannot pass by accident.
A_OFF=$(wc -c <"$LOG")
send 'SYSGEN'
wait_for 'SYSGEN>' 20 "$A_OFF"
send 'USE DEFAULT'
send 'SET SCSNODE TESTND'
send 'WRITE CURRENT'
send 'EXIT'
if wait_for '%SYSGEN-I-WRITTEN' 20 "$A_OFF"; then
    ok "process A's WRITE CURRENT completed"
else
    dump_and_die "process A's WRITE CURRENT never printed %SYSGEN-I-WRITTEN"
fi
A_SEG=$(segment_since "$A_OFF")
check_a() { if printf '%s\n' "$A_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
check_a '%SYSGEN-I-DEFLOADED' "USE DEFAULT loaded the compiled-in factory defaults"
check_a '%SYSGEN-I-SETPARAM, SCSNODE changed from OVMX to TESTND' \
    "SET SCSNODE changed the value away from the seed's OVMX"
# THE VERSION-BUMP PROOF: the seed already put ;1 on disk, so a correct
# WRITE CURRENT MUST land on ;2, not silently overwrite ;1.
check_a '%SYSGEN-I-WRITTEN, 30 parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR;2' \
    "WRITE CURRENT created version ;2 over the seed's ;1 (real vmsfs version, not an in-memory counter)"

# --- 4. Process B: an INDEPENDENT SYSGEN session's USE CURRENT --------------
# A fresh process, no shared memory with A -- if it sees TESTND, it read
# ;2 off the real volume, not a cached value.
B_OFF=$(wc -c <"$LOG")
send 'SYSGEN'
wait_for 'SYSGEN>' 20 "$B_OFF"
send 'USE CURRENT'
send 'SHOW SCSNODE'
send 'EXIT'
if wait_for '"TESTND ' 20 "$B_OFF"; then
    ok "process B's SHOW SCSNODE printed the value process A wrote"
else
    dump_and_die "process B's SHOW SCSNODE never showed TESTND"
fi
B_SEG=$(segment_since "$B_OFF")
check_b() { if printf '%s\n' "$B_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
check_b '%SYSGEN-I-LOADED, 30 parameters loaded from' \
    "USE CURRENT loaded a real file (not the NOCURRENT/factory-default fallback)"
check_b 'ovmxvmssys.par;2' \
    "USE CURRENT read the HIGHEST version (;2), matching the oracle's USE-reads-highest behavior"
if printf '%s\n' "$B_SEG" | grep -qF 'ovmxvmssys.par;1'; then
    bad "USE CURRENT did NOT read the stale ;1 (it read a path naming ;1)"
else
    ok "USE CURRENT did NOT read the stale ;1"
fi

# --- 5. Independent corroboration: DIRECTORY sees BOTH real file versions ---
DIR_OFF=$(wc -c <"$LOG")
DIR_CMD='DIRECTORY SYS$SYSTEM:*.PAR'
send "$DIR_CMD"
wait_for 'Total of' 20 "$DIR_OFF"
DIR_SEG=$(segment_since "$DIR_OFF")
DIR_BODY=$(printf '%s\n' "$DIR_SEG" | grep -vF "$DIR_CMD")
if printf '%s\n' "$DIR_BODY" | grep -qiF 'OVMXVMSSYS.PAR;1'; then
    ok "DIRECTORY SYS\$SYSTEM:*.PAR shows OVMXVMSSYS.PAR;1 (the seeded version)"
else
    bad "DIRECTORY SYS\$SYSTEM:*.PAR shows OVMXVMSSYS.PAR;1 (the seeded version)"
fi
if printf '%s\n' "$DIR_BODY" | grep -qiF 'OVMXVMSSYS.PAR;2'; then
    ok "DIRECTORY SYS\$SYSTEM:*.PAR shows OVMXVMSSYS.PAR;2 (process A's write)"
else
    bad "DIRECTORY SYS\$SYSTEM:*.PAR shows OVMXVMSSYS.PAR;2 (process A's write)"
fi
if printf '%s\n' "$DIR_BODY" | grep -qE 'Total of [1-9][0-9]* files?[.,]'; then
    ok "DIRECTORY reported a nonzero file count (not an empty/broken listing)"
else
    bad "DIRECTORY reported a nonzero file count (not an empty/broken listing)"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript: process A (USE DEFAULT / SET SCSNODE / WRITE CURRENT) ==="
printf '%s\n' "$A_SEG" | grep -E '%SYSGEN' | sed 's/^/  /'
echo "=== transcript: process B (USE CURRENT / SHOW SCSNODE) ==="
printf '%s\n' "$B_SEG" | grep -E '%SYSGEN|SCSNODE|TESTND' | sed 's/^/  /'
echo "=== transcript: \$ DIRECTORY SYS\$SYSTEM:*.PAR ==="
printf '%s\n' "$DIR_SEG" | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL SYSGEN VERSIONING E2E CHECKS PASSED -- REAL VMSFS VERSIONS, NOT A MOCK"
    exit 0
fi
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
