#!/bin/bash
# test_virtio_nic.sh - virtio-net NIC on the OVMX QEMU runtime (vms-7bd).
#
# Parent: vms-67f (TCP/IP Services for OVMX). This item is the LINUX-LAYER NIC
# infra ONLY -- the VMS device face (EWAn:) is a separate downstream item.
#
# WHAT THIS PROVES
#   1. The runtime launcher (distro/boot/run-qemu.sh) attaches exactly one
#      virtio-net-pci device, defaulting to user-mode NAT (SLIRP) so it needs
#      ZERO host config and runs unprivileged / in CI, keeps -nodefaults, and
#      adds NO inbound (no hostfwd) by default. Opt-in tap/bridge modes emit
#      the right -netdev without the launcher ever touching host networking.
#      (Deterministic, no boot -- uses the launcher's OVMX_QEMU_DRYRUN hook.)
#   2. When boot artifacts are present (the ovmx-boot container / CI runner),
#      a real boot with the default user-mode NIC has the GUEST KERNEL
#      enumerate the virtio-net Ethernet controller (PCI 1af4:1000/1041,
#      class 0x020000), the PXE option ROM is suppressed (no boot pause), and
#      the executive still attaches -- i.e. adding the NIC did not break boot.
#
# WHY loglevel is raised for the boot check: the shipped runtime boots with
# `quiet loglevel=3`, which SUPPRESSES the kernel's KERN_INFO PCI-probe lines.
# The enumeration itself is identical regardless of loglevel; the test raises
# it only to make the already-happening enumeration observable on the console.
# The guest kernel has virtio_net BUILT IN (CONFIG_VIRTIO_NET=y, verified from
# the shipped vmlinux .modinfo), and virtio_net is silent on a successful probe
# (unlike virtio_blk), so the PCI enumeration line is the honest guest-side
# ground-source available without a guest shell in this minimal initramfs.
#
# GAPS (documented, not faked): asserting the netdev INTERFACE is up
# (`ip link`), and that user-mode NAT actually reaches the host gateway,
# needs an in-guest introspection channel this bootstrap initramfs does not
# have (init is STARTUP.EXE; no busybox/dhclient) -- that arrives with the
# EWAn: VMS device face + TCP/IP stack (downstream vms-3be/vms-9d2). tap and
# bridge modes need a privileged, operator-provisioned host tap/bridge and so
# cannot be exercised in unprivileged CI; only their arg construction is tested.
#
# Usage (deterministic arg checks, runs anywhere with bash + qemu):
#   tests/qemu/test_virtio_nic.sh
# Usage (full, inside the ovmx-boot image, adds the real-boot enumeration):
#   docker run --rm -v $PWD/tests/qemu/test_virtio_nic.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit 0 = all executed checks pass (skipped checks do not fail the run).

set -uo pipefail

PASS=0
FAIL=0
SKIP=0
TOTAL=0

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

# Locate the launcher relative to this test (repo checkout) or absolute (CI).
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
RUN_QEMU=""
for cand in "$SELF_DIR/../../distro/boot/run-qemu.sh" /src/distro/boot/run-qemu.sh; do
    if [ -f "$cand" ]; then RUN_QEMU="$(cd "$(dirname "$cand")" && pwd)/$(basename "$cand")"; break; fi
done

echo "=== OVMX virtio-net NIC test (vms-7bd) ==="
echo "Launcher: ${RUN_QEMU:-<not found>}"
echo ""

# ---------------------------------------------------------------------------
# Part 1: launcher argument construction (deterministic, no boot)
# ---------------------------------------------------------------------------
echo "--- Part 1: run-qemu.sh NIC argument construction (dry-run) ---"
if [ -z "$RUN_QEMU" ]; then
    skip "run-qemu.sh not found -- cannot check launcher args"
else
    # run-qemu.sh requires a kernel + initrd to exist before it builds args.
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    : >"$TMP/vmlinuz"
    : >"$TMP/initrd"

    # Pass NIC env vars via `env` (VAR=val args) -- an expanded "$@" is NOT
    # treated as assignment prefixes by the shell, so `env` is the correct tool.
    dryrun() { env OVMX_QEMU_DRYRUN=1 "$@" bash "$RUN_QEMU" "$TMP/vmlinuz" "$TMP/initrd" 2>&1; }
    # Join argv tokens with spaces for substring assertions.
    joined() { dryrun "$@" | tr '\n' ' '; }

    # Default = user mode.
    DEF="$(joined)"
    if printf '%s' "$DEF" | grep -q -- '-device virtio-net-pci,netdev=net0,romfile='; then rc=0; else rc=1; fi
    record "default: attaches virtio-net-pci (netdev=net0, PXE ROM disabled)" "$rc"

    if printf '%s' "$DEF" | grep -q -- '-netdev user,id=net0'; then rc=0; else rc=1; fi
    record "default: user-mode NAT (SLIRP), zero host config" "$rc"

    # No inbound by default (no hostfwd).
    if printf '%s' "$DEF" | grep -q 'hostfwd'; then rc=1; else rc=0; fi
    record "default: NO inbound (no hostfwd)" "$rc"

    # -nic none is gone from the default; -nodefaults preserved.
    if printf '%s' "$DEF" | grep -q -- '-nic none'; then rc=1; else rc=0; fi
    record "default: legacy '-nic none' removed" "$rc"

    if printf '%s' "$DEF" | grep -q -- '-nodefaults'; then rc=0; else rc=1; fi
    record "default: -nodefaults preserved" "$rc"

    # Opt-in user-mode inbound forwarding.
    FWD="$(joined OVMX_NET_HOSTFWD=tcp::2223-:23)"
    if printf '%s' "$FWD" | grep -q 'hostfwd=tcp::2223-:23'; then rc=0; else rc=1; fi
    record "OVMX_NET_HOSTFWD: opt-in inbound forwarding" "$rc"

    # tap mode.
    TAP="$(joined OVMX_NET_MODE=tap OVMX_NET_TAP=tap9)"
    if printf '%s' "$TAP" | grep -q -- '-netdev tap,id=net0,ifname=tap9,script=no,downscript=no'; then rc=0; else rc=1; fi
    record "tap mode: attaches to pre-existing tap, script=no (no host mutation)" "$rc"
    if printf '%s' "$TAP" | grep -q -- '-device virtio-net-pci,netdev=net0'; then rc=0; else rc=1; fi
    record "tap mode: still a virtio-net-pci device" "$rc"

    # bridge mode.
    BR="$(joined OVMX_NET_MODE=bridge OVMX_NET_BRIDGE=br9)"
    if printf '%s' "$BR" | grep -q -- '-netdev bridge,id=net0,br=br9'; then rc=0; else rc=1; fi
    record "bridge mode: attaches to existing host bridge" "$rc"

    # none escape hatch.
    NONE="$(joined OVMX_NET_MODE=none)"
    if printf '%s' "$NONE" | grep -q -- '-nic none'; then rc=0; else rc=1; fi
    if printf '%s' "$NONE" | grep -q 'virtio-net'; then rc=1; fi
    record "none mode: no NIC (legacy behavior restored)" "$rc"

    # explicit MAC.
    MAC="$(joined OVMX_NET_MAC=52:54:00:12:34:56)"
    if printf '%s' "$MAC" | grep -q 'mac=52:54:00:12:34:56'; then rc=0; else rc=1; fi
    record "OVMX_NET_MAC: honored" "$rc"

    # bad mode is rejected.
    if dryrun OVMX_NET_MODE=bogus >/dev/null 2>&1; then rc=1; else rc=0; fi
    record "unknown OVMX_NET_MODE rejected" "$rc"
fi
echo ""

# ---------------------------------------------------------------------------
# Part 2: Dockerfile.bootable runtime CMD carries the same NIC (static)
# ---------------------------------------------------------------------------
echo "--- Part 2: Dockerfile.bootable runtime CMD (static) ---"
DOCKERFILE=""
for cand in "$SELF_DIR/../../distro/Dockerfile.bootable" /src/distro/Dockerfile.bootable; do
    [ -f "$cand" ] && DOCKERFILE="$cand" && break
done
if [ -z "$DOCKERFILE" ]; then
    skip "Dockerfile.bootable not found"
else
    if grep -q 'virtio-net-pci,\$DEV_OPTS' "$DOCKERFILE"; then rc=0; else rc=1; fi
    record "runtime CMD attaches virtio-net-pci" "$rc"
    if grep -q 'OVMX_NET_MODE' "$DOCKERFILE"; then rc=0; else rc=1; fi
    record "runtime CMD honors OVMX_NET_MODE" "$rc"
    # The default path must not hardwire -nic none (only the 'none' mode does).
    if grep -qE '^\s*-nic none' "$DOCKERFILE"; then rc=1; else rc=0; fi
    record "runtime CMD default is not a hardwired -nic none" "$rc"
fi
echo ""

# ---------------------------------------------------------------------------
# Part 3: real boot -- guest kernel enumerates the virtio-net device
# ---------------------------------------------------------------------------
echo "--- Part 3: real boot, guest enumerates the NIC ---"
KERNEL="${KERNEL:-/boot/vmlinuz}"
INITRD=""
for cand in "${INITRD_OVERRIDE:-}" /boot/initramfs-ovmx-slim.cpio.gz /boot/initramfs-ovmx.cpio.gz; do
    [ -n "$cand" ] && [ -f "$cand" ] && INITRD="$cand" && break
done
DISTRIB_IMG=/boot/ovmx-distrib.img

ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64; MACHINE=""; CONSOLE="console=ttyS0"
fi

if ! command -v "$QEMU" >/dev/null 2>&1; then
    skip "$QEMU not installed -- real-boot enumeration check needs the ovmx-boot image / CI runner"
elif [ ! -f "$KERNEL" ] || [ -z "$INITRD" ]; then
    skip "boot artifacts absent ($KERNEL / initramfs) -- run inside the ovmx-boot image / CI"
else
    DISK="$(mktemp -d)/nic-sysdisk.img"
    if [ -f "$DISTRIB_IMG" ]; then cp "$DISTRIB_IMG" "$DISK"; else truncate -s 64M "$DISK"; fi
    CONSOLE_LOG="$(mktemp)"
    echo "  kernel=$KERNEL initrd=$INITRD disk=$DISK"
    # Mirror run-qemu.sh's DEFAULT user-mode NIC, but raise loglevel so the
    # KERN_INFO PCI-probe line (suppressed by the shipped `quiet loglevel=3`)
    # becomes observable. romfile= mirrors the launcher (no PXE pause).
    # shellcheck disable=SC2086
    timeout 90 "$QEMU" $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$INITRD" \
        -nographic \
        -append "$CONSOLE loglevel=7 ignore_loglevel" \
        -m 512M -smp 2 \
        -netdev user,id=net0 \
        -device virtio-net-pci,netdev=net0,romfile= \
        -nodefaults \
        -serial mon:stdio \
        -drive file="$DISK",format=raw,if=virtio \
        -no-reboot >"$CONSOLE_LOG" 2>&1
    rc_boot=$?
    echo "  (qemu exited rc=$rc_boot, $(wc -l <"$CONSOLE_LOG") console lines)"

    # Guest kernel enumerated a virtio Ethernet controller (class 0x020000,
    # vendor 1af4 device 1000 legacy or 1041 modern).
    if grep -aE '1af4:(1000|1041).*class 0x020000|class 0x020000.*1af4:(1000|1041)' "$CONSOLE_LOG" >/dev/null \
       || grep -aE '\[1af4:(1000|1041)\]' "$CONSOLE_LOG" | grep -aq 'class 0x020000'; then rc=0; else rc=1; fi
    record "guest kernel enumerates the virtio-net Ethernet controller (PCI 1af4, class 0x020000)" "$rc"
    if [ "$rc" -ne 0 ]; then
        echo "    --- PCI/virtio lines seen ---"
        grep -aiE 'pci .*1af4|virtio|class 0x02' "$CONSOLE_LOG" | sed 's/^/    /' | head -20
    fi

    # PXE option ROM disabled (romfile=): no iPXE boot prompt / pause.
    if grep -aqi 'iPXE\|Press Ctrl-B' "$CONSOLE_LOG"; then rc=1; else rc=0; fi
    record "PXE option ROM suppressed (no network-boot pause)" "$rc"

    # NIC did not break boot: the executive still attaches.
    if grep -aq '%OVMX-I-EXEC' "$CONSOLE_LOG"; then rc=0; else rc=1; fi
    record "executive still attaches with the NIC present (no boot regression)" "$rc"

    rm -f "$CONSOLE_LOG"; rm -rf "$(dirname "$DISK")"
fi
echo ""

echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL passed, $FAIL failed, $SKIP skipped"
echo "=========================================="
[ "$FAIL" -eq 0 ]
