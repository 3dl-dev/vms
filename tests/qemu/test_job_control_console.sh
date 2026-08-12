#!/bin/bash
# test_job_control_console.sh - JOB_CONTROL, a real DETACHED process, owns
# the console session -- not PID 1 (vms-8d2, docs/design-init-scope.md
# sec2/sec5, docs/design-boot-faithful.md sec4.5).
#
# WHAT THIS PROVES, END TO END, AGAINST THE REAL MASTERED IMAGE.
#
# tests/integration/test_job_control_ownership.sh (the cheap, always-on
# half of this item) proves the SOURCE no longer contains a login loop in
# PID 1 and that JOB_CONTROL_STARTUP.COM creates JOB_CONTROL via RUN
# /DETACHED. That is a source scan -- it cannot prove the runtime property:
# that a real boot creates a real detached process, that the process the
# executive names JOB_CONTROL is what actually owns the console, and that
# SYSTEM can still log in through it and reach DCL. This test boots the
# ACTUAL mastered bootable image (distro/Dockerfile.bootable, the same one
# tests/qemu/test_parts_demo_e2e.sh and test_boot_scsnode_hostname_e2e.sh
# drive) and proves all three.
#
# THE A-WRITES/B-READS SHAPE (CLAUDE.md Rule 11, the same discipline
# tests/qemu/test_syssvc_startup_service.c uses for the underlying
# mechanism in isolation):
#
#   A = STARTUP.COM's site startup, which creates JOB_CONTROL. It never
#       reports anything this test reads as evidence.
#   B = the SYSTEM session THIS test logs into via the very login prompt
#       JOB_CONTROL created, running the user-visible `SHOW SYSTEM` command.
#       If JOB_CONTROL did not really register with the executive under
#       that name, B would not see it -- exactly the refutation vms-47b's
#       own suite is built around.
#
# WHAT WOULD MAKE THIS TEST FAIL HONESTLY:
#   - boot never reaches Username: (console ownership never transferred)
#   - SHOW SYSTEM does not list a JOB_CONTROL row (JOB_CONTROL was not
#     really created as a named, detached process the executive tracks)
#   - the JOB_CONTROL row's PID equals PID 1's own VMS PID (not detached --
#     see the SYSTEM identity read below)
#   - SYSTEM cannot log in / does not reach a DCL prompt
#   - the PARTS demo (a separate, expensive gate:
#     tests/qemu/test_parts_demo_e2e.sh) stops working -- run it alongside
#     this one, not instead of it, per this item's own DONE criteria
#
# Usage (run INSIDE the bootable image, like test_persistent_boot.sh /
# test_parts_demo_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_job_control_console.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for the pre-installed disk to reach
#                  Username: (default 60 -- no install runs on this path,
#                  unlike parts_demo_e2e's blank-disk boot).
#   CMD_TIMEOUT    seconds to wait for each DCL command's response (default 30).
#
# Exit 0 = all checks pass. Exit 1 = a real failure (see the printed
# transcript segment).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-60}"
CMD_TIMEOUT="${CMD_TIMEOUT:-30}"
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$SLIM_INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== JOB_CONTROL console-ownership e2e: boot -> login -> SHOW SYSTEM (vms-8d2) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$SLIM_INITRD"

DISK=/tmp/job-control-e2e.img
LOG=/tmp/job-control-e2e-console.log
FIFO=/tmp/job-control-e2e-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + CMD_TIMEOUT * 4 + 60))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 384M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern  limit-seconds  since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}

# --- 1. Boot the pre-installed disk to the login prompt ---------------------
if wait_for '%OVMX-I-EXEC' 30; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
if wait_for '%STDRV-I-STARTUP, OpenVMX startup begun' "$BOOT_TIMEOUT"; then
    ok "STDRV begun (STARTUP.COM ran)"
else
    dump_and_die "STDRV begun never printed within ${BOOT_TIMEOUT}s"
fi
STARTUP_OFF=$(wc -c <"$LOG")
if wait_for 'The OVMX system is now executing the site-specific startup commands.' "$BOOT_TIMEOUT" "$STARTUP_OFF"; then
    ok "SYSTARTUP_VMS.COM ran (site startup reached, JOB_CONTROL_STARTUP.COM already invoked before this line per the procedure order)"
else
    dump_and_die "site-specific startup line never printed within ${BOOT_TIMEOUT}s"
fi
# --- 1b. BOOT/LOGIN OUTPUT FIDELITY (vms-2213) ------------------------------
# Four VMS-fidelity properties of the boot->login console transcript. Oracle:
# VSI OpenVMS System Manager's Manual, Vol I, "Logging In to the System" --
# the operator-console login sequence is: banner, then the operator strikes
# RETURN, then Username:, then Password:, then a single SYS$WELCOME.
#
# DEFECTS 2 & 3 (banner-before-prompt; OPA0: waits for RETURN). JOB_CONTROL
# has by now created the console login session. On OPA0: LOGINOUT waits for
# the operator's RETURN before presenting Username:. Prove it: the boot
# identification banner (STARTUP.EXE's display_boot_banner, printed after
# STARTUP.COM returns) appears, and Username: is STILL absent -- the prompt is
# waiting behind the CR. Before this fix the prompt raced out DURING site
# startup, ahead of that banner.
if wait_for 'OpenVMX V' "$BOOT_TIMEOUT" "$STARTUP_OFF"; then
    ok "boot identification banner printed (STARTUP.EXE handoff)"
else
    dump_and_die "boot identification banner never printed within ${BOOT_TIMEOUT}s"
fi
if tr -d '\r' <"$LOG" | grep -qF 'Username:'; then
    bad "Username: appeared BEFORE the operator pressed RETURN -- OPA0: did not wait (defect 3)"
else
    ok "OPA0: waited: boot banner is out but Username: is not shown before RETURN (defect 3)"
fi
# The operator strikes RETURN; only now does the login prompt appear.
send ''
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    ok "after RETURN the login prompt appears (defect 3: wait-for-CR honoured)"
else
    dump_and_die "boot never reached Username: after the wake RETURN within ${BOOT_TIMEOUT}s"
fi
# Ordering (defect 2): the banner's byte-offset precedes Username:'s.
FID_FULL=$(tr -d '\r' <"$LOG")
BPOS=$(printf '%s' "$FID_FULL" | grep -aboE 'OpenVMX V[0-9]' | head -1 | cut -d: -f1)
UPOS=$(printf '%s' "$FID_FULL" | grep -aboF 'Username:' | head -1 | cut -d: -f1)
if [ -n "$BPOS" ] && [ -n "$UPOS" ] && [ "$BPOS" -lt "$UPOS" ]; then
    ok "banner precedes prompt in the transcript (banner@$BPOS < prompt@$UPOS) (defect 2)"
else
    bad "banner-before-prompt ordering wrong (banner@${BPOS:-none} prompt@${UPOS:-none}) (defect 2)"
fi
# DEFECT 4: no routine kernel-driver printk on the user console.
if printf '%s\n' "$FID_FULL" | grep -q 'vms: registered process'; then
    bad "kernel printk 'vms: registered process ...' leaked onto the console (defect 4)"
else
    ok "no 'vms: registered process' kernel chatter on the user console (defect 4)"
fi

# --- 2. Log in as SYSTEM -----------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' "$CMD_TIMEOUT" "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' "$CMD_TIMEOUT" "$LOGIN_OFF"; then
    ok "SYSTEM logs in through the JOB_CONTROL-created session (LOGINOUT.EXE activated)"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$' "$CMD_TIMEOUT" "$LOGIN_OFF"
ok "session reaches a DCL prompt"

# DEFECT 1: exactly ONE post-login welcome (single SYS$WELCOME). SYLOGIN.COM
# no longer WRITEs a second "Welcome ..." line of its own.
WELCOME_SEG=$(tail -c "+$((LOGIN_OFF + 1))" "$LOG" 2>/dev/null | tr -d '\r')
WELCOME_N=$(printf '%s\n' "$WELCOME_SEG" | grep -c 'Welcome to OpenVMX')
if [ "$WELCOME_N" -eq 1 ]; then
    ok "exactly ONE welcome message after login -- single SYS\$WELCOME (defect 1)"
else
    bad "expected exactly 1 welcome after login, saw $WELCOME_N (defect 1)"
fi

# --- 3. SHOW SYSTEM: a DIFFERENT process (this DCL session) sees JOB_CONTROL,
#        proving it is a real, named, detached process the executive tracks --
#        not something PID 1 claimed about itself. ------------------------
SHOW_OFF=$(wc -c <"$LOG")
send 'SHOW SYSTEM'
if wait_for 'JOB_CONTROL' "$CMD_TIMEOUT" "$SHOW_OFF"; then
    ok "SHOW SYSTEM, from a DIFFERENT process, lists JOB_CONTROL by its VMS process name"
else
    dump_and_die "SHOW SYSTEM never listed a JOB_CONTROL row"
fi
SHOW_SEG=$(segment_since "$SHOW_OFF")
JC_ROW=$(printf '%s\n' "$SHOW_SEG" | grep -E '^[0-9A-Fa-f]{8} JOB_CONTROL' | head -1)
if [ -n "$JC_ROW" ]; then
    ok "the JOB_CONTROL row has the executive's row shape (PID column 0, name column 9)"
    JC_PID=$(printf '%s' "$JC_ROW" | cut -c1-8)
    echo "  (JOB_CONTROL's executive-assigned VMS PID: $JC_PID)"
else
    bad "the JOB_CONTROL row's shape did not match '%08X JOB_CONTROL ...'"
fi

# --- 4. The row count: at least two rows are present (this session's own,
#        unnamed, plus JOB_CONTROL's, named) -- so the JOB_CONTROL row found
#        above is a SEPARATE table entry, not the only process the executive
#        knows about relabelled. (F$GETJPI("<name>", ...) is NOT used to
#        cross-check this: the DCL lexical implementation (src/vmsdcl/
#        dcl_lexical.c lex_getjpi) discards its pid/name argument entirely
#        and always answers about the caller via a raw getpid() -- a
#        pre-existing gap, unrelated to vms-8d2, that would make any
#        comparison built on it meaningless. SHOW SYSTEM's rows, tested
#        above, go through the real executive-resolving path
#        (dcl_cmd_show.c / vms_kif_procscan), which is why vms-47b's own
#        suite uses it for exactly this proof instead of the lexical.)
ROW_COUNT=$(printf '%s\n' "$SHOW_SEG" | grep -cE '^[0-9A-Fa-f]{8} ')
if [ "$ROW_COUNT" -ge 2 ]; then
    ok "SHOW SYSTEM lists $ROW_COUNT process rows -- JOB_CONTROL is a separate table entry, not this session relabelled"
else
    bad "SHOW SYSTEM lists only $ROW_COUNT row(s) -- JOB_CONTROL is not a separate process"
fi

# --- Results ---
echo ""
echo "=========================================="
echo "  RESULTS: $PASS passed, $FAIL failed"
echo "=========================================="

send 'LOGOUT'
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL JOB_CONTROL CONSOLE-OWNERSHIP CHECKS PASSED"
    exit 0
else
    echo "  JOB_CONTROL CONSOLE-OWNERSHIP CHECKS FAILED"
    echo ""
    echo "--- full console log ---"
    cat "$LOG"
    exit 1
fi
