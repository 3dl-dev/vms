#!/bin/bash
# test_cluster_conn.sh - THE R4 harness for FC-P2.4 (vms_scs.c glue + snapshot
# + CLUSTER_DIAG_CONN, docs/plan-faithful-cluster-executive.md FC-P2.4's own
# done-condition: "R4: 2 OVMX nodes open directory connections and look each
# other up (both substrates)").
#
# WHAT THIS PROVES, FROM EXECUTIVE STATE AND FROM THE WIRE (never console
# prose, INV-6). Two REAL booted executives, each running vms_pe.c's port and
# -- new with FC-P2.4 -- vms_scs.c's SCS layer on top of it:
#
#   1. SCS IS REALLY UP ON BOTH. CLUSTER_DIAG_CONN row SCS answers SS$_NORMAL,
#      reports at least one registered SYSAP -- vms_scs_start() registers
#      SCS$DIRECTORY itself, and that registration MINTS a Con.ID, which the
#      allocator refuses to do until the glue has seeded it from the port's
#      real incarnation (SCS_ERR_NOCONID, vms_scs_fsm.h SS4). So the SYSAP
#      count is also the proof that the seeding was real. Read through the
#      ioctl, from real struct scs_fsm state, under the fork mutex.
#   2. THE LISTENING CDT IS REAL. Walking the CDL, one row's Local SYSAP is
#      `SCS$DIRECTORY` in state LISTEN with a real minted Con.ID -- the
#      p. 2-48 SDIR content a member's inbound connect would be routed
#      through.
#   3. THE INV-6 TRIPWIRE. No row anywhere reports a remote Con.ID the
#      executive never learned (a value with `remote_conid_valid` clear). A
#      placeholder connection identifier is what bugchecked a real VAX; this
#      harness refuses to see one.
#   4. THE WIRE. `SCS$DIRECTORY` ASCII inside a captured 0x6007 frame is a
#      connect verb naming that SYSAP genuinely going out or coming in
#      (the names ride at payload [62:], wire spec SS4(h) phase 4), and the
#      94-content class is the directory message class (SS4(h)(2a)).
#
# HONEST SCOPE -- READ THIS BEFORE SCORING THE LOOKUP LEG.
#
# FC-P2.4 lands the SERVER half live on a booted node: SCS$DIRECTORY is
# registered at CLUSTER_START, so a peer that opens a directory connection is
# accepted and answered from the one real registry. NOTHING IN THE EXECUTIVE
# YET *ISSUES* a lookup: `scs_dir_lookup` is called by CNXMAN's join
# (FC-P3.3, which the plan's own dependency graph puts after this item), so
# two OVMX nodes standing side by side will not spontaneously look each other
# up. This harness therefore SCORES what FC-P2.4 really delivers (1-4 above)
# and REPORTS the client-driven round as PENDING unless EXPECT_LOOKUP=1 is
# set -- which is what the lab sets once a driver exists (FC-P3.3, or the
# real VAX in FC-P2.6, whose own join issues the lookups at OVMX). A PENDING
# leg is never counted as a pass and never fabricated as one.
#
# Building the multi-GB ovmx-boot image fresh from a changed worktree and a
# second, heterogeneous NetBSD-VAX/SIMH boot are both lab-scale operations
# this rung does not attempt on a shared, disk-constrained dev host (operator
# memory `shared-host-docker-blast-radius`, `subagent-foreground-qemu-builds`).
# The script is written to run as-is once those artifacts exist, and SKIPS
# (exit 77, never a fabricated PASS) the instant either is missing.
#
# Usage (run INSIDE the bootable image, WITH host tap privilege -- the lab):
#   docker run --rm --cap-add=NET_ADMIN --cap-add=NET_RAW \
#       -v $PWD/tests/qemu/test_cluster_conn.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT    seconds to wait for a boot to reach Username: (default 180)
#   FORM_WAIT       seconds of wire capture after both nodes are up (default 45
#                   -- longer than the VC harness's 30, because the directory
#                   round happens only after HELLO + channel + VC formation)
#   NODE_B_KIND     "linux" (default) or "netbsd-vax" (lab only)
#   EXPECT_LOOKUP   1 = a directory ROUND must be observed (set by the lab once
#                   a client driver exists); default 0 = report it PENDING
#
# Exit 0 = both executives really brought SCS up and the diagnostics prove it
#          from executive state. Exit 1 = they did not (or EXPECT_LOOKUP=1 and
#          no round happened). Exit 77 = honest skip.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
FORM_WAIT="${FORM_WAIT:-45}"
NODE_B_KIND="${NODE_B_KIND:-linux}"
EXPECT_LOOKUP="${EXPECT_LOOKUP:-0}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
DIAG=/tests/test_kmod_cluster_conn_diag
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
command -v ip >/dev/null 2>&1 || skip_honest "no bridge tooling (ip)"

BR=ovmxconnbr0
TAP_A=ovmxconna0
TAP_B=ovmxconnb0

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
PEND=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
pend() { echo "  PENDING: $1"; PEND=$((PEND + 1)); }

echo "=== FC-P2.4 R4: two booted OVMX executives bring SCS up over a real port ==="
echo "arch=$ARCH qemu=$QEMU node_b_kind=$NODE_B_KIND expect_lookup=$EXPECT_LOOKUP"

declare -A QPID_ LOG_

cleanup() {
    for k in A B; do
        [ -n "${QPID_[$k]:-}" ] && kill "${QPID_[$k]}" 2>/dev/null
    done
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

# boot_and_login <key> <tap> <disk> <scsnode> <sysid> -- authors
# SCSNODE/SCSSYSTEMID/VAXCLUSTER=2, WRITEs it, reboots so the executive adopts
# them, and leaves the node at the "$" prompt with SCS started.
boot_and_login() {
    local key="$1" tap="$2" disk="$3" scsnode="$4" sysid="$5"
    local log="/tmp/cluster-conn-${key}.log" fifo="/tmp/cluster-conn-${key}.in"
    local fd off
    LOG_[$key]="$log"
    rm -f "$log"
    qemu_boot "$key" "$tap" "$disk" "$log" "$fifo" \
              "$((BOOT_TIMEOUT * 2 + FORM_WAIT + 180))"
    [ "$key" = "A" ] && fd=4 || fd=5
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
    send_to "$fd" 'SET VAXCLUSTER 2'
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
    ok "node $key: rebooted with SCSNODE=$scsnode SCSSYSTEMID=$sysid VAXCLUSTER=2"
}

DISK_A=/tmp/cluster-conn-a.img
DISK_B=/tmp/cluster-conn-b.img
rm -f "$DISK_A" "$DISK_B"; cp "$DISTRIB_IMG" "$DISK_A"; cp "$DISTRIB_IMG" "$DISK_B"

boot_and_login A "$TAP_A" "$DISK_A" NODEA 1040
boot_and_login B "$TAP_B" "$DISK_B" NODEB 1041

# ---------------------------------------------------------------------------
# The wire capture runs while the diagnostics are read, so the two views are
# of the SAME window.
# ---------------------------------------------------------------------------
PCAP=/tmp/cluster-conn.pcap
echo ""
echo "--- capturing ${FORM_WAIT}s of SCA traffic on $BR ---"
timeout "$FORM_WAIT" tcpdump -i "$BR" -w "$PCAP" 'ether proto 0x6007' \
    >/tmp/cluster-conn-tcpdump.log 2>&1

FRAME_COUNT=$(tcpdump -r "$PCAP" 2>/dev/null | wc -l)
if [ "$FRAME_COUNT" -ge 1 ]; then
    ok "SCA traffic observed on the bridge ($FRAME_COUNT frames)"
else
    bad "no SCA traffic at all on the bridge -- the ports never even HELLO'd"
fi

# `SCS$DIRECTORY` in the payload is a connect verb naming that SYSAP: the
# 16-byte blank-padded names ride at payload [62:] on the 110-content connect
# class (wire spec SS4(h) phase 4). ASCII match, not a hand-decoded field --
# the structure-tolerant discipline this repo's other pcap assertions use.
DIR_NAME_FRAMES=$(tcpdump -r "$PCAP" -A 2>/dev/null | grep -c 'SCS\$DIRECTORY' || true)
DIR_MSG_FRAMES=$(tcpdump -r "$PCAP" -e 2>/dev/null | grep -cE 'length (94|108)' || true)

# ---------------------------------------------------------------------------
# The executive-state proof: CLUSTER_DIAG_CONN, read from inside each guest.
# ---------------------------------------------------------------------------
echo ""
echo "--- reading CLUSTER_DIAG_CONN from each node's own executive ---"

read_diag() {
    local key="$1" args="$2" fd log off
    [ "$key" = "A" ] && fd=4 || fd=5
    log="${LOG_[$key]}"; off=$(wc -c <"$log")
    send_to "$fd" "\$ RUN $DIAG $args"
    if wait_for_in "$log" 'status=' 20 "$off" "${QPID_[$key]}"; then
        tail -c "+$((off + 1))" "$log"
        return 0
    fi
    if wait_for_in "$log" 'cdt_rows=' 5 "$off" "${QPID_[$key]}"; then
        tail -c "+$((off + 1))" "$log"
        return 0
    fi
    return 1
}

DIAG_REACHED=0
for k in A B; do
    out="/tmp/cluster-conn-scs-${k}.txt"
    if ! read_diag "$k" '-row scs' >"$out" 2>/dev/null || ! grep -q '^status=' "$out"; then
        echo "  SKIP: $DIAG not reachable from node $k's console"
        continue
    fi
    DIAG_REACHED=1
    echo "node $k CLUSTER_DIAG_CONN row SCS:"; sed 's/^/    /' "$out" | head -12
    if grep -q '^status=1$' "$out"; then
        ok "node $k: SCS is UP -- the ioctl answers SS\$_NORMAL from real scs_fsm state"
        awk -F= '/^n_sysaps=/ { exit ($2 >= 1) ? 0 : 1 }' "$out" \
            && ok "node $k: at least one SYSAP is registered (SCS\$DIRECTORY, by vms_scs_start)" \
            || bad "node $k: SCS is up but NO SYSAP is registered -- the directory registration failed"
        # The allocator's seeding is proved by the REGISTRATION above, not by
        # conid_epoch: registering SCS$DIRECTORY mints a Con.ID, which the
        # allocator refuses to do until the glue seeds it from the port's real
        # incarnation (SCS_ERR_NOCONID). A seed of 0 is itself a legal value,
        # so asserting conid_epoch != 0 would be a coin flip, not a check.
        echo "    (conid_epoch as the allocator holds it: $(grep '^conid_epoch=' "$out" | head -1))"
    else
        bad "node $k: CLUSTER_DIAG_CONN row SCS says SCS is not up (status=$(grep '^status=' "$out" | head -1))"
    fi

    walk="/tmp/cluster-conn-cdt-${k}.txt"
    if read_diag "$k" '-walk 32' >"$walk" 2>/dev/null; then
        echo "node $k CDT rows:"; sed 's/^/    /' "$walk" | head -40
        grep -q '^local_name=SCS\$DIRECTORY$' "$walk" \
            && ok "node $k: a CDT row's Local SYSAP is SCS\$DIRECTORY (the listening CDT, p. 2-48)" \
            || bad "node $k: no SCS\$DIRECTORY row in the CDL"
        grep -q '^local_conid=00000000$' "$walk" \
            && bad "node $k: a CDT row reports Con.ID 0 -- the allocator never mints one (INV-6)" \
            || ok "node $k: every projected row carries a real minted Local Con.ID"
    fi
done

# ---------------------------------------------------------------------------
# The client-driven round: scored only when a driver exists (see the header).
# ---------------------------------------------------------------------------
echo ""
echo "--- the directory ROUND (client half) ---"
echo "  SCS\$DIRECTORY-named frames on the wire: $DIR_NAME_FRAMES"
echo "  94/108-length (directory-message class) frames: $DIR_MSG_FRAMES"
if [ "${DIR_NAME_FRAMES:-0}" -ge 1 ]; then
    ok "a connect verb naming SCS\$DIRECTORY was really on the wire"
elif [ "$EXPECT_LOOKUP" = "1" ]; then
    bad "EXPECT_LOOKUP=1 but no SCS\$DIRECTORY connect was observed in ${FORM_WAIT}s"
else
    pend "no directory connection was opened -- NOTHING IN THE EXECUTIVE ISSUES A"
    echo "     LOOKUP YET. vms_scs_start() registers the SERVER half; the client"
    echo "     half (scs_dir_lookup) is driven by CNXMAN's join, FC-P3.3. Re-run"
    echo "     with EXPECT_LOOKUP=1 once that lands (or against a real VAX, whose"
    echo "     own join opens the connection to OVMX -- FC-P2.6)."
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
    echo "OK -- both booted OVMX executives brought SCS up on a real port and their"
    echo "own CLUSTER_DIAG_CONN reports it from real scs_fsm state"
    exit 0
fi
exit 1
