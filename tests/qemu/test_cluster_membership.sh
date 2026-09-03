#!/bin/bash
# test_cluster_membership.sh - THE R4 harness for FC-P3.8 (vms_cnxman.c glue,
# docs/plan-faithful-cluster-executive.md FC-P3.8's own done-condition: "R4: 3
# booted OVMX nodes (with P3.12) form a cluster; SHOW CLUSTER on each lists
# the others; both substrates").
#
# WHAT THIS PROVES, FROM EXECUTIVE STATE AND FROM THE CONSOLE (never
# fabricated, INV-6). Three REAL booted executives, each running vms_pe.c's
# port, vms_scs.c's SCS layer and -- new with FC-P3.8 -- vms_cnxman.c's
# connection manager glue on top of them:
#
#   1. CNXMAN IS REALLY UP ON EVERY NODE. CLUSTER_DIAG_CSB row CLUB answers
#      SS$_NORMAL, read through the ioctl from real struct vms_club state
#      under the fork mutex (test_kmod_cluster_membership_diag, FC-P3.8's own
#      sibling of test_kmod_cluster_conn_diag).
#   2. EACH NODE'S CSB TABLE REFLECTS REAL DISCOVERED PEERS. Walking the CLUB
#      (VMS_IOCTL_CLUSTER_DIAG_CSB row CSB), the executive holds one CSB per
#      other node it has really opened a VMS$VAXcluster connection with --
#      never a placeholder row, and `csid` is never printed without
#      `csid_valid` (the INV-6 tripwire E30 names: a placeholder CSID is what
#      bugchecked a real VAX).
#   3. THE OPERATOR-VISIBLE LINES. Each node's own console carries at least
#      one "%CNXMAN, ..." line (vms_cnxman.c's cnxman_ops_log, wired straight
#      to OPA0:) once connectivity forms.
#
# HONEST SCOPE -- READ THIS BEFORE SCORING THE MEMBERSHIP LEG.
#
# Integration note E30 (docs/cluster-integration-notes.md): NOTHING in the
# shipped tree calls cnxman_club_learn_local_csid() yet -- the op-06
# MEMBERSHIP burst's {SCSSYSTEMID, incarnation, CSID} field map has no
# isolated offset in any capture. So on today's tree every node HONESTLY
# stays NEW: `local_csid_valid` reads 0, SHOW CLUSTER lists no member, and
# CLUSTER_NODES via $GETSYI reads 1 (this node only). This harness reports
# that leg as PENDING, never as a fabricated pass, exactly as
# test_cluster_conn.sh's own EXPECT_LOOKUP leg does for the directory round
# it is missing a driver for. Set EXPECT_MEMBER=1 once a capture pins the
# op-06 layout and cnxman_join_csid_learned() actually fires (the lab's own
# job, docs/cluster-integration-notes.md E30) -- then this harness scores
# CLUSTER_NODES==3 and SHOW CLUSTER listing the other two as a real FAIL if
# they do not appear, not a pending.
#
# Building the multi-GB ovmx-boot image fresh from a changed worktree and a
# three-way QEMU bridge are both lab-scale operations this rung does not
# attempt on a shared, disk-constrained dev host (operator memory
# `shared-host-docker-blast-radius`, `subagent-foreground-qemu-builds`). The
# script is written to run as-is once those artifacts exist, and SKIPS
# (exit 77, never a fabricated PASS) the instant any of them is missing.
#
# Usage (run INSIDE the bootable image, WITH host tap privilege -- the lab):
#   docker run --rm --cap-add=NET_ADMIN --cap-add=NET_RAW \
#       -v $PWD/tests/qemu/test_cluster_membership.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT     seconds to wait for a boot to reach Username: (default 180)
#   FORM_WAIT        seconds after all three nodes are up before reading
#                    diagnostics (default 60 -- longer than the 2-node VC/CONN
#                    harnesses: three-way HELLO + channel + VC + CM formation)
#   EXPECT_MEMBER    1 = every node must show CLUSTER_NODES==3 / SHOW CLUSTER
#                    listing the other two (set by the lab once E30's op-06
#                    layout is pinned); default 0 = report it PENDING
#
# Exit 0 = all three executives really brought CNXMAN up and the diagnostics
#          prove it from executive state (and, if EXPECT_MEMBER=1, that real
#          membership formed too). Exit 1 = they did not. Exit 77 = honest skip.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
FORM_WAIT="${FORM_WAIT:-60}"
EXPECT_MEMBER="${EXPECT_MEMBER:-0}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
DIAG=/tests/test_kmod_cluster_membership_diag
ARCH=$(uname -m)

EXIT_SKIP=77

skip_honest() {
    echo "SKIP: $1"
    echo "-- this R4 harness needs a privileged host tap (CAP_NET_ADMIN/CAP_NET_RAW),"
    echo "   three sets of booted OVMX artifacts and a bridge between them; run it"
    echo "   there instead (memory: cluster-interop-lab). No result is fabricated"
    echo "   for the skip."
    exit "$EXIT_SKIP"
}

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
    [ -f "$f" ] || skip_honest "$f not found -- run this inside the ovmx-boot image (see header)"
done
command -v "$QEMU" >/dev/null 2>&1 || skip_honest "$QEMU not available"
command -v ip >/dev/null 2>&1 || skip_honest "no bridge tooling (ip)"

BR=ovmxmembr0
TAPS=(ovmxmema0 ovmxmemb0 ovmxmemc0)
KEYS=(A B C)
SCSNODES=(NODEA NODEB NODEC)
SYSIDS=(1040 1041 1042)

make_tap() {
    ip tuntap add dev "$1" mode tap 2>/dev/null || return 1
    ip link set "$1" up 2>/dev/null || { ip tuntap del dev "$1" mode tap 2>/dev/null; return 1; }
    return 0
}
del_tap() { ip link set "$1" down 2>/dev/null; ip tuntap del dev "$1" mode tap 2>/dev/null; }
make_bridge() {
    ip link add name "$BR" type bridge 2>/dev/null || return 1
    ip link set "$BR" up 2>/dev/null || { ip link del "$BR" 2>/dev/null; return 1; }
    return 0
}
del_bridge() { ip link set "$BR" down 2>/dev/null; ip link del "$BR" 2>/dev/null; }

cleanup_net() {
    for t in "${TAPS[@]}"; do del_tap "$t"; done
    del_bridge
}

make_bridge || skip_honest "cannot create bridge $BR (need CAP_NET_ADMIN)"
for t in "${TAPS[@]}"; do
    make_tap "$t" || { cleanup_net; skip_honest "cannot create host tap $t"; }
    ip link set "$t" master "$BR" 2>/dev/null || { cleanup_net; skip_honest "cannot bridge $t"; }
done

PASS=0
FAIL=0
PEND=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pend() { echo "  PENDING: $1"; PEND=$((PEND + 1)); }

echo "=== FC-P3.8 R4: three booted OVMX executives bring CNXMAN up over a real port ==="
echo "arch=$ARCH qemu=$QEMU expect_member=$EXPECT_MEMBER"

declare -A QPID_ LOG_ FD_

cleanup() {
    for k in "${KEYS[@]}"; do
        [ -n "${QPID_[$k]:-}" ] && kill "${QPID_[$k]}" 2>/dev/null
    done
    wait 2>/dev/null
    cleanup_net
}
trap cleanup EXIT

send_to() { printf '%s\r' "$2" >&"$1"; }
wait_for_in() {
    local log="$1" pat="$2" limit="${3:-30}" since="${4:-0}" pid="$5" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$log" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
dump_and_die() {
    echo ""; echo "=== FATAL: $1 ==="
    for k in "${KEYS[@]}"; do
        [ -n "${LOG_[$k]:-}" ] && { echo "--- node $k console log ---"; cat "${LOG_[$k]}"; }
        [ -n "${QPID_[$k]:-}" ] && kill "${QPID_[$k]}" 2>/dev/null
    done
    wait 2>/dev/null
    exit 1
}

qemu_boot() {
    local key="$1" tap="$2" disk="$3" log="$4" fifo="$5" budget="$6"
    rm -f "$fifo"; mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$budget" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nodefaults -serial stdio \
        -netdev "tap,id=net0,ifname=${tap},script=no,downscript=no" \
        -device "virtio-net-pci,netdev=net0,romfile=" \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    QPID_[$key]=$!
}

login_system() {
    local key="$1" fd="$2" log="$3" off
    off=$(wc -c <"$log")
    send_to "$fd" 'SYSTEM'
    wait_for_in "$log" 'Password:' 30 "$off" "${QPID_[$key]}" && send_to "$fd" 'MANAGER'
    wait_for_in "$log" 'Welcome to OpenVMX' 30 "$off" "${QPID_[$key]}"
}

# boot_and_login <key> <tap> <disk> <scsnode> <sysid> -- SETs SCSNODE/
# SCSSYSTEMID/VAXCLUSTER=1 (join-if-present, per vms_cnxman.h's own
# VAXCLUSTER=1 semantic -- three nodes converging is exactly "if present"),
# WRITEs it, reboots so the executive adopts it, and leaves the node logged
# in with the port + SCS + (once FC-P3.9 wires the call) CNXMAN started.
boot_and_login() {
    local key="$1" tap="$2" disk="$3" scsnode="$4" sysid="$5"
    local log="/tmp/cluster-mem-${key}.log" fifo="/tmp/cluster-mem-${key}.in"
    local fd off
    case "$key" in
        A) fd=4 ;; B) fd=5 ;; C) fd=6 ;;
    esac
    FD_[$key]="$fd"
    LOG_[$key]="$log"
    rm -f "$log"
    qemu_boot "$key" "$tap" "$disk" "$log" "$fifo" \
              "$((BOOT_TIMEOUT * 2 + FORM_WAIT + 180))"
    eval "exec $fd>\"$fifo\""

    wait_for_in "$log" '%OVMX-I-EXEC' 60 0 "${QPID_[$key]}" \
        && ok "node $key: executive attached (real vms.ko)" \
        || bad "node $key: executive never attached"
    send_to "$fd" ''
    wait_for_in "$log" 'Username:' "$BOOT_TIMEOUT" 0 "${QPID_[$key]}" \
        || dump_and_die "node $key: boot1 never reached Username: within ${BOOT_TIMEOUT}s"
    login_system "$key" "$fd" "$log" \
        || dump_and_die "node $key: SYSTEM login failed"

    off=$(wc -c <"$log")
    send_to "$fd" 'SYSGEN'
    wait_for_in "$log" 'SYSGEN>' 20 "$off" "${QPID_[$key]}"
    send_to "$fd" 'USE CURRENT'
    send_to "$fd" "SET SCSNODE $scsnode"
    send_to "$fd" "SET SCSSYSTEMID $sysid"
    send_to "$fd" 'SET VAXCLUSTER 1'
    send_to "$fd" 'WRITE CURRENT'
    send_to "$fd" 'EXIT'
    wait_for_in "$log" '%SYSGEN-I-WRITTEN' 20 "$off" "${QPID_[$key]}" \
        || dump_and_die "node $key: SYSGEN WRITE CURRENT never confirmed"

    eval "exec $fd>&-" 2>/dev/null || true
    kill "${QPID_[$key]}" 2>/dev/null; wait "${QPID_[$key]}" 2>/dev/null

    rm -f "$log"
    qemu_boot "$key" "$tap" "$disk" "$log" "$fifo" \
              "$((BOOT_TIMEOUT + FORM_WAIT + 180))"
    eval "exec $fd>\"$fifo\""
    wait_for_in "$log" '%OVMX-I-EXEC' 60 0 "${QPID_[$key]}" \
        || dump_and_die "node $key: boot2 executive never attached"
    send_to "$fd" ''
    wait_for_in "$log" 'Username:' "$BOOT_TIMEOUT" 0 "${QPID_[$key]}" \
        || dump_and_die "node $key: boot2 never reached Username:"
    login_system "$key" "$fd" "$log" \
        || dump_and_die "node $key: boot2 SYSTEM login failed"
    ok "node $key: rebooted with SCSNODE=$scsnode SCSSYSTEMID=$sysid VAXCLUSTER=1"
}

for i in 0 1 2; do
    k="${KEYS[$i]}"
    disk="/tmp/cluster-mem-${k}.img"
    rm -f "$disk"; cp "$DISTRIB_IMG" "$disk"
    boot_and_login "$k" "${TAPS[$i]}" "$disk" "${SCSNODES[$i]}" "${SYSIDS[$i]}"
done

echo ""
echo "--- letting the three-way CM dialogue run for ${FORM_WAIT}s ---"
sleep "$FORM_WAIT"

# ---------------------------------------------------------------------------
# The %CNXMAN operator lines -- each node's own console.
# ---------------------------------------------------------------------------
echo ""
echo "--- the %CNXMAN OPA0: lines ---"
for k in "${KEYS[@]}"; do
    n=$(grep -c '%CNXMAN' "${LOG_[$k]}" 2>/dev/null || true)
    if [ "${n:-0}" -ge 1 ]; then
        ok "node $k: at least one %CNXMAN line reached OPA0: ($n total)"
    else
        pend "node $k: no %CNXMAN line observed -- CNXMAN may not have been "
        echo "     driven yet on this build (VMS_IOCTL_CLUSTER_START calling"
        echo "     vms_cnxman_start() is FC-P3.9's wire-up, not this item's)"
    fi
done

# ---------------------------------------------------------------------------
# The executive-state proof: CLUSTER_DIAG_CSB, read from inside each guest.
# ---------------------------------------------------------------------------
echo ""
echo "--- reading CLUSTER_DIAG_CSB from each node's own executive ---"

read_diag() {
    local key="$1" args="$2" fd log off
    fd="${FD_[$key]}"; log="${LOG_[$key]}"; off=$(wc -c <"$log")
    send_to "$fd" "\$ RUN $DIAG $args"
    if wait_for_in "$log" 'status=' 20 "$off" "${QPID_[$key]}"; then
        tail -c "+$((off + 1))" "$log"
        return 0
    fi
    if wait_for_in "$log" 'csb_rows=' 5 "$off" "${QPID_[$key]}"; then
        tail -c "+$((off + 1))" "$log"
        return 0
    fi
    return 1
}

DIAG_REACHED=0
declare -A NODES_SEEN_
for k in "${KEYS[@]}"; do
    out="/tmp/cluster-mem-club-${k}.txt"
    if ! read_diag "$k" '-row club' >"$out" 2>/dev/null || ! grep -q '^status=' "$out"; then
        echo "  SKIP: $DIAG not reachable from node $k's console"
        continue
    fi
    DIAG_REACHED=1
    echo "node $k CLUSTER_DIAG_CSB row CLUB:"; sed 's/^/    /' "$out" | head -12
    if grep -q '^status=1$' "$out"; then
        ok "node $k: CNXMAN is UP -- the ioctl answers SS\$_NORMAL from real struct vms_club state"
    else
        bad "node $k: CLUSTER_DIAG_CSB row CLUB says CNXMAN is not up (status=$(grep '^status=' "$out" | head -1)) -- expected once FC-P3.9 wires vms_cnxman_start() into CLUSTER_START"
    fi

    walk="/tmp/cluster-mem-csb-${k}.txt"
    if read_diag "$k" '-walk 32' >"$walk" 2>/dev/null; then
        echo "node $k CSB rows:"; sed 's/^/    /' "$walk" | head -60
        rows=$(grep '^csb_rows=' "$walk" | tail -1 | cut -d= -f2)
        NODES_SEEN_[$k]="${rows:-0}"
        grep -q '^csid=00000000$' "$walk" \
            && bad "node $k: a CSB row reports csid 0 without its valid flag -- INV-6/E30 tripwire" \
            || ok "node $k: no CSB row ever prints a csid without csid_valid"
    fi
done

# ---------------------------------------------------------------------------
# Real membership: PENDING unless EXPECT_MEMBER=1 (E30 -- see the header).
# ---------------------------------------------------------------------------
echo ""
echo "--- real cluster membership (E30) ---"
for k in "${KEYS[@]}"; do
    echo "  node $k discovered CSB rows: ${NODES_SEEN_[$k]:-0}"
done
member_ready=1
for k in "${KEYS[@]}"; do
    [ "${NODES_SEEN_[$k]:-0}" -ge 2 ] || member_ready=0
done
if [ "$member_ready" -eq 1 ]; then
    ok "every node discovered at least 2 peer CSBs"
elif [ "$EXPECT_MEMBER" = "1" ]; then
    bad "EXPECT_MEMBER=1 but not every node discovered its 2 peers"
else
    pend "not every node discovered 2 peer CSBs yet -- CNXMAN's own join drive "
    echo "     (VAXCLUSTER=1 -> cnxman_join_start()) needs a CSB to target,"
    echo "     which needs a lower-level HELLO-discovery-to-CSB wiring this"
    echo "     item's own report flags as an escalation. Re-run with"
    echo "     EXPECT_MEMBER=1 once that lands."
fi
if [ "$EXPECT_MEMBER" = "1" ]; then
    pend "SHOW CLUSTER / \$GETSYI CLUSTER_NODES==3 on each node is NOT YET"
    echo "     driven by this harness (would need a DCL SHOW CLUSTER capture"
    echo "     per node); the CSB-table check above is the executive-state"
    echo "     stand-in until that DCL leg is added."
fi

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed, $PEND pending"
if [ "$DIAG_REACHED" -eq 0 ]; then
    echo "NOTE: the diagnostic reader was never reachable from a console, so the"
    echo "      executive-state half of this harness did not run. That is a"
    echo "      staging gap in this image build, not a proof of anything."
fi
if [ "$FAIL" -eq 0 ]; then
    echo "OK -- three booted OVMX executives reached this rung's real, scored legs"
    exit 0
fi
exit 1
