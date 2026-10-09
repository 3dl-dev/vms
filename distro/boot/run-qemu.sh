#!/bin/bash
# Boot OVMX in QEMU from extracted kernel + initramfs.
#
# Usage:
#   ./distro/boot/run-qemu.sh [kernel] [initrd]
#
# Environment variables:
#   MEMORY     - Guest RAM (default: 512M)
#   DISK       - Path to system disk image (optional; passed as /dev/vda if set)
#   OVMX_QEMU_ACCEL - x86_64 only: force the accelerator ("kvm" or "tcg")
#                instead of auto-detecting /dev/kvm. Forcing tcg on a
#                KVM-capable host reproduces the software-emulation timing the
#                browser demo and KVM-less CI runners boot under (rd vms-4ff);
#                forcing kvm fails loudly if /dev/kvm is unusable. Unset (the
#                default) keeps the auto-detect below.
#   BOOT_FLAGS - Conversational boot (vms-b81): "R5,R6" appended to the
#                kernel cmdline as ovmx.flags=R5,R6. Bit 0 of R6 is the
#                conversational bit -- BOOT_FLAGS=0,1 halts STARTUP.EXE at
#                a SYSBOOT> prompt before the executive attaches, matching
#                the oracle's own `boot -flags 0,1` example
#                (docs/design-boot-faithful.md §2.2/§3.1). Unset (the
#                default) boots straight through -- SYSBOOT> never appears.
#
# Networking (vms-7bd, parent vms-67f TCP/IP Services). The guest gets ONE
# virtio-net NIC. The guest kernel (ubuntu generic) has virtio_net built in
# (CONFIG_VIRTIO_NET=y), so the interface enumerates with no in-guest module
# load; this is the LINUX-LAYER NIC only -- the VMS device face (EWAn:) is a
# separate downstream item. The PXE option ROM is disabled (romfile=) so the
# guest never pauses to attempt a network boot.
#   OVMX_NET_MODE  - user  (DEFAULT) : user-mode NAT (SLIRP). Zero host config,
#                                      works unprivileged / in CI. NO inbound
#                                      by default (no hostfwd) -- outbound NAT
#                                      to the host/gateway only.
#                    tap             : attach to a PRE-EXISTING host tap device
#                                      (OVMX_NET_TAP, default tap0). script=no,
#                                      so QEMU never runs a host up/down script.
#                                      The operator creates and bridges the tap;
#                                      this launcher never touches host networking.
#                    bridge          : attach via qemu-bridge-helper to an
#                                      EXISTING host bridge (OVMX_NET_BRIDGE,
#                                      default br0). Needs a setuid bridge helper
#                                      + an allow entry in the host's
#                                      /etc/qemu/bridge.conf; operator-provisioned.
#                    none            : no NIC at all (legacy -nic none behavior).
#   OVMX_NET_TAP   - tap ifname for OVMX_NET_MODE=tap    (default: tap0)
#   OVMX_NET_BRIDGE- bridge name for OVMX_NET_MODE=bridge (default: br0)
#   OVMX_NET_MAC   - guest NIC MAC (default: QEMU-assigned 52:54:00:xx:xx:xx)
#   OVMX_NET_HOSTFWD - user-mode ONLY: extra -netdev hostfwd rules for
#                      deliberate inbound (e.g. "tcp::2223-:23"). Empty by
#                      default -- opt-in, never inbound-by-default.
#
#   tap and bridge need host privileges / operator-provisioned host state and
#   will NOT work in unprivileged CI; user mode is the safe default. This
#   launcher never creates a tap or a bridge as a side effect.
#
# Testing hook:
#   OVMX_QEMU_DRYRUN=1 - print the fully-assembled QEMU argv (one token per
#                        line) and exit 0 instead of exec'ing QEMU. Lets a test
#                        assert the launch args deterministically without a boot.
#
# Initramfs variants:
#   initramfs-ovmx.cpio.gz       — fat: all binaries (first boot / install)
#   initramfs-ovmx-slim.cpio.gz  — slim: bootstrap only (needs system disk)

KERNEL="${1:-dist/boot/vmlinuz}"
INITRD="${2:-dist/boot/initramfs-ovmx.cpio.gz}"
MEMORY="${MEMORY:-512M}"
ARCH=$(uname -m)

if [ ! -f "$KERNEL" ]; then
    echo "Error: kernel not found at $KERNEL" >&2
    echo "Build first: ./boot.sh" >&2
    exit 1
fi

if [ ! -f "$INITRD" ]; then
    echo "Error: initramfs not found at $INITRD" >&2
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
    # x86-only concern: `machine virt` has no 8254 and no IO-APIC, so there is
    # no check_timer() to skip here (rd vms-4ff).
    TIMER_CHECK=""
else
    QEMU=qemu-system-x86_64
    # KVM acceleration (vms-fb8): use hardware virt when the runner exposes a
    # writable /dev/kvm -- GitHub-hosted Linux runners do, and self-hosted
    # bare-metal does. Without it QEMU falls back to TCG software emulation:
    # identical behavior, ~10x slower (the boot/e2e wall this change exists to
    # cut). Only x86_64-on-x86_64 can use KVM; the aarch64 guest above is always
    # cross-emulated on our x86_64 runners, so it stays TCG.
    ACCEL="${OVMX_QEMU_ACCEL:-}"
    if [ -z "$ACCEL" ]; then
        if [ -w /dev/kvm ]; then ACCEL=kvm; else ACCEL=tcg; fi
    fi
    case "$ACCEL" in
        kvm) MACHINE="-accel kvm -cpu host" ;;
        tcg) MACHINE="-accel tcg" ;;
        *)   echo "Error: unknown OVMX_QEMU_ACCEL='$ACCEL' (want: kvm|tcg)" >&2; exit 1 ;;
    esac
    CONSOLE="console=ttyS0"

    # TCG ONLY: skip the kernel's 8254-to-IO-APIC verification (rd vms-4ff).
    #
    # arch/x86/kernel/apic/io_apic.c check_timer() verifies the legacy timer
    # IRQ by calling timer_irq_works(), which spins in delay_with_tsc() for
    # 40e9/HZ TSC CYCLES and then demands that jiffies advanced by more than 4.
    # Under TCG the guest TSC advances at HOST WALL-CLOCK rate while the vCPU
    # executes orders of magnitude slower than real silicon, so the test asks a
    # software-emulated CPU to take and service 5 IRQ0 ticks inside ~18 ms of
    # WALL time. A slowed or descheduled vCPU cannot, and the kernel concludes
    # its own (perfectly good, fully emulated) timer is broken.
    #
    # MEASURED on the shipped V0.7-8 kernel, same image, same arguments, host
    # qemu 8.2.2 + TCG, qemu pinned to one core shared with N spinners
    # (tests/lab/captures/vms-4ff-timer-check-20261009/):
    #   unstarved   -- check passes, IRQ0 stays on IO-APIC pin 2, boots
    #   ~6x and ~8x -- "..MP-BIOS bug: 8254 timer not connected to IO-APIC",
    #                  the IO-APIC pin is TORN DOWN and IRQ0 silently falls
    #                  back to Virtual Wire: a load-dependent interrupt
    #                  topology from one unchanged image
    #   ~12x        -- all four routes "failed" -> panic "IO-APIC + timer
    #                  doesn't work!" at guest t=0.012 s, never boots
    #   ~12x + this flag -- boots (mount 69 s, STARTUP 108 s)
    #
    # This is upstream Linux's own policy for virtual machines, not an OVMX
    # workaround: arch/x86/kernel/kvm.c and arch/x86/kernel/cpu/vmware.c both
    # set `no_timer_check = 1` for guests they identify, because the check is
    # only meaningful on physical hardware. A TCG guest is a virtual machine
    # Linux cannot identify, so it must be told. Its ONLY consumer is
    # timer_irq_works() (io_apic.c), so the flag skips the MEASUREMENT and
    # changes nothing else -- the routing kept is the same IO-APIC pin a
    # correct measurement keeps.
    #
    # Deliberately NOT in distro/kernel/ovmx-x86_64.config: on BARE METAL
    # (where OVMX also boots) a genuinely miswired 8254 is a real fault and the
    # kernel must still catch it. It belongs to the emulated machine, which is
    # what this launcher is.
    if [ "$ACCEL" = tcg ]; then
        TIMER_CHECK=" no_timer_check"
    else
        # KVM guests get no_timer_check from the kernel itself (kvm.c
        # paravirt_ops_setup) -- passing it again would assert nothing.
        TIMER_CHECK=""
    fi
fi

# Build disk arguments if DISK is set
DISK_ARGS=""
if [ -n "$DISK" ]; then
    if [ ! -f "$DISK" ]; then
        echo "Error: disk image not found: $DISK" >&2
        exit 1
    fi
    DISK_ARGS="-drive file=$DISK,format=raw,if=virtio,cache=writeback"
fi

# loglevel=3 alone (vms-300): a trailing "quiet" here used to RAISE the
# console level back up (Linux's `quiet` cmdline flag sets console_loglevel
# to CONSOLE_LOGLEVEL_QUIET=4, looser than the loglevel=3 just to its left,
# so cmdline parsing order made "quiet" win) for the pre-PID1 window before
# ovmx_boot_mute_kernel_console() (src/ovmx_init/ovmx_boot_linux.c) gets a
# chance to run. loglevel=3 already restricts the console to EMERG/ALERT/
# CRIT, which is stricter than what "quiet" alone provides, so "quiet" is
# redundant once the ordering bug is gone -- belt-and-suspenders alongside
# that userspace mute, not the primary fix.
APPEND="$CONSOLE loglevel=3$TIMER_CHECK"
if [ -n "$BOOT_FLAGS" ]; then
    APPEND="$APPEND ovmx.flags=$BOOT_FLAGS"
fi

# --- virtio-net NIC (vms-7bd) ------------------------------------------------
# Build the -netdev/-device pair for the chosen mode. We keep -nodefaults, so
# nothing but this device is attached. romfile= disables the PXE option ROM so
# the guest does not pause to attempt a network boot.
NET_MODE="${OVMX_NET_MODE:-user}"
NET_MAC="${OVMX_NET_MAC:-}"
NET_ARGS=()
DEV_OPTS="netdev=net0,romfile="
if [ -n "$NET_MAC" ]; then
    DEV_OPTS="$DEV_OPTS,mac=$NET_MAC"
fi
case "$NET_MODE" in
    user)
        NETDEV="user,id=net0"
        if [ -n "${OVMX_NET_HOSTFWD:-}" ]; then
            NETDEV="$NETDEV,hostfwd=${OVMX_NET_HOSTFWD}"
        fi
        NET_ARGS=(-netdev "$NETDEV" -device "virtio-net-pci,$DEV_OPTS")
        ;;
    tap)
        NET_ARGS=(-netdev "tap,id=net0,ifname=${OVMX_NET_TAP:-tap0},script=no,downscript=no" \
                  -device "virtio-net-pci,$DEV_OPTS")
        ;;
    bridge)
        NET_ARGS=(-netdev "bridge,id=net0,br=${OVMX_NET_BRIDGE:-br0}" \
                  -device "virtio-net-pci,$DEV_OPTS")
        ;;
    none)
        NET_ARGS=(-nic none)
        ;;
    *)
        echo "Error: unknown OVMX_NET_MODE='$NET_MODE' (want: user|tap|bridge|none)" >&2
        exit 1
        ;;
esac

QEMU_CMD=("$QEMU" $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$APPEND" \
    -m "$MEMORY" \
    -smp 2 \
    "${NET_ARGS[@]}" \
    -nodefaults \
    -serial mon:stdio \
    -no-reboot \
    $DISK_ARGS)

if [ -n "${OVMX_QEMU_DRYRUN:-}" ]; then
    printf '%s\n' "${QEMU_CMD[@]}"
    exit 0
fi

exec "${QEMU_CMD[@]}"
