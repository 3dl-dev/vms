#!/bin/bash
# Reproduce the demo's vCPU-slowdown condition without a browser: pin Node A's
# qemu to ONE host core and make it share that core with <hogs> equal-priority
# spinners, so the guest runs at roughly 1/(hogs+1) of host TCG speed -- the
# qemu-wasm-in-a-tab condition (three heavy Workers timesharing the tab's cores).
#
# usage: slowboot.sh <tag> <hogs> <secs> [extra cmdline words...]
set -u
RIG=/tmp/4ff-rig
TAG=$1; HOGS=$2; SECS=$3; shift 3
EXTRA="$*"
CORE=11
LOG=$RIG/slow-$TAG.log
DISK=$(mktemp /tmp/4ff-rig/slow-disk.XXXX.qcow2)
cp -f "$RIG/sysdisk-nodeA.qcow2" "$DISK"

pids=()
for i in $(seq 1 "$HOGS"); do
  taskset -c $CORE python3 -c '
while True: pass' &
  pids+=($!)
done

timeout -s KILL "$SECS" taskset -c $CORE qemu-system-x86_64 -nographic -monitor none \
  -M pc -m 256M -smp ${SMP:-1} -accel tcg,tb-size=500 \
  -netdev socket,id=vmnic,listen=127.0.0.1:0 \
  -device virtio-net-pci,netdev=vmnic,mac=52:54:00:00:00:0A \
  -kernel "$RIG/vmlinuz" -initrd "$RIG/initramfs-nodeA.cpio.gz" \
  -append "console=ttyS0 loglevel=3 $EXTRA" \
  -drive file=$DISK,format=qcow2,if=virtio -no-reboot \
  < /dev/null 2>&1 | python3 "$RIG/stamp.py" > "$LOG"

for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done
rm -f "$DISK"
echo "== $TAG (hogs=$HOGS, cmdline='$EXTRA') -> $LOG"
grep -qiE 'Kernel panic' "$LOG" && echo "   PANIC: $(grep -m1 -iE 'Kernel panic' "$LOG")"
grep -qiE 'MP-BIOS bug' "$LOG" && echo "   8254-check failure line present"
grep -m1 'MOUNTED' "$LOG" || echo "   never mounted"
grep -m1 'STDRV-I-STARTUP' "$LOG" || echo "   never reached STDRV"
