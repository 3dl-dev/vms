#!/bin/bash
# test_decnet_startnet_boot_e2e.sh - a booted OVMX becomes a LIVE DECnet Phase IV
# endnode the VMS way, and NETACP is proven RUNNING with its datalink OPEN
# (rd vms-1f69). Not "STARTNET printed its message" -- the process and the wire.
#
# Boot 1 (SYSTEM; DECnet unconfigured at boot): NCP SET EXECUTOR ADDRESS 1.42 / NAME OVMX / STATE ON
#   persists SYS$SYSTEM:NETNODE_LOCAL.DAT through RMS over the Files-11 ACP;
#   NCP SHOW EXECUTOR reads it back. Settle for guest writeback, power off.
# Both boots carry a virtio NIC whose guest->wire frames QEMU dumps to a pcap
#   (-object filter-dump), so boot 1 is the wire's negative control.
# Boot 2 (same disk): the LPBETA startup phase runs
#   @SYS$MANAGER:STARTNET exactly as the system does; its F$SEARCH gate finds
#   NETNODE_LOCAL.DAT and it starts NETACP (SYS$SYSTEM:DECNETD.EXE) detached.
#   Asserted:
#     (a) the console carries %DECNET-I-STARTNET and NOT %DECNET-E-STARTNET /
#         %RUN-F-CREPRC (the launch really happened -- STARTNET checks $STATUS);
#     (b) SHOW SYSTEM, run well after boot, lists the NETACP process: it is
#         RUNNING. NETACP exits on ANY datalink failure (no NIC, no PHY_IO,
#         wrong interface), so a live NETACP is a NETACP whose datalink opened;
#     (c) THE WIRE: the pcap holds Phase IV endnode-hello frames -- destination
#         AB-00-00-03-00-00 (all routers), SOURCE AA-00-04-00-2A-04 (the
#         algorithmic station of 1.42, stamped by the executive), ethertype
#         0x6003 -- sent through the executive's L2 datalink by a process with
#         no CAP_NET_RAW. Negative control: the boot-1 pcap (no DECnet
#         configured at boot) holds NO such frame.
#
# Reboot mechanics + writeback settle: identical to test_tcpip_reboot_e2e.sh
# (no DCL REBOOT under -no-reboot; a reboot is a fresh QEMU on the same disk).
# Runs inside the shipped bootable image (ovmx-boot); the runner
# run_decnet_startnet_boot_e2e.sh docker-runs it. Exit 0 = all checks pass.
set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"
SETTLE_SECS="${SETTLE_SECS:-60}"
NETACP_SETTLE="${NETACP_SETTLE:-45}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx-slim.cpio.gz          # the shipped slim initramfs
DISTRIB_IMG=/boot/ovmx-distrib.img
[ -f "$INITRD" ] || INITRD=/boot/initramfs-ovmx.cpio.gz
ARCH=$(uname -m)

[ -f "$DISTRIB_IMG" ] || { echo "FATAL: $DISTRIB_IMG missing - mastering did not run"; exit 1; }
[ -f "$INITRD" ] || { echo "FATAL: no initramfs under /boot"; ls /boot; exit 1; }

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64;   MACHINE="";                              CONSOLE="console=ttyS0"
fi
ACCEL=""
[ -w /dev/kvm ] && ACCEL="-enable-kvm"

PASS=0; FAIL=0
record() { if [ "$2" -eq 0 ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
waitfor() { local pat="$1" lim="${2:-60}" log="$3" w=0
    while [ $w -lt $((lim*4)) ]; do
        grep -qaF -- "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w+1))
    done; return 1; }
# count frames whose first 14 bytes are dst|src|ethertype = $1 (hex, no spaces)
frames_with() { [ -s "$2" ] || { echo 0; return; }
    od -An -v -tx1 "$2" | tr -d ' \n' | grep -o "$1" | wc -l; }

DISK=/tmp/dn-startnet-e2e.img
LOG1=/tmp/dn-startnet-boot1.log
LOG2=/tmp/dn-startnet-boot2.log
PCAP1=/tmp/dn-startnet-boot1.pcap
PCAP2=/tmp/dn-startnet-boot2.pcap
FIFO=/tmp/dn-startnet.in
HELLO_142="ab0000030000aa0004002a046003"   # dst AB-00-00-03-00-00, src AA-00-04-00-2A-04, 0x6003
rm -f "$DISK" "$LOG1" "$LOG2" "$PCAP1" "$PCAP2" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

echo "=== DECnet: NCP configures, boot-time STARTNET starts NETACP, hellos on the wire (vms-1f69) ==="
echo "arch=$ARCH qemu=$QEMU"

send() { printf '%s\r' "$1" >&4; }
wake_login() { local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 150 ]; do send ''; sleep 1; w=$((w+1)); done; }
login_system() { local logf="$1"
    send 'SYSTEM'; sleep 1; send 'MANAGER'; sleep 1
    waitfor 'Welcome to OpenVMX' 45 "$logf"; }

# --- Boot 1: configure the executor (NIC present, DECnet NOT yet configured) ---
# shellcheck disable=SC2086
timeout -k 15 $((BOOT_TIMEOUT + SETTLE_SECS + 120)) $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nodefaults -serial stdio \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= \
    -object filter-dump,id=fd0,netdev=n0,file="$PCAP1" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG1" 2>&1 &
qp=$!
exec 4>"$FIFO"

wake_login "$LOG1"
if waitfor 'Username:' "$BOOT_TIMEOUT" "$LOG1"; then rc=0; else rc=1; fi
record "boot 1: reaches the login prompt" "$rc"
if [ "$rc" -eq 0 ] && login_system "$LOG1"; then
    record "boot 1: SYSTEM logs in" 0
    send 'NCP :== $SYS$SYSTEM:NCP.EXE'; sleep 1
    send 'NCP SET EXECUTOR ADDRESS 1.42'; sleep 3
    send 'NCP SET EXECUTOR NAME OVMX'; sleep 3
    send 'NCP SET EXECUTOR STATE ON'; sleep 3
    send 'NCP SHOW EXECUTOR'
    if waitfor 'Executor node = 1.42 (OVMX)' 30 "$LOG1"; then rc=0; else rc=1; fi
    record "boot 1: NCP SET EXECUTOR persisted 1.42 (OVMX) -- read back by a fresh NCP image" "$rc"
    if grep -qaF '%NCP-E-' "$LOG1"; then rc=1; else rc=0; fi
    record "boot 1: no %NCP-E- error (written through RMS over the ACP)" "$rc"
    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
else
    record "boot 1: SYSTEM logs in" 1
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

n1=$(frames_with "$HELLO_142" "$PCAP1")
if [ "$n1" -eq 0 ]; then rc=0; else rc=1; fi
record "NEGCTL boot 1 (DECnet unconfigured at boot): NO 1.42 endnode hello on the wire (count=$n1) -- the wire check can tell" "$rc"

# --- Boot 2: STARTNET at LPBETA starts NETACP; prove it runs + talks -----------
rm -f "$FIFO"; mkfifo "$FIFO"
# shellcheck disable=SC2086
timeout -k 15 $((BOOT_TIMEOUT + NETACP_SETTLE + 420)) $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nodefaults -serial stdio \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= \
    -object filter-dump,id=fd0,netdev=n0,file="$PCAP2" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG2" 2>&1 &
qp=$!
exec 4>"$FIFO"

wake_login "$LOG2"
if waitfor 'Username:' "$BOOT_TIMEOUT" "$LOG2"; then rc=0; else rc=1; fi
record "boot 2: reaches the login prompt" "$rc"
boot2_up=$rc
if grep -qaF '%DECNET-I-STARTNET' "$LOG2"; then rc=0; else rc=1; fi
record "boot 2: boot-time STARTNET found NETNODE_LOCAL.DAT and launched NETACP (%DECNET-I-STARTNET)" "$rc"
if grep -qaE '%DECNET-E-STARTNET|%RUN-F-CREPRC' "$LOG2"; then rc=1; else rc=0; fi
record "boot 2: no refused launch (%DECNET-E-STARTNET / %RUN-F-CREPRC absent)" "$rc"

if [ "$boot2_up" -eq 0 ] && login_system "$LOG2"; then
    record "boot 2: SYSTEM logs in" 0
    echo "  (letting NETACP run ${NETACP_SETTLE}s before looking)"
    sleep "$NETACP_SETTLE"
    off=$(wc -c <"$LOG2")
    send 'SHOW SYSTEM'; sleep 4
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep -qE '(^|[[:space:]])NETACP([[:space:]]|$)'; then rc=0; else rc=1; fi
    record "boot 2: SHOW SYSTEM lists a RUNNING NETACP ${NETACP_SETTLE}s after boot (it exits on any datalink failure)" "$rc"
    if printf '%s\n' "$seg" | grep -qF 'Process Name'; then rc=0; else rc=1; fi
    record "NEGCTL boot 2: the SHOW SYSTEM segment is real output (its 'Process Name' column header is present)" "$rc"

    # rd vms-dda: an outbound link from a user process, brokered through the
    # RUNNING boot-time NETACP over $ASSIGN _NET: + $QIO (the DCL battery VM has
    # no NIC, hence no _NET:; this boot has both). COPY 0"SYSTEM MANAGER":: over
    # NETACP's local loopback to a FAL.EXE server process and back.
    off=$(wc -c <"$LOG2")
    send 'DNETACC :== $SYS$SYSTEM:DECNETD.EXE'; sleep 1
    send 'DNETACC --net-loopback-accept-test'
    waitfor 'DECNETD-NET-LOOPBACK-ACCEPT:' 240 "$LOG2" || true
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep -qF 'DECNETD-NET-LOOPBACK-ACCEPT: PASS'; then rc=0; else rc=1; fi
    record "boot 2: a \$QIO _NET: link is brokered through the RUNNING NETACP -- COPY 0:: to FAL.EXE and back, byte-verified (vms-dda)" "$rc"
    if printf '%s\n' "$seg" | grep -qF 'completes IO$_ACCESS SS$_INVLOGIN'; then rc=0; else rc=1; fi
    record "boot 2: a bad password is refused at IO\$_ACCESS (INVLOGIN) through NETACP" "$rc"
    if printf '%s\n' "$seg" | grep -qF 'DECNETD-I-NETLOOP'; then rc=0; else rc=1; fi
    record "NEGCTL boot 2: the brokered-link segment is real output (the test's banner is present)" "$rc"
else
    record "boot 2: SYSTEM logs in" 1
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

n2=$(frames_with "$HELLO_142" "$PCAP2")
if [ "$n2" -ge 1 ]; then rc=0; else rc=1; fi
record "boot 2: the wire carries Phase IV endnode hellos to AB-00-00-03-00-00 SOURCED FROM AA-00-04-00-2A-04, ethertype 0x6003 (count=$n2)" "$rc"

if [ "$FAIL" -ne 0 ]; then
    echo "--- boot 1 log ---"; cat "$LOG1"
    echo "--- boot 2 log ---"; cat "$LOG2"
    for p in "$PCAP1" "$PCAP2"; do
        echo "--- $p: $(stat -c %s "$p" 2>/dev/null || echo 0) bytes; 0x6003 frames: $(frames_with '6003' "$p") ---"
    done
fi

echo ""
echo "  RESULTS: $PASS/$((PASS+FAIL)) checks passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "  BOOTED OVMX IS A LIVE DECnet ENDNODE: NETACP RUNNING, HELLOS FROM AA-00-04-00-2A-04 ON THE WIRE"
    exit 0
fi
exit 1
