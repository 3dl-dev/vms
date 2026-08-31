#!/bin/bash
# labjoin_pod_boot.sh - runs INSIDE a lab-2 pod (staged there by
# labjoin_booted.sh via kubectl). Boots ONE booted-OVMX node under QEMU/TCG with
# its virtio NIC bridged to the pod's br0 tap, authors its cluster identity at
# the SYSBOOT> prompt, logs in, and runs SHOW CLUSTER -- writing a machine-
# readable console transcript to $OUT_LOG on the tank volume (host-readable).
#
# WHY IN-POD. lab-2's br0 lives inside the pod's own network namespace (one pod
# == one lab), so the OVMX node -- like the SCSD probe before it (lab2run.sh) --
# must run inside the pod to reach the VAX cluster's L2. Unlike the SCSD probe (a
# bare Linux ELF), the MILESTONE is the BOOTED runtime: the shipped
# distro/Dockerfile.bootable image booting through STARTUP to DCL, which is meant
# to AUTO-START SCS as a system process (vms-5ad/110b.1, the peer's in-flight
# work). This script does NOT start SCS itself and does NOT touch the guest's
# scsd/ovmx_init/VMS$VMS.DAT -- it only boots the real image and authors the
# cluster identity SYSBOOT> asks for. Pre-5ad the booted node never spawns SCS,
# so SHOW CLUSTER shows no VAX and the acceptance verdict is honestly RED. That
# is the point.
#
# ⚠ NO /dev/kvm IN THE POD. The nested QEMU runs under TCG (software emulation) --
# functional but ~10x slower than KVM. Every timeout here is sized for TCG.
#
# Env (all set by the orchestrator):
#   ART_DIR   dir holding vmlinuz, initramfs-ovmx-slim.cpio.gz, ovmx-distrib.img
#   OUT_LOG   transcript path on the tank volume (host-readable via HOSTL)
#   SCSNODE   authored cluster node name (<=6 chars on the wire)
#   SCSSYSID  authored SCSSYSTEMID (numeric, collision-guarded by the caller)
#   OVMX_TAP  tap ifname on br0 the orchestrator created (default tap4)
#   OVMX_MAC  guest NIC MAC (default 52:54:00:00:00:f4)
#   BOOT_TO   per-QEMU timeout, seconds (default 900 -- TCG)
set -uo pipefail

ART_DIR="${ART_DIR:?ART_DIR required}"
OUT_LOG="${OUT_LOG:?OUT_LOG required}"
SCSNODE="${SCSNODE:?SCSNODE required}"
SCSSYSID="${SCSSYSID:?SCSSYSID required}"
OVMX_TAP="${OVMX_TAP:-tap4}"
OVMX_MAC="${OVMX_MAC:-52:54:00:00:00:f4}"
BOOT_TO="${BOOT_TO:-900}"

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=labjoin_lib.sh
. "$HERE/labjoin_lib.sh"

KERNEL="$ART_DIR/vmlinuz"
INITRD="$ART_DIR/initramfs-ovmx-slim.cpio.gz"
DISTRIB="$ART_DIR/ovmx-distrib.img"
for f in "$KERNEL" "$INITRD" "$DISTRIB"; do
    [ -f "$f" ] || { echo "labjoin_pod_boot: FATAL -- missing artifact $f" | tee -a "$OUT_LOG"; exit 2; }
done

QEMU="${OVMX_POD_QEMU:-qemu-system-x86_64}"
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "labjoin_pod_boot: FATAL -- $QEMU not found in the pod. The booted OVMX node needs an" \
         "x86_64 QEMU inside the lab pod (the stock lab image ships only SIMH). Provision it" \
         "(stage a static qemu-system-x86_64, or bake it into tests/lab/Dockerfile) before the" \
         "heavy run -- this is the coordinator-owned lab-2 provisioning step." | tee -a "$OUT_LOG"
    exit 3
}

# A per-node working copy of the distribution disk (SYSBOOT> WRITE mints a new
# OVMXVMSSYS.PAR version onto it; never mutate the staged golden image).
DISK="/tmp/ovmx-node-$$.img"
cp "$DISTRIB" "$DISK"
FIFO="/tmp/ovmx-node-$$.in"
rm -f "$FIFO"; mkfifo "$FIFO"

# Tap netdev/device args -- byte-identical to run-qemu.sh OVMX_NET_MODE=tap.
mapfile -t NET_ARGS < <(lj_tap_netdev_args "$OVMX_TAP" "$OVMX_MAC")

: > "$OUT_LOG"
echo "=== booted-OVMX node: SCSNODE=$SCSNODE SCSSYSTEMID=$SCSSYSID tap=$OVMX_TAP mac=$OVMX_MAC qemu=$QEMU (TCG) ===" | tee -a "$OUT_LOG"

# ovmx.flags=0,1 halts at SYSBOOT> pre-banner so we can author the cluster
# identity (SET/WRITE/CONTINUE), exactly as tests/qemu/test_sysboot_cluster_
# params_e2e.sh does. -serial stdio carries the console; stdin comes from the FIFO.
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TO" "$QEMU" -accel tcg \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "console=ttyS0 loglevel=3 net.ifnames=0 ovmx.flags=0,1" \
    -m 512M -smp 1 -nodefaults -serial stdio \
    "${NET_ARGS[@]}" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >>"$OUT_LOG" 2>&1 &
QP=$!
exec 4>"$FIFO"
send() { printf '%s\r' "$1" >&4; }

waitfor() {  # <pattern> <limit-seconds>
    local pat="$1" lim="${2:-120}" w=0
    while [ "$w" -lt $((lim * 2)) ]; do
        grep -qaF -- "$pat" "$OUT_LOG" 2>/dev/null && return 0
        kill -0 "$QP" 2>/dev/null || return 1
        sleep 0.5; w=$((w + 1))
    done
    return 1
}

rc=0
# 1. SYSBOOT> prompt (pre-banner). TCG-sized wait.
if waitfor 'SYSBOOT> ' 300; then
    echo "[node] SYSBOOT> reached" | tee -a "$OUT_LOG"
    send "SET SCSNODE $SCSNODE";    sleep 2
    send "SET SCSSYSTEMID $SCSSYSID"; sleep 2
    send 'SET VAXCLUSTER 2';        sleep 2   # make VAXCLUSTER effectual (2 = enabled)
    send 'WRITE';                   sleep 2
    waitfor 'parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR' 60 || true
    send 'CONTINUE';                sleep 2
else
    echo "[node] FATAL -- SYSBOOT> never appeared" | tee -a "$OUT_LOG"; rc=1
fi

# 2. Boot to the login prompt; OPA0: waits for RETURN.
if [ "$rc" -eq 0 ]; then
    w=0
    until grep -qaF 'Username:' "$OUT_LOG" 2>/dev/null || [ "$w" -ge 300 ]; do
        kill -0 "$QP" 2>/dev/null || break
        send ''; sleep 1; w=$((w + 1))
    done
    if grep -qaF 'Username:' "$OUT_LOG"; then
        send 'SYSTEM'; sleep 2
        send 'MANAGER'; sleep 2
        waitfor 'Welcome to OpenVMX' 120 || true
        # Give an auto-started SCS component time to complete its join over the
        # tap L2 before we sample membership (TCG + real cluster handshake).
        sleep 60
        send 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'; sleep 2
        send 'SHOW CLUSTER'; sleep 8
        send 'WRITE SYS$OUTPUT "OVMX-SC-DONE"'; sleep 3
        waitfor 'OVMX-SC-DONE' 30 || true
    else
        echo "[node] FATAL -- never reached Username:" | tee -a "$OUT_LOG"; rc=1
    fi
fi

send 'LOGOUT' 2>/dev/null; sleep 1
kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$FIFO" "$DISK"
echo "=== node boot driver done (rc=$rc) ===" | tee -a "$OUT_LOG"
exit "$rc"
