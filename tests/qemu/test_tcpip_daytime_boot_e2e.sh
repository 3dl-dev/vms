#!/bin/bash
# test_tcpip_daytime_boot_e2e.sh (rd vms-21b) -- runs INSIDE the daytime test-overlay
# bootable image (built --build-arg OVMX_TEST_ENABLE_TCPIP=1). Cold-boots the OVMX
# distro under QEMU with the aux server auto-started (the test overlay's SYSTARTUP
# runs TCPIP$CONFIG then @SYS$STARTUP:TCPIP$STARTUP) and a SLIRP hostfwd to guest
# port 13, then connects INBOUND and asserts the DAYTIME service answers -- proving
# the (B) launch mechanism end to end on a booted distro: TCPIP$INETD accepts over
# BGn:, resolves SYS$SYSTEM:TCPIP$DAYTIME.EXE off the ODS-2 ACP, stages it to a
# tmpfs path and execve's it, and its RFC 867 line rides the executive socket back
# out. No auth, no privsep, no posture -- the minimal proof of the mechanism SSH
# (vms-843a) then rides.
#
# This asserts a REAL cold boot: the shipped image opens no port; only this
# test-overlay build (OVMX_TEST_ENABLE_TCPIP=1) auto-starts TCP/IP.
set -u
FAIL=0
ok(){  echo "  PASS: $*"; }
bad(){ echo "  FAIL: $*"; FAIL=1; }

# Boot artifacts the bootable image ships (distro/Dockerfile.bootable runner stage).
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
[ -f "$INITRD" ] || INITRD=/boot/initramfs-ovmx-slim.cpio.gz
IMG=/boot/ovmx-distrib.img
for f in "$KERNEL" "$INITRD" "$IMG"; do
    [ -f "$f" ] || { echo "FATAL: missing boot artifact $f (image /boot layout)"; ls -la /boot 2>/dev/null; exit 1; }
done

DISK=/tmp/daytime-e2e.img
cp "$IMG" "$DISK"                       # writable copy for -drive
LOG=/tmp/daytime-e2e-console.log
HOSTPORT=2213                           # host side of the SLIRP forward -> guest :13
rm -f "$LOG"

ACCEL="-accel tcg"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

# Hand-rolled QEMU (same shape as test_release_acceptance_e2e.sh) but with a
# virtio-net NIC + inbound hostfwd instead of `-nic none`.
qemu-system-x86_64 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "console=ttyS0 loglevel=3 quiet" \
    -m 512M -smp 2 $ACCEL \
    -netdev "user,id=net0,hostfwd=tcp::${HOSTPORT}-:13" \
    -device virtio-net-pci,netdev=net0 \
    -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot </dev/null >"$LOG" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT

wait_for(){ # pattern limit-seconds
    local pat="$1" lim="$2" i=0
    while [ "$i" -lt "$lim" ]; do
        grep -qF "$pat" "$LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 1     # qemu died
        sleep 2; i=$((i+2))
    done
    return 1
}

if wait_for '%OVMX-I-EXEC' 120; then
    ok "executive attached on a real cold boot (vms.ko)"
else
    bad "executive never attached within 120s"
fi

# The test-overlay SYSTARTUP brings the NIC up (TCPIP$CONFIG) and starts the aux
# server (TCPIP$STARTUP) during LPMAIN, before the login prompt.
if wait_for '%TCPIP-I-AUXSTARTED' 180; then
    ok "aux server (TCPIP\$INETD) started detached on cold boot"
else
    bad "aux server never started (TCPIP\$CONFIG / TCPIP\$STARTUP) -- see console tail"
fi

sleep 3                                 # let the listener bind :13 over BGn:

# Connect inbound (SLIRP-forwarded) and read the one-line RFC 867 response.
RESP=""
if exec 3<>/dev/tcp/127.0.0.1/"$HOSTPORT" 2>/dev/null; then
    RESP=$(timeout 10 cat <&3 2>/dev/null)
    exec 3<&- 3>&- 2>/dev/null || true
else
    bad "could not connect inbound to guest :13 via hostfwd 127.0.0.1:${HOSTPORT}"
fi
echo "  daytime response: [$RESP]"

# RFC 867 ctime-style line: "Www Mmm dd hh:mm:ss yyyy" (tcpip_daytime.h RFC867_FMT).
if printf '%s' "$RESP" | grep -qE '(Mon|Tue|Wed|Thu|Fri|Sat|Sun) [A-Z][a-z][a-z] +[0-9]+ [0-9][0-9]:[0-9][0-9]:[0-9][0-9] [0-9]{4}'; then
    ok "DAYTIME answered inbound with an RFC 867 ctime-style line -- TCPIP\$INETD launched TCPIP\$DAYTIME.EXE (SYS\$SYSTEM: ACP-stage -> execve) on a cold boot, bytes over the executive BGn: socket (vms-21b)"
else
    bad "daytime response did not match the RFC 867 ctime-style shape"
fi

if [ "$FAIL" != 0 ]; then
    echo "--- console tail ---"; tail -60 "$LOG" 2>/dev/null
fi
echo "=== test_tcpip_daytime_boot_e2e: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL) ==="
exit "$FAIL"
