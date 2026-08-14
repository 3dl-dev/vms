#!/bin/bash
# test_sysboot_conversational.sh - vms-b81: SYSBOOT> halts before the
# executive attaches, on a boot-flag request, and its parameter set is
# in-memory-only unless explicitly WRITEn.
#
# Runs inside the ovmx-boot image (has QEMU + kernel + initramfs + expect).
#
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_sysboot_conversational.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# WHAT THIS PROVES (design doc docs/design-boot-faithful.md §2.2/§3.1/§4.2)
#
#   Boot A (flagless):        SYSBOOT> never appears.
#   Boot B (ovmx.flags=0,1):  SYSBOOT> appears with no VMS BANNER and no
#                             executive narration preceding it (the executive
#                             attaches only after CONTINUE) -- the firmware/
#                             substrate preamble (SeaBIOS, the SYSKRNL identity
#                             line) legitimately precedes it, exactly as the
#                             SRM console block precedes SYSBOOT> in the §3.1
#                             oracle; SHOW SCSNODE's table is byte-shaped to
#                             the oracle capture; SET SCSNODE ZZ9 + CONTINUE
#                             boots showing node ZZ9.
#   Boot C (flagless again, SAME disk): node name is back to the disk's
#                             real value -- ZZ9 never persisted, because
#                             SET never WRITEs (VMS persistence semantics).
#
# Every QEMU invocation is wrapped in `timeout` -- a past boot proof here ran
# unbounded for 1h43m; this file does not repeat that (CLAUDE.md).
#
# Exit code 0 = all checks pass, 1 = failures

set -uo pipefail

TIMEOUT=90
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# A real, already-installed system disk: PID 1 no longer installs one
# (vms-2f0/§6) -- require_installed_system() halts on a blank volume, and
# SYSBOOT itself needs SYS$SYSTEM:OVMXVMSSYS.PAR to already exist to prove
# anything about ITS real value (Boot A/C's baseline check below).
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

PASS=0
FAIL=0

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

# check() and run_qemu() mirror test_executive_integral.sh exactly (same
# harness idiom, same pipefail-safety note on why `check` matches against a
# fully-materialized string rather than a live pipe).
check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    if grep -qF -- "$pattern" <<<"$output"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (found but must be absent: $pattern)"; FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (not found: $pattern)"; FAIL=$((FAIL + 1))
        fi
    fi
}

run_qemu() {
    local disk="$1" extra_append="${2:-}"
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$INITRD" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet${extra_append:+ $extra_append}" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$disk",format=raw,if=virtio \
        -no-reboot \
        </dev/null 2>&1 || true
}

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FAIL: $DISTRIB_IMG is missing -- this test needs an installed disk."
    exit 1
fi
if ! command -v expect >/dev/null 2>&1; then
    echo "FAIL: expect is missing -- distro/Dockerfile.bootable's runner stage"
    echo "  must install it (vms-b81)."
    exit 1
fi

DISK=/tmp/sysboot-conv.img
rm -f "$DISK"
cp "$DISTRIB_IMG" "$DISK"

# --- Boot A: flagless boot on the freshly-installed disk -------------------
# Baseline: SYSBOOT> never appears, and %OVMX-I-SCSNODE shows the disk's
# real, factory-default SCSNODE (read_boot_parameters(), vms-b6a7).
echo "--- Boot A: flagless boot (baseline) ---"
OUT_A=$(run_qemu "$DISK")

check "Boot A: no SYSBOOT prompt on a flagless boot" "$OUT_A" 'SYSBOOT>' absent
check "Boot A: node name set from the real parameter file" "$OUT_A" \
    '%OVMX-I-SCSNODE, node name OVMX set from SYS$SYSTEM:OVMXVMSSYS.PAR'
echo ""

# --- Boot B: conversational boot, driven interactively over the REAL
# console via expect -----------------------------------------------------
echo "--- Boot B: conversational boot (SYSBOOT> SET SCSNODE ZZ9) ---"
EXP_TIMEOUT=$((TIMEOUT - 10))
OUT_B=$(timeout "$TIMEOUT" expect -c "
    log_user 1
    set timeout $EXP_TIMEOUT
    spawn $QEMU $MACHINE -kernel $KERNEL -initrd $INITRD -nographic \
        -append \"$CONSOLE loglevel=3 quiet ovmx.flags=0,1\" -m 256M -smp 1 \
        -nic none -nodefaults -serial mon:stdio \
        -drive file=$DISK,format=raw,if=virtio -no-reboot
    expect {
        \"SYSBOOT> \" {}
        timeout { puts \"EXPECT-TIMEOUT: never saw SYSBOOT>\"; exit 1 }
        eof { puts \"EXPECT-EOF: qemu exited before SYSBOOT>\"; exit 1 }
    }
    send \"SHOW SCSNODE\r\"
    expect {
        \"SYSBOOT> \" {}
        timeout { puts \"EXPECT-TIMEOUT: no SYSBOOT> after SHOW SCSNODE\"; exit 1 }
    }
    send \"SET SCSNODE ZZ9\r\"
    expect {
        \"SYSBOOT> \" {}
        timeout { puts \"EXPECT-TIMEOUT: no SYSBOOT> after SET\"; exit 1 }
    }
    send \"CONTINUE\r\"
    expect {
        \"%OVMX-I-SCSNODE\" {}
        timeout { puts \"EXPECT-TIMEOUT: no SCSNODE line after CONTINUE\"; exit 1 }
    }
    # Give the rest of boot a moment to settle, then quit QEMU cleanly
    # (Ctrl-A X, the same escape run-qemu.sh/boot.sh document) rather than
    # relying on the outer timeout to reap it.
    sleep 3
    send \"\x01x\"
    expect eof
" 2>&1 || true)

# The load-bearing proof (§3.1, design-boot-faithful.md line "No banner
# precedes SYSBOOT>"): no VMS BANNER and no executive narration precedes the
# prompt -- SYSBOOT> halts BEFORE the executive attaches. It is NOT "the console
# is byte-empty before SYSBOOT>": the oracle §3.1 capture itself shows firmware/
# console bootstrap output (the SRM `P00>>>` block: "block 0 ... valid boot
# block", "jumping to bootstrap code") preceding SYSBOOT>. The OVMX substrate
# analog is the unavoidable firmware/kernel preamble -- expect's own `spawn`
# echo, SeaBIOS, "Booting from ROM", the ANSI clear-screen, and the substrate
# identity line "OVMX/Linux -- SYSKRNL (...)" -- none of which is a VMS banner or
# executive line. Slice everything up to and including the FIRST 'SYSBOOT> ' and
# demand it carries NO OVMX/VMS executive marker (the `%OVMX-...` narration incl.
# the `%OVMX-I-EXEC ... executive attached` line, and the `OpenVMX Vx.x` banner).
PRE_PROMPT=$(sed -n '1,/SYSBOOT> /p' <<<"$OUT_B" | sed 's/SYSBOOT> $//')
if LEAK=$(grep -aE '%OVMX-|OpenVMX V[0-9]' <<<"$PRE_PROMPT"); then
    echo "  FAIL: Boot B: a VMS banner/executive line precedes SYSBOOT>:"
    FAIL=$((FAIL + 1))
    sed 's/^/    | /' <<<"$LEAK"
else
    echo "  PASS: Boot B: no VMS banner/executive line precedes SYSBOOT> (§3.1)"
    PASS=$((PASS + 1))
fi

# SHOW SCSNODE table, byte-shaped to docs/design-boot-faithful.md §3.1's
# oracle capture: header, dashes, and the row itself (OVMX's real SCSNODE
# default is "OVMX", not the oracle's "ALPHA1" -- the FORMAT is pinned, not
# the node name).
check "Boot B: SHOW SCSNODE table header byte-shaped to the oracle" "$OUT_B" \
    'Parameter Name            Current    Default     Min.       Max.   Unit  Dynamic'
check "Boot B: SHOW SCSNODE dashes row byte-shaped to the oracle" "$OUT_B" \
    '--------------            -------    -------   -------    -------  ----  -------'
check "Boot B: SHOW SCSNODE row - quoted/padded current value" "$OUT_B" \
    'SCSNODE                 "OVMX    "    "    "    "    "     "ZZZZ" Ascii'
check "Boot B: SET took effect for THIS boot (node name ZZ9)" "$OUT_B" \
    '%OVMX-I-SCSNODE, node name ZZ9 set from SYS$SYSTEM:OVMXVMSSYS.PAR'
echo ""

# --- Boot C: flagless boot again, on the SAME disk --------------------------
# Persistence proof: SET was in-memory only (no WRITE), so the disk's real
# OVMXVMSSYS.PAR is untouched -- this boot must read the SAME value Boot A
# did, never "ZZ9".
echo "--- Boot C: flagless boot again (persistence proof) ---"
OUT_C=$(run_qemu "$DISK")

check "Boot C: no SYSBOOT prompt on a flagless boot" "$OUT_C" 'SYSBOOT>' absent
check "Boot C: node name is the FILE's value again, not ZZ9" "$OUT_C" \
    '%OVMX-I-SCSNODE, node name OVMX set from SYS$SYSTEM:OVMXVMSSYS.PAR'
check "Boot C: the in-memory SET from Boot B did NOT persist" "$OUT_C" 'ZZ9' absent
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "=== RESULTS: $PASS checks passed, 0 failed ==="
    echo "ALL SYSBOOT CONVERSATIONAL-BOOT CHECKS PASSED"
    exit 0
fi

echo "=== RESULTS: $PASS passed, $FAIL FAILED ==="
echo ""
echo "--- Boot A console output ---"
tail -40 <<<"$OUT_A"
echo "--- Boot B console output (conversational) ---"
tail -60 <<<"$OUT_B"
echo "--- Boot C console output ---"
tail -40 <<<"$OUT_C"
exit 1
