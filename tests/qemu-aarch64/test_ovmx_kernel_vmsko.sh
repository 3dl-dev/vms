#!/bin/bash
# test_ovmx_kernel_vmsko.sh -- the OVMX arm64 kernel boots and its vms.ko gives
# /dev/vms (rd vms-a07, epic vms-0ad0: aarch64 ships in V1.0).
#
#   tests/qemu-aarch64/test_ovmx_kernel_vmsko.sh <dir with vmlinuz + vms.ko>
#
# The kernel is distro/Dockerfile.bootable's kernel-build stage built with
# TARGETARCH=arm64: the pinned 6.12.103 source, the OVMX exit/exec notifier
# patches, ovmx-arm64.config, and vms.ko built IN-TREE and signed. It boots
# under qemu-system-aarch64 -machine virt (TCG) in a container, with PID 1 a
# busybox-arm64 script (kernel_vmsko_init.sh). Two boots:
#   load   : insmod vms.ko -> /dev/vms MUST exist, the kernel MUST be untainted
#            (in-tree + signed), and the guest MUST report machine=aarch64.
#   noload : the same boot without the module -> /dev/vms MUST be absent
#            (negative control: the /dev/vms check has teeth).
# Containerized; nothing is installed on the host.
set -euo pipefail
K=${1:?usage: test_ovmx_kernel_vmsko.sh <dir with vmlinuz + vms.ko>}
K=$(cd "$K" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
[ -f "$K/vmlinuz" ] && [ -f "$K/vms.ko" ] || { echo "need $K/vmlinuz and $K/vms.ko"; exit 2; }
docker run --rm -v "$K:/k:ro" -v "$HERE:/h:ro" debian:trixie bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
dpkg --add-architecture arm64
apt-get update -qq -o Acquire::Retries=5
apt-get install -qq -y --no-install-recommends -o Acquire::Retries=5 qemu-system-arm cpio gzip file ca-certificates >/dev/null
T=$(mktemp -d); cd "$T"
apt-get download -qq busybox-static:arm64
mkdir -p r/bin r/dev r/proc r/sys
dpkg-deb -x busybox-static_*arm64.deb bb
cp "$(ls bb/usr/bin/busybox bb/bin/busybox 2>/dev/null | head -1)" r/bin/busybox
grep -q "ARM aarch64" <<<"$(file r/bin/busybox)" || { echo "busybox is not arm64"; exit 3; }
cp /h/kernel_vmsko_init.sh r/init; chmod 755 r/init
cp /k/vms.ko r/vms.ko
( cd r && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > initrd.gz
file /k/vmlinuz
fail=0
for mode in load noload; do
  echo "== boot ($mode) =="
  OUT=$(timeout 600 qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 512M -smp 1 \
        -kernel /k/vmlinuz -initrd initrd.gz \
        -append "console=ttyAMA0 panic=-1 rdinit=/init ovmxmode=$mode" \
        -nographic -no-reboot -nic none -nodefaults -serial mon:stdio 2>&1 | tr -d "\r" || true)
  printf "%s\n" "$OUT" | grep -a "OVMX_A64\|vms:\|Kernel panic\|Unable to\|BUG\|Oops" | head -20
  grep -q "OVMX_A64_KERNEL .*machine=aarch64" <<<"$OUT" || { echo "FAIL[$mode]: guest did not report machine=aarch64"; fail=1; }
  if [ $mode = load ]; then
    grep -q "OVMX_A64_VMSKO_LOADED" <<<"$OUT" || { echo "FAIL[load]: vms.ko did not load"; fail=1; }
    grep -q "OVMX_A64_DEVVMS_PRESENT" <<<"$OUT" || { echo "FAIL[load]: /dev/vms absent after loading vms.ko"; fail=1; }
    grep -q "OVMX_A64_TAINTED=0$" <<<"$OUT" || { echo "FAIL[load]: kernel tainted after loading vms.ko (expected in-tree + signed)"; fail=1; }
  else
    grep -q "OVMX_A64_DEVVMS_ABSENT" <<<"$OUT" || { echo "FAIL[noload]: /dev/vms present without vms.ko -- the check has no teeth"; fail=1; }
  fi
done
[ $fail = 0 ] && echo "PASS: OVMX arm64 kernel boots, vms.ko loads (in-tree, signed, untainted), /dev/vms exists; absent without it"
exit $fail
'
