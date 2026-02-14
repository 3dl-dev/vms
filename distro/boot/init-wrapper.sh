#!/bin/sh
# PID 1 bootstrap for bare-metal OVMX
# Uses busybox for early setup, then hands off to ovmx_init.

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
mkdir -p /dev/pts /dev/shm
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp

hostname OVMX

# Load VMS kernel modules
insmod /lib/modules/vms.ko 2>/dev/null
insmod /lib/modules/vmsfs.ko 2>/dev/null

# Mount vmsfs over /vms for case-insensitive file access
# vmsfs needs a backing directory separate from the mount point
if [ -d /vms ]; then
    mkdir -p /var/vmsfs
    cp -a /vms/. /var/vmsfs/
    mount -t vmsfs -o backing=/var/vmsfs,case_blind=1 none /vms 2>/dev/null
fi

# Generate /etc/passwd for musl getpwnam()
cat > /etc/passwd << 'EOF'
root:x:0:0:SYSTEM:/root:/bin/vmsdcl
EOF
cat > /etc/group << 'EOF'
system:x:0:root
EOF

# Hand off to ovmx_init
exec /sbin/init
