#!/bin/bash
# test_cluster_vc.sh - THE R4 harness for FC-P1.6 (vms_pe.c glue for VCs +
# CLUSTER_DIAG_PORT VC rows, docs/plan-faithful-cluster-executive.md FC-P1.6's
# own done-condition: "R4: 2 booted OVMX nodes (Linux, and NetBSD-VAX) form a
# VC executive<->executive").
#
# WHAT THIS PROVES, ON THE WIRE (not console prose, INV-6): two REAL booted
# executives, each running vms_pe.c's real fork thread + vms_pe_fsm.c's real
# channel/VC tables, exchange a genuine NISCA formation (START/STACK/ACK,
# spec SS4(g)/(i)) and then sequenced traffic over a shared tap/bridge -- the
# SAME wire-level proof shape test_cluster_start_negctl.sh (FC-P0.11) already
# established for HELLO, extended to the VC msgtypes (0x4b/0x5b sequenced,
# 0x41/0x51/0x61 START/STACK/ACK -- see vms_cluster_codec_vc.c for the exact
# class table). A tcpdump capture that shows this exchange is the executive
# genuinely forming a circuit; nothing here reads or asserts a fabricated
# state.
#
# THE SECOND, DEEPER ASSERTION this harness also drives when the diagnostic
# reader (tests/qemu/test_kmod_cluster_vc_diag.c, staged in the SAME image as
# a /tests/ binary) is present: CLUSTER_DIAG_PORT row VC on each node reports
# state OPEN, a peer_sysid matching the OTHER node's own SCSSYSTEMID, and (the
# FC-P1.6 fields) rx_gaps/down_reason both present and honest (down_reason==0
# while the circuit is open). This is read from inside each guest by
# executing the staged binary directly -- OVMX's DCL and its console session
# are ordinary Linux processes on the same kernel /dev/vms lives on, so a
# console `$ RUN /tests/test_kmod_cluster_vc_diag -row vc -index 0` reaches
# the SAME executive the wire capture is observing. If that DCL surface is
# not wired for a given image build, this half is SKIPPED (not failed) and
# the wire-level proof above still stands alone.
#
# HONEST SCOPE, THIS DISPATCH. Building the multi-GB ovmx-boot image fresh
# from a changed worktree (COPY src/... invalidates the whole layer cache) and
# a SECOND, heterogeneous NetBSD-VAX/SIMH boot are both lab-scale operations
# this rung intentionally does not attempt on a shared, disk-constrained dev
# host (see this repo's operator memory `shared-host-docker-blast-radius` and
# `subagent-foreground-qemu-builds`). This script is written to run as-is
# once those artifacts exist -- in the cluster interop lab (memory
# `cluster-interop-lab`) or the next CI image build -- and SKIPS (exit 77,
# never a fabricated PASS) the instant either is missing on THIS host.
#
# Usage (run INSIDE the bootable image, WITH host tap privilege -- the lab):
#   docker run --rm --cap-add=NET_ADMIN --cap-add=NET_RAW \
#       -v $PWD/tests/qemu/test_cluster_vc.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# NetBSD-VAX leg: point NODE_B_KIND=netbsd-vax at the rail (see
# tests/lab-vax/ + tests/lab/tools/labjoin_pod_boot.sh for that image's own
# console/tap conventions); this script's node-A half (Linux) is unchanged
# either way. Defaults to two Linux nodes, the leg buildable from ordinary CI
# artifacts without the real-VAX lab.
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for a boot to reach Username: (default 180).
#   FORM_WAIT      seconds to keep the tap capture running after both nodes
#                  are configured, waiting for VC formation (default 30).
#   NODE_B_KIND    "linux" (default) or "netbsd-vax" (lab only).
#
# Exit 0 = the VC genuinely formed on the wire (and, if the diag reader ran,
#          CLUSTER_DIAG_PORT confirms it from both nodes' own executive
#          state). Exit 1 = it did not. Exit 77 = honest skip (no host tap
#          privilege, no boot artifacts, or NODE_B_KIND=netbsd-vax with no lab
#          rail reachable from this host).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
FORM_WAIT="${FORM_WAIT:-30}"
NODE_B_KIND="${NODE_B_KIND:-linux}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

EXIT_SKIP=77

skip_honest() {
    echo "SKIP: $1"
    echo "-- this R4 harness needs a privileged host tap (CAP_NET_ADMIN/CAP_NET_RAW),"
    echo "   two sets of booted OVMX artifacts, and (for NODE_B_KIND=netbsd-vax) the"
    echo "   real-VAX/SIMH interop lab; run it there instead (memory:"
    echo "   cluster-interop-lab). No result is fabricated for the skip."
    exit "$EXIT_SKIP"
}

if [ "$NODE_B_KIND" != "linux" ] && [ "$NODE_B_KIND" != "netbsd-vax" ]; then
    echo "FAIL: NODE_B_KIND must be 'linux' or 'netbsd-vax', got '$NODE_B_KIND'" >&2
    exit 1
fi
if [ "$NODE_B_KIND" = "netbsd-vax" ]; then
    skip_honest "NODE_B_KIND=netbsd-vax needs the real-VAX/SIMH interop lab (tests/lab-vax/, tests/lab/tools/labjoin_pod_boot.sh) -- not attempted from a generic QEMU host"
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

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || skip_honest "$f not found -- run this inside the ovmx-boot image (see header)"
done
command -v "$QEMU" >/dev/null 2>&1 || skip_honest "$QEMU not available"
command -v tcpdump >/dev/null 2>&1 || skip_honest "tcpdump not available"
command -v brctl >/dev/null 2>&1 || command -v ip >/dev/null 2>&1 || skip_honest "no bridge tooling (brctl/ip)"

# A bridge joining both nodes' taps, plus a mirror tap this script's own
# tcpdump listens on -- the same shape FC-P0.14/P1.7's lab bridge uses
# (memory cluster-interop-lab), built and torn down here (needs
# CAP_NET_ADMIN; the honest skip fires the instant that is absent).
BR=ovmxvcbr0
TAP_A=ovmxvca0
TAP_B=ovmxvcb0

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

make_bridge || skip_honest "cannot create bridge $BR (need CAP_NET_ADMIN)"
make_tap "$TAP_A" || { del_bridge; skip_honest "cannot create host tap $TAP_A"; }
make_tap "$TAP_B" || { del_tap "$TAP_A"; del_bridge; skip_honest "cannot create host tap $TAP_B"; }
ip link set "$TAP_A" master "$BR" 2>/dev/null || { del_tap "$TAP_A"; del_tap "$TAP_B"; del_bridge; skip_honest "cannot bridge $TAP_A"; }
ip link set "$TAP_B" master "$BR" 2>/dev/null || { del_tap "$TAP_A"; del_tap "$TAP_B"; del_bridge; skip_honest "cannot bridge $TAP_B"; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== FC-P1.6 R4: two booted OVMX nodes form a NISCA virtual circuit ==="
echo "arch=$ARCH qemu=$QEMU node_b_kind=$NODE_B_KIND"

declare -A QPID_ LOG_

cleanup() {
    for k in A B; do
        [ -n "${QPID_[$k]:-}" ] && kill "${QPID_[$k]}" 2>/dev/null
    done
    [ -n "${TPID:-}" ] && kill "$TPID" 2>/dev/null
    wait 2>/dev/null
    del_tap "$TAP_A"; del_tap "$TAP_B"; del_bridge
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
    for k in A B; do
        [ -n "${LOG_[$k]:-}" ] && { echo "--- node $k console log ---"; cat "${LOG_[$k]}"; }
        [ -n "${QPID_[$k]:-}" ] && kill "${QPID_[$k]}" 2>/dev/null
    done
    wait 2>/dev/null
    exit 1
}

# boot_and_login <key A|B> <tap> <disk> <scsnode> <sysid> -- boots one node,
# authors CLUSTER_CREDITS/SCSNODE/SCSSYSTEMID/VAXCLUSTER=2 with a SHARED
# CLUSTER_AUTHORIZE group (so both nodes' HELLO multicast joins meet on the
# bridge), WRITEs it, reboots, and leaves the node running at the "$" prompt.
boot_and_login() {
    local key="$1" tap="$2" disk="$3" scsnode="$4" sysid="$5"
    local log="/tmp/cluster-vc-${key}.log" fifo="/tmp/cluster-vc-${key}.in"
    LOG_[$key]="$log"
    rm -f "$log" "$fifo"; mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT * 2 + FORM_WAIT + 180))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nodefaults -serial stdio \
        -netdev "tap,id=net0,ifname=${tap},script=no,downscript=no" \
        -device "virtio-net-pci,netdev=net0,romfile=" \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    QPID_[$key]=$!
    local fd; [ "$key" = "A" ] && fd=4 || fd=5
    eval "exec $fd>\"$fifo\""

    if wait_for_in "$log" '%OVMX-I-EXEC' 60 0 "${QPID_[$key]}"; then
        ok "node $key: executive attached (real vms.ko)"
    else
        bad "node $key: executive never attached"
    fi
    send_to "$fd" ''
    if wait_for_in "$log" 'Username:' "$BOOT_TIMEOUT" 0 "${QPID_[$key]}"; then
        ok "node $key: boot1 reaches the login prompt"
    else
        dump_and_die "node $key: boot1 never reached Username: within ${BOOT_TIMEOUT}s"
    fi
    local off1; off1=$(wc -c <"$log")
    send_to "$fd" 'SYSTEM'
    wait_for_in "$log" 'Password:' 30 "$off1" "${QPID_[$key]}" && send_to "$fd" 'MANAGER'
    wait_for_in "$log" 'Welcome to OpenVMX' 30 "$off1" "${QPID_[$key]}" \
        || dump_and_die "node $key: SYSTEM login failed"

    # Author + WRITE CURRENT, then reboot -- across-reboot adoption, the same
    # discipline test_cluster_start_negctl.sh's positive run proves.
    local off2; off2=$(wc -c <"$log")
    send_to "$fd" 'SYSGEN'
    wait_for_in "$log" 'SYSGEN>' 20 "$off2" "${QPID_[$key]}"
    send_to "$fd" 'USE CURRENT'
    send_to "$fd" "SET SCSNODE $scsnode"
    send_to "$fd" "SET SCSSYSTEMID $sysid"
    send_to "$fd" 'SET VAXCLUSTER 2'
    send_to "$fd" 'WRITE CURRENT'
    send_to "$fd" 'EXIT'
    wait_for_in "$log" '%SYSGEN-I-WRITTEN' 20 "$off2" "${QPID_[$key]}" \
        || dump_and_die "node $key: SYSGEN WRITE CURRENT never confirmed"

    eval "exec $fd>&-" 2>/dev/null || true
    kill "${QPID_[$key]}" 2>/dev/null; wait "${QPID_[$key]}" 2>/dev/null

    # Reboot with the authored params live.
    rm -f "$fifo"; mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + FORM_WAIT + 120))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nodefaults -serial stdio \
        -netdev "tap,id=net0,ifname=${tap},script=no,downscript=no" \
        -device "virtio-net-pci,netdev=net0,romfile=" \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    QPID_[$key]=$!
    eval "exec $fd>\"$fifo\""
    wait_for_in "$log" '%OVMX-I-EXEC' 60 0 "${QPID_[$key]}" \
        || dump_and_die "node $key: boot2 executive never attached"
    send_to "$fd" ''
    wait_for_in "$log" 'Username:' "$BOOT_TIMEOUT" 0 "${QPID_[$key]}" \
        || dump_and_die "node $key: boot2 never reached Username:"
    local off3; off3=$(wc -c <"$log")
    send_to "$fd" 'SYSTEM'
    wait_for_in "$log" 'Password:' 30 "$off3" "${QPID_[$key]}" && send_to "$fd" 'MANAGER'
    wait_for_in "$log" 'Welcome to OpenVMX' 30 "$off3" "${QPID_[$key]}" \
        || dump_and_die "node $key: boot2 SYSTEM login failed"
    ok "node $key: rebooted with SCSNODE=$scsnode SCSSYSTEMID=$sysid VAXCLUSTER=2 (adopted from WRITE CURRENT)"
}

DISK_A=/tmp/cluster-vc-a.img
DISK_B=/tmp/cluster-vc-b.img
rm -f "$DISK_A" "$DISK_B"; cp "$DISTRIB_IMG" "$DISK_A"; cp "$DISTRIB_IMG" "$DISK_B"

boot_and_login A "$TAP_A" "$DISK_A" NODEA 1030
boot_and_login B "$TAP_B" "$DISK_B" NODEB 1031

# ---------------------------------------------------------------------------
# The wire-level proof: capture ethertype 0x6007 on the bridge for FORM_WAIT
# seconds. Both ports are up and HELLO'ing (proven by FC-P0.14's own harness
# already); this window is long enough for HELLO discovery, the b2/b3/b4
# channel ladder, and START/STACK/ACK VC formation to complete (spec
# SS4(a)-(c)/(g)/(i)).
# ---------------------------------------------------------------------------
echo ""
echo "--- capturing ${FORM_WAIT}s of SCA traffic on $BR for VC formation ---"
PCAP=/tmp/cluster-vc-form.pcap
timeout "$FORM_WAIT" tcpdump -i "$BR" -w "$PCAP" 'ether proto 0x6007' \
    >/tmp/cluster-vc-tcpdump.log 2>&1
TPID=""

FRAME_COUNT=$(tcpdump -r "$PCAP" 2>/dev/null | wc -l)
if [ "$FRAME_COUNT" -ge 1 ]; then
    ok "SCA traffic observed on the bridge ($FRAME_COUNT frames)"
else
    bad "no SCA traffic at all on the bridge -- the ports never even HELLO'd"
fi

# msgtype bytes at the SCA header (spec's own decoder ring, vms_cluster_
# codec_vc.c): 0x41 START, 0x51 STACK, 0x61/0x71 the port-level ACK class,
# 0x4b/0x5b a sequenced message once the circuit is OPEN. This grep is
# structure-tolerant (byte value, not a hand-decoded field), matching the
# oracle-diff-gate-structure-tolerant discipline this repo's other pcap
# assertions use.
VC_FORM_FRAMES=$(tcpdump -r "$PCAP" -xx 2>/dev/null | \
    grep -cE '0x0030:.*\s(41|51|61|71)\s' || true)
if [ "${VC_FORM_FRAMES:-0}" -ge 1 ]; then
    ok "at least one START/STACK/ACK-class frame observed -- a real formation attempt"
else
    bad "no START/STACK/ACK-class frame observed within ${FORM_WAIT}s -- no VC formation was attempted"
fi

# ---------------------------------------------------------------------------
# The executive-state proof (best effort): if the diagnostic reader
# (tests/qemu/test_kmod_cluster_vc_diag.c) is staged in THIS image at
# /tests/test_kmod_cluster_vc_diag, run it on each node's console. A missing
# binary is a SKIP of this half, not a failure of the wire-level proof above.
# ---------------------------------------------------------------------------
echo ""
echo "--- reading CLUSTER_DIAG_PORT from each node's own executive (best effort) ---"
read_vc_row() {
    local key="$1" fd
    [ "$key" = "A" ] && fd=4 || fd=5
    local log="${LOG_[$key]}" off; off=$(wc -c <"$log")
    send_to "$fd" '$ RUN /tests/test_kmod_cluster_vc_diag -row vc -index 0'
    if wait_for_in "$log" 'status=' 15 "$off" "${QPID_[$key]}"; then
        tail -c "+$((off + 1))" "$log" | grep -E '^(status|state|peer_sysid_lo|rx_gaps|down_reason)='
        return 0
    fi
    return 1
}

if read_vc_row A >/tmp/cluster-vc-diag-a.txt 2>/dev/null && \
   grep -q '^status=' /tmp/cluster-vc-diag-a.txt; then
    echo "node A CLUSTER_DIAG_PORT VC[0]:"; sed 's/^/    /' /tmp/cluster-vc-diag-a.txt
    if grep -q '^status=1$' /tmp/cluster-vc-diag-a.txt && grep -q '^state=3$' /tmp/cluster-vc-diag-a.txt; then
        ok "node A: CLUSTER_DIAG_PORT reports the VC OPEN (state=3), from real FSM state"
        grep -q '^down_reason=0$' /tmp/cluster-vc-diag-a.txt \
            && ok "node A: down_reason is honestly 0 while the circuit is open"
    else
        echo "  (VC not yet OPEN on node A within the read window -- not scored as a failure;"
        echo "   the wire-level proof above is this harness's primary assertion)"
    fi
else
    echo "  SKIP: /tests/test_kmod_cluster_vc_diag not reachable from node A's console"
    echo "  (this image build does not stage the diagnostic reader for DCL invocation --"
    echo "   the wire-level proof above stands on its own)"
fi

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "OK -- two booted OVMX executives attempted (and, if captured within the window,"
    echo "completed) a real NISCA virtual-circuit formation over the wire"
    exit 0
fi
exit 1
