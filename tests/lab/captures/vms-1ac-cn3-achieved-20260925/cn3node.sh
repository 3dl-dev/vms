#!/bin/bash
# cn3node.sh - boot ONE booted-OVMX node under QEMU/KVM on the isolated CN=3
# bridge and hold it LIVE, polling SHOW CLUSTER, for the whole window.
# rd vms-4f0. Cluster identity comes from the injected initramfs (no SYSBOOT
# authoring). CAP_NET_RAW is DROPPED from the QEMU subtree: a join can then
# only be the executive's own kernel socket doing the L2 I/O.
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
    [ -n "$s" ] && case "$(echo "$s" | awk '/CapBnd/{print $2}')" in
        *) if [ "$(printf '%s\n' "$s" | awk '/CapBnd/{print $2}')" != "" ] && \
              [ $(( 0x$(printf '%s\n' "$s" | awk '/CapBnd/{print $2}') & 0x2000 )) -eq 0 ]; then
               printf '%s\n' "$s" > "$CAPS"; break
           fi;; esac
    sleep 0.5
done
[ -s "$CAPS" ] || grep -E 'Cap(Prm|Eff|Bnd)' "/proc/$QP/status" > "$CAPS" 2>/dev/null
echo "[$NAME] caps: $(cat "$CAPS" | tr '\n' ' ')" | tee -a "$OUT"
# SYSBOOT> -- author this node's cluster identity onto its own disk copy
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
if grep -qaF 'Username:' "$OUT"; then
    send 'SYSTEM'; sleep 2; send 'MANAGER'; sleep 4
    send 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'; sleep 2
    p=0
    while [ "$p" -lt "$DUR" ]; do
        kill -0 "$QP" 2>/dev/null || break
        send "WRITE SYS\$OUTPUT \"CN3-POLL-$NAME-$p\""; sleep 1
        send 'SHOW CLUSTER'; sleep 6
        sleep 8; p=$((p+15))
    done
    send 'WRITE SYS$OUTPUT "CN3-CNXTRACE-BEGIN"'; sleep 2
    send 'RUN SYS$SYSTEM:CNXTRACE.EXE'; sleep 20
    send 'WRITE SYS$OUTPUT "CN3-CNXTRACE-END"'; sleep 3
    send 'SHOW CLUSTER/LOCAL_PORTS'; sleep 4
    send 'WRITE SYS$OUTPUT "CN3-DONE"'; sleep 3
else
    echo "[$NAME] FATAL -- never reached Username:" | tee -a "$OUT"
fi
send 'LOGOUT' 2>/dev/null; sleep 1
kill "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$FIFO" "$DISK"
echo "=== $NAME done ===" | tee -a "$OUT"
