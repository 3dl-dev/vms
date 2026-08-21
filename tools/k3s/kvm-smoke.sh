#!/usr/bin/env bash
# tools/k3s/kvm-smoke.sh -- prove QEMU hardware acceleration (/dev/kvm) works
# in-pod on the rail. BUILD/TEST TOOLING ONLY (CLAUDE.md Rule 9).
#
# Run via: tools/k3s/run-on-rail.sh <ref> "bash tools/k3s/kvm-smoke.sh"
#
# It boots the builder image's stock kernel under QEMU with STRICT `-accel kvm`
# (no `:tcg` fallback -- QEMU aborts if KVM is unavailable, so a TCG fallback
# can never masquerade as a pass) and asserts the *guest* detected the KVM
# hypervisor. Linux prints "Hypervisor detected: KVM" only when it is actually
# running on KVM-accelerated vCPUs, so that line is positive proof the vCPU is
# KVM-backed -- not merely that /dev/kvm exists.

set -euo pipefail

echo "=== /dev/kvm ==="
ls -l /dev/kvm

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64)  QEMU=qemu-system-x86_64;  MACHINE=q35 ;;
  aarch64) QEMU=qemu-system-aarch64; MACHINE=virt ;;
  *) echo "kvm-smoke: unsupported arch $ARCH" >&2; exit 2 ;;
esac
command -v "$QEMU" >/dev/null || { echo "kvm-smoke: $QEMU not installed" >&2; exit 127; }

KERNEL=/boot/vmlinuz
[ -e "$KERNEL" ] || KERNEL="$(ls -1 /boot/vmlinuz-* 2>/dev/null | sort -V | tail -1)"
[ -n "$KERNEL" ] && [ -e "$KERNEL" ] || { echo "kvm-smoke: no kernel image found" >&2; exit 1; }
echo "=== kernel: $KERNEL ==="

echo "=== accelerators QEMU sees ==="
"$QEMU" -accel help 2>&1 | sed 's/^/  /'

LOG="$(mktemp)"
echo "=== booting under STRICT -accel kvm (no tcg fallback) ==="
# No rootfs is staged: the kernel boots the vCPU (the point -- proves KVM), then
# panics unable to mount root. panic=-1 + -no-reboot make it exit promptly.
# STRICT `-accel kvm`: if KVM were unavailable QEMU would abort here, non-zero.
timeout 90 "$QEMU" \
  -accel kvm \
  -M "$MACHINE" \
  -cpu host \
  -m 512 \
  -smp 2 \
  -nographic -no-reboot \
  -kernel "$KERNEL" \
  -append "console=ttyS0 panic=-1 loglevel=8" \
  ${QEMU_EXTRA:-} \
  > "$LOG" 2>&1 || true    # kernel panic (no root) is expected; we grade the log

echo "=== guest serial (head) ==="
sed -n '1,40p' "$LOG"

echo "======================================================================"
if grep -qi "Hypervisor detected: KVM" "$LOG"; then
  echo "PASS: guest detected KVM hypervisor -- vCPU is KVM-accelerated (not TCG)."
  exit 0
fi

# Fallback positive signal: some kernels word it differently; accept an
# explicit kvm-clock / KVM paravirt line too. But NEVER accept a TCG boot.
if grep -qiE "kvm-clock|kvm-guest|KVM setup" "$LOG"; then
  echo "PASS: guest shows KVM paravirt (kvm-clock/kvm-guest) -- KVM-accelerated."
  exit 0
fi

echo "FAIL: no KVM hypervisor signal in guest boot -- acceleration not proven."
echo "----- full log -----"
cat "$LOG"
exit 1
