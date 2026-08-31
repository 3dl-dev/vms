#!/bin/bash
# test_scs_autostart.sh - a booted OVMX runtime auto-starts SCS (the cluster
# connection manager, SCSD.EXE) as a detached system process when the node
# participates in a VMScluster (vms-5ad, child of vms-110b).
#
# WHAT THIS PROVES, END TO END, AGAINST THE REAL MASTERED IMAGE.
#
# Before this item nothing started SCSD at boot: only a hand-launched lab
# probe (always given explicit --iface/--connect flags) ever ran it. This
# item adds two halves, both exercised here against a real boot:
#
#   1. src/vmsscs/scsd.c's BOOT-CLUSTER MODE: invoked with NO argv at all
#      (exactly what RUN /DETACHED gives it -- OVMX's RUN has no
#      /PARAMETERS qualifier), SCSD reads VAXCLUSTER off the SYSGEN store
#      (SYS$SYSTEM:OVMXVMSSYS.PAR, sysgen_read_param -- the same store
#      SCSNODE/SCSSYSTEMID/ALLOCLASS already come from) and either exits 0
#      declining to run (VAXCLUSTER==0, standalone) or self-resolves the
#      primary Ethernet NIC (scs_datalink_primary_iface(), the userspace
#      twin of the executive's exec_netdev_primary() -- the same device the
#      device table names ETH0:) and runs the --connect path persistently.
#   2. distro/rootfs/.../SYS$STARTUP/SCS_STARTUP.COM, a CONFIG-phase STDRV
#      component registered in VMS$VMS.DAT, which RUN/DETACHED's SCSD.EXE
#      with no arguments -- the boot-time half that reaches (1).
#
# THREE ARMS, THREE MASTERED DISKS.
#
#   ARM P (positive): boot 1 authors VAXCLUSTER=2 via SYSGEN and reboots
#     (SCS_STARTUP.COM runs very early in EVERY boot, long before
#     SYSTARTUP_VMS.COM's own SYSGEN authoring could take effect -- so the
#     freshly authored value is only live starting on the NEXT boot, same
#     shape as tests/qemu/test_cluster_param_adoption.sh). Boot 2 asserts a
#     real, persistent SCS_SERVER detached process and a boot-cluster-mode
#     run log naming a real NIC (never the lab default "br0").
#   ARM N (VAXCLUSTER negative control): the shipped default, VAXCLUSTER=0,
#     unauthored -- one boot, asserts SCSD ran, self-gated honestly, and
#     created NO detached process.
#   ARM R (registration negative control): boots
#     /boot/ovmx-distrib-scs-negctl.img -- a SECOND mastered image built by
#     distro/Dockerfile.bootable from the identical staged tree with
#     SCS_STARTUP.COM's CONFIG-phase registration removed and nothing else
#     changed. Boot 1 authors VAXCLUSTER=2 on THIS image too (so SCS WOULD
#     start if only the in-daemon VAXCLUSTER gate mattered); boot 2 proves
#     SCSD.EXE is never RUN/DETACHED anyway -- the registration, not the
#     daemon's own self-gate, is what actually creates it. Mirrors
#     tests/qemu/test_job_control_negctl.sh's shape exactly, applied to SCS.
#
# A KNOWN, GROUNDED LIMITATION -- FLAGGED, NOT PAPERED OVER (Rule 5). The
# original scope for this test asserted SHOW CLUSTER would show the LOCAL
# node in ARM P. Reading scsd_publish_membership() (src/vmsscs/scsd.c)
# during this item's own implementation showed that is not true today: a
# node with VAXCLUSTER!=0 but NO peer never becomes a "member" in the
# executive's membership block -- membership is only asserted once a peer's
# VMS$VAXcluster VC reaches OPEN or a coordinator membership burst is
# received (peer_members==0 => the local node is never added, and SHOW
# CLUSTER honestly reports NOTMEMBER). This is arguably an incompleteness
# (real OpenVMS lets a satisfied-quorum solo node show itself as a one-node
# cluster), but changing scsd's membership-computation policy is a distinct
# design question from "does SCSD start at boot", is unrelated to vms-5ad's
# scope, and risks the authenticity invariants around what counts as a real
# member (INV-6). So ARM P asserts what is actually true end to end: the
# executive is REACHABLE (no %SYSTEM-W-NOSUCHDEV -- SCSD is running against
# a real /dev/vms-backed SHOW CLUSTER path) and reports the honest,
# ungrounded-membership NOTMEMBER a lone node genuinely has. A follow-up
# item for solo-quorum self-membership is recommended, not implemented here.
#
# Usage (run INSIDE the bootable image):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_scs_autostart.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for a boot to reach Username: (default 180).
#
# Exit 0 = every assertion passed. Exit 1 = a real failure (see the printed
# transcript). Exit 77 = honest skip: no /dev/kvm on this host. Every boot
# this test drives needs a real vms.ko executive behind /dev/vms inside the
# guest -- KVM is what keeps that boot fast and reliable (TCG flakes on the
# heavy legs, per this project's own operating notes); without it this test
# cannot produce a trustworthy signal, so it skips rather than report a
# false result (the same honest-absence contract test_syssvc_*.c's
# on-host /dev/vms check follows, adapted to a host-driven console test that
# never touches /dev/vms itself -- /dev/vms lives inside the guest here).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
SCS_NEGCTL_IMG=/boot/ovmx-distrib-scs-negctl.img
ARCH=$(uname -m)

if [ "$ARCH" != "x86_64" ]; then
    echo "SKIP: test_scs_autostart.sh only drives the x86_64 KVM leg today (ARCH=$ARCH)"
    exit 77
fi
QEMU=qemu-system-x86_64
CONSOLE="console=ttyS0"

if [ ! -w /dev/kvm ]; then
    echo "SKIP: /dev/kvm not writable -- this test needs KVM acceleration for a"
    echo "      trustworthy multi-boot signal (TCG heavy legs flake); see this"
    echo "      script's header for the honest-skip rationale."
    exit 77
fi
MACHINE="-accel kvm -cpu host"

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG" "$SCS_NEGCTL_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== SCS boot-autostart e2e (vms-5ad, child of vms-110b) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD accel=kvm"

# Global console-driver state, reset per boot by qemu_launch.
QPID=""; LOG=""; FIFO=""

# qemu_launch DISK LOGPATH FIFOPATH -- start QEMU on DISK in the BACKGROUND,
# with a virtio-net NIC (VAXCLUSTER=2 needs a real, non-loopback Ethernet
# device for scsd's boot-cluster mode to bind -- see resolve_cluster_iface()
# in scsd.c). No peer is on the other end; nothing here depends on the wire
# actually reaching anywhere, only on a bindable ARPHRD_ETHER device
# existing (the same thing the executive's device table needs to name
# ETH0:).
qemu_launch() {
    local disk="$1"
    LOG="$2"; FIFO="$3"
    rm -f "$LOG" "$FIFO"
    mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + 120))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nodefaults -serial stdio \
        -netdev user,id=net0 \
        -device virtio-net-pci,netdev=net0,mac=52:54:00:5a:d0:01,romfile= \
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

login_system() {  # LABEL -- requires an already-launched boot; leaves a '$' DCL prompt
    if wait_for '%OVMX-I-EXEC' 60; then ok "$1: executive attached (real vms.ko)"; else bad "$1: executive never attached"; fi
    send ''   # wake OPA0: -- LOGINOUT waits for RETURN before Username:
    if wait_for 'Username:' "$BOOT_TIMEOUT"; then
        ok "$1: boot reaches the login prompt"
    else
        echo "=== FATAL: $1 boot never reached Username: ==="; cat "$LOG"; qemu_halt; exit 1
    fi
    local off; off=$(wc -c <"$LOG")
    send 'SYSTEM'
    wait_for 'Password:' 30 "$off" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 30 "$off"; then
        ok "$1: SYSTEM logs in (LOGINOUT.EXE activated)"
    else
        echo "=== FATAL: $1 SYSTEM login failed ==="; cat "$LOG"; qemu_halt; exit 1
    fi
    wait_for '$' 20 "$off"
}

# author_vaxcluster LABEL -- SYSGEN SET VAXCLUSTER 2 / WRITE CURRENT, assert
# %SYSGEN-I-WRITTEN, return the authoring transcript segment in A_SEG.
author_vaxcluster() {
    local lbl="$1"
    local off; off=$(wc -c <"$LOG")
    send 'SYSGEN'
    wait_for 'SYSGEN>' 20 "$off"
    send 'USE CURRENT'
    send 'SET VAXCLUSTER 2'
    send 'WRITE CURRENT'
    send 'EXIT'
    wait_for '%SYSGEN-I-WRITTEN' 30 "$off"
    A_SEG=$(segment_since "$off")
    if printf '%s\n' "$A_SEG" | grep -qF '%SYSGEN-I-WRITTEN'; then
        ok "$lbl: WRITE CURRENT minted a new OVMXVMSSYS.PAR version (VAXCLUSTER=2) on the persistent disk"
    else
        bad "$lbl: WRITE CURRENT never printed %SYSGEN-I-WRITTEN"
    fi
}

# =============================================================================
# ARM N -- VAXCLUSTER negative control: the shipped default (VAXCLUSTER=0,
# unauthored). SCSD must run, self-gate honestly, and create NO process.
# =============================================================================
DISK_N=/tmp/scs-autostart-n.img
rm -f "$DISK_N"; cp "$DISTRIB_IMG" "$DISK_N"

echo ""
echo "--- ARM N: shipped default VAXCLUSTER=0 (unauthored) -- SCSD must self-gate off ---"
qemu_launch "$DISK_N" /tmp/scs-n-boot.log /tmp/scs-n-boot.in
login_system "ARM N"

N_OFF=$(wc -c <"$LOG")
send 'TYPE SYS$MANAGER:SCSD.OUT'
wait_for '$' 20 "$N_OFF"
N_SEG=$(segment_since "$N_OFF")
if printf '%s\n' "$N_SEG" | grep -qF 'SCSD-I-STANDALONE, VAXCLUSTER=0'; then
    ok "ARM N: SCSD ran at boot and self-gated off honestly (SCSD-I-STANDALONE, VAXCLUSTER=0)"
else
    bad "ARM N: SCSD.OUT never showed SCSD-I-STANDALONE, VAXCLUSTER=0"
    printf '%s\n' "$N_SEG" | sed 's/^/    seen: /'
fi

N2_OFF=$(wc -c <"$LOG")
send 'SHOW SYSTEM'
wait_for '$' 20 "$N2_OFF"
N2_SEG=$(segment_since "$N2_OFF")
if printf '%s\n' "$N2_SEG" | grep -qE '^[0-9A-Fa-f]{8} SCS_SERVER'; then
    bad "ARM N: SHOW SYSTEM lists an SCS_SERVER process despite VAXCLUSTER=0"
else
    ok "ARM N: SHOW SYSTEM lists NO SCS_SERVER process (standalone node stayed standalone)"
fi

N3_OFF=$(wc -c <"$LOG")
send 'SHOW CLUSTER'
wait_for '$' 20 "$N3_OFF"
N3_SEG=$(segment_since "$N3_OFF")
if printf '%s\n' "$N3_SEG" | grep -qF '%SYSTEM-I-NOTMEMBER'; then
    ok "ARM N: SHOW CLUSTER honestly reports NOTMEMBER"
else
    bad "ARM N: SHOW CLUSTER did not report NOTMEMBER"
    printf '%s\n' "$N3_SEG" | sed 's/^/    seen: /'
fi
qemu_halt

# =============================================================================
# ARM P -- the positive proof. Author VAXCLUSTER=2, power-cycle, boot 2 must
# run SCSD persistently against a real Ethernet NIC.
# =============================================================================
DISK_P=/tmp/scs-autostart-p.img
rm -f "$DISK_P"; cp "$DISTRIB_IMG" "$DISK_P"

echo ""
echo "--- ARM P / BOOT 1: author VAXCLUSTER=2 ---"
qemu_launch "$DISK_P" /tmp/scs-p-b1.log /tmp/scs-p-b1.in
login_system "ARM P boot1"
author_vaxcluster "ARM P boot1"
qemu_halt

echo ""
echo "--- ARM P / BOOT 2: fresh boot of the SAME disk -- SCSD self-starts persistently ---"
qemu_launch "$DISK_P" /tmp/scs-p-b2.log /tmp/scs-p-b2.in
login_system "ARM P boot2"

P_OFF=$(wc -c <"$LOG")
send 'SHOW SYSTEM'
wait_for '$' 20 "$P_OFF"
P_SEG=$(segment_since "$P_OFF")
SCS_ROW=$(printf '%s\n' "$P_SEG" | grep -E '^[0-9A-Fa-f]{8} SCS_SERVER' | head -1)
if [ -n "$SCS_ROW" ]; then
    ok "ARM P boot2: SHOW SYSTEM lists a real SCS_SERVER detached process ($SCS_ROW)"
else
    bad "ARM P boot2: SHOW SYSTEM never listed an SCS_SERVER process"
    printf '%s\n' "$P_SEG" | sed 's/^/    seen: /'
fi

P2_OFF=$(wc -c <"$LOG")
send 'TYPE SYS$MANAGER:SCSD.OUT'
wait_for '$' 20 "$P2_OFF"
P2_SEG=$(segment_since "$P2_OFF")
if printf '%s\n' "$P2_SEG" | grep -qE 'SCSD-I-BOOTCLUSTER, VAXCLUSTER=2: starting cluster connection manager on [^ ]+, SCSNODE='; then
    ok "ARM P boot2: SCSD.OUT shows SCSD-I-BOOTCLUSTER (self-configured from VAXCLUSTER=2)"
else
    bad "ARM P boot2: SCSD.OUT never showed SCSD-I-BOOTCLUSTER"
    printf '%s\n' "$P2_SEG" | sed 's/^/    seen: /'
fi
if printf '%s\n' "$P2_SEG" | grep -qE 'SCSD-I-LISTEN, raw socket bound to .br0.'; then
    bad "ARM P boot2: SCSD bound the lab default 'br0' instead of resolving a real NIC"
else
    ok "ARM P boot2: SCSD did NOT bind the lab default 'br0' (resolved a real interface instead)"
fi
if printf '%s\n' "$P2_SEG" | grep -qF 'SCSD-E-NONIC'; then
    bad "ARM P boot2: SCSD reported SCSD-E-NONIC -- no NIC was found to bind"
fi

# The single-node membership limitation, documented at the top of this file:
# the executive must be REACHABLE (no NOSUCHDEV -- SCSD is genuinely running
# against a real /dev/vms), and the honest answer for a peerless node is
# NOTMEMBER, not a fabricated self-membership.
P3_OFF=$(wc -c <"$LOG")
send 'SHOW CLUSTER'
wait_for '$' 20 "$P3_OFF"
P3_SEG=$(segment_since "$P3_OFF")
if printf '%s\n' "$P3_SEG" | grep -qF 'NOSUCHDEV'; then
    bad "ARM P boot2: SHOW CLUSTER reported NOSUCHDEV -- the executive membership path is unreachable"
elif printf '%s\n' "$P3_SEG" | grep -qF '%SYSTEM-I-NOTMEMBER'; then
    ok "ARM P boot2: SHOW CLUSTER reaches the executive (no NOSUCHDEV) and honestly reports NOTMEMBER (no peer -- see this file's header note)"
else
    bad "ARM P boot2: SHOW CLUSTER neither NOSUCHDEV nor NOTMEMBER"
    printf '%s\n' "$P3_SEG" | sed 's/^/    seen: /'
fi

# SCS_SERVER must still be alive after all the console traffic above --
# --connect runs with duration=0 (until SIGINT/SIGTERM), so it must not have
# exited on its own.
P4_OFF=$(wc -c <"$LOG")
send 'SHOW SYSTEM'
wait_for '$' 20 "$P4_OFF"
P4_SEG=$(segment_since "$P4_OFF")
if printf '%s\n' "$P4_SEG" | grep -qE '^[0-9A-Fa-f]{8} SCS_SERVER'; then
    ok "ARM P boot2: SCS_SERVER is still running (persistent --connect mode, duration=0)"
else
    bad "ARM P boot2: SCS_SERVER is no longer listed -- it exited when it should run persistently"
fi
qemu_halt

# =============================================================================
# ARM R -- registration negative control. Same authored VAXCLUSTER=2, but on
# the image with SCS_STARTUP.COM's CONFIG registration removed. SCSD must
# never be created, proving the REGISTRATION (not the in-daemon self-gate)
# is what actually creates it.
# =============================================================================
DISK_R=/tmp/scs-autostart-r.img
rm -f "$DISK_R"; cp "$SCS_NEGCTL_IMG" "$DISK_R"

echo ""
echo "--- ARM R / BOOT 1: author VAXCLUSTER=2 on the REGISTRATION-REMOVED image ---"
qemu_launch "$DISK_R" /tmp/scs-r-b1.log /tmp/scs-r-b1.in
login_system "ARM R boot1"
author_vaxcluster "ARM R boot1"
qemu_halt

echo ""
echo "--- ARM R / BOOT 2: fresh boot, VAXCLUSTER=2 authored, registration STILL absent ---"
qemu_launch "$DISK_R" /tmp/scs-r-b2.log /tmp/scs-r-b2.in
login_system "ARM R boot2"

R_OFF=$(wc -c <"$LOG")
send 'SHOW SYSTEM'
wait_for '$' 20 "$R_OFF"
R_SEG=$(segment_since "$R_OFF")
if printf '%s\n' "$R_SEG" | grep -qE '^[0-9A-Fa-f]{8} SCS_SERVER'; then
    bad "ARM R boot2: SHOW SYSTEM lists SCS_SERVER despite the CONFIG registration being removed"
else
    ok "ARM R boot2: SHOW SYSTEM lists NO SCS_SERVER -- the registration removal held even with VAXCLUSTER=2 authored"
fi
# JOB_CONTROL's own registration is untouched on this image, so exactly ONE
# %RUN-S-PROC_ID (JOB_CONTROL's) is expected for the whole boot -- a SECOND
# one would mean something (SCSD or otherwise) was RUN/DETACHED that should
# not have been.
FULL_LOG=$(tr -d '\r' <"$LOG")
PROC_COUNT=$(printf '%s' "$FULL_LOG" | grep -cF '%RUN-S-PROC_ID')
if [ "$PROC_COUNT" -eq 1 ]; then
    ok "ARM R boot2: exactly one %RUN-S-PROC_ID for the whole boot (JOB_CONTROL only -- SCSD was never RUN/DETACHED)"
else
    bad "ARM R boot2: saw $PROC_COUNT %RUN-S-PROC_ID lines, expected exactly 1 (JOB_CONTROL only)"
fi
qemu_halt

# --- Results ---
echo ""
echo "=========================================="
echo "  RESULTS: $PASS passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL SCS BOOT-AUTOSTART CHECKS PASSED (ARM N + ARM P + ARM R, vms-5ad)"
    exit 0
else
    echo "  SCS BOOT-AUTOSTART CHECKS FAILED"
    exit 1
fi
