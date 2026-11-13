#!/bin/bash
# test_decnet_startnet_boot_e2e.sh - a booted OVMX becomes a LIVE DECnet Phase IV
# endnode the VMS way, and NETACP is proven RUNNING with its datalink OPEN
# (rd vms-1f69). Not "STARTNET printed its message" -- the process and the wire.
#
# Boot 1 (SYSTEM; DECnet unconfigured at boot): NCP SET EXECUTOR ADDRESS 1.42 / NAME OVMX and
#   NCP DEFINE EXECUTOR STATE ON (the permanent database: the network is not
#   turned on yet) persist SYS$SYSTEM:NETNODE_LOCAL.DAT through RMS over the Files-11 ACP;
#   NCP DEFINE NODE 1.1 NAME VAX1 adds a remote node; NCP LIST EXECUTOR reads
#   the permanent database back. With NO NETACP yet, NCP SHOW EXECUTOR fails
#   %NCP-F-OPEFAI and SHOW NETWORK has no DECNET line (rd vms-30e: SHOW reads
#   the RUNNING network). Then (g) THE STARTNET AUDIT (rd vms-0a7 S3): SYSTEM
#   MOUNTs the scratch volume and runs STARTNETAUDIT.EXE (tests/qemu/
#   startnet_audit.c), which ptraces DCL @SYS$MANAGER:STARTNET -> NCP SET
#   EXECUTOR STATE ON -> the NETACP NCP creates -> the FAL.EXE NETACP creates
#   for an inbound COPY 0:: (driven by NETACP_TEST.EXE off the same volume),
#   asserts 0 pipe, 0 poll, 0 thread, 0 host socket, no exec of NETACP.EXE,
#   and stops that NETACP. The wire's negative control is the boot-1 pcap UP TO
#   the audit; after it, the hellos of the NETACP NCP started are on the wire.
#   Settle for guest writeback, power off.
# Both boots carry a virtio NIC whose guest->wire frames QEMU dumps to a pcap
#   (-object filter-dump), so boot 1 is the wire's negative control.
# Boot 2 (same disk; its timeout carries 300s more for (f)'s audit): the LPBETA startup phase runs
#   @SYS$MANAGER:STARTNET exactly as the system does; its F$SEARCH gate finds
#   NETNODE_LOCAL.DAT and NCP SET EXECUTOR STATE ON creates NETACP
#   (SYS$SYSTEM:NETACP.EXE) detached.
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
#         configured at boot) holds NO such frame;
#     (d) NCP SHOW + SHOW NETWORK READ THAT RUNNING NETACP (rd vms-30e): SHOW
#         EXECUTOR / CHARACTERISTICS / COUNTERS / KNOWN NODES / KNOWN LINKS print
#         NETACP's own values in the real OpenVMS VAX V7.3 layout
#         (docs/oracle/vax-ncp-show/), and SHOW NETWORK carries the DECNET
#         product line -- which boot 1, with no NETACP, did not;
#     (e) NCP SHOW KNOWN CIRCUITS / KNOWN LINES / KNOWN OBJECTS read that NETACP
#         too (rd vms-2d0): its circuit and line ETH-0 (device-native: the
#         executive's ETH0:), state on; its volatile object database -- FAL 17
#         FAL.EXE byte-identical to the real VAX row, CTERM 42 served in place
#         by NETACP's own PID (the PID SHOW SYSTEM lists for NETACP). Boot 1,
#         with no NETACP, fails SHOW KNOWN OBJECTS %NCP-F-OPEFAI while LIST
#         KNOWN OBJECTS reads the permanent file.
#
#     (f) THE COMMANDS STAY OFF LINUX LIBC (rd vms-0a7): boot 2 also carries a
#         scratch ODS-2 volume (VDA100:, label DCLAUDIT) mastered here with
#         vmsfs_master from the runner image's DCLNETAUDIT.EXE
#         (tests/qemu/test_syssvc_dcl_remote_syscall_audit.c, built static by
#         distro/Dockerfile.bootable as test tooling -- the shipped disk carries
#         no audit tool). SYSTEM MOUNTs it and runs it as a foreign command; it
#         ptraces the shipped DCL.EXE running DIRECTORY / TYPE / COPY (both
#         directions) of 0"SYSTEM MANAGER":: against THIS NETACP and its
#         FAL.EXE, and asserts 0 pipe, 0 poll-family wait, 0 thread, 0 host
#         socket, no exec of NETACP.EXE (or of anything but that DCL), while
#         the trace sees each command's /dev/vms calls and its real output (the
#         listing's 'Total of', the TYPE record, each COPY read back).
#         WHY HERE, AS A MOUNTED-VOLUME IMAGE: this is the one place a NETACP
#         and a FAL both run, and running an image off a mounted volume is how
#         a VMS user brings a tool to a system without changing its system
#         disk. The kernel-executive harness runs the same binary with no
#         NETACP (honest refusal + the same audit); that is where its facility
#         negative control (defects.d/rms-remote-link-via-host-pipe) runs.
#
# Boot 3 (same disk, -nic none; rd vms-5ec): ETH0: is absent but _NET: exists,
#   and NETACP_TEST.EXE --fal-proc-accept-test does a FAL access over 0:: --
#   the device face is NIC-independent, only the circuit needs an adapter.
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
# send a command; wait (bounded) for $3 in its output; $seg = the output
# (only output AFTER the command is searched, so an earlier match cannot satisfy it)
run_seg() { local o w=0; o=$(wc -c <"$2"); send "$1"
    while [ $w -lt $(( ${4:-20} * 4 )) ]; do
        tail -c "+$((o + 1))" "$2" | grep >/dev/null -aF -- "$3" && break
        kill -0 "$qp" 2>/dev/null || break
        sleep 0.25; w=$((w+1))
    done
    sleep 2; seg=$(tail -c "+$((o + 1))" "$2" | tr -d '\r'); }
has() { printf '%s\n' "$seg" | grep >/dev/null -F -- "$1"; }
frames_with() { [ -s "$2" ] || { echo 0; return; }
    od -An -v -tx1 "$2" | tr -d ' \n' | grep -a -o "$1" | wc -l; }

DISK=/tmp/dn-startnet-e2e.img
AUDIT_SRC="${OVMX_DCLNETAUDIT:-/usr/local/lib/ovmx-test/DCLNETAUDIT.EXE}"
# rd vms-0a7 S3: the DECnet test image (every self-test the shipped NETACP.EXE
# does not carry) and the STARTNET audit, on the same scratch volume.
NETTEST_SRC="${OVMX_NETACP_TEST:-/usr/local/lib/ovmx-test/NETACP_TEST.EXE}"
SNAUDIT_SRC="${OVMX_STARTNETAUDIT:-/usr/local/lib/ovmx-test/STARTNETAUDIT.EXE}"
AUDIT_DISK=/tmp/dn-startnet-audit.img
AUDIT_TREE=/tmp/dn-startnet-audit-tree
LOG1=/tmp/dn-startnet-boot1.log
LOG2=/tmp/dn-startnet-boot2.log
LOG3=/tmp/dn-startnet-boot3.log
PCAP1=/tmp/dn-startnet-boot1.pcap
PCAP2=/tmp/dn-startnet-boot2.pcap
FIFO=/tmp/dn-startnet.in
HELLO_142="ab0000030000aa0004002a046003"   # dst AB-00-00-03-00-00, src AA-00-04-00-2A-04, 0x6003
rm -f "$DISK" "$LOG1" "$LOG2" "$LOG3" "$PCAP1" "$PCAP2" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"
rm -rf "$AUDIT_TREE" "$AUDIT_DISK"; mkdir -p "$AUDIT_TREE"
# STARTNETAUDIT's two DCL procedures ride the same volume: DCL opens a procedure
# through RMS + the Files-11 ACP only, never a host path (rd vms-0a7 S3). The
# traced one starts the network as the system does; the untraced client copies
# through 0:: as a user does.
printf '%s\n' '$ @SYS$MANAGER:STARTNET' '$ EXIT $STATUS' >"$AUDIT_TREE/SNAUDIT_SN.COM"
printf '%s\n' \
    '$ SET NOON' \
    '$ COPY SYS$MANAGER:STARTNET.COM 0"SYSTEM MANAGER"::SYS$SYSROOT:[SYSMGR]SNAUDIT_A.TMP' \
    '$ IF .NOT. $STATUS THEN WRITE SYS$OUTPUT "SNAUDIT-FAIL COPY-TO"' \
    '$ COPY 0"SYSTEM MANAGER"::SYS$SYSROOT:[SYSMGR]SNAUDIT_A.TMP SYS$SYSROOT:[SYSMGR]SNAUDIT_B.TMP' \
    '$ IF .NOT. $STATUS THEN WRITE SYS$OUTPUT "SNAUDIT-FAIL COPY-FROM"' \
    '$ TYPE SYS$SYSROOT:[SYSMGR]SNAUDIT_B.TMP' \
    '$ DELETE SYS$SYSROOT:[SYSMGR]SNAUDIT_A.TMP;*,SNAUDIT_B.TMP;*' \
    '$ WRITE SYS$OUTPUT "SNAUDIT-CLIENT-END"' >"$AUDIT_TREE/SNAUDIT_CL.COM"
if [ -f "$AUDIT_SRC" ] && cp "$AUDIT_SRC" "$AUDIT_TREE/DCLNETAUDIT.EXE" \
   && [ -f "$NETTEST_SRC" ] && cp "$NETTEST_SRC" "$AUDIT_TREE/NETACP_TEST.EXE" \
   && [ -f "$SNAUDIT_SRC" ] && cp "$SNAUDIT_SRC" "$AUDIT_TREE/STARTNETAUDIT.EXE" \
   && vmsfs_master --ods2 master "$AUDIT_DISK" DCLAUDIT "$AUDIT_TREE" 16 >/dev/null; then rc=0; else rc=1; fi
record "the test volume (DCLNETAUDIT.EXE, NETACP_TEST.EXE, STARTNETAUDIT.EXE) is mastered onto a scratch ODS-2 volume -- the shipped disk carries no test image" "$rc"
[ -f "$AUDIT_DISK" ] || truncate -s 16M "$AUDIT_DISK"

echo "=== DECnet: NCP configures, boot-time STARTNET starts NETACP, hellos on the wire (vms-1f69) ==="
echo "arch=$ARCH qemu=$QEMU"

send() { printf '%s\r' "$1" >&4; }
wake_login() { local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 150 ]; do send ''; sleep 1; w=$((w+1)); done; }
login_system() { local logf="$1"
    send 'SYSTEM'; sleep 1; send 'MANAGER'; sleep 1
    waitfor 'Welcome to OpenVMX' 45 "$logf" && waitfor '$ ' 90 "$logf"; }

# --- Boot 1: configure the executor (NIC present, DECnet NOT yet configured) ---
# shellcheck disable=SC2086
timeout -k 15 $((BOOT_TIMEOUT + SETTLE_SECS + 720)) $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nodefaults -serial stdio \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= \
    -object filter-dump,id=fd0,netdev=n0,file="$PCAP1" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -drive file="$AUDIT_DISK",format=raw,if=virtio,cache=writethrough \
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
    # DEFINE: the permanent database only. SET EXECUTOR STATE ON would turn the
    # network on now (NCP creates NETACP, rd vms-0a7 S3) -- that is (g) below.
    send 'NCP DEFINE EXECUTOR STATE ON'; sleep 3
    send 'NCP DEFINE NODE 1.1 NAME VAX1'; sleep 3
    send 'NCP LIST EXECUTOR'
    if waitfor 'Executor node = 1.42 (OVMX)' 30 "$LOG1"; then rc=0; else rc=1; fi
    record "boot 1: NCP SET EXECUTOR persisted 1.42 (OVMX) -- read back from the permanent database by a fresh NCP image (LIST)" "$rc"
    if grep -qaF '%NCP-E-' "$LOG1"; then rc=1; else rc=0; fi
    record "boot 1: no %NCP-E- error (written through RMS over the ACP)" "$rc"
    # No NETACP on this boot: SHOW has no volatile database to read.
    run_seg 'NCP SHOW EXECUTOR' "$LOG1" '%NCP-F-OPEFAI' 30
    if has '%NCP-F-OPEFAI' && ! has 'Node Volatile Summary'; then rc=0; else rc=1; fi
    record "boot 1: with no NETACP running, NCP SHOW EXECUTOR fails %NCP-F-OPEFAI -- the configured file is never shown as the running network" "$rc"
    run_seg 'NCP SHOW KNOWN OBJECTS' "$LOG1" '%NCP-F-OPEFAI' 30
    if has '%NCP-F-OPEFAI' && ! has 'Known Object Volatile Summary'; then rc=0; else rc=1; fi
    record "boot 1: with no NETACP running, NCP SHOW KNOWN OBJECTS fails %NCP-F-OPEFAI (no volatile object database; rd vms-2d0)" "$rc"
    run_seg 'NCP LIST KNOWN OBJECTS' "$LOG1" 'Known Object Permanent Summary' 30
    if has 'Known Object Permanent Summary' && ! has '%NCP-'; then rc=0; else rc=1; fi
    record "boot 1: NCP LIST KNOWN OBJECTS reads the PERMANENT object database without a NETACP (the VMS SHOW/LIST split)" "$rc"
    run_seg 'SHOW NETWORK' "$LOG1" 'Product:' 20
    if has 'Product: OVMX TCP/IP' && ! has 'Product:  DECNET'; then rc=0; else rc=1; fi
    record "boot 1: SHOW NETWORK has NO DECNET product line while no NETACP serves (TCP/IP line unchanged)" "$rc"
    # The wire's negative control ends here: everything up to now ran with no
    # NETACP. (QEMU appends to the dump as frames go; a copy is the prefix.)
    sleep 2; cp "$PCAP1" "$PCAP1.pre" 2>/dev/null || : > "$PCAP1.pre"

    # (g) rd vms-0a7 S3: STARTNET -> NCP -> NETACP -> an inbound FAL.EXE, traced.
    run_seg 'MOUNT VDA100: DCLAUDIT' "$LOG1" 'MOUNT' 60
    if has '%MOUNT-I-MOUNTED, DCLAUDIT mounted on _VDA100:'; then rc=0; else rc=1; fi
    record "boot 1: the scratch test volume mounts (VDA100:, DCLAUDIT)" "$rc"
    send 'STARTNETAUDIT :== $VDA100:[000000]STARTNETAUDIT.EXE'; sleep 1
    run_seg 'STARTNETAUDIT VDA100:[000000]NETACP_TEST.EXE' "$LOG1" '=== startnet_audit: ' 600
    printf '%s\n' "$seg" | sed -n '/=== startnet_audit (/,/=== startnet_audit: /p'
    if printf '%s\n' "$seg" | grep >/dev/null -E '^=== startnet_audit: [0-9]+ passed, 0 failed ===$' \
       && ! printf '%s\n' "$seg" | grep >/dev/null -E '^  FAIL: '; then rc=0; else rc=1; fi
    record "boot 1: @SYS\$MANAGER:STARTNET -> NCP SET EXECUTOR STATE ON -> NETACP -> an inbound FAL.EXE under ptrace: 0 pipe, 0 poll, 0 thread, 0 host socket, no NETACP.EXE exec; the COPY 0:: through it byte-verified (rd vms-0a7 S3)" "$rc"
    if printf '%s\n' "$seg" | grep >/dev/null -F 'PASS: control: the audit flags a pipe, a poll, a socket(AF_INET), a thread and an exec of another image'; then rc=0; else rc=1; fi
    record "NEGCTL boot 1: the STARTNET audit's control child is flagged on the booted node" "$rc"
    # Diagnostics only: what the audited NETACP logged.
    run_seg 'TYPE SYS$MANAGER:NETACP.LOG' "$LOG1" '$ ' 30
    printf '%s\n' "$seg" | head -40
    send 'DISMOUNT VDA100:'; sleep 3
    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
else
    record "boot 1: SYSTEM logs in" 1
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

n1=$(frames_with "$HELLO_142" "$PCAP1.pre")
if [ "$n1" -eq 0 ]; then rc=0; else rc=1; fi
record "NEGCTL boot 1 (DECnet unconfigured at boot): NO 1.42 endnode hello on the wire before the network is turned on (count=$n1) -- the wire check can tell" "$rc"
n1b=$(frames_with "$HELLO_142" "$PCAP1")
if [ "$n1b" -ge 1 ]; then rc=0; else rc=1; fi
record "boot 1: once NCP SET EXECUTOR STATE ON created NETACP (the audit), its 1.42 endnode hellos are on the wire (count=$n1b)" "$rc"
rm -f "$PCAP1.pre"

# --- Boot 2: STARTNET at LPBETA starts NETACP; prove it runs + talks -----------
# (f)'s scratch volume: the audit image on a volume of its own (rd vms-0a7).
rm -f "$FIFO"; mkfifo "$FIFO"
# shellcheck disable=SC2086
timeout -k 15 $((BOOT_TIMEOUT + NETACP_SETTLE + 720)) $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nodefaults -serial stdio \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= \
    -object filter-dump,id=fd0,netdev=n0,file="$PCAP2" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -drive file="$AUDIT_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG2" 2>&1 &
qp=$!
exec 4>"$FIFO"

wake_login "$LOG2"
if waitfor 'Username:' "$BOOT_TIMEOUT" "$LOG2"; then rc=0; else rc=1; fi
record "boot 2: reaches the login prompt" "$rc"
boot2_up=$rc
# rd vms-158d: STARTNET prints no launch line of its own -- a real VMS console
# shows only RUN's %RUN-S-PROC_ID and NETACP's OPCOM "DECnet starting". That
# NETACP was launched and runs is SHOW SYSTEM's to say (below), and its OPCOM
# record is read back from OPERATOR.LOG.
if grep -qaE '%DECNET-[IE]-STARTNET|DECnet Phase IV -- Startup' "$LOG2"; then rc=1; else rc=0; fi
record "boot 2: STARTNET puts no OVMX launch line on the console (rd vms-158d)" "$rc"
if grep -qaE '%RUN-F-CREPRC|%NCP-F-OPEFAI' "$LOG2"; then rc=1; else rc=0; fi
record "boot 2: no refused launch (no %NCP-F-OPEFAI from STARTNET's NCP SET EXECUTOR STATE ON, no %RUN-F-CREPRC)" "$rc"

if [ "$boot2_up" -eq 0 ] && login_system "$LOG2"; then
    record "boot 2: SYSTEM logs in" 0
    echo "  (letting NETACP run ${NETACP_SETTLE}s before looking)"
    sleep "$NETACP_SETTLE"
    off=$(wc -c <"$LOG2")
    send 'SHOW SYSTEM'; sleep 4
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep >/dev/null -E '(^|[[:space:]])NETACP([[:space:]]|$)'; then rc=0; else rc=1; fi
    record "boot 2: SHOW SYSTEM lists a RUNNING NETACP ${NETACP_SETTLE}s after boot (it exits on any datalink failure)" "$rc"
    if printf '%s\n' "$seg" | grep >/dev/null -F 'Process Name'; then rc=0; else rc=1; fi
    record "NEGCTL boot 2: the SHOW SYSTEM segment is real output (its 'Process Name' column header is present)" "$rc"
    netacp_pid=$(printf '%s\n' "$seg" | grep -a -oE '^[0-9A-F]{8} NETACP( |$)' | head -1 | cut -c1-8)

    # rd vms-158d: NETACP announced itself the VMS way -- OPCOM, "DECnet
    # starting" -- and OPERATOR.LOG carries it.
    off=$(wc -c <"$LOG2")
    send 'TYPE SYS$MANAGER:OPERATOR.LOG'; sleep 6
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep >/dev/null -F 'DECnet starting'; then rc=0; else rc=1; fi
    record "boot 2: OPERATOR.LOG carries NETACP's OPCOM message 'DECnet starting'" "$rc"
    # rd vms-798: ... and it reached OPA0: the way the real VAX shows it -- an
    # OPCOM broadcast, in OPCOM's format (tests/lab/captures/
    # vms-52d-0b01-oracle-20261013/armA-vax1-console.log). Read BEFORE the TYPE
    # above could have put the same words on the console: the segment up to it.
    if head -c "$off" "$LOG2" | tr -d '\r' | grep -A2 -aE '^%%%%%%%%%%%  OPCOM ' | grep >/dev/null -aF 'DECnet starting'; then rc=0; else rc=1; fi
    record "boot 2: OPA0: shows NETACP's OPCOM message 'DECnet starting' (as on the real VAX)" "$rc"

    # rd vms-dda: an outbound link from a user process, brokered through the
    # RUNNING boot-time NETACP over $ASSIGN _NET: + $QIO (this boot has a NIC
    # and the boot-time NETACP; boot 3 has neither). COPY 0"SYSTEM MANAGER":: over
    # NETACP's local loopback to a FAL.EXE server process and back.
    # rd vms-0a7 S3: the test image runs off the scratch test volume.
    run_seg 'MOUNT VDA100: DCLAUDIT' "$LOG2" 'MOUNT' 60
    if has '%MOUNT-I-MOUNTED, DCLAUDIT mounted on _VDA100:'; then rc=0; else rc=1; fi
    record "boot 2: the scratch test volume mounts (VDA100:, DCLAUDIT)" "$rc"
    off=$(wc -c <"$LOG2")
    send 'DNETACC :== $VDA100:[000000]NETACP_TEST.EXE'; sleep 1
    send 'DNETACC --net-loopback-accept-test'
    if ! waitfor 'NETACP-NET-LOOPBACK-ACCEPT:' 240 "$LOG2"; then
        # Diagnostics only (the verdicts below are unchanged): interrupt the
        # stuck image and show what NETACP logged while the proof ran.
        printf '\031' >&4; sleep 3
        netacp_log_off=${netacp_log_off:-$(wc -c <"$LOG2")}
        send 'TYPE SYS$MANAGER:NETACP.LOG'; sleep 8
    fi
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep >/dev/null -F 'NETACP-NET-LOOPBACK-ACCEPT: PASS'; then rc=0; else rc=1; fi
    record "boot 2: a \$QIO _NET: link is brokered through the RUNNING NETACP -- COPY 0:: to FAL.EXE and back, byte-verified (vms-dda)" "$rc"
    if printf '%s\n' "$seg" | grep >/dev/null -F 'completes IO$_ACCESS SS$_INVLOGIN'; then rc=0; else rc=1; fi
    record "boot 2: a bad password is refused at IO\$_ACCESS (INVLOGIN) through NETACP" "$rc"
    if printf '%s\n' "$seg" | grep >/dev/null -F 'NETACP-I-NETLOOP'; then rc=0; else rc=1; fi
    record "NEGCTL boot 2: the brokered-link segment is real output (the test's banner is present)" "$rc"

    # rd vms-6a4: OVMX's own DCL DIRECTORY and TYPE of a node:: spec, through
    # the same NETACP loopback to a FAL.EXE server process. The loopback proof
    # above left SYS$SYSROOT:[SYSMGR]NETLB_SRC.TXT and NETLB_GOT.TXT, each
    # holding one known record. The forms and messages are the real VAX's
    # (tests/lab/captures/decnet-fal-verbs-20261008/).
    run_seg 'DIRECTORY 0"SYSTEM MANAGER"::' "$LOG2" 'Total of' 120
    if has 'Directory 0"SYSTEM password"::' && has 'NETLB_SRC.TXT;' && has 'NETLB_GOT.TXT;' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^Total of [0-9]+ files?\.$'; then rc=0; else rc=1; fi
    record "boot 2: DIRECTORY 0\"SYSTEM MANAGER\":: lists SYSTEM's remote login directory through NETACP + FAL.EXE: VAX header (password masked), the NETLB files, Total line (vms-6a4)" "$rc"
    if has 'DCL-E-ABORT' || has 'NETACP-'; then rc=1; else rc=0; fi
    record "boot 2: ... with no image or NETACP chatter under it" "$rc"
    run_seg 'DIRECTORY 0"SYSTEM MANAGER"::SYS$SYSROOT:[SYSMGR]NETLB_*.TXT' "$LOG2" 'Total of' 120
    if printf '%s\n' "$seg" | grep >/dev/null -E '^Directory 0"SYSTEM password"::.*\]$' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^NETLB_GOT\.TXT;[0-9]+ +NETLB_SRC\.TXT;[0-9]+ +$' \
       && has 'Total of 2 files.'; then rc=0; else rc=1; fi
    record "boot 2: DIRECTORY of a remote wildcard prints the two names in 20-column cells and 'Total of 2 files.' (VAX layout)" "$rc"
    run_seg 'DIRECTORY 0"SYSTEM MANAGER"::SYS$SYSROOT:[SYSMGR]NOSUCH_6A4.TXT' "$LOG2" 'NOFILES' 120
    if has '%DIRECT-W-NOFILES, no files found' && ! has 'Total of'; then rc=0; else rc=1; fi
    record "boot 2: DIRECTORY of a remote spec that matches nothing is %DIRECT-W-NOFILES" "$rc"
    run_seg 'TYPE 0"SYSTEM MANAGER"::SYS$SYSROOT:[SYSMGR]NETLB_SRC.TXT' "$LOG2" 'local loopback' 120
    if printf '%s\n' "$seg" | grep >/dev/null -xF 'NETACP-brokered _NET: COPY over the local loopback' \
       && ! has '::SYS$SYSROOT:[SYSMGR]NETLB_SRC.TXT;'; then rc=0; else rc=1; fi
    record "boot 2: TYPE 0\"SYSTEM MANAGER\"::file prints the remote file's record, with no per-file header for one named file (vms-6a4)" "$rc"
    # Both files' records are the same line, so wait for the SRC header and
    # then give the last record time to arrive before reading the segment.
    off=$(wc -c <"$LOG2")
    send 'TYPE 0"SYSTEM MANAGER"::SYS$SYSROOT:[SYSMGR]NETLB_*.TXT'
    waitfor 'NETLB_SRC.TXT;' 120 "$LOG2"; sleep 8
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep >/dev/null -E '^0"SYSTEM password"::.*NETLB_GOT\.TXT;[0-9]+$' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^0"SYSTEM password"::.*NETLB_SRC\.TXT;[0-9]+$' \
       && [ "$(printf '%s\n' "$seg" | grep -cxF 'NETACP-brokered _NET: COPY over the local loopback')" -ge 2 ]; then rc=0; else rc=1; fi
    record "boot 2: TYPE of a remote wildcard prints each file under its own NODE::name header, records after (VAX form)" "$rc"
    run_seg 'TYPE 0"SYSTEM WRONGPW"::SYS$SYSROOT:[SYSMGR]NETLB_SRC.TXT' "$LOG2" 'INVLOGIN' 120
    if has '%TYPE-W-SEARCHFAIL, error searching for 0"SYSTEM password"::SYS$SYSROOT:[SYSMGR]NETLB_SRC.TXT;' \
       && has '-RMS-E-FND, ACP file or directory lookup failed' \
       && has '-SYSTEM-F-INVLOGIN, login information invalid at remote node' \
       && ! has 'NETACP-brokered'; then rc=0; else rc=1; fi
    record "boot 2: TYPE with a bad password: SEARCHFAIL, -RMS-E-FND, -SYSTEM-F-INVLOGIN as the VAX prints it, and no record shown" "$rc"
    if has 'DCL-E-ABORT' || printf '%s\n' "$seg" | grep -vF 'TYPE 0"SYSTEM WRONGPW"' | grep >/dev/null -F 'WRONGPW'; then rc=1; else rc=0; fi
    record "NEGCTL boot 2: ... no image line, and the password is never echoed back in a message" "$rc"

    # (f) rd vms-0a7: the same verbs, traced, stay off Linux libc.
    send 'DCLNETAUDIT :== $VDA100:[000000]DCLNETAUDIT.EXE'; sleep 1
    run_seg 'DCLNETAUDIT NETACP' "$LOG2" '=== test_syssvc_dcl_remote_syscall_audit: ' 600
    printf '%s\n' "$seg" | sed -n '/=== test_syssvc_dcl_remote_syscall_audit (/,/=== test_syssvc_dcl_remote_syscall_audit: /p'
    if printf '%s\n' "$seg" | grep >/dev/null -F '[booted node: NETACP + FAL]' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^=== test_syssvc_dcl_remote_syscall_audit: [0-9]+ passed, 0 failed ===$' \
       && ! printf '%s\n' "$seg" | grep >/dev/null -E '^  FAIL: '; then rc=0; else rc=1; fi
    record "boot 2: DIRECTORY / TYPE / COPY (both directions) of 0\"SYSTEM MANAGER\":: under ptrace -- 0 pipe, 0 poll, 0 thread, 0 host socket, no NETACP.EXE exec; /dev/vms calls and the real output seen (rd vms-0a7)" "$rc"
    if printf '%s\n' "$seg" | grep >/dev/null -F 'PASS: control: the audit flags a pipe, a poll, a socket(AF_INET), a thread and an exec of another image'; then rc=0; else rc=1; fi
    record "NEGCTL boot 2: the audit's control child is flagged on the booted node (the trace can see a pipe, a poll, a socket, a thread, an exec)" "$rc"

    # (d) NCP SHOW + SHOW NETWORK read the RUNNING NETACP (rd vms-30e).
    send 'NCP :== $SYS$SYSTEM:NCP.EXE'; sleep 1
    run_seg 'NCP SHOW EXECUTOR' "$LOG2" 'Identification' 30
    if has 'Node Volatile Summary as of ' && has 'Executor node = 1.42 (OVMX)' \
       && has 'State                    = on' \
       && has 'Identification           = OVMX DECnet-compatible'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW EXECUTOR reads the running NETACP: 1.42 (OVMX), State on, OVMX identification, in the VAX layout" "$rc"
    run_seg 'NCP SHOW EXECUTOR CHARACTERISTICS' "$LOG2" 'Type ' 30
    if has 'Node Volatile Characteristics as of ' && has 'NSP version              = V4.1.0' \
       && has 'Maximum links            = 32' && has 'Routing version          = V2.0.0' \
       && has 'Type                     = nonrouting IV'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW EXECUTOR CHARACTERISTICS prints NETACP's NSP/routing versions, pool size and type" "$rc"
    run_seg 'NCP SHOW EXECUTOR COUNTERS' "$LOG2" 'Maximum logical links active' 30
    if printf '%s\n' "$seg" | grep >/dev/null -E '^ +[0-9]+  Maximum logical links active$'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW EXECUTOR COUNTERS prints NETACP's counted 'Maximum logical links active' in the VAX column" "$rc"
    run_seg 'NCP SHOW KNOWN NODES' "$LOG2" '1.1 (VAX1)' 30
    if has 'Known Node Volatile Summary as of ' \
       && has '    Node           State      Active  Delay   Circuit     Next node' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^ 1\.1 \(VAX1\) +[A-Z0-9]+-[0-9]+ +0$'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW KNOWN NODES lists 1.1 (VAX1) from NETACP's node database on its circuit, in the VAX table columns" "$rc"
    run_seg 'NCP SHOW KNOWN LINKS' "$LOG2" 'Known Link Volatile Summary' 30
    if has 'No information in database'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW KNOWN LINKS with no logical links says 'No information in database' (VAX wording)" "$rc"
    run_seg 'SHOW NETWORK' "$LOG2" 'Product:' 30
    if has 'Product:  DECNET        Node:  OVMX                 Address(es):  1.42' \
       && has 'Product: OVMX TCP/IP'; then rc=0; else rc=1; fi
    record "boot 2: SHOW NETWORK carries the DECNET product line from the running NETACP (VAX columns); TCP/IP line unchanged" "$rc"

    # (e) KNOWN CIRCUITS / LINES / OBJECTS from the same NETACP (rd vms-2d0).
    run_seg 'NCP SHOW KNOWN CIRCUITS' "$LOG2" 'Routing Node' 30
    if has 'Known Circuit Volatile Summary as of ' \
       && has '   Circuit          State                   Loopback     Adjacent' \
       && has '                                              Name      Routing Node' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^  ETH-0             on( |$)'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW KNOWN CIRCUITS prints NETACP's circuit ETH-0 (the executive's ETH0:), state on, in the VAX columns" "$rc"
    run_seg 'NCP SHOW KNOWN LINES' "$LOG2" 'Known Line Volatile Summary' 30
    if has '   Line             State' \
       && printf '%s\n' "$seg" | grep >/dev/null -E '^  ETH-0             on$'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW KNOWN LINES prints the line ETH-0, state on, in the VAX columns" "$rc"
    run_seg 'NCP SHOW KNOWN OBJECTS' "$LOG2" 'CTERM' 30
    if has 'Known Object Volatile Summary as of ' \
       && has '   Object   Number  File/PID                   User Id          Password' \
       && printf '%s\n' "$seg" | grep >/dev/null -xF '  FAL           17  FAL.EXE'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW KNOWN OBJECTS lists NETACP's FAL 17 FAL.EXE (byte-identical to the real VAX row) under the VAX heading" "$rc"
    cterm_pid=$(printf '%s\n' "$seg" | grep -a -oE '^  CTERM         42  [0-9A-F]{8}$' | cut -c21-28)
    if [ -n "$cterm_pid" ] && [ -n "${netacp_pid:-}" ] && [ "$cterm_pid" = "$netacp_pid" ]; then rc=0; else rc=1; fi
    record "boot 2: SHOW KNOWN OBJECTS' CTERM 42 row carries the PID of the NETACP serving it ($cterm_pid), the PID SHOW SYSTEM lists for NETACP (${netacp_pid:-none})" "$rc"
    run_seg 'NCP SHOW OBJECT FAL' "$LOG2" 'Object Volatile Summary' 30
    if printf '%s\n' "$seg" | grep >/dev/null -xF '  FAL           17  FAL.EXE'; then rc=0; else rc=1; fi
    record "boot 2: NCP SHOW OBJECT FAL reads the same NETACP entry" "$rc"

    if grep -qaF '%NCP-F-' <(tail -c "+$((off + 1))" "$LOG2"); then rc=1; else rc=0; fi
    record "boot 2: no %NCP-F- failure while NETACP is serving" "$rc"

    # rd vms-5843: SET HOST to a node the database does not know says VMS's
    # SS$_NOSUCHNODE (652, "remote node is unknown") once, and nothing about
    # an image the user never ran. It said "%SYSTEM-W-NOSUCHDEV, no such
    # device" and then "%DCL-E-ABORT, image SYS$SYSTEM:NETACP.EXE exited
    # with error status %X00000001".
    run_seg 'SET HOST NOSUCH' "$LOG2" 'NOSUCHNODE' 45
    if has '%SYSTEM-F-NOSUCHNODE, remote node is unknown'; then rc=0; else rc=1; fi
    record "boot 2: SET HOST to an unknown node says %SYSTEM-F-NOSUCHNODE, remote node is unknown" "$rc"
    if has 'DCL-E-ABORT' || has 'NOSUCHDEV'; then rc=1; else rc=0; fi
    record "boot 2: ... and no %DCL-E-ABORT image line or NOSUCHDEV under it" "$rc"

    # rd vms-c89: STARTNET runs NETACP /DETACHED /OUTPUT=/ERROR=
    # SYS$MANAGER:NETACP.LOG. As on VMS, that file IS its SYS$OUTPUT: created
    # through RMS when the image starts, every line a record. Its first report
    # names the datalink it opened, so a NETACP.LOG that TYPE reads back with
    # that line is the file this boot's NETACP wrote.
    # Done LAST: the console before it is what the vms-5843 chatter check
    # below scans, and TYPE shows NETACP's lines on the terminal on purpose.
    off=$(wc -c <"$LOG2")
    netacp_log_off=$off
    send 'TYPE SYS$MANAGER:NETACP.LOG'; sleep 6
    seg=$(tail -c "+$((off + 1))" "$LOG2" | tr -d '\r')
    if printf '%s\n' "$seg" | grep >/dev/null -F 'NETACP-I-DATALINK, datalink open on' &&
       printf '%s\n' "$seg" | grep >/dev/null -F 'NETACP-I-STARTED, NETACP up'; then rc=0; else rc=1; fi
    record "boot 2: SYS\$MANAGER:NETACP.LOG (NETACP's /OUTPUT, written through RMS) carries its DATALINK and STARTED reports (vms-c89)" "$rc"
    if printf '%s\n' "$seg" | grep >/dev/null -E '%TYPE-W-SEARCHFAIL|%RMS-E-FNF'; then rc=1; else rc=0; fi
    record "boot 2: TYPE finds NETACP.LOG (no SEARCHFAIL / FNF)" "$rc"
else
    record "boot 2: SYSTEM logs in" 1
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

# --- Boot 3: NO network adapter (-nic none); _NET: is there, 0:: works --------
# rd vms-5ec: like VMS's NETDRIVER, the executive enters the DECnet device face
# _NET: whether or not the node has a LAN adapter; only the circuit (ETH-0 over
# ETH0:) needs one. So on a node with NO NIC: ETH0: does not exist (the honest
# "no circuit" state, and the proof this boot really has no adapter), _NET:
# does, and a FAL access over the local node 0:: completes -- the DCL
# acceptance battery's FAL persona/verbs proof, on a -nic none VM.
rm -f "$FIFO"; mkfifo "$FIFO"
# shellcheck disable=SC2086
timeout -k 15 $((BOOT_TIMEOUT + 600)) $QEMU $MACHINE $ACCEL \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 1 -nodefaults -serial stdio -nic none \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -drive file="$AUDIT_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG3" 2>&1 &
qp=$!
exec 4>"$FIFO"

wake_login "$LOG3"
if waitfor 'Username:' "$BOOT_TIMEOUT" "$LOG3"; then rc=0; else rc=1; fi
record "boot 3 (no NIC): reaches the login prompt" "$rc"
if [ "$rc" -eq 0 ] && login_system "$LOG3"; then
    record "boot 3 (no NIC): SYSTEM logs in" 0
    run_seg 'WRITE SYS$OUTPUT "NIC3=",F$GETDVI("ETH0:","EXISTS"),"/NET3=",F$GETDVI("_NET:","EXISTS")' "$LOG3" '/NET3=' 20
    if has 'NIC3=FALSE/'; then rc=0; else rc=1; fi
    record "NEGCTL boot 3: the node really has no network adapter -- F\$GETDVI(\"ETH0:\",\"EXISTS\") is FALSE (no circuit to ride)" "$rc"
    if has '/NET3=TRUE'; then rc=0; else rc=1; fi
    record "boot 3 (no NIC): the DECnet device face _NET: exists anyway, entered by the executive as VMS's NETDRIVER is (rd vms-5ec)" "$rc"
    run_seg 'MOUNT VDA100: DCLAUDIT' "$LOG3" 'MOUNT' 60
    if has '%MOUNT-I-MOUNTED, DCLAUDIT mounted on _VDA100:'; then rc=0; else rc=1; fi
    record "boot 3 (no NIC): the scratch test volume mounts (VDA100:, DCLAUDIT)" "$rc"
    send 'DNETACC :== $VDA100:[000000]NETACP_TEST.EXE'; sleep 1
    run_seg 'DNETACC --fal-proc-accept-test' "$LOG3" 'NETACP-FAL-PROC-ACCEPT:' 300
    printf '%s\n' "$seg" | grep -aE '^  (PASS|FAIL): |NETACP-' | head -40
    if has 'NETACP-FAL-PROC-ACCEPT: PASS'; then rc=0; else rc=1; fi
    record "boot 3 (no NIC): a FAL access over the local node 0:: works -- \$ASSIGN _NET:, IO\$_ACCESS 0\"user password\"::, a FAL.EXE server process with the user's UIC, SYSTEM served and GUEST refused (rd vms-5ec)" "$rc"
    if has 'NETACP-I-FALPROC'; then rc=0; else rc=1; fi
    record "NEGCTL boot 3: the FAL segment is real output (the test's banner is present)" "$rc"
else
    record "boot 3 (no NIC): SYSTEM logs in" 1
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
rm -rf "$AUDIT_TREE" "$AUDIT_DISK"

n2=$(frames_with "$HELLO_142" "$PCAP2")
if [ "$n2" -ge 1 ]; then rc=0; else rc=1; fi
record "boot 2: the wire carries Phase IV endnode hellos to AB-00-00-03-00-00 SOURCED FROM AA-00-04-00-2A-04, ethertype 0x6003 (count=$n2)" "$rc"

# rd vms-5843: NETACP is a DETACHED process with no terminal. Its own status
# lines (datalink open, started, per-hello, CTERM listen, broker) must never
# appear on the operator console -- they used to, because $CREPRC left it on
# the creator's descriptors when its /OUTPUT file spec could not be opened,
# and on a booted node a hello line landed on OPA0: every 15 s, in the middle
# of the login prompt. Read over the whole boot-2 console, which spans many
# hello periods (the run above sleeps NETACP_SETTLE alone).
# rd vms-c89: the scan stops where this script TYPEs SYS$MANAGER:NETACP.LOG,
# which shows NETACP's lines on the terminal because it was asked to.
nchat=$(head -c "${netacp_log_off:-$(wc -c <"$LOG2")}" "$LOG2" | grep -acE 'NETACP-I-(DATALINK|STARTED|HELLOSENT|ADJINIT|CTERMLISTEN|BROKER),' || true)
if [ "${nchat:-0}" -eq 0 ]; then rc=0; else rc=1; fi
record "boot 2: NETACP's own status lines never reach the operator console (detached, no terminal; $nchat found)" "$rc"

if [ "$FAIL" -ne 0 ]; then
    echo "--- boot 1 log ---"; cat "$LOG1"
    echo "--- boot 2 log ---"; cat "$LOG2"
    echo "--- boot 3 log ---"; cat "$LOG3"
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
