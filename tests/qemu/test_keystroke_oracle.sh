#!/bin/sh
# test_keystroke_oracle.sh - the keystroke oracle on a booted OVMX (rd vms-370).
#
# Boots the REAL runtime (the real vms.ko executive) and plays every keystroke
# case script (tools/oracle/keystroke/cases/*.ks) into its console, OPA0:, with
# the same timing the scripts were played into the console of real OpenVMS VAX
# V7.3 / Alpha V8.4 nodes. Then diffs what the terminal showed, step by step,
# against those goldens (docs/oracle/keystroke/<CASE>/) through the known-diff
# ratchet (docs/oracle/keystroke/known-diff.txt): a difference that is not
# listed fails, and so does a listed one that now matches.
#
# Runs on the CI HOST (python3; stdlib only). The guest runs in the ovmx-boot
# image -- `docker run -i` whose stdin/stdout IS the serial line -- or, with
# KS_BOOT_DIR=<dir holding vmlinuz, initramfs-ovmx-slim.cpio.gz,
# ovmx-distrib.img>, under the host's own qemu-system-x86_64 (off-CI use with
# the build-boot-artifacts artifact). The disk is a scratch copy either way.
#
#   tests/qemu/test_keystroke_oracle.sh [OUT_DIR]
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="${1:-${OUT_DIR:-keystroke-out}}"
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot:latest}"
KS="$ROOT/tools/oracle/keystroke"

QEMU_ARGS='-nographic -append "console=ttyS0 loglevel=3 quiet" -m 512M -smp 2 -nic none -nodefaults -serial stdio -no-reboot'

if [ -n "${KS_BOOT_DIR:-}" ]; then
    W=$(mktemp -d)
    trap 'rm -rf "$W"' EXIT
    cp "$KS_BOOT_DIR/ovmx-distrib.img" "$W/disk.img"
    ACCEL=""
    [ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
    set -- sh -c "exec qemu-system-x86_64 $ACCEL -kernel '$KS_BOOT_DIR/vmlinuz' \
        -initrd '$KS_BOOT_DIR/initramfs-ovmx-slim.cpio.gz' $QEMU_ARGS \
        -drive file='$W/disk.img',format=raw,if=virtio,cache=writethrough"
else
    # The container's own scratch copy of the distribution disk; KVM when the
    # runner passes /dev/kvm through (.github/actions/enable-kvm), TCG if not.
    set -- docker run --rm -i --entrypoint sh "$IMAGE" -c \
        "cp /boot/ovmx-distrib.img /tmp/disk.img && A='' && { [ -w /dev/kvm ] && A='-enable-kvm -cpu host'; } ; \
         exec qemu-system-x86_64 \$A -kernel /boot/vmlinuz -initrd /boot/initramfs-ovmx-slim.cpio.gz $QEMU_ARGS \
         -drive file=/tmp/disk.img,format=raw,if=virtio,cache=writethrough"
fi

echo "=== OVMX keystroke oracle (vms-370): play $(ls "$KS"/cases/*.ks | wc -l) cases into OPA0: ==="
mkdir -p "$OUT"
python3 "$KS/ksplay.py" run --transport spawn --user SYSTEM --password MANAGER \
    --out "$OUT" --label "OVMX OPA0:" --boot-wait 240 -- "$@"
PLAY=$?
# A case that could not reach its start state still leaves a transcript; the
# diff judges it. Only a player crash (no transcripts at all) is fatal here.
ls "$OUT"/*.ks.txt >/dev/null 2>&1 || { echo "FATAL: ksplay produced no transcript (exit $PLAY)"; exit 1; }
python3 "$KS/ksdiff.py" "$OUT" --report
