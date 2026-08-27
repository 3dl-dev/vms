#!/bin/bash
# test_kernel_taint.sh - THE taint-clean acceptance gate (rd vms-566, epic
#                        vms-19e "owns-kernel"). Proves OVMX's kernel is
#                        UNTAINTED by its own modules after the real boot.
#
# Runs inside the ovmx-boot Docker image (has QEMU + the from-source OVMX kernel
# + the boot initramfs + /boot/ovmx-distrib.img, and `modinfo` from kmod).
#
# WHAT THIS PROVES, AND WHY IT SUPERSEDES THE TWO EARLIER GATES
# ------------------------------------------------------------
# Two predecessor gates each proved one half by SCRAPING A LOG STRING:
#   - test_intree_modules.sh (vms-934): no "loading out-of-tree module taints
#     kernel" line -> the O bit (bit 12, value 4096) is not set.
#   - test_signed_modules.sh (vms-ff5): no "module verification failed" line ->
#     the E bit (bit 13, value 8192) is not set.
# Both boot-time checks are ABSENCE checks on a KERN_WARNING/KERN_NOTICE line,
# and PID 1 lowers the console log level to 3 before any module loads
# (ovmx_boot_mute_kernel_console, vms-300) -- so at any loglevel those lines are
# muted and their absence proves little on its own. The DURABLE assertion is the
# NUMERIC mask, read straight from /proc/sys/kernel/tainted on the real boot.
#
# This gate CONSOLIDATES both into one standing acceptance check and HARDENS it
# with that numeric read (test_intree_modules.sh + test_signed_modules.sh are
# removed in the same change -- every assertion they made is folded in below, so
# coverage strictly increases). It has THREE independent proofs:
#
#   1. GROUND-SOURCE NUMERIC MASK (the durable assertion). Boot the real runtime
#      with the "ovmx.taintreport" boot flag set. PID 1 (ovmx_init.c's
#      report_kernel_taint), AFTER loading vms.ko, reads the actual
#      /proc/sys/kernel/tainted and prints "%OVMX-I-TAINT, kernel taint mask =
#      <N> (0x..)". We scrape <N> and FAIL if (N & 4096) [O] or (N & 8192) [E].
#      This is not a log-string scrape: it is the kernel's own numeric taint
#      state, read by the booted PID 1 from the real proc file after the real
#      modules loaded. O and E are NEVER allowlisted. Any OTHER nonzero bit is
#      failed too, unless it is in TAINT_ALLOWED_BITS below (empty today, since
#      the vms.ko module is MODULE_LICENSE("GPL") built in-tree and signed, so a
#      clean self-built kernel boots with mask 0).
#
#   2. STATIC modinfo on the SHIPPED artifacts (the exact regression
#      discriminators, from the two predecessor gates). Extract vms.ko
#      FROM the boot initramfs that actually boots and assert, on the bytes that
#      boot: `modinfo -F intree` == "Y" (built in OUR tree -> modpost skips
#      TAINT_OUT_OF_TREE) AND `modinfo -F signer` is non-empty with a PKCS#7
#      `sig_id` (signed -> finit_module verifies -> no TAINT_UNSIGNED_MODULE).
#
#   3. BOOT-TIME corroboration (best-effort). The modules demonstrably load
#      (%OVMX-I-EXEC + %OVMX-I-MOUNTED) so "mask is clean" is meaningful, and the
#      would-be taint log lines are absent. Kept as corroboration only; #1 is the
#      assertion.
#
# TWO-SIDEDNESS is proven by the companion tests/qemu/test_kernel_taint_negctl.sh
# (run in CI right after this gate): it sources the pure helpers below and shows
# taint_mask_forbidden() reddens for the O, E and O+E masks, and that stripping a
# real shipped module's signature / intree stamp reddens ko_signed_ok /
# ko_intree_ok. See that file's header.
#
# SOURCEABLE: when sourced (OVMX_TAINT_GATE_SOURCE=1), this file DEFINES the pure
# helpers and returns WITHOUT booting, so the negctl can exercise them directly.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_kernel_taint.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

# ===========================================================================
# PURE HELPERS (also sourced by test_kernel_taint_negctl.sh)
# ===========================================================================

# The forbidden bits. Named so the negctl and the gate share ONE definition.
TAINT_O_BIT=4096      # bit 12, TAINT_OUT_OF_TREE  -- cleared by vms-934
TAINT_E_BIT=8192      # bit 13, TAINT_UNSIGNED_MODULE -- cleared by vms-ff5

# ALLOWLIST of taint bits that are genuinely unavoidable in this harness. EMPTY
# today: both OVMX modules are GPL, built in-tree (vms-934) and signed (vms-ff5),
# so a clean self-built kernel boots with tainted == 0. O and E are NEVER added
# here (they are the whole point of the untaint work). Any bit added MUST carry
# an inline reason naming the source and why it is unavoidable, e.g.:
#   TAINT_ALLOWED_BITS=32   # bit 5, TAINT_FORCED_MODULE: <reason it is forced>
TAINT_ALLOWED_BITS=0

# taint_mask_forbidden <mask> -- echoes a human reason and returns 0 (FORBIDDEN,
# gate must fail) when the mask carries O, E, or any non-allowlisted bit;
# returns 1 (acceptable) otherwise. Single source of the pass/fail rule.
taint_mask_forbidden() {
    local mask="$1" reasons="" residual
    if (( mask & TAINT_O_BIT )); then
        reasons="${reasons} O(out-of-tree,bit12)"
    fi
    if (( mask & TAINT_E_BIT )); then
        reasons="${reasons} E(unsigned,bit13)"
    fi
    # Any bit set that is neither O, E, nor explicitly allowed.
    residual=$(( mask & ~TAINT_O_BIT & ~TAINT_E_BIT & ~TAINT_ALLOWED_BITS ))
    if (( residual != 0 )); then
        reasons="${reasons} unexpected-bits(0x$(printf '%x' "$residual"))"
    fi
    if [ -n "$reasons" ]; then
        echo "FORBIDDEN:${reasons}"
        return 0
    fi
    echo "clean"
    return 1
}

# ko_intree_ok <path-to-.ko> -- returns 0 iff modinfo reports intree=Y (built in
# OUR tree; the exact kernel-side condition under which modpost/add_taint_module
# skips TAINT_OUT_OF_TREE). An out-of-tree build stamps intree "N" or omits it.
ko_intree_ok() {
    local v
    v=$(modinfo -F intree "$1" 2>/dev/null)
    [ "$v" = "Y" ]
}

# ko_signed_ok <path-to-.ko> -- returns 0 iff modinfo reports a non-empty signer
# AND a PKCS#7 sig_id (signed; the exact condition under which finit_module
# verifies and does not set TAINT_UNSIGNED_MODULE). An unsigned module has no
# signer/sig_id field.
ko_signed_ok() {
    local signer sigid
    signer=$(modinfo -F signer "$1" 2>/dev/null)
    sigid=$(modinfo -F sig_id "$1" 2>/dev/null)
    [ -n "$signer" ] && echo "$sigid" | grep -qi 'PKCS#7'
}

# When sourced for the pure helpers, stop here -- do not boot QEMU.
if [ "${OVMX_TAINT_GATE_SOURCE:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

# ===========================================================================
# GATE BODY
# ===========================================================================

TIMEOUT=90
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qaiF "$pattern"; then
        if [ "$expect" = "present" ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
        else echo "  FAIL: $desc (found but should be absent: $pattern)"; FAIL=$((FAIL + 1)); fi
    else
        if [ "$expect" = "absent" ]; then echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else echo "  FAIL: $desc (not found: $pattern)"; FAIL=$((FAIL + 1)); fi
    fi
}

echo "=== OVMX kernel taint-clean acceptance gate (vms-566) ==="
echo "Architecture: $ARCH   Kernel: $KERNEL"
echo "Forbidden bits: O=$TAINT_O_BIT (out-of-tree), E=$TAINT_E_BIT (unsigned); allowlist=$TAINT_ALLOWED_BITS"
echo ""

# --- (2) STATIC: modinfo intree=Y AND signed on the SHIPPED modules ----------
echo "--- STATIC: modinfo intree + signature on the shipped vms.ko ---"
WORK=$(mktemp -d)
if gzip -dc "$INITRD" | ( cd "$WORK" && cpio -idm 2>/dev/null ); then
    record "boot initramfs unpacked" 0
else
    record "boot initramfs unpacked" 1
    echo "FATAL: could not unpack $INITRD"; exit 1
fi

# DISCOVER every kernel module shipped in the boot initramfs and gate each one
# (vms-bae: the drivers/ovmx/ home is a MENU, so a new OVMX module that ships in
# the image must inherit this taint gate for free -- not just the one named
# below). vms.ko is ALSO asserted present explicitly as a floor, so an
# empty/broken initramfs cannot silently pass. (vms-165 retired vmsfs.ko; vms.ko
# is the only OVMX module now.)
mapfile -t SHIPPED_KOS < <(find "$WORK" -name '*.ko' | sort)
record "at least one kernel module shipped in boot initramfs" $([ "${#SHIPPED_KOS[@]}" -gt 0 ] && echo 0 || echo 1)
for req in vms; do
    find "$WORK" -name "${req}.ko" | grep -q .; record "${req}.ko present in boot initramfs (required)" $?
done

for ko in "${SHIPPED_KOS[@]}"; do
    mod=$(basename "$ko" .ko)

    intree=$(modinfo -F intree "$ko" 2>/dev/null)
    echo "    modinfo -F intree  ${mod}.ko => '${intree:-<empty>}'"
    ko_intree_ok "$ko"; record "${mod}.ko intree=Y (built in OUR tree, no out-of-tree taint)" $?

    signer=$(modinfo -F signer "$ko" 2>/dev/null)
    sigid=$(modinfo -F sig_id "$ko" 2>/dev/null)
    echo "    modinfo -F signer  ${mod}.ko => '${signer:-<empty>}'"
    echo "    modinfo -F sig_id  ${mod}.ko => '${sigid:-<empty>}'"
    ko_signed_ok "$ko"; record "${mod}.ko SIGNED (PKCS#7 sig present, no unsigned-module taint)" $?

    vermagic=$(modinfo -F vermagic "$ko" 2>/dev/null)
    echo "    modinfo -F vermagic ${mod}.ko => '${vermagic}'"
    echo "$vermagic" | grep -q -- '-ovmx'; record "${mod}.ko vermagic is the OVMX from-source kernel" $?
done
rm -rf "$WORK"
echo ""

# --- (1)+(3) BOOT-TIME: read the REAL /proc/sys/kernel/tainted mask ----------
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64; MACHINE=""; CONSOLE="console=ttyS0"
fi

# Boot to a captured console log, feeding CR to reach login (OPA0: waits for the
# operator's RETURN). "ovmx.taintreport" tells PID 1 to read
# /proc/sys/kernel/tainted after both modules load and print the numeric mask on
# the console as %OVMX-I-TAINT. loglevel=7 keeps the (best-effort) kernel taint
# strings in play for corroboration; the numeric line is a userspace printf and
# is unaffected by the console log level.
run_qemu() {
    local disk="$1" log fifo qp w=0
    log=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" -nographic \
        -append "$CONSOLE loglevel=7 ovmx.taintreport=1" \
        -m 256M -smp 1 -nic none -nodefaults -serial stdio \
        -drive file="$disk",format=raw,if=virtio -no-reboot \
        <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 6>"$fifo"; trap '' PIPE
    while kill -0 "$qp" 2>/dev/null; do
        grep -qaF 'Username:' "$log" 2>/dev/null && break
        printf '\r' >&6 2>/dev/null
        sleep 1; w=$((w + 1)); [ "$w" -ge "$TIMEOUT" ] && break
    done
    exec 6>&-; kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null
    cat "$log"; rm -f "$log" "$fifo"
}

echo "--- BOOT-TIME: boot with ovmx.taintreport, read /proc/sys/kernel/tainted ---"
if [ ! -f "$DISTRIB_IMG" ]; then
    record "distribution image present" 1
    echo "FATAL: $DISTRIB_IMG missing"; exit 1
fi
DISK=$(mktemp -u).img
cp "$DISTRIB_IMG" "$DISK"
OUT=$(run_qemu "$DISK")
rm -f "$DISK"
echo "$OUT" | tail -30
echo "[... console truncated ...]"
echo ""

# Positive: the executive actually loaded (so a clean mask is meaningful).
check "vms.ko loaded (executive attached)"          "$OUT" "%OVMX-I-EXEC" present
check "system disk mounted (Files-11 ACP in vms.ko)" "$OUT" "%OVMX-I-MOUNTED" present

# THE DURABLE ASSERTION: the numeric mask, read from the real proc file by the
# booted PID 1. Extract "%OVMX-I-TAINT, kernel taint mask = <N> ...".
TOTAL=$((TOTAL + 1))
TAINT_LINE=$(echo "$OUT" | grep -aoE '%OVMX-I-TAINT, kernel taint mask = [0-9]+' | head -1)
if [ -z "$TAINT_LINE" ]; then
    echo "  FAIL: no %OVMX-I-TAINT readout on the console -- PID 1 did not report"
    echo "        /proc/sys/kernel/tainted (did the ovmx.taintreport flow run after"
    echo "        the modules loaded?). The numeric assertion cannot be made."
    FAIL=$((FAIL + 1))
else
    MASK=$(echo "$TAINT_LINE" | grep -aoE '[0-9]+$')
    echo "  read /proc/sys/kernel/tainted = ${MASK} (0x$(printf '%x' "$MASK"))"
    verdict=$(taint_mask_forbidden "$MASK")
    if [ "$verdict" = "clean" ]; then
        echo "  PASS: kernel taint mask is clean of O/E and all non-allowlisted bits (${MASK})"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: kernel is TAINTED -- ${verdict} (mask ${MASK})"
        FAIL=$((FAIL + 1))
    fi
fi

# (3) Best-effort corroboration: the predecessor gates' log-string absence.
check "no 'loading out-of-tree module' line (O)"      "$OUT" "loading out-of-tree module" absent
check "no 'module verification failed' notice (E)"    "$OUT" "module verification failed" absent
check "no generic 'taints kernel' line"               "$OUT" "taints kernel" absent

echo ""
echo "=== $PASS/$TOTAL passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
