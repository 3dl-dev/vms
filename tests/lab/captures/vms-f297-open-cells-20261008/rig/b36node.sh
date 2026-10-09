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
# rd vms-b64: PROMPT-SYNCHRONISED login. The console interleaves %CNXMAN
# kernel lines with the login prompts, and a timed sequence typed the poll's
# SHOW CLUSTER into 'Username:' (arms FM-5, FM-11, HM-8). Each field is sent
# only after its OWN prompt appears (counted past a baseline, so an old prompt
# never satisfies the wait), and a session found back at 'Username:' before a
# poll logs in again.
cnt() { tr -d '\r\000' < "$OUT" | grep -acE -- "$1"; true; }
wait_more() { local pat=$1 base=$2 lim=$3 i
    for i in $(seq 1 "$lim"); do [ "$(cnt "$pat")" -gt "$base" ] && return 0; sleep 1; done; return 1; }
# A DCL prompt is a line that STARTS with "$": the console appends kernel
# lines to it ("$ [ 31.15] %CNXMAN, ..." in arm IM-2), so it need not end there.
PROMPT='^\$( |$)'
last_prompt() { tr -d '\r\000' < "$OUT" | grep -aoE "Username:|Password:|$PROMPT" | tail -1; }
last_prompt_is_username() { last_prompt | grep -q 'Username:'; }
at_dcl() { last_prompt | grep -qE "$PROMPT"; }
login() { local a u p d
    for a in 1 2 3 4 5; do
        at_dcl && break
        u=$(cnt 'Username:'); send ''
        wait_more 'Username:' "$u" 20 || continue
        p=$(cnt 'Password:'); sleep 1; send 'SYSTEM'
        wait_more 'Password:' "$p" 20 || continue
        d=$(cnt "$PROMPT"); sleep 1; send 'MANAGER'
        wait_more "$PROMPT" "$d" 40 || continue
        break
    done
    if at_dcl; then
        send 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'; sleep 2
        echo "[$NAME] logged in (attempt $a)" >> "$OUT.login"; return 0
    fi
    echo "[$NAME] LOGIN FAILED" >> "$OUT.login"; return 1; }
if grep -qaF 'Username:' "$OUT"; then
    login
    p=0
    while [ "$p" -lt "$DUR" ]; do
        kill -0 "$QP" 2>/dev/null || break
        last_prompt_is_username && login
        send "WRITE SYS\$OUTPUT \"B36-POLL-$NAME-$p\""; sleep 1
        send 'SHOW CLUSTER'; sleep 6
        # rd vms-e88: the executive's own join transcript, on the joiner
        if [ "$NAME" = "OVMXB" ] && { [ "$p" -eq 30 ] || [ "$p" -eq 60 ] || [ "$p" -eq 120 ] || { [ "${TRACE_ALL:-0}" = 1 ] && [ "$p" -ge 45 ]; }; }; then
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
