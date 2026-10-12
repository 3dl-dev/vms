#!/bin/bash
# evnode.sh - boot ONE booted-OVMX node (QEMU; KVM if present else TCG) on the
# pod bridge, set cluster SYSBOOT params, log in prompt-synchronised, then hold
# it LIVE. Commands are typed by writing "<cmd>\r" to $FIFO (kept open here).
# Derived from tests/lab/captures/vms-1ac-cn3-achieved-20260925/cn3node.sh via
# dlmlab b36node.sh (rd vms-b64 prompt-sync login). CAP_NET_RAW is DROPPED.
# Extra disks: EXTRA_DRIVES="file[:ro] ..." -> additional virtio drives.
set -uo pipefail
ART="${ART:?}"; OUT="${OUT:?}"; TAP="${TAP:?}"; MAC="${MAC:?}"; NAME="${NAME:?}"
DUR="${DUR:-7200}"; SCSNODE="${SCSNODE:?}"; SCSSYSID="${SCSSYSID:?}"
VOTES="${VOTES:-1}"; EXPVOTES="${EXPVOTES:-1}"; LOCKDIRWT="${LOCKDIRWT:-}"
MEM="${MEM:-896M}"
DISK="/tmp/ovmx-$NAME.img"; cp --sparse=always "$ART/ovmx-distrib.img" "$DISK"
FIFO="${OUT}.in"; rm -f "$FIFO"; mkfifo "$FIFO"
: > "$OUT"
exec 4<>"$FIFO"
send() { printf '%s\r' "$1" >&4; }
ACC="-accel tcg"; CPU="-cpu max"; [ -c /dev/kvm ] && { ACC="-accel kvm"; CPU="-cpu host"; }
XD=()
for d in ${EXTRA_DRIVES:-}; do
  f="${d%%:ro}"; ro=""; [ "$f" != "$d" ] && ro=",readonly=on"
  XD+=(-drive "file=$f,format=raw,if=virtio,cache=writethrough$ro")
done
capsh --drop=cap_net_raw -- -c 'exec "$@"' _ \
  timeout -k 15 "$((DUR + 300))" qemu-system-x86_64 $ACC $CPU \
    -kernel "$ART/vmlinuz" -initrd "$ART/initramfs-ovmx-slim.cpio.gz" \
    -nographic -append "console=ttyS0 loglevel=3 net.ifnames=0 ovmx.flags=0,1 ${KAPPEND:-}" \
    -m "$MEM" -smp 2 -nodefaults -serial stdio \
    -netdev tap,id=n0,ifname="$TAP",script=no,downscript=no \
    -device virtio-net-pci,netdev=n0,mac="$MAC" \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough "${XD[@]}" \
    -no-reboot <"$FIFO" >>"$OUT" 2>&1 &
QP=$!; echo "$QP" > "$OUT.qpid"
grep -E 'CapBnd' /proc/$QP/status > "$OUT.caps" 2>/dev/null
w=0; until grep -qaF 'SYSBOOT> ' "$OUT" || [ $w -ge 1800 ]; do kill -0 $QP || break; sleep 1; w=$((w+1)); done
if grep -qaF 'SYSBOOT> ' "$OUT"; then
  for c in "SET SCSNODE $SCSNODE" "SET SCSSYSTEMID $SCSSYSID" 'SET VAXCLUSTER 2' "SET VOTES $VOTES" "SET EXPECTED_VOTES $EXPVOTES"; do send "$c"; sleep 3; done
  [ -n "$LOCKDIRWT" ] && { send "SET LOCKDIRWT $LOCKDIRWT"; sleep 3; }
  send 'WRITE'; sleep 5; send 'CONTINUE'
else echo "[$NAME] FATAL no SYSBOOT>" >> "$OUT"; fi
cnt() { tr -d '\r\000' < "$OUT" | grep -ac -- "$1"; true; }
wait_more() { local i; for i in $(seq 1 "$3"); do [ "$(cnt "$1")" -gt "$2" ] && return 0; sleep 1; done; return 1; }
PROMPT='^\$( |$)'
last_prompt() { tr -d '\r\000' < "$OUT" | grep -aoE "Username:|Password:|$PROMPT" | tail -1; }
w=0; until grep -qaF 'Username:' "$OUT" || [ $w -ge 2400 ]; do kill -0 $QP || break; send ''; sleep 2; w=$((w+2)); done
for a in 1 2 3 4 5; do
  last_prompt | grep -qE "$PROMPT" && break
  u=$(cnt 'Username:'); send ''; wait_more 'Username:' "$u" 30 || continue
  p=$(cnt 'Password:'); sleep 1; send 'SYSTEM'; wait_more 'Password:' "$p" 30 || continue
  d=$(cnt "$PROMPT"); sleep 1; send 'MANAGER'; wait_more "$PROMPT" "$d" 90 || continue
done
send 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'
echo "[$NAME] READY $(date -u +%H:%M:%S)" >> "$OUT.login"
wait $QP
rm -f "$FIFO" "$DISK"; echo "=== $NAME done ===" >> "$OUT"
