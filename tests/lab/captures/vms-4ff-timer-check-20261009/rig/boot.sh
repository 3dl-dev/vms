#!/bin/bash
# Boot Node A's deployed artifacts the way the demo worker does, on host QEMU/TCG.
# usage: boot.sh <logfile> <seconds-to-run> [extra cmdline words...]
set -u
RIG=/tmp/4ff-rig
LOG=$1; shift
SECS=$1; shift
EXTRA="$*"
DISK=$(mktemp /tmp/4ff-rig/run-disk.XXXX.qcow2)
cp -f "$RIG/sysdisk-nodeA.qcow2" "$DISK"
# Same machine model, memory, accel and devices as demo/cluster/node-worker.js.
# The socket netdev listens instead of connecting (no WS shim on the host); the
# early-boot phases under study run before any frame is exchanged.
timeout -s KILL "$SECS" qemu-system-x86_64 -nographic -monitor none -M pc -m 256M \
  -accel tcg,tb-size=500 \
  -netdev socket,id=vmnic,listen=127.0.0.1:0 \
  -device virtio-net-pci,netdev=vmnic,mac=52:54:00:00:00:0A \
  -kernel "$RIG/vmlinuz" -initrd "$RIG/initramfs-nodeA.cpio.gz" \
  -append "console=ttyS0 loglevel=3 $EXTRA" \
  -drive file=$DISK,format=qcow2,if=virtio -no-reboot \
  < /dev/null 2>&1 | python3 "$RIG/stamp.py" > "$LOG"
rm -f "$DISK"
echo "---- done: $LOG ($(wc -l < "$LOG") lines)"
