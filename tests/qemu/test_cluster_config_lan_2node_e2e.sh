#!/bin/bash
# test_cluster_config_lan_2node_e2e.sh - vms-23b9 rung 2: TWO OVMX nodes, EACH
# configured via the DOCUMENTED procedure @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM,
# reboot into their authored identities, then FORM a cluster over a real
# point-to-point QEMU network: node A founds, node B joins, both reach CN=2,
# and SHOW CLUSTER on both lists both members.
#
# =============================================================================
# WHAT THIS PROVES THAT RUNG 1 DID NOT
# =============================================================================
# Rung 1 (test_cluster_config_lan_e2e.sh) proved author -> reboot -> adopt on
# ONE standalone node, using the CHANGE path (menu choice 2). CHANGE never
# authors VAXCLUSTER (see CLUSTER_CONFIG_LAN.COM's CC_ENABLE, set only on the
# ADD path) -- so a CHANGE-only node's VAXCLUSTER stays at the shipped default
# of 0 (tools/vms_sysgen.c), and VAXCLUSTER=0 means vms_cnxman_start() returns
# immediately with no port, no HELLO, and no possibility of ever founding or
# joining anything (src/ovmx_init/ovmx_init.c start_cluster_port(),
# src/kernel-core/vms_cnxman.c vms_cnxman_start()). This gate therefore drives
# the ADD path (menu choice 1), which additionally sets VAXCLUSTER=2 --
# without it there is no cluster to form no matter what VOTES says.
#
# Two nodes, each configured through the same documented procedure, then
# booted with a real QEMU point-to-point network between them: node A
# (VOTES=1, EXPECTED_VOTES=1) satisfies quorum by its own votes and FOUNDS
# (cnxman_coord_found()'s own predicate, vms_cnxman_coord_fsm.c); node B
# (VOTES=0) can never found and must JOIN what A formed. This is the SAME
# founder/joiner asymmetry rd vms-f6b's genesis rig measures, but sourced
# entirely from the documented operator procedure instead of the kernel
# command line -- the delta this rung exists to prove.
#
# =============================================================================
# WHY loglevel=7 (NOT loglevel=3 quiet) ON THE SECOND (NETWORKED) BOOT -- AND
# WHY THIS GATE DOES NOT ASSERT ON THE KERNEL'S OWN %CNXMAN LINES ANYWAY
# =============================================================================
# loglevel=7 is raised on the second (networked) boot only, to surface as much
# early kernel-side context as possible for diagnosis on a failure.
#
# HONEST FINDING FROM RUNNING THIS GATE (not assumed, not designed around
# ahead of time): the connection manager's own %CNXMAN lines
# (join_log/coord_log/phase2_log -> cnxman_ops_log -> exec_console_printf,
# src/kernel-core/vms_cnxman*.c) do NOT reach this harness's captured serial
# console AT ANY loglevel -- confirmed by their total absence (including the
# vms.ko module's own unconditional "vms: device table initialized..." boot
# line) even though ordinary Linux kernel messages at the same or lower
# priority appear throughout the same capture. Only ovmx_init's OWN userspace
# echo of the boot-time cluster state (report_cluster_state(), no bracketed
# kernel timestamp) reaches this console -- which is what the single
# "%CNXMAN, waiting to form or join an OpenVMS Cluster" line seen early in
# each node's boot actually is. This is a substrate/console-routing fact
# about THIS harness, not a defect in the connection manager or its logging --
# recorded here rather than worked around silently. Every membership
# assertion in this gate therefore reads DCL's `SHOW CLUSTER` /
# `SHOW CLUSTER/CLUSTER` instead, which demonstrably DOES reach the console
# (rung 1 already proved this for F$GETSYI) and reads the SAME real CLUB
# through VMS_IOCTL_CLUSTER_DIAG_CSB / _MEMBER_GET (INV-6) -- an equally
# executive-backed, just DCL-surfaced, source of truth.
#
# =============================================================================
# WHY SHOW CLUSTER/CLUSTER's "Nodes" LINE, NOT F$GETSYI("CLUSTER_NODES")
# =============================================================================
# HONEST FINDING (checked in code, not assumed): the DCL F$GETSYI LEXICAL
# (src/vmsdcl/dcl_lexical.c, lex_getsyi()) implements only NODENAME, SCSNODE,
# SCSSYSTEMID, ALLOCLASS, VERSION, ARCH_NAME, HW_NAME and BOOTTIME -- anything
# else, including CLUSTER_NODES and CLUSTER_MEMBER, falls through to the
# literal string "0". SYI$_CLUSTER_NODES is real and executive-backed, but
# only through the $GETSYI SYSTEM SERVICE (src/libvms/syssvc/sys_misc.c), which
# a plain DCL command has no way to invoke without a compiled helper image.
# SHOW CLUSTER/CLUSTER (src/vmsdcl/dcl_cmd_show.c show_cluster_club()) prints
# "Nodes    %u" from a.club.cluster_nodes -- read through
# VMS_IOCTL_CLUSTER_DIAG_CSB, the SAME CLUB the $GETSYI service and SHOW
# CLUSTER's SYSTEMS/MEMBERS table both read -- so this gate asserts on that
# line instead. This is a disclosed substitution for an unimplemented DCL
# lexical item, not a weakened assertion: the field is identical, only the DCL
# surface used to read it differs.
#
# =============================================================================
# TIMING: THE DISCOVERY WINDOW, THE STAGGER, AND WHY BOTH ARE HONEST
# =============================================================================
# RECNXINTERVAL defaults to 20s (tools/vms_sysgen.c) and CLUSTER_CONFIG_LAN.COM
# does not prompt for it, so it stays at the shipped default on both nodes. At
# CLUSTER_START (early boot, before the system disk is even mounted) the node
# only ARMS a genesis_due_ms deadline RECNXINTERVAL out
# (src/kernel-core/vms_cnxman.c cnxman_genesis_arm()); the actual founding
# decision is retried every second by the fork thread's beat
# (cnxman_try_genesis()) once that deadline passes AND the node is still alone
# (cnxman_genesis_may_ask()/coord_has_peer_csb()). Node A's own boot to a login
# prompt already takes tens of seconds under TCG (or several under KVM) -- by
# the time this script finishes logging into node A, RECNXINTERVAL has already
# elapsed and node A has either founded or is still trying every second, no
# extra sleep required. Node B is only started (its OWN second boot, the one
# with a network) AFTER node A's login succeeds, so it can only ever discover
# an ALREADY-open node A -- it is never in a race to found one itself.
#
# =============================================================================
# HARNESS SHAPE (inherited from test_cluster_config_lan_e2e.sh; read its own
# header for the base rationale -- INLINE QEMU launches only, no boot_qemu()
# helper, `timeout -k 15` around every QEMU process, mkfifo consoles, one
# `send` per line with settle sleeps). NEW HERE: TWO nodes, so TWO consoles/
# fifos are held open concurrently (fd 4 for node A, fd 6 for node B) so this
# script can return to node A's console AFTER node B has joined, to prove node
# A itself learned the new membership -- not just that B claims it did.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_cluster_config_lan_2node_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# RIG_NEGCTL=1 - the negative control (rd vms-f6b's own teeth, reused here):
# node A is ALSO configured VOTES=0. Neither node then has quorum by its own
# votes, so cnxman_coord_found()'s predicate refuses BOTH -- no CSID is ever
# minted on either side. HONEST FINDING: with VAXCLUSTER=2 the local CSB
# exists from CLUSTER_START onward regardless of VOTES (only FOUNDING needs
# them), so SHOW CLUSTER renders a table (not "%SYSTEM-I-NOTMEMBER") showing
# the local system as STATUS=LOCAL and the discovered peer as STATUS=OPEN --
# never MEMBER, and never a CSID, on either side. Exit 0 = the control held
# (nobody founded, nobody fabricated a membership).
#
# Exit 0 = every assertion below passed against two real, separately booted
# and separately configured OVMX nodes. Exit 1 = a real failure; the
# transcript segment is printed.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"    # boot 1 (configure, no net) per node
JOIN_TIMEOUT="${JOIN_TIMEOUT:-900}"    # boot 2 (networked): kept alive the WHOLE run
SETTLE_SECS="${SETTLE_SECS:-60}"       # writeback settle (see rung 1's header)
JOIN_POLL="${JOIN_POLL:-150}"          # seconds to poll SHOW CLUSTER for CN=2
NEGCTL="${RIG_NEGCTL:-0}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
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
if [ -w /dev/kvm ] && [ "$ARCH" != "aarch64" ] && [ "$ARCH" != "arm64" ]; then
    ACCEL="-accel kvm -cpu host"
else
    ACCEL="-accel tcg"
fi

PASS=0
FAIL=0
record() {
    local desc="$1" rc="$2"
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}
check() {
    local desc="$1" log="$2" pattern="$3" expect="${4:-present}"
    if grep -qaF -- "$pattern" "$log" 2>/dev/null; then
        if [ "$expect" = "present" ]; then record "$desc" 0; else record "$desc" 1; fi
    else
        if [ "$expect" = "absent" ]; then record "$desc" 0; else record "$desc" 1; fi
    fi
}
check_re() {
    local desc="$1" log="$2" pattern="$3"
    if grep -qaE -- "$pattern" "$log" 2>/dev/null; then record "$desc" 0; else record "$desc" 1; fi
}
waitfor() {  # pattern limit-seconds log pid
    local pat="$1" lim="${2:-60}" log="$3" pid="$4" w=0
    while [ $w -lt $((lim * 4)) ]; do
        grep -qaF -- "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.25; w=$((w + 1))
    done
    return 1
}
wake_login() {  # logf pid
    local logf="$1" pid="$2" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        kill -0 "$pid" 2>/dev/null || return
        printf '\r' >&"$3"
        sleep 1; w=$((w + 1))
    done
}

echo "=== 2-node OVMX cluster: @CLUSTER_CONFIG_LAN.COM (ADD) -> reboot -> found/join (vms-23b9 rung 2) ==="
echo "arch=$ARCH qemu=$QEMU accel=${ACCEL#-accel } negctl=$NEGCTL"

NODE_A=OVMXA; ID_A=1029; EXP_A=1
VOTES_A=1
[ "$NEGCTL" = "1" ] && VOTES_A=0
NODE_B=OVMXB; ID_B=1030; VOTES_B=0; EXP_B=1

SEGMENT_PORT=16109
MAC_A=52:54:00:00:20:25
MAC_B=52:54:00:00:20:26

A_DISK=/tmp/ccl2-a.img
B_DISK=/tmp/ccl2-b.img
A_LOG1=/tmp/ccl2-a-boot1.log
A_LOG2=/tmp/ccl2-a-boot2.log
B_LOG1=/tmp/ccl2-b-boot1.log
B_LOG2=/tmp/ccl2-b-boot2.log
A_FIFO=/tmp/ccl2-a.in
B_FIFO=/tmp/ccl2-b.in
rm -f "$A_DISK" "$B_DISK" "$A_LOG1" "$A_LOG2" "$B_LOG1" "$B_LOG2" "$A_FIFO" "$B_FIFO"
cp "$DISTRIB_IMG" "$A_DISK"
cp "$DISTRIB_IMG" "$B_DISK"

# ==========================================================================
# drive_configure_add - the documented procedure's ADD path (menu choice 1),
# run interactively over an already-open console fifo. $1=fd $2=log $3=pid
# $4=node $5=sysid $6=votes $7=expected_votes $8=role (for PASS/FAIL labels)
# ==========================================================================
drive_configure_add() {
    local fd="$1" log="$2" pid="$3" node="$4" sysid="$5" votes="$6" exp="$7" role="$8"

    printf '@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM\r' >&"$fd"; sleep 2
    if waitfor 'Enter choice' 30 "$log" "$pid"; then rc=0; else rc=1; fi
    record "$role boot1: CLUSTER_CONFIG_LAN.COM presents the menu" "$rc"

    printf '1\r' >&"$fd"; sleep 2       # ADD -- enables VAXCLUSTER=2, not CHANGE
    if waitfor "node's SCSNODE name" 20 "$log" "$pid"; then rc=0; else rc=1; fi
    record "$role boot1: ADD prompts for SCSNODE" "$rc"

    printf '%s\r' "$node" >&"$fd"; sleep 2
    if waitfor 'SCSSYSTEMID' 20 "$log" "$pid"; then rc=0; else rc=1; fi
    record "$role boot1: valid SCSNODE ($node) accepted, prompts for SCSSYSTEMID" "$rc"

    printf '%s\r' "$sysid" >&"$fd"; sleep 2
    printf '\r' >&"$fd"; sleep 1        # Allocation class [0] -> default
    printf '%s\r' "$votes" >&"$fd"; sleep 1   # Votes this node contributes
    printf '%s\r' "$exp" >&"$fd"; sleep 1     # Expected total cluster votes
    if waitfor 'Is this correct' 20 "$log" "$pid"; then rc=0; else rc=1; fi
    record "$role boot1: procedure shows the confirmation screen" "$rc"

    check "$role boot1: confirmation echoes SCSNODE = $node" "$log" "SCSNODE         = $node"
    check "$role boot1: confirmation echoes SCSSYSTEMID = $sysid" "$log" "SCSSYSTEMID     = $sysid"
    check "$role boot1: confirmation echoes VAXCLUSTER (ADD enables participation)" \
        "$log" "VAXCLUSTER      = 2  (cluster participation enabled)"

    printf 'Y\r' >&"$fd"; sleep 3

    check "$role boot1: SYSGEN SET SCSNODE authored $node" \
        "$log" "%SYSGEN-I-SETPARAM, SCSNODE changed"
    check "$role boot1: SYSGEN SET VAXCLUSTER authored 2" \
        "$log" "%SYSGEN-I-SETPARAM, VAXCLUSTER changed from 0 to 2"
    if waitfor '%SYSGEN-I-WRITTEN' 20 "$log" "$pid"; then rc=0; else rc=1; fi
    record "$role boot1: WRITE CURRENT minted a new OVMXVMSSYS.PAR version" "$rc"
    check "$role boot1: procedure reports ADD complete" "$log" "ADD complete. The new cluster identity is written to"

    printf '5\r' >&"$fd"; sleep 2
    check "$role boot1: procedure exits cleanly" "$log" "Exiting the cluster configuration procedure"
}

# ==========================================================================
# CONFIGURE: node A, boot 1 (minimal, no net) -- authors OVMXA/1029, VOTES_A
# ==========================================================================
echo ""
echo "--- node A boot 1: configure via @CLUSTER_CONFIG_LAN.COM ADD ---"
mkfifo "$A_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$A_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$A_FIFO" >"$A_LOG1" 2>&1 &
qp=$!
exec 4>"$A_FIFO"
wake_login "$A_LOG1" "$qp" 4
if waitfor 'Username:' 120 "$A_LOG1" "$qp"; then rc=0; else rc=1; fi
record "node A boot1: pre-installed disk boots to login" "$rc"
if [ "$rc" -eq 0 ]; then
    printf 'SYSTEM\r' >&4; sleep 1
    printf 'MANAGER\r' >&4; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$A_LOG1" "$qp"; then rc=0; else rc=1; fi
    record "node A boot1: SYSTEM logs in" "$rc"
fi
if [ "$rc" -eq 0 ]; then
    drive_configure_add 4 "$A_LOG1" "$qp" "$NODE_A" "$ID_A" "$VOTES_A" "$EXP_A" "node A"
    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$A_FIFO"

# ==========================================================================
# CONFIGURE: node B, boot 1 (minimal, no net) -- authors OVMXB/1030, VOTES=0
# ==========================================================================
echo ""
echo "--- node B boot 1: configure via @CLUSTER_CONFIG_LAN.COM ADD ---"
mkfifo "$B_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$B_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$B_FIFO" >"$B_LOG1" 2>&1 &
qp=$!
exec 6>"$B_FIFO"
wake_login "$B_LOG1" "$qp" 6
if waitfor 'Username:' 120 "$B_LOG1" "$qp"; then rc=0; else rc=1; fi
record "node B boot1: pre-installed disk boots to login" "$rc"
if [ "$rc" -eq 0 ]; then
    printf 'SYSTEM\r' >&6; sleep 1
    printf 'MANAGER\r' >&6; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$B_LOG1" "$qp"; then rc=0; else rc=1; fi
    record "node B boot1: SYSTEM logs in" "$rc"
fi
if [ "$rc" -eq 0 ]; then
    drive_configure_add 6 "$B_LOG1" "$qp" "$NODE_B" "$ID_B" "$VOTES_B" "$EXP_B" "node B"
    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 6>&- 2>/dev/null
rm -f "$B_FIFO"

# ==========================================================================
# BOOT 2: node A, power-cycle, WITH the segment netdev (listen). A must be
# alone for RECNXINTERVAL and then FOUND (VOTES=1 EXPECTED_VOTES=1) unless
# NEGCTL, where it also carries VOTES=0 and can never found.
# ==========================================================================
echo ""
echo "--- node A boot 2 (power-cycle, networked): adopt identity, found/wait ---"
mkfifo "$A_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$JOIN_TIMEOUT" $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=7 net.ifnames=0 biosdevname=0" \
    -m 256M -smp 1 -nodefaults -serial stdio \
    -netdev "socket,id=net0,listen=127.0.0.1:${SEGMENT_PORT}" \
    -device "virtio-net-pci,netdev=net0,mac=${MAC_A},romfile=" \
    -drive file="$A_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$A_FIFO" >"$A_LOG2" 2>&1 &
AP=$!
exec 4>"$A_FIFO"
wake_login "$A_LOG2" "$AP" 4
if waitfor 'Username:' 180 "$A_LOG2" "$AP"; then rc=0; else rc=1; fi
record "node A boot2 (reboot): reaches the login prompt" "$rc"

check "node A boot2: %OVMX-I-SCSNODE console line names the authored $NODE_A" \
    "$A_LOG2" "%OVMX-I-SCSNODE, node name $NODE_A set from SYS\$SYSTEM:OVMXVMSSYS.PAR"

if [ "$rc" -eq 0 ]; then
    printf 'SYSTEM\r' >&4; sleep 1
    printf 'MANAGER\r' >&4; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$A_LOG2" "$AP"; then rc=0; else rc=1; fi
    record "node A boot2: SYSTEM logs in" "$rc"

    printf 'HOSTA = F$GETSYI("NODENAME")\r' >&4; sleep 1
    printf 'SHOW SYMBOL HOSTA\r' >&4; sleep 1
    check "node A boot2: F\$GETSYI(NODENAME) reads $NODE_A (adopted live hostname)" \
        "$A_LOG2" "HOSTA = \"$NODE_A\""
fi

# ==========================================================================
# THE REAL STAGGER. Under KVM a full boot to login can take LESS than
# RECNXINTERVAL (measured: ~13s) -- far faster than the TCG timings rung 1's
# own comments assumed. Node B's HELLO must not reach node A while node A is
# still inside its discovery window, or (exactly the genesis rig's own
# documented failure mode) node A perpetually sees "a peer is here" and
# refuses to found while node B, having no votes, can never found either --
# both wait forever. So this script does not merely PROCEED once node A has
# logged in; it POLLS node A's own console, via DCL, for real founding
# evidence before ever starting node B's networked boot.
#
# HONEST FINDING (checked, not assumed): the kernel-side %CNXMAN lines
# (cnxman_ops_log -> exec_console_printf, src/kernel-core/vms_cnxman.c) do NOT
# reach this harness's captured serial console at all, at any loglevel --
# unlike ovmx_init's own userspace echo of the boot-time state (which DOES
# reach it, with no bracketed kernel timestamp, and is what "waiting to form
# or join" above actually was). This is a substrate/console-routing fact
# discovered running this gate, not a defect in the connection manager: `SHOW
# CLUSTER/CLUSTER` is DCL reading the SAME real CLUB through
# VMS_IOCTL_CLUSTER_DIAG_CSB (src/vmsdcl/dcl_cmd_show.c), so it is used here
# and throughout this gate instead -- equally executive-backed (INV-6), and
# proven to reach the console (rung 1's F$GETSYI checks already established
# this). A founder's "Local CSID" reads a real hex value once
# cnxman_coord_found() has minted one; "(not yet assigned by the cluster)" is
# the honest pre-genesis answer. (Skipped under NEGCTL: node A also carries
# VOTES=0 there and can never found regardless of ordering.)
# ==========================================================================
if [ "$NEGCTL" != "1" ]; then
    echo ""
    echo "--- waiting for node A to found ALONE before node B ever starts (RECNXINTERVAL=20s) ---"
    A_FOUNDED_ALONE=1
    fw=0
    while [ "$fw" -lt 60 ]; do
        printf 'SHOW CLUSTER/CLUSTER\r' >&4 2>/dev/null
        sleep 3
        if grep -qaE 'Local CSID +[0-9A-Fa-f]{8}' "$A_LOG2" 2>/dev/null; then
            A_FOUNDED_ALONE=0; break
        fi
        kill -0 "$AP" 2>/dev/null || break
        fw=$((fw + 3))
    done
    record "node A founded ALONE before node B's networked boot ever started" "$A_FOUNDED_ALONE"
fi

# ==========================================================================
# CONFIGURE + BOOT 2: node B, power-cycle, WITH the segment netdev (connect).
# Started only AFTER node A's login succeeded AND node A had already founded
# (or, under NEGCTL, after the same login check alone). B (VOTES=0) can never
# found; it must join.
# ==========================================================================
echo ""
echo "--- node B boot 2 (power-cycle, networked): adopt identity, join ---"
mkfifo "$B_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$JOIN_TIMEOUT" $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=7 net.ifnames=0 biosdevname=0" \
    -m 256M -smp 1 -nodefaults -serial stdio \
    -netdev "socket,id=net0,connect=127.0.0.1:${SEGMENT_PORT}" \
    -device "virtio-net-pci,netdev=net0,mac=${MAC_B},romfile=" \
    -drive file="$B_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$B_FIFO" >"$B_LOG2" 2>&1 &
BP=$!
exec 6>"$B_FIFO"
wake_login "$B_LOG2" "$BP" 6
if waitfor 'Username:' 180 "$B_LOG2" "$BP"; then rc=0; else rc=1; fi
record "node B boot2 (reboot): reaches the login prompt" "$rc"

check "node B boot2: %OVMX-I-SCSNODE console line names the authored $NODE_B" \
    "$B_LOG2" "%OVMX-I-SCSNODE, node name $NODE_B set from SYS\$SYSTEM:OVMXVMSSYS.PAR"

if [ "$rc" -eq 0 ]; then
    printf 'SYSTEM\r' >&6; sleep 1
    printf 'MANAGER\r' >&6; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$B_LOG2" "$BP"; then rc=0; else rc=1; fi
    record "node B boot2: SYSTEM logs in" "$rc"

    printf 'HOSTB = F$GETSYI("NODENAME")\r' >&6; sleep 1
    printf 'SHOW SYMBOL HOSTB\r' >&6; sleep 1
    check "node B boot2: F\$GETSYI(NODENAME) reads $NODE_B (adopted live hostname)" \
        "$B_LOG2" "HOSTB = \"$NODE_B\""
fi

if [ "$NEGCTL" = "1" ]; then
    # ----------------------------------------------------------------
    # NEGATIVE CONTROL: both nodes VOTES=0. Neither may found
    # (cnxman_coord_found()'s own votes predicate), so neither can ever
    # mint a CSID or hold a CLUB, and SHOW CLUSTER must report NOTMEMBER
    # on both -- for the whole poll window, not just once.
    # ----------------------------------------------------------------
    echo ""
    echo "--- NEGCTL: polling ${JOIN_POLL}s -- neither node may ever claim membership ---"
    pw=0
    while [ "$pw" -lt "$JOIN_POLL" ]; do
        printf 'SHOW CLUSTER\r' >&4 2>/dev/null
        printf 'SHOW CLUSTER\r' >&6 2>/dev/null
        printf 'SHOW CLUSTER/CLUSTER\r' >&4 2>/dev/null
        printf 'SHOW CLUSTER/CLUSTER\r' >&6 2>/dev/null
        sleep 10; pw=$((pw + 10))
    done
    printf 'LOGOUT\r' >&4 2>/dev/null; printf 'LOGOUT\r' >&6 2>/dev/null
    sleep 1
    kill "$AP" "$BP" 2>/dev/null; wait "$AP" 2>/dev/null; wait "$BP" 2>/dev/null
    exec 4>&- 2>/dev/null; exec 6>&- 2>/dev/null
    rm -f "$A_FIFO" "$B_FIFO"

    # HONEST FINDING (checked, not assumed): with VAXCLUSTER=2 the local CSB
    # exists from CLUSTER_START onward regardless of VOTES (only the FOUNDING
    # decision needs votes), so show_cluster_systems() (n_members>=1 once the
    # local CSB exists) renders a SYSTEMS/MEMBERS table rather than
    # "%SYSTEM-I-NOTMEMBER" -- measured here: the local row reads STATUS=LOCAL
    # and the discovered peer reads STATUS=OPEN, NEITHER ever "MEMBER", and
    # both CSID columns stay blank throughout. That -- not the NOTMEMBER
    # string, which this configuration never prints -- is the real negative
    # control: no CSID minted, no MEMBER status, on either node, for the whole
    # poll window.
    check "NEGCTL: node A's own CLUB never minted a Local CSID (SHOW CLUSTER/CLUSTER)" \
        "$A_LOG2" "Local CSID          (not yet assigned by the cluster)"
    check "NEGCTL: node A's SHOW CLUSTER never shows a MEMBER row" \
        "$A_LOG2" "MEMBER " absent
    check "NEGCTL: node B's own CLUB never minted a Local CSID (SHOW CLUSTER/CLUSTER)" \
        "$B_LOG2" "Local CSID          (not yet assigned by the cluster)"
    check "NEGCTL: node B's SHOW CLUSTER never shows a MEMBER row" \
        "$B_LOG2" "MEMBER " absent

    if [ "$FAIL" -ne 0 ]; then
        echo "--- node A boot2 log (tail) ---"; tail -n 80 "$A_LOG2"
        echo "--- node B boot2 log (tail) ---"; tail -n 80 "$B_LOG2"
    fi
    echo ""
    echo "=========================================="
    echo "  RESULTS: $PASS/$((PASS + FAIL)) checks passed, $FAIL failed"
    echo "=========================================="
    if [ "$FAIL" -eq 0 ]; then
        echo "  NEGATIVE CONTROL HELD (rd vms-23b9 rung 2): with VOTES=0 on both"
        echo "  nodes, neither founded and neither ever claimed membership --"
        echo "  the proof run's own MEMBER/CN=2 is therefore a real measurement,"
        echo "  not a constant."
        echo "=========================================="
        exit 0
    fi
    exit 1
fi

# ==========================================================================
# POSITIVE RUN: poll node B until it reports CN=2, then return to node A's
# still-open console and confirm IT independently learned CN=2 too.
# ==========================================================================
echo ""
echo "--- polling node B up to ${JOIN_POLL}s for admission (CN=2) ---"
B_GOT2=1
pw=0
while [ "$pw" -lt "$JOIN_POLL" ]; do
    printf 'SHOW CLUSTER/CLUSTER\r' >&6 2>/dev/null
    sleep 8
    if grep -qaE 'Nodes +2' "$B_LOG2" 2>/dev/null; then B_GOT2=0; break; fi
    kill -0 "$BP" 2>/dev/null || break
    sleep 2; pw=$((pw + 10))
done
record "node B: SHOW CLUSTER/CLUSTER reports Nodes 2 within ${JOIN_POLL}s" "$B_GOT2"

printf 'SHOW CLUSTER\r' >&6 2>/dev/null; sleep 3
# HONEST FINDING (checked, not assumed, and already documented in
# docs/cluster-configuration-guide.md's "What SHOW CLUSTER reports"): a peer's
# SCSNODE has not been learned by this join (show_cluster_row(),
# src/vmsdcl/dcl_cmd_show.c, falls back to the SCSSYSTEMID when
# m->scsnode[0]=='\0') -- measured here: each node lists ITSELF by name but the
# PEER by its numeric SCSSYSTEMID ("1029"/"1030"), never a fabricated name it
# never received. Accept either form for the peer row; a node's OWN row is
# always by name (checked separately below).
check_re "node B: SHOW CLUSTER lists node A ($NODE_A or its SCSSYSTEMID $ID_A) as a MEMBER" \
    "$B_LOG2" "($NODE_A|$ID_A)[^|]*\|[^|]*\|[^|]*\|[^|]*MEMBER"
check_re "node B: SHOW CLUSTER lists itself ($NODE_B) as a MEMBER" \
    "$B_LOG2" "$NODE_B[^|]*\|[^|]*\|[^|]*\|[^|]*MEMBER"
printf 'SHOW CLUSTER/CLUSTER\r' >&6 2>/dev/null; sleep 3
check_re "node B: SHOW CLUSTER/CLUSTER shows a real minted Local CSID (it joined, not fabricated)" \
    "$B_LOG2" "Local CSID +[0-9A-Fa-f]{8}"

echo ""
echo "--- returning to node A's console to confirm IT also reports CN=2 ---"
A_GOT2=1
pw=0
while [ "$pw" -lt "$JOIN_POLL" ]; do
    printf 'SHOW CLUSTER/CLUSTER\r' >&4 2>/dev/null
    sleep 8
    if grep -qaE 'Nodes +2' "$A_LOG2" 2>/dev/null; then A_GOT2=0; break; fi
    kill -0 "$AP" 2>/dev/null || break
    sleep 2; pw=$((pw + 10))
done
record "node A: SHOW CLUSTER/CLUSTER reports Nodes 2 within ${JOIN_POLL}s" "$A_GOT2"

printf 'SHOW CLUSTER\r' >&4 2>/dev/null; sleep 3
check_re "node A: SHOW CLUSTER lists itself ($NODE_A) as a MEMBER" \
    "$A_LOG2" "$NODE_A[^|]*\|[^|]*\|[^|]*\|[^|]*MEMBER"
# Same honest peer-name-not-yet-learned finding as node B's check above.
check_re "node A: SHOW CLUSTER lists node B ($NODE_B or its SCSSYSTEMID $ID_B) as a MEMBER" \
    "$A_LOG2" "($NODE_B|$ID_B)[^|]*\|[^|]*\|[^|]*\|[^|]*MEMBER"
printf 'SHOW CLUSTER/CLUSTER\r' >&4 2>/dev/null; sleep 3
check_re "node A: SHOW CLUSTER/CLUSTER shows a real minted Local CSID (it founded, not fabricated)" \
    "$A_LOG2" "Local CSID +[0-9A-Fa-f]{8}"

# Never-crash-a-peer: neither console shows a bugcheck/panic anywhere in the run.
for LABEL in "node A" "node B"; do
    LOG="$A_LOG2"; [ "$LABEL" = "node B" ] && LOG="$B_LOG2"
    if grep -aqE 'Kernel panic|BUG: |Oops: |general protection|%CNXMAN, bugcheck' "$LOG" 2>/dev/null; then
        record "$LABEL: console shows no panic/bugcheck" 1
    else
        record "$LABEL: console shows no panic/bugcheck" 0
    fi
done

printf 'LOGOUT\r' >&4 2>/dev/null; printf 'LOGOUT\r' >&6 2>/dev/null
sleep 1
kill "$AP" "$BP" 2>/dev/null; wait "$AP" 2>/dev/null; wait "$BP" 2>/dev/null
exec 4>&- 2>/dev/null; exec 6>&- 2>/dev/null
rm -f "$A_FIFO" "$B_FIFO"

if [ "$FAIL" -ne 0 ]; then
    echo "--- node A boot1 log ---"; cat "$A_LOG1"
    echo "--- node A boot2 log ---"; cat "$A_LOG2"
    echo "--- node B boot1 log ---"; cat "$B_LOG1"
    echo "--- node B boot2 log ---"; cat "$B_LOG2"
fi

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$((PASS + FAIL)) checks passed, $FAIL failed"
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  2-NODE CLUSTER FORMED VIA THE DOCUMENTED PROCEDURE (vms-23b9 rung 2)"
    echo "  -- node $NODE_A founded, node $NODE_B joined, both reach CN=2, both"
    echo "  independently report the other as a MEMBER."
    exit 0
fi
exit 1
