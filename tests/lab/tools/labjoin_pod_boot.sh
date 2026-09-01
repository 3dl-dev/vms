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
# Anti-fabrication teeth (rd vms-fa1a leg (e)): drop CAP_NET_RAW from the booted-
# OVMX node process subtree, and record the resulting capability set as evidence.
# Default ON -- a green gate with the cap denied proves the executive did the L2
# I/O, not the pod's ambient CAP_NET_RAW (the crutch the 0.6 LARP rode). Setting
# OVMX_DROP_NET_RAW=0 is a DELIBERATE control (used only by the negctl to model
# "the crutch is present"); the real gate never does.
OVMX_DROP_NET_RAW="${OVMX_DROP_NET_RAW:-1}"
CAP_EVID="${CAP_EVID:-${OUT_LOG%.log}.caps}"

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

# --- CAP_NET_RAW drop prefix (anti-fabrication teeth) ----------------------
# Build the argv prefix that strips CAP_NET_RAW from the QEMU process subtree.
# capsh (libcap2-bin) is required when dropping; FATAL-honest if absent rather
# than silently launching WITH the crutch (which would let the gate lie green).
DROP_PREFIX=()
if [ "$OVMX_DROP_NET_RAW" = "1" ]; then
    command -v capsh >/dev/null 2>&1 || {
        echo "labjoin_pod_boot: FATAL -- capsh (libcap2-bin) not found in the pod, but the" \
             "cap-denied gate requires dropping CAP_NET_RAW from the booted-OVMX node so a" \
             "join PROVES the executive did the L2 I/O (not the pod's ambient CAP_NET_RAW)." \
             "Install libcap2-bin in tests/lab/Dockerfile and rebuild the lab image. Refusing" \
             "to run WITHOUT the drop -- a green there would be exactly the 0.6 fabrication." | tee -a "$OUT_LOG"
        exit 3
    }
    mapfile -t DROP_PREFIX < <(lj_netraw_deny_argv)
    echo "[node] CAP_NET_RAW will be DROPPED from the booted-OVMX subtree (capsh --drop=cap_net_raw)" | tee -a "$OUT_LOG"
else
    echo "[node] ⚠ OVMX_DROP_NET_RAW=0 -- booted node runs WITH ambient caps (control only; NOT the gate)" | tee -a "$OUT_LOG"
fi

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

# --- Open the FIFO read-write, up front, before backgrounding the reader ----
# ⚠ THE BUG THIS FIXES (vms-4363). A FIFO opened O_RDONLY blocks in open(2)
# until a writer exists, and that block happens while the shell sets up the
# background command's redirections -- BEFORE it execve()s anything. This used
# to open the write side (`exec 4>"$FIFO"`) only AFTER backgrounding the qemu
# job, so the qemu-launch job sat blocked on open("$FIFO") for read, still just
# a fork of THIS script's own pre-drop process -- it had not yet execve'd
# capsh/timeout/qemu at all. Sampling /proc/$QP/status in that window (the old
# code did, immediately after backgrounding) therefore captured THIS script's
# own undropped, ambient capability set, not the booted node's -- the leg (e)
# evidence was stale by construction, independent of whether the drop itself
# ever worked. Reproduced: with the writer opened late, CapEff/CapBnd read back
# with CAP_NET_RAW STILL SET (…a80435fb).
#
# Simply moving the writer-open earlier (`exec 4>"$FIFO"` before backgrounding)
# just flips which side deadlocks: O_WRONLY also blocks until a reader exists,
# and now nothing has opened the read side yet. The fix is to open O_RDWR
# instead (`4<>`) -- an O_RDWR open of a FIFO never blocks in Linux, and having
# it open for the life of the script also means there is ALWAYS at least one
# writer present, so the qemu job's own independent `<"$FIFO"` (O_RDONLY) open
# succeeds immediately instead of blocking. The exec chain (capsh -> bash ->
# timeout -> qemu) therefore completes before we ever read /proc/$QP/status,
# so the sampled caps reflect the real drop (…a80415fb, net_raw clear).
exec 4<>"$FIFO"
send() { printf '%s\r' "$1" >&4; }

# ovmx.flags=0,1 halts at SYSBOOT> pre-banner so we can author the cluster
# identity (SET/WRITE/CONTINUE), exactly as tests/qemu/test_sysboot_cluster_
# params_e2e.sh does. -serial stdio carries the console; stdin comes from the FIFO.
# The DROP_PREFIX (capsh --drop=cap_net_raw -- -c 'exec "$@"' _) runs the whole
# QEMU subtree with CAP_NET_RAW stripped; empty when OVMX_DROP_NET_RAW=0.
# shellcheck disable=SC2086
"${DROP_PREFIX[@]}" timeout -k 15 "$BOOT_TO" "$QEMU" -accel tcg \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "console=ttyS0 loglevel=3 net.ifnames=0 ovmx.flags=0,1" \
    -m 512M -smp 1 -nodefaults -serial stdio \
    "${NET_ARGS[@]}" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >>"$OUT_LOG" 2>&1 &
QP=$!

# --- Record the cap EVIDENCE for the acceptance verdict (leg (e)) -----------
# Capture the booted-node process's actual capability sets, live, so the verdict
# grades what really ran -- not what we intended. $QP is the root of the QEMU
# subtree (capsh -> exec'd command); its CapEff/CapBnd apply to every descendant.
# Written to the tank-visible $CAP_EVID so labjoin_booted.sh can read it host-side.
# The writer is already open (above) so $QP is no longer blocked in open(2) on
# the FIFO; give the exec chain a little headroom anyway (up to 2s) in case
# capsh/bash/timeout haven't finished their chained execve()s yet.
: > "$CAP_EVID"
{
    echo "# CAP_NET_RAW evidence for booted-OVMX node pid=$QP (OVMX_DROP_NET_RAW=$OVMX_DROP_NET_RAW)"
    for _try in 1 2 3 4 5 6 7 8 9 10; do
        if [ -r "/proc/$QP/status" ]; then
            grep -E 'Cap(Inh|Prm|Eff|Bnd|Amb)' "/proc/$QP/status" 2>/dev/null && break
        fi
        sleep 0.2
    done
} >> "$CAP_EVID" 2>/dev/null
echo "[node] recorded CAP_NET_RAW evidence -> $CAP_EVID:" | tee -a "$OUT_LOG"
sed 's/^/[node]   /' "$CAP_EVID" | tee -a "$OUT_LOG"

# --- FAIL-HONEST: abort now if a requested drop did not actually land -------
# Do not wait for the after-the-fact gate verdict (labjoin_booted.sh, run only
# once this whole boot finishes) to notice the crutch is present -- that would
# spend the entire TCG boot budget on a run that was never going to prove
# anything. Reuse lj_cap_denied_verdict (the SAME function the final gate
# grades with) so this early check can never drift from the real verdict.
if [ "$OVMX_DROP_NET_RAW" = "1" ]; then
    if ! lj_cap_denied_verdict "$(cat "$CAP_EVID")" > /tmp/ovmx-node-$$.capverdict 2>&1; then
        sed 's/^/[node] /' /tmp/ovmx-node-$$.capverdict | tee -a "$OUT_LOG"
        echo "[node] FATAL -- OVMX_DROP_NET_RAW=1 was requested but CAP_NET_RAW is not verified" \
             "clear on the booted-node process (see leg (e) reason above). Aborting rather than" \
             "booting with the ambient-cap crutch possibly still present -- a green here would be" \
             "exactly the 0.6 fabrication this gate exists to catch." | tee -a "$OUT_LOG"
        rm -f /tmp/ovmx-node-$$.capverdict
        kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null
        exec 4>&-
        rm -f "$FIFO" "$DISK"
        echo "=== node boot driver done (rc=4) ===" | tee -a "$OUT_LOG"
        exit 4
    fi
    sed 's/^/[node] /' /tmp/ovmx-node-$$.capverdict | tee -a "$OUT_LOG"
    rm -f /tmp/ovmx-node-$$.capverdict
fi

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
        send 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'; sleep 2
        # Poll membership ACROSS the join window instead of sampling once. The
        # NEW->MEMBER join sequencer + the real-VAX cluster handshake take tens
        # of seconds under TCG; the old code slept a fixed 60s then sampled a
        # single SHOW CLUSTER and logged out, tearing the node down before a
        # join could ever complete (and hiding any NOTMEMBER->MEMBER transition).
        # Poll SHOW CLUSTER across the FULL window and KEEP THE NODE UP the whole
        # time -- do NOT break early on the first non-NOTMEMBER sample, and do NOT
        # log out until the window ends. Two reasons, both learned on lab-2
        # (vms-a84d): (1) OVMX renders the cluster-view header (no NOTMEMBER) the
        # moment it *thinks* it joined -- but that self-report can run AHEAD of the
        # real VAX's BRK_NON->MEMBER promotion (OVMX rendered itself MEMBER while
        # the authoritative VAX still had it BRK_NON). Breaking on the OVMX self-
        # report tears the node down before the VAX actually admits it -- and before
        # an independent VAX-side watch can catch OVMXJ0 STATUS==MEMBER live. The
        # teardown must be gated by the VAX-side MEMBER verdict, never an OVMX self-
        # report. (2) both-sides-by-eye needs a sustained LIVE member to observe.
        # qemu's BOOT_TO bounds it; kill -0 breaks if qemu exits.
        JOIN_POLL="${JOIN_POLL:-240}"; pw=0
        while [ "$pw" -lt "$JOIN_POLL" ]; do
            kill -0 "$QP" 2>/dev/null || break
            send "WRITE SYS\$OUTPUT \"OVMX-POLL-$pw\""; sleep 1
            send 'SHOW CLUSTER'; sleep 8
            sleep 11; pw=$((pw + 20))
        done
        echo "[node] join-poll held the node LIVE ~${pw}s (full window; teardown is gated by the VAX-side STATUS==MEMBER verdict, not an OVMX self-report)" | tee -a "$OUT_LOG"
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
