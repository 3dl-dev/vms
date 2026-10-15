#!/bin/busybox sh
# PID 1 for the aarch64 OVMX-kernel proof (rd vms-a07). Loads the in-tree,
# signed vms.ko into the OVMX arm64 kernel and reports whether /dev/vms exists.
# ovmxmode=load|noload on the kernel command line picks the arm (noload is the
# negative control: the same boot without the module must NOT show /dev/vms).
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
MODE=$(sed -n 's/.*ovmxmode=\([a-z]*\).*/\1/p' /proc/cmdline)
echo "OVMX_A64_KERNEL release=$(uname -r) machine=$(uname -m) mode=$MODE"
if [ "$MODE" = load ]; then
    if insmod /vms.ko; then echo "OVMX_A64_VMSKO_LOADED"; else echo "OVMX_A64_VMSKO_LOAD_FAILED"; fi
fi
sleep 1
if [ -c /dev/vms ]; then echo "OVMX_A64_DEVVMS_PRESENT"; else echo "OVMX_A64_DEVVMS_ABSENT"; fi
echo "OVMX_A64_TAINTED=$(cat /proc/sys/kernel/tainted)"
poweroff -f
