#!/bin/bash
# test_qemu_timer_check_cmdline.sh - the emulated-x86 boot path tells the kernel
# not to verify the 8254-to-IO-APIC wiring (rd vms-4ff).
#
# WHY THIS GATE EXISTS
#   arch/x86/kernel/apic/io_apic.c check_timer() verifies the legacy timer IRQ
#   with timer_irq_works(), which spins in delay_with_tsc() for 40e9/HZ TSC
#   CYCLES and then demands jiffies advanced by more than 4. Under TCG the guest
#   TSC advances at HOST WALL-CLOCK rate while the vCPU runs orders of magnitude
#   slower than silicon, so the test asks a software-emulated CPU to service 5
#   IRQ0 ticks inside ~18 ms of wall time. MEASURED on the shipped kernel
#   (tests/lab/captures/vms-4ff-timer-check-20261009/): at ~6x slowdown the
#   kernel tears the IO-APIC pin down and silently re-routes IRQ0 to Virtual
#   Wire; at ~12x all four routes fail and it panics
#   "IO-APIC + timer doesn't work!" before OVMX prints a single line. With
#   no_timer_check the same image boots.
#
#   This is upstream Linux's own policy for virtual machines -- kvm.c and
#   cpu/vmware.c set no_timer_check = 1 for guests they identify -- and a TCG
#   guest is a VM Linux cannot identify, so the launcher must say it.
#
# WHAT IS ASSERTED (deterministic, no boot: OVMX_QEMU_DRYRUN + OVMX_QEMU_ACCEL)
#   1. the x86_64 TCG arm carries no_timer_check on the kernel command line
#   2. the x86_64 KVM arm does NOT (the kernel sets it itself under KVM; passing
#      it there would assert nothing, and the asymmetry must stay visible)
#   3. an unknown OVMX_QEMU_ACCEL is refused, not silently defaulted
#   4. the flag is NOT baked into the kernel config -- on BARE METAL a miswired
#      8254 is a real fault the kernel must still catch
#
# Usage: test_qemu_timer_check_cmdline.sh [source-dir]

set -uo pipefail

SRC="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
RUN_QEMU="$SRC/distro/boot/run-qemu.sh"
KCONFIG="$SRC/distro/kernel/ovmx-x86_64.config"

PASS=0
FAIL=0

record() {
    local desc="$1" rc="$2"
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}

# The launcher insists on a real kernel + initrd before it assembles argv, and
# it picks its machine from `uname -m`. A `uname` shim on PATH lets this gate
# exercise the x86_64 branch from any host (the same mocking tests/lab's
# test_lab2run_staging.sh does for kubectl) -- the gate is about the ARGUMENTS,
# which are pure text, not about what this machine can boot.
make_stubs() {
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    : >"$TMP/vmlinuz"
    : >"$TMP/initrd"
    mkdir -p "$TMP/bin"
    cat >"$TMP/bin/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in
    -m) echo x86_64 ;;
    *)  exec /usr/bin/uname "$@" ;;
esac
EOF
    chmod +x "$TMP/bin/uname"
}

# Run the launcher's dry-run for one accelerator, as x86_64.
dryrun() {
    local launcher="$1" accel="$2"
    env "PATH=$TMP/bin:$PATH" OVMX_QEMU_DRYRUN=1 OVMX_QEMU_ACCEL="$accel" \
        bash "$launcher" "$TMP/vmlinuz" "$TMP/initrd" 2>&1
}

# The fully-assembled argv for one accelerator, as one line.
dryrun_argv() { dryrun "$RUN_QEMU" "$1" | tr '\n' ' '; }

# The -append value only (the kernel command line), not the rest of argv.
dryrun_cmdline() { dryrun "$RUN_QEMU" "$1" | awk '/^-append$/ { getline; print; exit }'; }

check_tcg_arm_has_flag() {
    local cmdline; cmdline="$(dryrun_cmdline tcg)"
    echo "    tcg cmdline: $cmdline"
    printf '%s' "$cmdline" | grep -qw 'no_timer_check'
    record "x86_64 TCG arm passes no_timer_check" $?
    printf '%s' "$(dryrun_argv tcg)" | grep -q -- '-accel tcg'
    record "x86_64 TCG arm really selects TCG" $?
}

check_kvm_arm_omits_flag() {
    local cmdline; cmdline="$(dryrun_cmdline kvm)"
    echo "    kvm cmdline: $cmdline"
    if printf '%s' "$cmdline" | grep -qw 'no_timer_check'; then
        record "x86_64 KVM arm leaves the flag to the kernel (kvm.c sets it)" 1
    else
        record "x86_64 KVM arm leaves the flag to the kernel (kvm.c sets it)" 0
    fi
    printf '%s' "$(dryrun_argv kvm)" | grep -q -- '-accel kvm'
    record "x86_64 KVM arm really selects KVM" $?
}

check_console_untouched() {
    # The flag is an ADDITION: the console + loglevel contract (vms-300) stands.
    printf '%s' "$(dryrun_cmdline tcg)" | grep -q 'console=ttyS0 loglevel=3'
    record "console=ttyS0 loglevel=3 contract intact on the TCG arm" $?
}

check_unknown_accel_refused() {
    local out rc
    out="$(dryrun "$RUN_QEMU" nitrous)"; rc=$?
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'unknown OVMX_QEMU_ACCEL'; then
        record "an unknown OVMX_QEMU_ACCEL is refused loudly" 0
    else
        record "an unknown OVMX_QEMU_ACCEL is refused loudly (got rc=$rc)" 1
    fi
}

# TEETH: the gate must be able to tell the flag's absence from its presence.
# Run the real launcher with the flag stripped out and require the TCG
# assertion to go RED -- otherwise this file greens on anything.
check_gate_has_teeth() {
    local stripped="$TMP/run-qemu-noflag.sh"
    sed 's/ no_timer_check"/"/' "$RUN_QEMU" >"$stripped"
    local cmdline
    cmdline="$(dryrun "$stripped" tcg | awk '/^-append$/ { getline; print; exit }')"
    if printf '%s' "$cmdline" | grep -qw 'no_timer_check'; then
        record "teeth: a launcher without the flag is detected" 1
    else
        record "teeth: a launcher without the flag is detected" 0
    fi
}

# Bare metal must keep the check: the flag belongs to the emulated machine, not
# to every OVMX x86_64 kernel ever built.
check_not_in_kernel_config() {
    if [ ! -f "$KCONFIG" ]; then
        record "kernel config fragment present to check" 1
        return
    fi
    if grep -q 'CONFIG_CMDLINE' "$KCONFIG"; then
        record "no_timer_check is NOT baked into the kernel's built-in cmdline" 1
    else
        record "no_timer_check is NOT baked into the kernel's built-in cmdline" 0
    fi
}

echo "=== OVMX emulated-x86 timer-check cmdline gate (rd vms-4ff) ==="
echo "Launcher: $RUN_QEMU"
if [ ! -f "$RUN_QEMU" ]; then
    echo "  FAIL: launcher not found"
    exit 1
fi

make_stubs
check_tcg_arm_has_flag
check_kvm_arm_omits_flag
check_console_untouched
check_unknown_accel_refused
check_gate_has_teeth
check_not_in_kernel_config

echo ""
echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
