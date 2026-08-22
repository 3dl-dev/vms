#!/bin/bash
# test_cluster_param_adoption.sh - vms-495 (epic vms-098 R1.3): THE R1 RELEASE
# PROOF. A rebooted OVMX executive adopts the operator-authored cluster
# identity/quorum parameter set off the PERSISTENT system disk.
#
# =============================================================================
# WHAT THIS PROVES, AND WHY R1.1/R1.2 DO NOT ALREADY PROVE IT
# =============================================================================
# R1.1 (test_cluster_params_recnx_e2e.sh) authors the 6-param cluster set via
# SYSGEN and then, IN THE SAME BOOT, reads it back from a fresh SCSD --
# authoring->persist->adopt within one power-on. R1.2 (test_sysman_store_
# coherence.sh, the SYSMAN reader path) round-trips the same store on the host.
# Neither proves the property the R1 acceptance actually needs: that a WHOLE NEW
# executive -- a fresh PID 1 booted from the SAME disk on a later power cycle --
# comes up already carrying the authored identity. That is ADOPTION ON REBOOT,
# not file persistence, and it is exactly what a real operator relies on when
# they SYSGEN a node's SCSNODE/SCSSYSTEMID/VOTES and then reboot it into the
# cluster.
#
# The proof shape is the across-reboot two-QEMU-process model that
# test_boot_scsnode_hostname_e2e.sh established (read its "WHY TWO SEPARATE QEMU
# PROCESSES ON THE SAME DISK IS 'REBOOT' HERE" header): -no-reboot means a guest
# reboot(2) exits QEMU, so a genuine new PID 1 execution against the identical,
# persistent disk file is the closest thing OVMX's platform has to a reboot.
# This gate proves the CLUSTER-PARAM SLICE rides the adopt-at-boot mechanism
# (vms-46c, gap #1, already on main and proven for SCSNODE by that gate); it
# does NOT reimplement that mechanism.
#
# =============================================================================
# THE CASES
# =============================================================================
# CASE 1 (POSITIVE, own disk): boot 1 authors the cluster set the VMS way --
#   SYSGEN USE CURRENT / SET SCSNODE NODEB / SET SCSSYSTEMID 1026 / SET VOTES 1 /
#   SET EXPECTED_VOTES 2 / WRITE CURRENT (a REAL new vmsfs ;2 over the seed's ;1).
#   The QEMU process is killed after a writeback settle -- a power cycle. Boot 2
#   on the SAME disk, a fresh executive:
#     - the boot CONSOLE itself announces %OVMX-I-SCSNODE ... NODEB before any
#       DCL runs (SCSNODE adopted at boot, the vms-46c path);
#     - a fresh SCSD --show-identity (a genuinely separate process, no socket,
#       pure read of the persisted store) reports
#         SCSNODE=NODEB SCSSYSTEMID=1026 ... VOTES=1 EXPECTED_VOTES=2
#       -- every authored value adopted on the reboot;
#     - F$GETSYI("SCSSYSTEMID") in a fresh SYSTEM session independently reads
#       1026 through dcl_lexical.c's own store reader.
#
# CASE 2 (CONTROL BRACKET, DIFFERENT values, separate disk): a second, wholly
#   independent disk copy is authored with DIFFERENT values -- SCSNODE NODEC /
#   SCSSYSTEMID 1027 / VOTES 2 / EXPECTED_VOTES 3 -- on its boot 1, then its
#   boot 2 comes up carrying THOSE. This is what proves identity TRACKS THE
#   AUTHORED STORE rather than a hardcoded default or CASE 1's values: the same
#   image, authored differently, boots differently; NODEB/1026 never appear on
#   this disk, and NODEC/1027 never appeared on CASE 1's.
#
# The paired MEASURED negative control is test_cluster_param_adoption_negctl.sh:
# the SAME image with NO authoring boots to the seeded DEFAULTS (SCSNODE=OVMX,
# SCSSYSTEMID=0, VOTES=1, EXPECTED_VOTES=1) and NEVER to either case's authored
# values -- so a faked/canned/wrong-store read cannot let either positive pass.
#
# =============================================================================
# SCOPE (vms-495, conductor-set): param READ-BACK adoption ONLY. VOTES and
# EXPECTED_VOTES are read back as CONFIG values from the authored store (the
# vms-495 resolve_votes/resolve_expected_votes surface in scsd.c). The live
# two-node VC quorum RECOMPUTE from authored VOTES is the operator-reserved
# R1-split (vms-41d, cluster-wire session) and is NOT exercised here -- this
# gate touches no wire frame and no quorum arithmetic. It proves the booted
# executive READS BACK the authored VOTES/EXPECTED_VOTES, nothing more.
#
# =============================================================================
# HARNESS NOTES (inherited from test_boot_scsnode_hostname_e2e.sh -- read its
# header for the full rationale)
# =============================================================================
#   - Every QEMU launch is INLINE (no boot_qemu() helper -- the function+$(...)
#     shape reliably wedges launching a backgrounded, redirected process).
#   - Every launch is wrapped in `timeout -k 15` so an unresponsive QEMU is
#     SIGKILLed 15s after the SIGTERM -- a real ceiling, not a best-effort one.
#   - THE WRITEBACK TRAP: Linux's dirty_expire_centisecs (30s) means a QEMU
#     killed right after a DCL write can lose it before it reaches the backing
#     file. SETTLE_SECS makes each boot-1 edit real for boot 2 to read.
#   - LOGINOUT on OPA0: waits for the operator's RETURN before Username:
#     (vms-2213); wake_login feeds a CR/second until the prompt appears.
#
# Usage (run INSIDE the bootable image, like the other cluster-param e2e gates):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_cluster_param_adoption.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for a boot to reach Username: (default 180).
#   SETTLE_SECS    guest-writeback settle after a boot-1 authoring (default 60).
#
# Exit 0 = every assertion passed against the real mounted volume.
# Exit 1 = a real failure (the relevant transcript segment is printed).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETTLE_SECS="${SETTLE_SECS:-60}"     # see THE WRITEBACK TRAP above
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# PRE-INSTALLED distribution disk (vms-8ab): PID 1 does not install on a blank
# disk (vms-2f0), so each case seeds its disk from the mastered image, which is
# where distro/rootfs's seeded OVMXVMSSYS.PAR;1 (SCSNODE=OVMX, SCSSYSTEMID=0,
# VOTES=1, EXPECTED_VOTES=1, the full param set) lands.
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing - the mastering stage did not run"
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
record() {
    local desc="$1" rc="$2"
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}
check() {
    local desc="$1" log="$2" pattern="$3" expect="${4:-present}"
    # "--" is load-bearing: several patterns start with "-" (VMS continuation
    # lines) and grep would otherwise parse them as options.
    if grep -qaF -- "$pattern" "$log" 2>/dev/null; then
        if [ "$expect" = "present" ]; then record "$desc" 0; else record "$desc" 1; fi
    else
        if [ "$expect" = "absent" ]; then record "$desc" 0; else record "$desc" 1; fi
    fi
}
waitfor() {  # pattern  limit-seconds  log
    local pat="$1" lim="${2:-60}" log="$3" w=0
    while [ $w -lt $((lim * 4)) ]; do
        grep -qaF "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w + 1))
    done
    return 1
}

echo "=== cluster-param adoption ACROSS REBOOT (vms-495, epic vms-098 R1.3) ==="
echo "arch=$ARCH qemu=$QEMU  settle=${SETTLE_SECS}s"

# author_boot1  DISK  LOG  FIFO  NODE  SID  VOTES  XVOTES  LABEL
# Boot 1: author the cluster set via SYSGEN, WRITE CURRENT, settle, power off.
# Sets the module-global `qp` to the (dead, waited-on) QEMU pid.
author_boot1() {
    local disk="$1" log="$2" fifo="$3" node="$4" sid="$5" votes="$6" xvotes="$7" label="$8" rc
    rm -f "$disk" "$log" "$fifo"
    cp "$DISTRIB_IMG" "$disk"
    mkfifo "$fifo"

    # shellcheck disable=SC2086
    timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nic none -nodefaults -serial stdio \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 4>"$fifo"
    send() { printf '%s\r' "$1" >&4; }
    local w=0
    until grep -qaF 'Username:' "$log" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
    if waitfor 'Username:' 120 "$log"; then rc=0; else rc=1; fi
    record "[$label] boot 1: pre-installed disk boots to login" "$rc"
    if [ "$rc" -eq 0 ]; then
        send 'SYSTEM'; sleep 1
        send 'MANAGER'; sleep 1
        if waitfor 'Welcome to OpenVMX' 30 "$log"; then rc=0; else rc=1; fi
        record "[$label] boot 1: SYSTEM logs in" "$rc"

        send 'SYSGEN'; sleep 1
        send 'USE CURRENT'; sleep 1
        send "SET SCSNODE $node"; sleep 1
        send "SET SCSSYSTEMID $sid"; sleep 1
        send "SET VOTES $votes"; sleep 1
        send "SET EXPECTED_VOTES $xvotes"; sleep 1
        send 'WRITE CURRENT'; sleep 1
        send 'EXIT'; sleep 1
        if waitfor '%SYSGEN-I-WRITTEN, 31 parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR;2' 20 "$log"; then
            rc=0; else rc=1; fi
        record "[$label] boot 1: WRITE CURRENT minted OVMXVMSSYS.PAR;2 (real vmsfs version)" "$rc"
        check "[$label] boot 1: SET SCSNODE -> $node" "$log" \
            "%SYSGEN-I-SETPARAM, SCSNODE changed from OVMX to $node"
        check "[$label] boot 1: SET SCSSYSTEMID -> $sid" "$log" \
            "%SYSGEN-I-SETPARAM, SCSSYSTEMID changed from 0 to $sid"
        check "[$label] boot 1: SET EXPECTED_VOTES -> $xvotes" "$log" \
            "%SYSGEN-I-SETPARAM, EXPECTED_VOTES changed from 1 to $xvotes"

        echo "  ([$label] settling ${SETTLE_SECS}s for guest writeback)"
        sleep "$SETTLE_SECS"
    fi
    kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
    rm -f "$fifo"
}

# adopt_boot2  DISK  LOG  FIFO  NODE  SID  VOTES  XVOTES  OTHERNODE  LABEL
# Boot 2 on the SAME disk, a FRESH executive. Proves it adopts every authored
# value, and that the OTHER case's node name never bleeds onto this disk.
adopt_boot2() {
    local disk="$1" log="$2" fifo="$3" node="$4" sid="$5" votes="$6" xvotes="$7" other="$8" label="$9" rc
    rm -f "$log" "$fifo"
    mkfifo "$fifo"

    # shellcheck disable=SC2086
    timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nic none -nodefaults -serial stdio \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 4>"$fifo"
    send() { printf '%s\r' "$1" >&4; }
    local w=0
    until grep -qaF 'Username:' "$log" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
    if waitfor 'Username:' 120 "$log"; then rc=0; else rc=1; fi
    record "[$label] boot 2 (reboot): fresh boot reaches login (mount-or-halt unaffected)" "$rc"

    # SCSNODE is adopted at BOOT, before any DCL -- the vms-46c console line.
    check "[$label] boot 2: %OVMX-I-SCSNODE console line names the authored $node" "$log" \
        "%OVMX-I-SCSNODE, node name $node set from SYS\$SYSTEM:OVMXVMSSYS.PAR"
    check "[$label] boot 2: the OTHER case's node name ($other) never appears (separate disk)" \
        "$log" "$other" absent

    if [ "$rc" -eq 0 ]; then
        send 'SYSTEM'; sleep 1
        send 'MANAGER'; sleep 1
        if waitfor 'Welcome to OpenVMX' 30 "$log"; then rc=0; else rc=1; fi
        record "[$label] boot 2: SYSTEM logs in" "$rc"

        # THE READ-BACK: a fresh SCSD (separate process, no socket) reports the
        # whole authored identity/quorum set off the persisted store.
        send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'; sleep 1
        send 'SCSD --show-identity'; sleep 1
        if waitfor 'SCSD-I-IDENT' 20 "$log"; then rc=0; else rc=1; fi
        record "[$label] boot 2: SCSD --show-identity ran (foreign command, no socket)" "$rc"
        check "[$label] boot 2: SCSD adopted SCSNODE=$node SCSSYSTEMID=$sid on reboot" \
            "$log" "SCSD-I-IDENT, SCSNODE=$node SCSSYSTEMID=$sid"
        check "[$label] boot 2: SCSD read back the authored VOTES=$votes EXPECTED_VOTES=$xvotes" \
            "$log" "VOTES=$votes EXPECTED_VOTES=$xvotes"

        # Independent DCL surface: F$GETSYI reads SCSSYSTEMID from the store.
        send "SIDP = F\$GETSYI(\"SCSSYSTEMID\")"; sleep 1
        send 'SHOW SYMBOL SIDP'; sleep 1
        check "[$label] boot 2: F\$GETSYI(SCSSYSTEMID) independently reads $sid" \
            "$log" "SIDP = $sid   Hex ="
    fi
    kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
    rm -f "$fifo"
}

# =============================================================================
# CASE 1: POSITIVE - author NODEB/1026, VOTES=1/EXPECTED_VOTES=2, reboot, adopt
# =============================================================================
echo ""
echo "--- CASE 1 (positive): author NODEB / 1026 / VOTES 1 / EXPECTED_VOTES 2, reboot ---"
P_DISK=/tmp/clu-adopt-pos.img
P_L1=/tmp/clu-adopt-pos-boot1.log; P_L2=/tmp/clu-adopt-pos-boot2.log
P_FIFO=/tmp/clu-adopt-pos.in
author_boot1 "$P_DISK" "$P_L1" "$P_FIFO" NODEB 1026 1 2 "POS"
adopt_boot2  "$P_DISK" "$P_L2" "$P_FIFO" NODEB 1026 1 2 NODEC "POS"
if [ "$FAIL" -ne 0 ]; then
    echo "--- CASE 1 boot 1 log ---"; cat "$P_L1"
    echo "--- CASE 1 boot 2 log ---"; cat "$P_L2"
fi

# =============================================================================
# CASE 2: CONTROL BRACKET - a DIFFERENT disk authored NODEC/1027 boots with THOSE
# =============================================================================
echo ""
echo "--- CASE 2 (control bracket): a second disk authored NODEC / 1027 / VOTES 2 / EXPECTED_VOTES 3 ---"
C_DISK=/tmp/clu-adopt-ctl.img
C_L1=/tmp/clu-adopt-ctl-boot1.log; C_L2=/tmp/clu-adopt-ctl-boot2.log
C_FIFO=/tmp/clu-adopt-ctl.in
BEFORE_CTL_FAIL=$FAIL
author_boot1 "$C_DISK" "$C_L1" "$C_FIFO" NODEC 1027 2 3 "CTL"
adopt_boot2  "$C_DISK" "$C_L2" "$C_FIFO" NODEC 1027 2 3 NODEB "CTL"
if [ "$FAIL" -ne "$BEFORE_CTL_FAIL" ]; then
    echo "--- CASE 2 boot 1 log ---"; cat "$C_L1"
    echo "--- CASE 2 boot 2 log ---"; cat "$C_L2"
fi

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$((PASS + FAIL)) checks passed, $FAIL failed"
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL CLUSTER-PARAM ADOPTION CHECKS PASSED -- REAL REBOOT, REAL PARAMETER FILE, AUTHORED VALUES ADOPTED"
    exit 0
fi
exit 1
