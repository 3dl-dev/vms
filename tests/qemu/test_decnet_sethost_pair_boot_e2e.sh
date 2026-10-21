#!/bin/bash
# test_decnet_sethost_pair_boot_e2e.sh - $ SET HOST between TWO booted OVMX
# nodes keeps the remote password off the originating terminal, and puts
# %REM-S-END on a line of its own (rd vms-a353).
#
# Two disks are configured as DECnet endnodes with NCP (boot 1, both nodes at
# once): node A = 1.42 OVMXA, node B = 1.43 OVMXB, each knowing the other by
# name. Boot 2 runs both on ONE Ethernet segment (a QEMU socket netdev pair:
# A listens, B connects); boot-time STARTNET starts NETACP on each. From A's
# console: SET HOST OVMXB, log in SYSTEM / MANAGER on B, SHOW TERMINAL there
# (the session's terminal is B's virtual terminal _RTAn:), LOGOUT.
#
# The assertions read A's console OUTPUT only -- the guest's serial output,
# QEMU's stdout to a file; the keystrokes go in through a FIFO and are never in
# that file -- so a typed string appears in it only if something ECHOED it.
#   (a) B's LOGINOUT prompts Username: and Password: on A's console and the
#       login completes; the session's terminal is an RTAn: virtual terminal.
#   (b) THE PASSWORD IS NOT ECHOED: between B's "Password:" prompt and B's
#       first DCL prompt, A's console never shows MANAGER. VMS never echoes a
#       password; the CTERM Start Read for Password: is a no-echo read
#       (docs/oracle/vax-sethost-cterm.pcap: FLAGS 0x00b808).
#       NEGATIVE CONTROL: the USERNAME read echoes (FLAGS 0x00b008), so the
#       same window for Username: DOES show SYSTEM -- the check can see an
#       echo when there is one.
#   (c) %REM-S-END IS ON ITS OWN LINE: in A's console bytes, "%REM-S-END,
#       control returned to node OVMXA::" is preceded by a line feed -- not run
#       onto "SYSTEM logged out at ..." by a bare carriage return (oracle
#       docs/oracle/vax-sethost-cterm.md sec 2 item 5).
#   (d) control returns to A: SHOW TERMINAL after it names A's console OPA0:.
#
# Reboot mechanics + writeback settle: as test_decnet_startnet_boot_e2e.sh.
# Runs inside the shipped bootable image (ovmx-boot); the runner
# run_decnet_sethost_pair_boot_e2e.sh docker-runs it. Exit 0 = all checks pass.
set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"
SETTLE_SECS="${SETTLE_SECS:-60}"
NETACP_SETTLE="${NETACP_SETTLE:-30}"
# The boot set: the shipped image's /boot (the runner), or a local one.
KERNEL="${OVMX_KERNEL:-/boot/vmlinuz}"
INITRD="${OVMX_INITRD:-/boot/initramfs-ovmx-slim.cpio.gz}"
DISTRIB_IMG="${OVMX_DISTRIB_IMG:-/boot/ovmx-distrib.img}"
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

W="${OVMX_PAIR_WORK:-/tmp/dn-pair}"
rm -rf "$W"; mkdir -p "$W"
PORT="${OVMX_PAIR_PORT:-12370}"
# node: letter, address, name, MAC
NODES="A:1.42:OVMXA:52:54:00:00:2a:0a B:1.43:OVMXB:52:54:00:00:2b:0b"
declare -A QP

# boot <node> <phase> <netdev-args...>: one QEMU on $W/<node>.img, console to
# $W/<node>-<phase>.log, keystrokes from FIFO $W/<node>.in (fd 4 = A, 5 = B).
boot() { local n="$1" ph="$2" mac="$3"; shift 3
    rm -f "$W/$n.in"; mkfifo "$W/$n.in"
    # shellcheck disable=SC2086
    timeout -k 15 1800 $QEMU $MACHINE $ACCEL \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 1 -nodefaults -serial stdio \
        "$@" -device "virtio-net-pci,netdev=n0,mac=$mac,romfile=" \
        -drive file="$W/$n.img",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$W/$n.in" >"$W/$n-$ph.log" 2>&1 &
    QP[$n]=$!
    if [ "$n" = A ]; then exec 4>"$W/$n.in"; else exec 5>"$W/$n.in"; fi
}
send() { if [ "$1" = A ]; then printf '%s\r' "$2" >&4; else printf '%s\r' "$2" >&5; fi; }
waitfor() { local n="$1" pat="$2" lim="$3" log="$4" w=0
    while [ $w -lt $((lim*4)) ]; do
        grep -qaF -- "$pat" "$log" 2>/dev/null && return 0
        kill -0 "${QP[$n]}" 2>/dev/null || return 1
        sleep 0.25; w=$((w+1))
    done; return 1; }
# wait for $pat in $log AFTER byte offset $off
waitafter() { local n="$1" pat="$2" lim="$3" log="$4" off="$5" w=0
    while [ $w -lt $((lim*4)) ]; do
        tail -c "+$((off + 1))" "$log" | grep >/dev/null -aF -- "$pat" && return 0
        kill -0 "${QP[$n]}" 2>/dev/null || return 1
        sleep 0.25; w=$((w+1))
    done; return 1; }
login() { local n="$1" log="$2" w=0
    until grep -qaF 'Username:' "$log" 2>/dev/null || [ "$w" -ge "$BOOT_TIMEOUT" ]; do
        send "$n" ''; sleep 1; w=$((w+1)); done
    grep -qaF 'Username:' "$log" || return 1
    send "$n" 'SYSTEM'; sleep 1; send "$n" 'MANAGER'; sleep 1
    waitfor "$n" 'Welcome to OpenVMX' 60 "$log" && waitfor "$n" '$ ' 90 "$log"; }
stop() { local n
    for n in A B; do [ -n "${QP[$n]:-}" ] && { kill "${QP[$n]}" 2>/dev/null; wait "${QP[$n]}" 2>/dev/null; }; done
    exec 4>&- 5>&- 2>/dev/null; }
trap stop EXIT

echo "=== DECnet SET HOST between two booted OVMX nodes: no password echo, %REM-S-END on its own line (vms-a353) ==="
echo "arch=$ARCH qemu=$QEMU"

# --- Boot 1: configure both disks (both nodes at once) ----------------------
for spec in $NODES; do
    n=${spec%%:*}; rest=${spec#*:}; mac=${rest#*:*:}
    cp "$DISTRIB_IMG" "$W/$n.img"
    boot "$n" cfg "$mac" -netdev user,id=n0
done
for spec in $NODES; do
    n=${spec%%:*}; rest=${spec#*:}; addr=${rest%%:*}; rest=${rest#*:}; name=${rest%%:*}
    if login "$n" "$W/$n-cfg.log"; then rc=0; else rc=1; fi
    record "boot 1 ($n): SYSTEM logs in" "$rc"
    [ "$rc" -eq 0 ] || continue
    send "$n" 'NCP :== $SYS$SYSTEM:NCP.EXE'; sleep 1
    send "$n" "NCP SET EXECUTOR ADDRESS $addr"; sleep 3
    send "$n" "NCP SET EXECUTOR NAME $name"; sleep 3
    send "$n" 'NCP SET EXECUTOR STATE ON'; sleep 3
    for o in $NODES; do
        on=${o%%:*}; [ "$on" = "$n" ] && continue
        orest=${o#*:}; oaddr=${orest%%:*}; orest=${orest#*:}; oname=${orest%%:*}
        send "$n" "NCP DEFINE NODE $oaddr NAME $oname"; sleep 3
    done
    send "$n" 'NCP LIST EXECUTOR'
    if waitfor "$n" "Executor node = $addr ($name)" 30 "$W/$n-cfg.log"; then rc=0; else rc=1; fi
    record "boot 1 ($n): NCP persisted executor $addr ($name)" "$rc"
done
echo "  (settling ${SETTLE_SECS}s for guest writeback)"
sleep "$SETTLE_SECS"
stop

# --- Boot 2: both nodes on one segment; SET HOST A -> B ----------------------
A_MAC=${NODES%% *}; A_MAC=${A_MAC#*:*:*:}
B_MAC=${NODES##* };  B_MAC=${B_MAC#*:*:*:}
boot A run "$A_MAC" -netdev "socket,id=n0,listen=127.0.0.1:$PORT"
sleep 2
boot B run "$B_MAC" -netdev "socket,id=n0,connect=127.0.0.1:$PORT"
LA="$W/A-run.log"; LB="$W/B-run.log"
if login A "$LA"; then ra=0; else ra=1; fi
record "boot 2 (A): SYSTEM logs in" "$ra"
if login B "$LB"; then rb=0; else rb=1; fi
record "boot 2 (B): SYSTEM logs in" "$rb"

if [ "$ra" -eq 0 ] && [ "$rb" -eq 0 ]; then
    echo "  (letting the two NETACPs hear each other ${NETACP_SETTLE}s)"
    sleep "$NETACP_SETTLE"
    # Boot-time STARTNET launched NETACP on each node. STARTNET prints no
    # launch line of its own (rd vms-158d), so ask the process table.
    for n in A B; do
        lg="$W/$n-run.log"
        o=$(wc -c <"$lg")
        send "$n" 'SHOW SYSTEM'
        waitafter "$n" 'NETACP' 30 "$lg" "$o"; sleep 2
        seg=$(tail -c "+$((o + 1))" "$lg" | tr -d '\r')
        if printf '%s\n' "$seg" | grep >/dev/null -E '(^|[[:space:]])NETACP([[:space:]]|$)' \
           && printf '%s\n' "$seg" | grep >/dev/null -F 'Process Name'; then rc=0; else rc=1; fi
        record "boot 2 ($n): SHOW SYSTEM lists the NETACP boot-time STARTNET started" "$rc"
    done

    o0=$(wc -c <"$LA")
    send A 'SET HOST OVMXB'
    if waitafter A 'Username:' 120 "$LA" "$o0"; then rc=0; else rc=1; fi
    record "A: SET HOST OVMXB reaches B's LOGINOUT Username: prompt" "$rc"
    sleep 1
    o1=$(wc -c <"$LA")
    send A 'SYSTEM'
    waitafter A 'Password:' 60 "$LA" "$o1"; sleep 1
    o2=$(wc -c <"$LA")
    send A 'MANAGER'
    if waitafter A 'Welcome to OpenVMX' 90 "$LA" "$o2" && waitafter A '$ ' 90 "$LA" "$o2"; then rc=0; else rc=1; fi
    record "A: the remote login on B completes" "$rc"
    sleep 2
    o3=$(wc -c <"$LA")
    send A 'SHOW TERMINAL'
    if waitafter A 'Terminal: _RTA' 60 "$LA" "$o3"; then rc=0; else rc=1; fi
    record "A: the session runs on B's virtual terminal (SHOW TERMINAL: _RTAn:)" "$rc"
    sleep 1
    o4=$(wc -c <"$LA")
    send A 'LOGOUT'
    if waitafter A '%REM-S-END' 90 "$LA" "$o4"; then rc=0; else rc=1; fi
    record "A: LOGOUT on B returns control (%REM-S-END)" "$rc"
    sleep 3
    o5=$(wc -c <"$LA")
    send A 'SHOW TERMINAL'
    if waitafter A 'Terminal: _OPA0:' 60 "$LA" "$o5"; then rc=0; else rc=1; fi
    record "A: control is back on A's own console (SHOW TERMINAL: _OPA0:)" "$rc"

    # (b) the windows: what A's console showed while each remote read was out.
    user_win=$(tail -c "+$((o1 + 1))" "$LA" | head -c "$((o2 - o1))" | tr -d '\r\000')
    pass_win=$(tail -c "+$((o2 + 1))" "$LA" | head -c "$((o3 - o2))" | tr -d '\r\000')
    if printf '%s' "$user_win" | grep >/dev/null -F 'SYSTEM'; then rc=0; else rc=1; fi
    record "NEGCTL: the Username: read echoes -- A's console shows the typed SYSTEM (the check can see an echo)" "$rc"
    if printf '%s' "$pass_win" | grep >/dev/null -F 'MANAGER'; then rc=1; else rc=0; fi
    record "A's console never shows the remote password: nothing echoed between B's Password: and its DCL prompt" "$rc"
    if grep -qaE 'Password: *MANAGER' "$LA"; then rc=1; else rc=0; fi
    record "A's whole console has no 'Password: MANAGER'" "$rc"

    # (c) %REM-S-END on a line of its own: the byte before it is a line feed.
    before=$(tail -c "+$((o4 + 1))" "$LA" | od -An -v -tx1 | tr -d ' \n' \
             | grep -o '..2552454d2d532d454e442c' | head -1 | cut -c1-2)
    if [ "$before" = "0a" ]; then rc=0; else rc=1; fi
    record "%REM-S-END starts its own line (preceded by LF, not run onto 'logged out at' by a bare CR; byte before = ${before:-none})" "$rc"
    if tail -c "+$((o4 + 1))" "$LA" | tr -d '\r\000' | grep >/dev/null -E '^%REM-S-END, control returned to node OVMXA::$'; then rc=0; else rc=1; fi
    record "the line reads exactly '%REM-S-END, control returned to node OVMXA::'" "$rc"
    if tail -c "+$((o4 + 1))" "$LA" | tr -d '\r\000' | grep >/dev/null -E 'logged out at '; then rc=0; else rc=1; fi
    record "NEGCTL: B's 'logged out at' line precedes it in the same window" "$rc"
fi
stop

if [ "$FAIL" -ne 0 ]; then
    for f in "$W"/A-cfg.log "$W"/B-cfg.log "$W"/A-run.log "$W"/B-run.log; do
        echo "--- $f ---"; cat -v "$f" 2>/dev/null
    done
fi
echo ""
echo "  RESULTS: $PASS/$((PASS+FAIL)) checks passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "  SET HOST BETWEEN BOOTED NODES: NO PASSWORD ECHO, %REM-S-END ON ITS OWN LINE"
    exit 0
fi
exit 1
