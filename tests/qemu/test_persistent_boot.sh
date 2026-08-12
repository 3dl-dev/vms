#!/bin/bash
# test_persistent_boot.sh - PID 1 is bootstrap-only: mount the installed disk,
#                           or HALT (vms-2f0)
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the (single)
# boot initramfs + /boot/ovmx-distrib.img, the mastered distribution disk).
#
# WHAT THIS PROVES (operator ruling 2026-08-10, docs/design-init-scope.md §6
# "STRIP ALL OF IT")
#
# PID 1 no longer installs, initializes, seeds or provisions ANYTHING. A booting
# VMS system finds its system disk installed or does not boot (§1). This test
# therefore no longer proves "blank disk -> install" (that path is deleted); it
# proves the two halves of the mount-or-halt shape the strip leaves behind:
#
#   POSITIVE: the boot initramfs boots the PRE-INSTALLED distribution disk
#             (/boot/ovmx-distrib.img, mastered at BUILD time by vms-8ab, NOT by
#             PID 1) and reaches a SYSTEM login prompt. No install / initialize /
#             overlay code path runs, and %STDRV-I-STARTUP begun PRECEDES the
#             login (the F1 STDRV-bracket fix), with NO "completed" counterpart.
#
#   FIRST-BOOT COMPLETION (vms-649): the same pre-installed disk is booted a
#             SECOND time and both boots are shown to be materially identical --
#             each reaches login and %STDRV-I-STARTUP begun, and NEITHER runs any
#             AUTOGEN / first-boot / one-time-completion phase. The Alpha 8.4
#             first-boot oracle (docs/oracle/installation-media-vax73-alpha84.md
#             §5) shows a real target's first boot running exactly such work, but
#             every step of it brings up a facility OVMX does not implement
#             (design-vms-faithful-install.md §3.5), so OVMX's first boot is a
#             justified no-op: the install already produces a fully-configured,
#             directly-bootable system, and nothing runs once-only.
#
#   NEGATIVE (halt control): a BLANK, uninitialized disk -> PID 1 HALTS honestly
#             with the OVMX-facility SYSINIT mount failure. NO login prompt, NO
#             install message, NO %STARTUP-W-MOUNTFAIL, NO overlay, NO STDRV
#             begun (STARTUP.COM never runs). The disk is NOT initialized and NOT
#             installed. This is run with the SAME boot initramfs as the positive
#             case (vms-1ab: there is exactly one initramfs now, no separate fat
#             variant to fork from) -- proving the halt is a property of the disk
#             state, not of which initramfs booted. (vms-1fb: the boot
#             identification banner DOES appear on this halt now -- it prints at
#             the SYSBOOT/EXEC_INIT handoff, before SYSINIT's disk mount is even
#             attempted, matching the oracle §3.5 ordering; banner presence alone
#             no longer distinguishes "came up" from "halted" -- see the checks
#             below for what still does.)
#
# A gate that cannot go red is decoration; the negative control is what proves
# the halt discriminates (same reasoning as test_executive_integral.sh Boot
# B/C). The full positive login + DCL-from-disk proof lives in
# test_distrib_boot.sh; this file focuses on the halt and the STDRV ordering.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_persistent_boot.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures

set -uo pipefail

TIMEOUT=90
DISTRIB_IMG=/boot/ovmx-distrib.img
KERNEL=/boot/vmlinuz
# There is exactly one initramfs (vms-1ab) -- both boots below use it.
INITRD=/boot/initramfs-ovmx.cpio.gz
ARCH=$(uname -m)

PASS=0
FAIL=0
TOTAL=0

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qF "$pattern"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern found but should be absent: $pattern)"; FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern not found: $pattern)"; FAIL=$((FAIL + 1))
        fi
    fi
}

record() {
    local desc="$1" rc="$2"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}

# Boot to a captured console log, up to TIMEOUT.
#
# vms-2213: on OPA0: LOGINOUT waits for the operator's RETURN before it presents
# "Username:" (the "press RETURN to log in" console behaviour). A single CR sent
# at t=0 is lost (the guest serial driver is not up yet), so QEMU is backgrounded
# on a FIFO and a CR is fed every second -- as a real operator would -- until the
# prompt appears in the log or the guest exits. The negative (blank disk) HALTS
# before any login exists, so QEMU exits quickly, the loop stops, and "Username:"
# stays absent. The full console log is printed on stdout, exactly as before.
run_qemu() {
    local initrd="$1" disk="$2" log fifo qp w=0
    log=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$initrd" \
        -nographic \
        -append "$CONSOLE loglevel=3 quiet" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$disk",format=raw,if=virtio \
        -no-reboot \
        <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 6>"$fifo"
    trap '' PIPE   # a CR fed just as the guest exits must not kill this subshell
    while kill -0 "$qp" 2>/dev/null; do
        grep -qaF 'Username:' "$log" 2>/dev/null && break
        printf '\r' >&6 2>/dev/null
        sleep 1; w=$((w + 1))
        [ "$w" -ge "$TIMEOUT" ] && break
    done
    exec 6>&-
    kill "$qp" 2>/dev/null
    wait "$qp" 2>/dev/null
    cat "$log"
    rm -f "$log" "$fifo"
}

echo "=== OVMX mount-or-halt gate (vms-2f0) ==="
echo "Architecture: $ARCH   QEMU: $QEMU"
echo "Kernel: $KERNEL   Distribution image: $DISTRIB_IMG"
echo ""

# --- Precondition: the mastered image exists -------------------------------
if [ -f "$DISTRIB_IMG" ]; then
    record "Mastered distribution image present in the built image" 0
else
    record "Mastered distribution image present in the built image" 1
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi
echo ""

# --- POSITIVE: boot initramfs boots the pre-installed disk to login --------
echo "--- POSITIVE: boot initramfs + pre-installed distribution disk ---"
POS_DISK="/tmp/mount-or-halt-installed.img"
rm -f "$POS_DISK"
cp "$DISTRIB_IMG" "$POS_DISK"
OUT_POS=$(run_qemu "$INITRD" "$POS_DISK")
echo "$OUT_POS" | head -40
echo "[... truncated ...]"
echo ""

check "positive: executive attached"          "$OUT_POS" "%OVMX-I-EXEC"
check "positive: system disk DKA0: mounted"    "$OUT_POS" "%OVMX-I-MOUNTED"
check "positive: STDRV begun printed"          "$OUT_POS" "%STDRV-I-STARTUP, OpenVMX startup begun"
check "positive: reaches the login prompt"     "$OUT_POS" "Username:"
# The strip: none of the install/initialize/overlay lines may appear.
check "positive: NO install ran"               "$OUT_POS" "%STARTUP-I-INSTALL"    absent
check "positive: NO blank-disk initialize"     "$OUT_POS" "%STARTUP-I-INIT"       absent
check "positive: NO overlay mount-fail warning" "$OUT_POS" "%STARTUP-W-MOUNTFAIL" absent
check "positive: NO honest halt on a good disk" "$OUT_POS" "%OVMX-F-SYSINIT"      absent
# F1: there is exactly ONE STDRV line and it is the "begun" one — the invented
# "completed" line is deleted, not repositioned.
check "positive: NO invented STDRV completed line" "$OUT_POS" "%STDRV-I-STARTUP, OpenVMX startup completed" absent

# F1 ordering: the STDRV "begun" bracket precedes the login prompt. On the
# collapsed pre-fix code they printed together, after startup; here "begun"
# comes first. Compare the byte offsets in the console stream.
BEGUN_POS=$(printf '%s' "$OUT_POS" | grep -aboF "%STDRV-I-STARTUP, OpenVMX startup begun" | head -1 | cut -d: -f1)
USER_POS=$(printf '%s' "$OUT_POS" | grep -aboF "Username:" | head -1 | cut -d: -f1)
if [ -n "$BEGUN_POS" ] && [ -n "$USER_POS" ] && [ "$BEGUN_POS" -lt "$USER_POS" ]; then rc=0; else rc=1; fi
record "positive: %STDRV-I-STARTUP begun precedes the login prompt (F1 bracket)" "$rc"

# vms-1fb ordering: the OS banner precedes %STDRV-I-STARTUP begun -- the
# banner-first fix (docs/design-boot-faithful.md §2.5/§3.5/§3.7). Before
# vms-1fb the banner printed LAST, after STARTUP.COM had already run.
BANNER_POS=$(printf '%s' "$OUT_POS" | grep -aboE 'OpenVMX V[0-9]' | head -1 | cut -d: -f1)
if [ -n "$BANNER_POS" ] && [ -n "$BEGUN_POS" ] && [ "$BANNER_POS" -lt "$BEGUN_POS" ]; then rc=0; else rc=1; fi
record "positive: OS banner precedes %STDRV-I-STARTUP begun (vms-1fb banner-first)" "$rc"
echo ""

# --- FIRST-BOOT COMPLETION, measured (vms-649) -----------------------------
# The Alpha 8.4 first-boot oracle (docs/oracle/installation-media-vax73-
# alpha84.md §5) shows a real OpenVMS target's first boot running one-time
# completion work -- AUTOGEN's five phases (GETDATA/GENPARAMS/GENFILES/
# SETPARAMS/REBOOT), an automatic reboot, then a second boot that first brings
# up the security server, ACME server and audit-server database and populates
# the rights database. EVERY one of those steps is the first-time bring-up of a
# facility OVMX does not implement (design-vms-faithful-install.md §3.5 maps
# each, with citations): OVMX has no AUTOGEN (its parameter file ships in the
# kit and SCSNODE is set at install), no security/ACME/audit server, and the
# four rights identifiers the oracle adds all name absent facilities that
# RIGHTSLIST.DAT already excludes by Rule 10. So OVMX's install produces a
# fully-configured, directly-bootable system and its first boot has NO honest
# completion work to run -- faking any would be the INV-6/Rule-10 LARP the
# authenticity invariants forbid.
#
# This is the ground-source proof of that justified no-op: the installed
# system's FIRST boot (OUT_POS above) already reached a login prompt, and it
# ran NO first-boot/AUTOGEN completion phase; booting the SAME disk AGAIN is
# materially identical -- no boot-1-only phase a later boot skips. FBRE matches
# any AUTOGEN / first-boot / one-time-completion marker; it must be absent on
# BOTH boots. (If a future OVMX facility ever needs a genuine first-boot phase,
# these become the negative-control guards that catch a fake one.)
echo "--- FIRST-BOOT COMPLETION (vms-649): the installed system's first boot is a justified no-op ---"
FBRE='AUTOGEN|GETDATA phase|GENPARAMS phase|GENFILES phase|SETPARAMS phase|first[ -]?boot|FIRSTBOOT|one-time completion'
if printf '%s' "$OUT_POS" | grep -qaiE "$FBRE"; then rc=1; else rc=0; fi
record "first-boot: the installed system's FIRST boot runs NO AUTOGEN/first-boot completion phase (vms-649 no-op)" "$rc"

# Boot the SAME persistent disk a second time. First-boot completion, such as
# it is (nothing), must not re-run, and the two boots must be structurally
# identical -- proving there is no deferred one-time configuration.
OUT_POS2=$(run_qemu "$INITRD" "$POS_DISK")
check "second boot: reaches the login prompt again (first boot consumed no one-shot)" "$OUT_POS2" "Username:"
check "second boot: %STDRV-I-STARTUP begun printed again"                             "$OUT_POS2" "%STDRV-I-STARTUP, OpenVMX startup begun"
if printf '%s' "$OUT_POS2" | grep -qaiE "$FBRE"; then rc=1; else rc=0; fi
record "second boot: runs NO AUTOGEN/first-boot completion phase (identical to boot 1 -- no boot-1-only phase)" "$rc"
echo ""

# --- NEGATIVE (halt control): blank disk -> honest halt --------------------
echo "--- NEGATIVE: blank/uninitialized disk + boot initramfs -> honest halt ---"
NEG_DISK="/tmp/mount-or-halt-blank.img"
rm -f "$NEG_DISK"
truncate -s 64M "$NEG_DISK"
OUT_NEG=$(run_qemu "$INITRD" "$NEG_DISK")
echo "$OUT_NEG" | head -40
echo "[... truncated ...]"
echo ""

check "negative: executive attached (halt is AFTER the executive)" "$OUT_NEG" "%OVMX-I-EXEC"
check "negative: honest OVMX-facility SYSINIT halt"                 "$OUT_NEG" "%OVMX-F-SYSINIT"
# Either honest halt is acceptable: the blank disk fails to mount as vmsfs, or
# (were an empty volume to mount) it carries no SYS$SYSTEM:DCL.EXE. Both name
# the system disk DKA0: and both are ovmx_sysinit_halt — neither installs it.
check "negative: halt names the system disk DKA0:"                 "$OUT_NEG" "system disk DKA0:"
# The strip: the blank disk must NOT be initialized, installed, or overlaid,
# and the boot must NOT reach a login prompt.
check "negative: NO blank-disk initialize"       "$OUT_NEG" "%STARTUP-I-INIT"       absent
check "negative: NO install ran"                 "$OUT_NEG" "%STARTUP-I-INSTALL"    absent
check "negative: NO overlay mount-fail warning"  "$OUT_NEG" "%STARTUP-W-MOUNTFAIL"  absent
# vms-1fb: the OS banner now prints right after the executive attaches
# (§3.5 oracle ordering: banner immediately after SYSBOOT hands over, BEFORE
# the system-disk mount) -- so on THIS halt (SYSBOOT/EXEC_INIT already
# succeeded; only SYSINIT's disk mount fails) the banner legitimately DOES
# appear now, unlike before vms-1fb. That is more oracle-faithful, not a
# regression: real VMS shows its identification banner at the same handoff
# point, before SYSINIT even attempts to mount the system disk. "The system
# did not come up" is proven by the checks that still matter -- no login
# prompt, no STDRV begun (STARTUP.COM never runs) -- not by banner absence.
check "negative: NO login prompt on an uninstalled disk"  "$OUT_NEG" "Username:"          absent
check "negative: NO STDRV begun (startup never ran)"      "$OUT_NEG" "%STDRV-I-STARTUP"   absent
echo ""

# --- Results ---
echo "=========================================="
echo "  RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL MOUNT-OR-HALT CHECKS PASSED"
    exit 0
else
    echo "  MOUNT-OR-HALT CHECKS FAILED"
    echo ""
    echo "--- POSITIVE full output ---"
    echo "$OUT_POS"
    echo ""
    echo "--- NEGATIVE full output ---"
    echo "$OUT_NEG"
    exit 1
fi
