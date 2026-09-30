#!/bin/bash
# b36node.sh - boot ONE booted-OVMX node under QEMU/KVM on the isolated rd
# vms-dfe bridge and hold it LIVE, polling SHOW CLUSTER. Derived verbatim from
# tests/lab/captures/vms-1ac-cn3-achieved-20260925/cn3node.sh; the ONLY change
# is the run root. CAP_NET_RAW is DROPPED from the QEMU subtree.
set -uo pipefail
ART="${ART:?}"; OUT="${OUT:?}"; TAP="${TAP:?}"; MAC="${MAC:?}"
DUR="${DUR:-900}"; NAME="${NAME:-node}"
SCSNODE="${SCSNODE:?}"; SCSSYSID="${SCSSYSID:?}"
VOTES="${VOTES:-1}"; EXPVOTES="${EXPVOTES:-1}"
DISK="/tmp/ovmx-$NAME-$$.img"; cp "$ART/ovmx-distrib.img" "$DISK"
FIFO="/tmp/ovmx-$NAME-$$.in"; rm -f "$FIFO"; mkfifo "$FIFO"
CAPS="${OUT%.log}.caps"
: > "$OUT"
exec 4<>"$FIFO"
send() { printf '%s\r' "$1" >&4; }
capsh --drop=cap_net_raw -- -c 'exec "$@"' _ \
  timeout -k 15 "$((DUR + 300))" qemu-system-x86_64 -accel kvm -cpu host \
    -kernel "$ART/vmlinuz" -initrd "$ART/initramfs-ovmx-slim.cpio.gz" \
    -nographic -append "console=ttyS0 loglevel=3 net.ifnames=0 ovmx.flags=0,1" \
    -m 1024M -smp 2 -nodefaults -serial stdio \
    -netdev tap,id=n0,ifname="$TAP",script=no,downscript=no \
    -device virtio-net-pci,netdev=n0,mac="$MAC" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >>"$OUT" 2>&1 &
QP=$!
for _t in $(seq 1 60); do
    s=$(grep -E 'Cap(Prm|Eff|Bnd)' "/proc/$QP/status" 2>/dev/null)
    if [ -n "$s" ]; then
        b=$(printf '%s\n' "$s" | awk '/CapBnd/{print $2}')
        if [ -n "$b" ] && [ $(( 0x$b & 0x2000 )) -eq 0 ]; then
            printf '%s\n' "$s" > "$CAPS"; break
        fi
    fi
    sleep 0.5
done
[ -s "$CAPS" ] || grep -E 'Cap(Prm|Eff|Bnd)' "/proc/$QP/status" > "$CAPS" 2>/dev/null
echo "[$NAME] caps: $(cat "$CAPS" | tr '\n' ' ')" | tee -a "$OUT"
w=0
until grep -qaF 'SYSBOOT> ' "$OUT" 2>/dev/null || [ "$w" -ge 600 ]; do
    kill -0 "$QP" 2>/dev/null || break
    sleep 1; w=$((w+1))
done
if grep -qaF 'SYSBOOT> ' "$OUT"; then
    echo "[$NAME] SYSBOOT> reached" | tee -a "$OUT"
    send "SET SCSNODE $SCSNODE";        sleep 2
    send "SET SCSSYSTEMID $SCSSYSID";   sleep 2
    send 'SET VAXCLUSTER 2';            sleep 2
    send "SET VOTES $VOTES";            sleep 2
    send "SET EXPECTED_VOTES $EXPVOTES"; sleep 2
    send 'WRITE';                       sleep 4
    send 'CONTINUE';                    sleep 2
else
    echo "[$NAME] FATAL -- SYSBOOT> never appeared" | tee -a "$OUT"
fi
w=0
until grep -qaF 'Username:' "$OUT" 2>/dev/null || [ "$w" -ge 600 ]; do
    kill -0 "$QP" 2>/dev/null || break
    send ''; sleep 1; w=$((w+1))
done
# rd vms-eb3: LOG IN AND PROVE IT. Typing SYSTEM/MANAGER blind raced a
# Password: prompt that arrived 14 s late while the node was busy joining (rig
# arm F-6): the password read took the next poll line, the login failed, and
# every later SHOW CLUSTER went to a Username: prompt -- the grader then read
# no view of the node at all. Wait for each prompt, and retry a failed login.
cnt() { grep -acF "$1" "$OUT" 2>/dev/null | head -1; }
wait_more() {   # <text> <baseline> <seconds>
    local i=0
    while [ "$i" -lt "$3" ]; do
        [ "$(cnt "$1")" -gt "$2" ] && return 0
        kill -0 "$QP" 2>/dev/null || return 1
        sleep 1; i=$((i+1))
    done
    return 1
}
logged_in=0
for try in 1 2 3 4 5; do
    pw=$(cnt 'Password:'); bad=$(cnt 'User authorization failure')
    send 'SYSTEM'
    wait_more 'Password:' "$pw" 120 || { send ''; continue; }
    send 'MANAGER'
    ok=$(grep -acE '^B36-LOGIN-OK' "$OUT" | head -1)
    sleep 4; send "WRITE SYS\$OUTPUT \"B36-LOGIN-OK\""
    for _w in $(seq 1 60); do
        [ "$(grep -acE '^B36-LOGIN-OK' "$OUT" | head -1)" -gt "$ok" ] && { logged_in=1; break; }
        [ "$(cnt 'User authorization failure')" -gt "$bad" ] && break
        sleep 1
    done
    [ "$logged_in" = 1 ] && break
    echo "[$NAME] login attempt $try failed -- retrying" | tee -a "$OUT"
    send ''; sleep 3
done
if [ "$logged_in" = 1 ]; then
    send 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'; sleep 2
    p=0
    while [ "$p" -lt "$DUR" ]; do
        kill -0 "$QP" 2>/dev/null || break
        send "WRITE SYS\$OUTPUT \"B36-POLL-$NAME-$p\""; sleep 1
        send 'SHOW CLUSTER'; sleep 6
        # rd vms-e88: the executive's own join transcript, on the joiner
        if [ "$NAME" = "OVMXB" ] && { [ "$p" -eq 30 ] || [ "$p" -eq 60 ] || [ "$p" -eq 120 ]; }; then
            send 'RUN SYS$SYSTEM:CNXTRACE'; sleep 8
        fi
        sleep 8; p=$((p+15))
    done
    send 'WRITE SYS$OUTPUT "B36-DONE"'; sleep 3
else
    echo "[$NAME] FATAL -- never reached Username:" | tee -a "$OUT"
fi
send 'LOGOUT' 2>/dev/null; sleep 1
kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$FIFO" "$DISK"
echo "=== $NAME done ===" | tee -a "$OUT"
