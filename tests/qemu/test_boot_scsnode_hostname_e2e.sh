#!/bin/bash
# test_boot_scsnode_hostname_e2e.sh - STARTUP.EXE's SYSBOOT role reads
# SYS$SYSTEM:OVMXVMSSYS.PAR and sets the REAL Linux hostname from SCSNODE
# (vms-b6a7, docs/design-boot-faithful.md sec 2.1/4.1). The hardcoded
# sethostname("OVMX", 4) in ovmx_init.c's bare_metal_init() is gone; this
# proves what replaced it, against a real boot on the real runtime target
# (CLAUDE.md Rule 9) -- not a unit mock.
#
# =============================================================================
# WHY F$GETSYI("NODENAME"), NOT F$GETSYI("SCSNODE") OR "SHOW SYSTEM"
# =============================================================================
# ovmx_node_name() (src/libvms/include/ovmx_identity.h), which backs SHOW
# SYSTEM's "on node ..." header and F$GETSYI("SCSNODE"), already reads
# SCSNODE straight out of the .PAR file through sysgen_read_string() --
# DELIBERATELY, per its own header comment, never the Linux hostname. That
# path is unrelated to this item and would read the new value even with ZERO
# changes to ovmx_init.c, so asserting it here would prove nothing new.
#
# F$GETSYI("NODENAME") is different: dcl_lexical.c's NODENAME case calls
# uname(2) and reports uts.nodename -- the actual Linux hostname, i.e.
# exactly what sethostname(2) sets. THAT is the value this item's PID 1
# change controls, and the only DCL-visible surface that proves it.
#
# =============================================================================
# WHY TWO SEPARATE QEMU PROCESSES ON THE SAME DISK IS "REBOOT" HERE
# =============================================================================
# sethostname(2) is set ONCE, by PID 1, at boot. There is no DCL REBOOT verb
# and no live "re-read the parameter file" path (VMS does not have one
# either -- SCSNODE is a boot parameter). Proving the change took effect
# therefore requires an actual new PID 1 execution against the SAME,
# persistent system disk: kill the first QEMU process (a power-cycle, since
# -no-reboot means a guest reboot(2) call would exit QEMU rather than
# restart it) and start a second one against the identical disk image file --
# the closest thing to a genuine reboot OVMX's platform has, and the same
# technique test_release_e2e.sh's "boot -> provision -> boot" already uses
# for an analogous problem (SYSUAF edits that only a fresh boot re-reads).
#
# =============================================================================
# TWO CASES
# =============================================================================
# POSITIVE (own disk): boot 1 -- SYSGEN USE CURRENT / SET SCSNODE ALPHA9 /
#   WRITE CURRENT (a REAL new vmsfs file version over the seeded ;1, same
#   proof shape as test_sysgen_versioning_e2e.sh). Boot 2, same disk: the
#   boot console itself announces the new node name before any prompt
#   exists, and F$GETSYI("NODENAME") in a fresh SYSTEM session reads ALPHA9,
#   not the old hardcoded OVMX.
#
# NEGATIVE (own disk, missing/corrupt .PAR on an INSTALLED volume): boot 1 --
#   a real SYSTEM session DELETEs the seeded SYS$SYSTEM:OVMXVMSSYS.PAR;1, the
#   ONLY version this fresh disk carries. Boot 2, same disk: this is NOT the
#   mount-or-halt condition (require_installed_system() already found
#   DCL.EXE) -- the boot must still reach Username:, must print an HONEST,
#   explicitly OVMX-facility warning (Rule 10: no invented VMS message for a
#   condition with no oracle capture), and must fall back to the compiled-in
#   default node name (OVMX), not silence and not a corrupted value.
#
# THE WRITEBACK TRAP (see test_release_e2e.sh's header for the full
# explanation): Linux's default dirty_expire_centisecs is 30s, so a QEMU
# process killed right after a DCL write can lose that write on the guest's
# side before it reaches the backing file. SETTLE_SECS below is what makes
# each case's boot-1 edit real for boot 2 to read.
#
# =============================================================================
# HARNESS HANG POST-MORTEM (2026-08-10, vms-b6a7)
# =============================================================================
# An earlier version of this file wrapped the QEMU launch in a helper
# function called via command substitution: QP=$(boot_qemu disk log fifo).
# That construct reliably STALLED with no qemu process ever appearing (ps
# inside the container showed only the stuck subshell, no timeout/qemu
# child at all) -- reproduced twice, isolated by direct comparison against
# an inlined version of the identical qemu invocation that boots to
# Username: in ~9 seconds every time. Root cause not fully isolated beyond
# "the function+$(...) shape reliably wedges launching a backgrounded
# process that has its own stdin/stdout/stderr redirected away from the
# subshell" -- but the FIX is not to explain bash's internals, it is to
# stop doing the thing that wedges: every QEMU boot below is launched
# INLINE, exactly as test_release_e2e.sh and test_sysgen_versioning_e2e.sh
# already do (both proven working). No boot_qemu() helper. Do not
# reintroduce one.
#
# Independently, `timeout N cmd` alone (no -k) only SENDS SIGTERM at N and
# then WAITS for the child to exit -- if the child is unresponsive that wait
# is itself unbounded. Every QEMU invocation below uses `timeout -k 15 N`
# so an unresponsive QEMU is SIGKILLed 15s after the SIGTERM, a real ceiling
# rather than a best-effort one (same principle as the df9 lane's
# `timeout 1860 qemu-system-x86_64 ...` hard-kill wrapper).
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_boot_scsnode_hostname_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit 0 = every assertion below passed against the real mounted volume.
# Exit 1 = a real failure (see the printed transcript segment).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETTLE_SECS="${SETTLE_SECS:-60}"     # see THE WRITEBACK TRAP above
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# PRE-INSTALLED distribution disk (vms-8ab): PID 1 does not install on a
# blank disk (vms-2f0), so both cases seed their disk from the mastered
# image, which is where distro/rootfs's seeded OVMXVMSSYS.PAR;1 (SCSNODE=OVMX)
# actually lands.
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing - the mastering stage did not run"
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

PASS=0
FAIL=0
record() {
    local desc="$1" rc="$2"
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}
check() {
    local desc="$1" log="$2" pattern="$3" expect="${4:-present}"
    # "--" is load-bearing: several patterns checked below start with "-"
    # (VMS's real continuation-line convention, e.g. "-OVMX-I-NOPARAMS, ...")
    # and without it grep tries to parse "-OVMX..." as options and silently
    # never matches (measured 2026-08-10: `grep -F "-OVMX-I-..."` exits 1
    # against a log that demonstrably contains the string byte-for-byte;
    # `grep -F -- "-OVMX-I-..."` finds it immediately).
    if grep -qF -- "$pattern" "$log" 2>/dev/null; then
        if [ "$expect" = "present" ]; then record "$desc" 0; else record "$desc" 1; fi
    else
        if [ "$expect" = "absent" ]; then record "$desc" 0; else record "$desc" 1; fi
    fi
}
waitfor() {  # pattern  limit-seconds  log
    local pat="$1" lim="${2:-60}" log="$3" w=0
    while [ $w -lt $((lim * 4)) ]; do
        grep -qF "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w + 1))
    done
    return 1
}

echo "=== SCSNODE -> real hostname across reboot (vms-b6a7) ==="
echo "arch=$ARCH qemu=$QEMU"

# =============================================================================
# CASE 1: POSITIVE - SET SCSNODE, reboot, the real hostname follows
# =============================================================================
echo ""
echo "--- CASE 1 (positive): SYSGEN SET SCSNODE ALPHA9 / WRITE CURRENT, reboot ---"

POS_DISK=/tmp/scsnode-e2e-pos.img
POS_LOG1=/tmp/scsnode-e2e-pos-boot1.log
POS_LOG2=/tmp/scsnode-e2e-pos-boot2.log
POS_FIFO=/tmp/scsnode-e2e-pos.in
rm -f "$POS_DISK" "$POS_LOG1" "$POS_LOG2" "$POS_FIFO"
cp "$DISTRIB_IMG" "$POS_DISK"
mkfifo "$POS_FIFO"

# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$POS_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$POS_FIFO" >"$POS_LOG1" 2>&1 &
qp=$!
exec 4>"$POS_FIFO"
send() { printf '%s\r' "$1" >&4; }
# vms-2213: on OPA0: LOGINOUT waits for the operator's RETURN before it
# presents "Username:". A single CR sent at t=0 is lost (the guest serial
# driver is not up yet), so feed a CR each second -- as a real operator
# would -- until the prompt actually appears in the log. Bounded; checks
# before each send so it stops as soon as the prompt is up.
wake_login() {
    local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
}

wake_login "$POS_LOG1"
if waitfor 'Username:' 120 "$POS_LOG1"; then rc=0; else rc=1; fi
record "boot 1: pre-installed disk boots to login" "$rc"
if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$POS_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: SYSTEM logs in" "$rc"

    # Baseline, before the change: the seed carries SCSNODE=OVMX, and the
    # real Linux hostname (set by boot 1's own PID 1) must already read it --
    # confirms the fix, not just the seed's SYSGEN default.
    # NODE = F$GETSYI(...) then SHOW SYMBOL NODE, not a bare WRITE SYS$OUTPUT
    # F$GETSYI(...) -- the same pattern tests/dcl/test_lexical_scsnode.sh
    # already proves this DCL supports for lexical results.
    send 'HOST1 = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOST1'; sleep 1
    check "boot 1: baseline F\$GETSYI(NODENAME) reads the seed's OVMX" "$POS_LOG1" 'HOST1 = "OVMX"'

    send 'SYSGEN'; sleep 1
    send 'USE CURRENT'; sleep 1
    send 'SET SCSNODE ALPHA9'; sleep 1
    send 'WRITE CURRENT'; sleep 1
    send 'EXIT'; sleep 1
    if waitfor '%SYSGEN-I-WRITTEN, 30 parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR;2' 20 "$POS_LOG1"; then
        rc=0
    else
        rc=1
    fi
    record "boot 1: WRITE CURRENT created ;2 over the seed's ;1 (real vmsfs version)" "$rc"
    check "boot 1: SET SCSNODE changed OVMX to ALPHA9" "$POS_LOG1" "%SYSGEN-I-SETPARAM, SCSNODE changed from OVMX to ALPHA9"

    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

rm -f "$POS_FIFO"; mkfifo "$POS_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$POS_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$POS_FIFO" >"$POS_LOG2" 2>&1 &
qp=$!
exec 4>"$POS_FIFO"

wake_login "$POS_LOG2"
if waitfor 'Username:' 120 "$POS_LOG2"; then rc=0; else rc=1; fi
record "boot 2 (reboot): still reaches the login prompt (mount-or-halt gate unaffected)" "$rc"

# The boot console ITSELF announces the new node name, before any DCL runs --
# a second, independent proof beyond the DCL session below.
check "boot 2: %OVMX-I-SCSNODE console line names ALPHA9" "$POS_LOG2" "%OVMX-I-SCSNODE, node name ALPHA9 set from SYS\$SYSTEM:OVMXVMSSYS.PAR"
check "boot 2: NO honest-halt (a good, present .PAR does not warn)" "$POS_LOG2" "%OVMX-W-NOPARAMS" absent

if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$POS_LOG2"; then rc=0; else rc=1; fi
    record "boot 2: SYSTEM logs in" "$rc"

    send 'HOST2 = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOST2'; sleep 1
    check "boot 2: F\$GETSYI(NODENAME) reads ALPHA9 (the REAL Linux hostname, set by sethostname())" \
        "$POS_LOG2" 'HOST2 = "ALPHA9"'
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

if [ "$FAIL" -ne 0 ]; then
    echo "--- CASE 1 boot 1 log ---"; cat "$POS_LOG1"
    echo "--- CASE 1 boot 2 log ---"; cat "$POS_LOG2"
fi

# =============================================================================
# CASE 2: NEGATIVE - missing SYS$SYSTEM:OVMXVMSSYS.PAR on an installed disk
# =============================================================================
echo ""
echo "--- CASE 2 (negative): delete the seeded OVMXVMSSYS.PAR;1, reboot on defaults ---"

NEG_DISK=/tmp/scsnode-e2e-neg.img
NEG_LOG1=/tmp/scsnode-e2e-neg-boot1.log
NEG_LOG2=/tmp/scsnode-e2e-neg-boot2.log
NEG_FIFO=/tmp/scsnode-e2e-neg.in
rm -f "$NEG_DISK" "$NEG_LOG1" "$NEG_LOG2" "$NEG_FIFO"
cp "$DISTRIB_IMG" "$NEG_DISK"
mkfifo "$NEG_FIFO"

# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$NEG_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$NEG_FIFO" >"$NEG_LOG1" 2>&1 &
qp=$!
exec 4>"$NEG_FIFO"

wake_login "$NEG_LOG1"
if waitfor 'Username:' 120 "$NEG_LOG1"; then rc=0; else rc=1; fi
record "boot 1: pre-installed disk boots to login" "$rc"
if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$NEG_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: SYSTEM logs in" "$rc"

    # A fresh copy of the distribution disk carries exactly one version.
    send 'DELETE SYS$SYSTEM:OVMXVMSSYS.PAR;1'; sleep 1
    send 'DIRECTORY SYS$SYSTEM:OVMXVMSSYS.PAR'; sleep 2
    # An exact (non-wildcard) filespec with zero matches reports the empty
    # "Total of 0 files." trailer. NOTE (vms-1c6): a bare DIRECTORY trailer
    # carries NO block count -- blocks appear only with /SIZE or /FULL (VSI
    # OpenVMS DCL Dictionary, DIRECTORY). OVMX previously printed "Total of 0
    # files, 0 blocks." here, which this assertion had pinned; corrected to the
    # authentic no-blocks form. (Real VMS also emits %DIRECT-W-NOFILES for a
    # zero-match exact spec -- that error-message fidelity is a separate
    # vms-1c6 slice, not fixed here.)
    check "boot 1: OVMXVMSSYS.PAR is gone (DIRECTORY finds nothing)" "$NEG_LOG1" "Total of 0 files."

    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

rm -f "$NEG_FIFO"; mkfifo "$NEG_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$NEG_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$NEG_FIFO" >"$NEG_LOG2" 2>&1 &
qp=$!
exec 4>"$NEG_FIFO"

wake_login "$NEG_LOG2"
if waitfor 'Username:' 120 "$NEG_LOG2"; then rc=0; else rc=1; fi
record "boot 2: missing .PAR is NOT the mount-or-halt condition - still reaches login" "$rc"

# The honest, OVMX-labeled warning -- never a fabricated VMS message.
check "boot 2: honest %OVMX-W-NOPARAMS warning printed" "$NEG_LOG2" "%OVMX-W-NOPARAMS, SYS\$SYSTEM:OVMXVMSSYS.PAR is missing or unreadable"
check "boot 2: warning names the default node name fallback" "$NEG_LOG2" "-OVMX-I-NOPARAMS, booting with default node name OVMX"
check "boot 2: NO invented VMS-facility message for this condition" "$NEG_LOG2" "%SYSBOOT-" absent
check "boot 2: NO mount-or-halt facility fired (this disk IS installed)" "$NEG_LOG2" "%OVMX-F-SYSINIT" absent
check "boot 2: the ;2 SCSNODE line from CASE 1 never fires here (separate disk)" "$NEG_LOG2" "ALPHA9" absent

if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$NEG_LOG2"; then rc=0; else rc=1; fi
    record "boot 2: SYSTEM logs in (identity is unrelated to SCSNODE)" "$rc"

    send 'HOST3 = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOST3'; sleep 1
    check "boot 2: F\$GETSYI(NODENAME) falls back to the default OVMX (not silence, not corrupted)" \
        "$NEG_LOG2" 'HOST3 = "OVMX"'
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

if [ "$FAIL" -ne 0 ]; then
    echo "--- CASE 2 boot 1 log ---"; cat "$NEG_LOG1"
    echo "--- CASE 2 boot 2 log ---"; cat "$NEG_LOG2"
fi

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$((PASS + FAIL)) checks passed, $FAIL failed"
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL SCSNODE/HOSTNAME BOOT CHECKS PASSED -- REAL BOOT, REAL PARAMETER FILE, NOT A MOCK"
    exit 0
fi
exit 1
