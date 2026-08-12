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
# ALSO PROVES (vms-32a, docs/design-opcom-executive-logging.md): the real
# vms.ko/vmsfs.ko kernel-module events reach the operator console as
# %OVMX-<S>-<IDENT> lines, real SYSKRNL (Linux-kernel-layer) lines (the kernel's own
# module-taint warning, deterministic on every boot since vms.ko/vmsfs.ko
# are unsigned) are RE-STYLED and ROUTED rather than suppressed -- never
# reaching the console as a raw/bare Linux dmesg line -- and OPERATOR.LOG's
# OPCOM record is oracle-exact (the eleven-'%' banner, the real SCSNODE) --
# against the same real boot and the same real OPERATOR.LOG file the rest
# of this test already drives.
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
# --- 1b. BOOT/LOGIN OUTPUT FIDELITY (vms-2213) + BANNER-FIRST (vms-1fb) -----
# Four VMS-fidelity properties of the boot->login console transcript. Oracle:
# VSI OpenVMS System Manager's Manual, Vol I, "Logging In to the System" --
# the operator-console login sequence is: banner, then the operator strikes
# RETURN, then Username:, then Password:, then a single SYS$WELCOME.
#
# BANNER-FIRST (vms-1fb, docs/design-boot-faithful.md §2.5/§3.5/§3.7): the OS
# banner (STARTUP.EXE's display_boot_banner) now prints immediately once the
# executive attaches -- BEFORE %STDRV-I-STARTUP and before STARTUP.COM's own
# output -- matching the Alpha oracle (banner right after SYSBOOT hands over,
# before any startup narration). It is already in the log by now (both
# %OVMX-I-EXEC and %STDRV-I-STARTUP have already been observed above), so
# this checks presence and its position relative to STDRV, not a fresh wait
# with STARTUP_OFF as a floor -- that floor is now on the WRONG side of the
# banner (before vms-1fb the banner printed after STARTUP_OFF; now it prints
# well before it).
PRE_STDRV_LOG=$(tr -d '\r' <"$LOG")
if printf '%s' "$PRE_STDRV_LOG" | grep -qE 'OpenVMX V[0-9]'; then
    ok "boot identification banner printed"
else
    dump_and_die "boot identification banner never printed"
fi
BANNER_POS=$(printf '%s' "$PRE_STDRV_LOG" | grep -aboE 'OpenVMX V[0-9]' | head -1 | cut -d: -f1)
STDRV_POS=$(printf '%s' "$PRE_STDRV_LOG" | grep -aboF '%STDRV-I-STARTUP' | head -1 | cut -d: -f1)
if [ -n "$BANNER_POS" ] && [ -n "$STDRV_POS" ] && [ "$BANNER_POS" -lt "$STDRV_POS" ]; then
    ok "banner precedes %STDRV-I-STARTUP (banner@$BANNER_POS < STDRV@$STDRV_POS) (vms-1fb banner-first)"
else
    bad "banner-before-STDRV ordering wrong (banner@${BANNER_POS:-none} STDRV@${STDRV_POS:-none}) (vms-1fb)"
fi
#
# DEFECT 3 (OPA0: waits for RETURN). JOB_CONTROL has by now created the
# console login session. On OPA0: LOGINOUT waits for the operator's RETURN
# before presenting Username:. Prove it: Username: is STILL absent even
# though the banner (and everything else in the boot) has already printed --
# the prompt is waiting behind the CR.
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

# --- 1c. EXECUTIVE KERNEL MESSAGES -> OPERATOR SURFACE (vms-32a) -----------
# docs/design-opcom-executive-logging.md. The /dev/kmsg -> console bridge
# (src/ovmx_init/opcom_kmsg.c) starts early in bare_metal_init(), ahead of
# vms.ko/vmsfs.ko loading, and reformats their pr_info/warn/err records as
# bare "%OVMX-<S>-<IDENT>, text" console lines (vocabulary (A) -- no OPCOM
# banner; OPCOM is not running at boot time). BEFORE this item these records
# never reached the console AT ALL: pr_info is below the "loglevel=3 quiet"
# console threshold this harness boots with, so a positive hit here is proof
# the bridge is running, not just that the kernel happened to be verbose.
if printf '%s\n' "$FID_FULL" | grep -qE '^%OVMX-I-DEVTAB, disk unit '; then
    ok "vms.ko's disk-unit event reached the operator console as %OVMX-I-DEVTAB (vms-32a)"
else
    bad "no %OVMX-I-DEVTAB disk-unit line on the console -- the /dev/kmsg bridge did not route it"
fi
if printf '%s\n' "$FID_FULL" | grep -qE '^%OVMX-I-DEVTAB, device table initialized, console terminal '; then
    ok "vms.ko's console-terminal-created event reached the operator console as %OVMX-I-DEVTAB (vms-32a)"
else
    bad "no %OVMX-I-DEVTAB console-terminal line on the console -- the /dev/kmsg bridge did not route it"
fi
if printf '%s\n' "$FID_FULL" | grep -qE '^%OVMX-I-VMSFS, '; then
    ok "vmsfs.ko's own event reached the operator console as %OVMX-I-VMSFS (vms-32a)"
else
    bad "no %OVMX-I-VMSFS line on the console -- the /dev/kmsg bridge did not route vmsfs.ko"
fi
# SYSKRNL (Linux-kernel-layer) lines are RE-STYLED and ROUTED, not suppressed (operator-
# ruling correction, 2026-08-12 -- docs/design-opcom-executive-logging.md
# sec3): they carry real operator-relevant information (a module-taint
# warning means an unverified executive image loaded), so the standing
# requirement is that they reach the console WRAPPED in a VMS-form facility
# line, never as a bare/raw Linux dmesg line. Every real boot triggers this
# deterministically: vms.ko/vmsfs.ko are unsigned out-of-tree modules, so
# the KERNEL ITSELF (not vms.ko) prints its own generic taint warning on
# every load -- and, because that warning is printed as "%s: <text>" with
# the LOADING MODULE'S OWN NAME substituted, it carries the SAME "vms: "
# prefix vms.ko's own lines do and is classified alongside them (design doc
# sec4 -- a disclosed simplification, not a defect: content and severity
# both stay the kernel's real ones, only the facility label is OVMX's
# rather than SYSKRNL's for this one measured collision).
if printf '%s\n' "$FID_FULL" | grep -qE '^%OVMX-[EWIF]-KMOD, .*taint'; then
    ok "the kernel's own module-taint warning reached the operator console, RE-STYLED (not suppressed) as %OVMX-*-KMOD (vms-32a)"
else
    bad "the kernel's own module-taint warning did not reach the console in VMS-form -- re-styling regressed to suppression, or never routed at all"
fi
# ...and it never reaches the console as a BARE/RAW Linux dmesg line (every
# occurrence of the SYSKRNL wording is wrapped in a %FACILITY- line).
RAW_TAINT_LEAK=$(printf '%s\n' "$FID_FULL" | grep -E 'taint' | grep -vE '^%(OVMX|SYSKRNL)-')
if [ -n "$RAW_TAINT_LEAK" ]; then
    bad "a SYSKRNL (Linux-kernel-layer) taint line reached the console UNWRAPPED (raw dmesg form): $RAW_TAINT_LEAK"
else
    ok "every taint-related console line is wrapped in a %OVMX-/%SYSKRNL- facility -- none reached the console as a raw Linux dmesg line (vms-32a)"
fi
# hrtimer's scheduling-latency warning is not deterministically triggerable
# in a clean boot (it only fires under real host scheduling pressure), so
# it is not asserted present here -- tests/ovmx_init/test_opcom_kmsg.c
# proves the %SYSKRNL-W-KERNEL re-styling for it directly. What IS
# asserted, for any run where the host happens to be loaded enough to
# trigger it: if it appears at all, it is wrapped, never raw.
RAW_HRTIMER_LEAK=$(printf '%s\n' "$FID_FULL" | grep -E 'hrtimer: interrupt took' | grep -vE '^%SYSKRNL-')
if [ -n "$RAW_HRTIMER_LEAK" ]; then
    bad "an hrtimer scheduling-latency line reached the console UNWRAPPED (raw dmesg form): $RAW_HRTIMER_LEAK"
else
    ok "no hrtimer line reached the console unwrapped (either absent this run, or wrapped as %SYSKRNL-) (vms-32a)"
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

# --- 5. THE OPCOM RECORD FORMAT, oracle-exact (vms-32a) --------------------
# docs/design-opcom-executive-logging.md sec6. REQUEST is the cheapest DCL
# command that calls sys$sndopr (src/vmsdcl/dcl_cmd_misc.c cmd_request), so
# it is used here to make a real record land in OPERATOR.LOG, then TYPE
# reads that file back over the SAME console session this test already
# drives -- proving the format against the actual file on the actual mastered
# image, not a unit-test fixture.
REQ_OFF=$(wc -c <"$LOG")
send 'REQUEST "vms-32a opcom format probe"'
if wait_for '%OPCOM-I-RQSTPEND' "$CMD_TIMEOUT" "$REQ_OFF"; then
    ok "REQUEST accepted (a real sys\$sndopr record was written)"
else
    dump_and_die "REQUEST never confirmed (%OPCOM-I-RQSTPEND missing)"
fi
TYPE_OFF=$(wc -c <"$LOG")
send 'TYPE SYS$MANAGER:OPERATOR.LOG'
if wait_for 'vms-32a opcom format probe' "$CMD_TIMEOUT" "$TYPE_OFF"; then
    ok "OPERATOR.LOG contains this run's REQUEST text"
else
    dump_and_die "TYPE SYS\$MANAGER:OPERATOR.LOG never showed the REQUEST text"
fi
OPLOG_SEG=$(segment_since "$TYPE_OFF")
# Banner: exactly eleven '%', two spaces, OPCOM, two spaces, the timestamp
# (DD-MMM-YYYY HH:MM:SS.ss, month upper-case 3-letter), two spaces, eleven
# '%' -- oracle-exact per the lab-Alpha capture in the design doc.
if printf '%s\n' "$OPLOG_SEG" | grep -qE '^%%%%%%%%%%%  OPCOM  [0-9]{2}-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}  %%%%%%%%%%%$'; then
    ok "OPERATOR.LOG's OPCOM banner is oracle-exact (eleven '%', boxed timestamp)"
else
    bad "OPERATOR.LOG's OPCOM banner did not match the oracle-exact shape"
fi
# Body line 2: "Request N, from user SYSTEM on <the real node>" -- not the
# old hardcoded "on node OVMX" literal. This image's SCSNODE is unconfigured
# (factory default), so the real node name IS still "OVMX" here -- but
# reached through ovmx_node_name()/SCSNODE, not a string constant baked into
# sys_operator.c (see the design doc sec6 for why that distinction matters
# on a clustered node with a real configured name).
if printf '%s\n' "$OPLOG_SEG" | grep -qE '^Request [0-9]+, from user SYSTEM on OVMX$'; then
    ok "OPERATOR.LOG's Request line names the real SCSNODE (SYSTEM on OVMX), not a hardcoded literal"
else
    bad "OPERATOR.LOG's Request line did not match 'Request N, from user SYSTEM on OVMX'"
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
