#!/bin/bash
# test_cluster_param_adoption.sh - vms-495 (epic vms-098 R1.3): a REBOOTED OVMX
# executive ADOPTS operator-authored cluster identity/quorum params off the
# PERSISTENT system disk -- adoption-on-reboot, not merely file persistence.
#
# WHAT THIS PROVES, AND WHY THE EXISTING GATES DO NOT.
#
# tests/qemu/test_cluster_params_recnx_e2e.sh proves the AUTHOR -> PERSIST ->
# ADOPT round trip WITHIN A SINGLE BOOT: SYSGEN WRITE CURRENT mints a real vmsfs
# version and a fresh SCSD in the SAME running system reads it back. It never
# power-cycles the node, so it cannot prove the authored value SURVIVES a full
# reboot of the persistent disk and is re-adopted by a fresh executive that did
# NO authoring of its own. tests/qemu/test_sysboot_cluster_params_e2e.sh authors
# at the SYSBOOT> prompt and boots ONCE with those params; its bracket is a
# FRESH disk, not the SAME disk rebooted. This gate closes exactly that gap, the
# R1.3 done-condition (vms-495):
#
#   BOOT 1 (author): boot the shipped image on a PERSISTENT disk, log in, and
#     author the cluster identity+quorum set the VMS way via SYSGEN.EXE --
#         USE CURRENT
#         SET SCSNODE NODEB        (string cluster node name)
#         SET SCSSYSTEMID 1026     (numeric cluster system id, lab 1025..1027)
#         SET VOTES 2              (quorum votes this node contributes; !=default 1)
#         SET EXPECTED_VOTES 2     (expected total cluster votes; !=default 1)
#         SET ALLOCLASS 7
#         WRITE CURRENT            (mints a REAL new vmsfs version on the DISK)
#         EXIT
#     then HALT this boot. cache=writethrough means the WRITE CURRENT block I/O
#     is on the backing disk file the instant %SYSGEN-I-WRITTEN prints, so the
#     authored params outlive this QEMU process.
#
#   BOOT 2 (adopt): relaunch QEMU on the SAME disk file, NO re-authoring, NO
#     re-copy. A fresh executive comes up; a fresh SCSD --show-identity (a pure
#     read of the persisted store through resolve_node_identity()/
#     resolve_scssystemid() -> sysgen_read_param(), no socket, INV-6: no
#     per-process fake) MUST report SCSNODE=NODEB SCSSYSTEMID=1026 ALLOCLASS=7 --
#     the identity the operator authored on the PREVIOUS boot. VOTES/
#     EXPECTED_VOTES read back through an independent SYSGEN USE CURRENT / SHOW
#     (scsd deliberately does NOT surface VOTES -- the live-VC quorum recompute
#     from authored VOTES is the OPERATOR-RESERVED R1 split vms-41d; R1.3 is
#     READ-BACK ONLY, so the authored quorum values are proven adopted via the
#     console SHOW, not via a live quorum computation).
#
# CONTROL-DISK BRACKET. A SECOND persistent disk is authored NODEC/1027/VOTES=3/
# EXPECTED_VOTES=3/ALLOCLASS=8 on its own BOOT 1 and rebooted the same way. Its
# BOOT 2 adopts NODEC/1027/3/3/8 -- a DIFFERENT identity from disk A's. Two disks
# that each boot with their OWN authored values prove BOOT 2 read the identity
# off THE DISK, not from a canned NODEB baked into the test or the image.
#
# The paired MEASURED negative control is test_cluster_param_adoption_negctl.sh:
# a persistent disk that is booted, authors NOTHING, and is rebooted MUST show
# the factory-seeded defaults (SCSNODE=OVMX SCSSYSTEMID=0), NEVER NODEB/1026 --
# so this positive cannot pass for the wrong reason (a hardcoded/mocked NODEB).
#
# WHAT WOULD MAKE THIS FAIL HONESTLY (the defects it guards): boot never reaches
# Username:; SYSGEN.EXE/SCSD.EXE missing; WRITE CURRENT does not mint a new
# version; the authored store does not survive the power cycle; BOOT 2's SCSD
# reports anything other than the authored identity; disk A and disk B report
# the SAME identity (the value did not come from the disk).
#
# Usage (run INSIDE the bootable image, like test_cluster_params_recnx_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_cluster_param_adoption.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for a boot to reach Username: (default 180).
#
# Exit 0 = every assertion passed against the real rebooted volumes.
# Exit 1 = a real failure (see the printed transcript).

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

echo "=== cluster-param ADOPTION-ON-REBOOT e2e (vms-495 R1.3): author on boot 1, power-cycle, adopt on boot 2 ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

# Global console-driver state, reset per boot by qemu_launch.
QPID=""; LOG=""; FIFO=""

# qemu_launch DISK LOGPATH FIFOPATH -- start QEMU on DISK in the BACKGROUND
# writing the console to LOGPATH, and open FD 4 on FIFOPATH for input. NO
# command substitution (the $(run_qemu) shape wedges an interactive boot -- see
# test_boot_scsnode_hostname_e2e.sh); the caller drives via send()/wait_for()
# against $LOG and ends the boot with qemu_halt.
qemu_launch() {
    local disk="$1"
    LOG="$2"; FIFO="$3"
    rm -f "$LOG" "$FIFO"
    mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + 120))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nic none -nodefaults -serial stdio \
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

login_system() {  # requires an already-launched boot; leaves a '$' DCL prompt
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

# author_identity NODE SYSID VOTES EXPVOTES ALLOC LABEL -- SYSGEN SET ... /
# WRITE CURRENT the identity+quorum set, assert %SYSGEN-I-WRITTEN, return the
# authoring transcript segment in the global A_SEG.
author_identity() {
    local node="$1" sysid="$2" votes="$3" exp="$4" alloc="$5" lbl="$6"
    local off; off=$(wc -c <"$LOG")
    send 'SYSGEN'
    wait_for 'SYSGEN>' 20 "$off"
    send 'USE CURRENT'
    send "SET SCSNODE $node"
    send "SET SCSSYSTEMID $sysid"
    send "SET VOTES $votes"
    send "SET EXPECTED_VOTES $exp"
    send "SET ALLOCLASS $alloc"
    send 'WRITE CURRENT'
    send 'EXIT'
    wait_for '%SYSGEN-I-WRITTEN' 30 "$off"
    A_SEG=$(segment_since "$off")
    if printf '%s\n' "$A_SEG" | grep -qF '%SYSGEN-I-WRITTEN'; then
        ok "$lbl: WRITE CURRENT minted a new OVMXVMSSYS.PAR version on the persistent disk"
    else
        bad "$lbl: WRITE CURRENT never printed %SYSGEN-I-WRITTEN"
    fi
}

# =============================================================================
# DISK A -- the primary proof.  Author NODEB/1026/VOTES=2/EXP=2/ALLOC=7,
# power-cycle, adopt.
# =============================================================================
DISK_A=/tmp/adopt-a.img
rm -f "$DISK_A"; cp "$DISTRIB_IMG" "$DISK_A"

echo ""
echo "--- DISK A / BOOT 1: author NODEB/1026 VOTES=2 EXPECTED_VOTES=2 ALLOCLASS=7 ---"
qemu_launch "$DISK_A" /tmp/adopt-a-b1.log /tmp/adopt-a-b1.in
login_system "A boot1"
author_identity NODEB 1026 2 2 7 "A boot1"
qemu_halt

echo ""
echo "--- DISK A / BOOT 2: fresh boot of the SAME disk -- executive adopts the authored identity ---"
qemu_launch "$DISK_A" /tmp/adopt-a-b2.log /tmp/adopt-a-b2.in
login_system "A boot2"

S_OFF=$(wc -c <"$LOG")
send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'
send 'SCSD --show-identity'
wait_for 'SCSD-I-IDENT' 20 "$S_OFF"
SA_SEG=$(segment_since "$S_OFF")
if printf '%s\n' "$SA_SEG" | grep -qF 'SCSD-I-IDENT, SCSNODE=NODEB SCSSYSTEMID=1026 ALLOCLASS=7'; then
    ok "A boot2: rebooted executive ADOPTED authored identity SCSNODE=NODEB SCSSYSTEMID=1026 ALLOCLASS=7"
else
    bad "A boot2: rebooted executive ADOPTED authored identity SCSNODE=NODEB SCSSYSTEMID=1026 ALLOCLASS=7"
    printf '%s\n' "$SA_SEG" | grep -F 'SCSD-I-IDENT' | sed 's/^/    seen: /'
fi

Q_OFF=$(wc -c <"$LOG")
send 'SYSGEN'
wait_for 'SYSGEN>' 20 "$Q_OFF"
send 'USE CURRENT'
send 'SHOW VOTES'
send 'SHOW EXPECTED_VOTES'
send 'EXIT'
wait_for '%SYSGEN-I-LOADED' 20 "$Q_OFF"
QA_SEG=$(segment_since "$Q_OFF")
if printf '%s\n' "$QA_SEG" | grep -qE '^ +VOTES +2 +1 +0 +32767'; then
    ok "A boot2: SYSGEN USE CURRENT reads authored VOTES=2 back off the rebooted volume"
else
    bad "A boot2: SYSGEN USE CURRENT reads authored VOTES=2 back off the rebooted volume"
    printf '%s\n' "$QA_SEG" | grep -E 'VOTES' | sed 's/^/    seen: /'
fi
if printf '%s\n' "$QA_SEG" | grep -qE '^ +EXPECTED_VOTES +2 +1 +1 +32767'; then
    ok "A boot2: SYSGEN USE CURRENT reads authored EXPECTED_VOTES=2 back off the rebooted volume"
else
    bad "A boot2: SYSGEN USE CURRENT reads authored EXPECTED_VOTES=2 back off the rebooted volume"
    printf '%s\n' "$QA_SEG" | grep -E 'EXPECTED_VOTES' | sed 's/^/    seen: /'
fi
qemu_halt

# =============================================================================
# DISK B -- the control bracket.  A DIFFERENT authored identity proves boot 2
# reads the value off THE DISK, not a canned NODEB.
# =============================================================================
DISK_B=/tmp/adopt-b.img
rm -f "$DISK_B"; cp "$DISTRIB_IMG" "$DISK_B"

echo ""
echo "--- DISK B / BOOT 1 (control): author NODEC/1027 VOTES=3 EXPECTED_VOTES=3 ALLOCLASS=8 ---"
qemu_launch "$DISK_B" /tmp/adopt-b-b1.log /tmp/adopt-b-b1.in
login_system "B boot1"
author_identity NODEC 1027 3 3 8 "B boot1"
qemu_halt

echo ""
echo "--- DISK B / BOOT 2 (control): fresh boot of the SAME disk -- adopts NODEC/1027, NOT disk A's NODEB ---"
qemu_launch "$DISK_B" /tmp/adopt-b-b2.log /tmp/adopt-b-b2.in
login_system "B boot2"
S_OFF=$(wc -c <"$LOG")
send 'SCSD :== $SYS$SYSTEM:SCSD.EXE'
send 'SCSD --show-identity'
wait_for 'SCSD-I-IDENT' 20 "$S_OFF"
SB_SEG=$(segment_since "$S_OFF")
if printf '%s\n' "$SB_SEG" | grep -qF 'SCSD-I-IDENT, SCSNODE=NODEC SCSSYSTEMID=1027 ALLOCLASS=8'; then
    ok "B boot2: control disk adopted ITS OWN authored identity SCSNODE=NODEC SCSSYSTEMID=1027 ALLOCLASS=8"
else
    bad "B boot2: control disk adopted ITS OWN authored identity SCSNODE=NODEC SCSSYSTEMID=1027 ALLOCLASS=8"
    printf '%s\n' "$SB_SEG" | grep -F 'SCSD-I-IDENT' | sed 's/^/    seen: /'
fi
# The bracket assertion: disk B must NOT report disk A's identity.
if printf '%s\n' "$SB_SEG" | grep -qF 'SCSNODE=NODEB'; then
    bad "B boot2: control disk wrongly reported disk A's NODEB (identity is not coming from the disk)"
else
    ok "B boot2: control disk does NOT report disk A's NODEB (each disk carries its own authored identity)"
fi
qemu_halt

echo ""
echo "=== transcript: DISK A boot1 authoring ==="
printf '%s\n' "$A_SEG" | grep -E '%SYSGEN|SCSNODE|SCSSYSTEMID|VOTES|ALLOCLASS' | sed 's/^/  /' | head -20
echo "=== transcript: DISK A boot2 SCSD --show-identity (adoption) ==="
printf '%s\n' "$SA_SEG" | grep -E 'SCSD-I-IDENT|SCSD-W' | sed 's/^/  /'
echo "=== transcript: DISK B boot2 SCSD --show-identity (control) ==="
printf '%s\n' "$SB_SEG" | grep -E 'SCSD-I-IDENT|SCSD-W' | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL CLUSTER-PARAM ADOPTION-ON-REBOOT CHECKS PASSED -- REBOOTED EXECUTIVE ADOPTS AUTHORED IDENTITY/QUORUM (vms-495 R1.3)"
    exit 0
fi
exit 1
