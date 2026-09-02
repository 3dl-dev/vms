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
# ALSO PROVES (vms-32a, docs/design-opcom-executive-logging.md): the
# /dev/kmsg -> OPERATOR.LOG bridge (src/ovmx_init/opcom_kmsg.c) NEVER
# touches the console -- neither vms.ko/vmsfs.ko's own OVMX-facility
# lifecycle records nor re-styled SYSKRNL (Linux-kernel-layer) lines (the
# kernel's own module-taint warning, deterministic on every boot since
# vms.ko/vmsfs.ko are unsigned) reach OPA0: in any form, so the console
# facility+ident sequence stays byte-identical to origin/main's
# oracle-conformant shape (tests/qemu/test_boot_conformance.sh's own pinned
# sequence, corroborated inline here) -- and separately, that OPERATOR.LOG's
# sys$sndopr-written OPCOM record is oracle-exact (the eleven-'%' banner,
# the real SCSNODE) -- against the same real boot and the same real
# OPERATOR.LOG file the rest of this test already drives.
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

# --- 1c. THE /dev/kmsg BRIDGE NEVER TOUCHES THE CONSOLE (vms-32a, round 3
# -- the definitive fix). docs/design-opcom-executive-logging.md.
#
# tests/qemu/test_boot_conformance.sh pins the EXACT ordered sequence of
# %FACILITY-SEVERITY-IDENT tokens the boot console may show, derived from
# the OpenVMS Alpha oracle, produced entirely by the boot orchestrator
# (ovmx_init/PROVISION.EXE/SYSTARTUP_VMS.COM) -- never by a kernel module's
# printk. Two earlier cuts of the /dev/kmsg -> operator bridge
# (src/ovmx_init/opcom_kmsg.c) routed vms.ko/vmsfs.ko's own OVMX-facility
# lines, then also re-styled SYSKRNL lines, to the console; BOTH broke that
# pinned sequence (measured on PR #365's "Persistent Boot Smoke Test" /
# "Boot console sequence conformance" CI job). The definitive fix: the
# bridge writes to SYS$MANAGER:OPERATOR.LOG ONLY (opcom_kmsg_classify()
# collapses to OPCOM_KMSG_DROP vs OPCOM_KMSG_OPERATOR_LOG; there is no
# OPCOM_KMSG_CONSOLE any more). This section proves the CONSOLE side of
# that: none of the bridge's own idents (DEVTAB, LNM, MBX, SYSID-from-kmsg,
# KMOD, VMSFS, SYSKRNL/KERNEL) appear anywhere in the boot console
# transcript. (docs/design-opcom-executive-logging.md's design record and
# the unit tests in tests/ovmx_init/test_opcom_kmsg.c cover that the SAME
# lines DO reach OPERATOR.LOG -- this file's job is the console's silence.)
for IDENT_PAT in '^%OVMX-[A-Z]-DEVTAB, ' '^%OVMX-[A-Z]-LNM, ' '^%OVMX-[A-Z]-MBX, ' \
                  '^%OVMX-[A-Z]-VMSFS, ' '^%SYSKRNL-'; do
    if printf '%s\n' "$FID_FULL" | grep -qE "$IDENT_PAT"; then
        bad "a kmsg-bridge line matching '$IDENT_PAT' reached the console -- the bridge must be OPERATOR.LOG-only (vms-32a round 3 / PR #365)"
    else
        ok "no kmsg-bridge line matching '$IDENT_PAT' reached the console (vms-32a round 3)"
    fi
done
# %OVMX-*-SYSID and %OVMX-*-KMOD are checked separately: SYSID has no other
# console user, but KMOD needs the message TEXT to distinguish the bridge's
# own idents from anything else that might legitimately share the facility
# (nothing does today, but the check is written to survive that changing).
if printf '%s\n' "$FID_FULL" | grep -qE '^%OVMX-[A-Z]-SYSID, '; then
    bad "a %OVMX-*-SYSID line (kmsg bridge) reached the console -- must be OPERATOR.LOG-only"
else
    ok "no %OVMX-*-SYSID line on the console (vms-32a round 3)"
fi
if printf '%s\n' "$FID_FULL" | grep -qE '^%OVMX-[A-Z]-KMOD, '; then
    bad "a %OVMX-*-KMOD line (kmsg bridge -- vms.ko lifecycle or the kernel's own taint-warning collision) reached the console -- must be OPERATOR.LOG-only"
else
    ok "no %OVMX-*-KMOD line on the console -- including the kernel's own module-taint warning, which the bridge now logs, never broadcasts (vms-32a round 3)"
fi
# ...and no bare/raw Linux dmesg-shaped noise reached the console either
# (the specific strings CI's failure quoted, from either bridge facility).
for NOISE in 'Loaded X.509 cert' 'Key type' '_CPC object is not present' \
             'hrtimer: interrupt took' 'taints kernel' 'tainting kernel'; do
    if printf '%s\n' "$FID_FULL" | grep -qF "$NOISE"; then
        bad "kernel-substrate noise ('$NOISE') reached the console in ANY form -- must be OPERATOR.LOG-only (PR #365)"
    else
        ok "no '$NOISE' noise on the console (absent this run, or correctly routed to OPERATOR.LOG only)"
    fi
done

# --- 1d. THE CONSOLE SEQUENCE MATCHES THE PINNED ORACLE SHAPE, INLINE ------
# The authoritative check is tests/qemu/test_boot_conformance.sh (run
# separately, same image); this is a lighter-weight inline corroboration on
# THIS run's own transcript, using the same token-extraction shape, so a
# regression here is caught by this test too, not only by the sibling.
BOOT_TOKENS=$(printf '%s\n' "$FID_FULL" | \
    grep -oE '%[A-Z][A-Z0-9_]*-[A-Z]-[A-Z0-9_]+|OpenVMX V[0-9]|Username:' | \
    sed -E 's/^OpenVMX V[0-9]$/__BANNER__/; s/^Username:$/__USERNAME__/' | \
    sed -n '1,/^__USERNAME__$/p')
EXPECTED_TOKENS='%OVMX-I-EXEC
__BANNER__
%OVMX-I-SYSDISK
%OVMX-I-MOUNTED
%OVMX-I-SCSNODE
%STDRV-I-STARTUP
%OVMX-I-EXEC
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%RUN-S-PROC_ID
__USERNAME__'
if [ "$BOOT_TOKENS" = "$EXPECTED_TOKENS" ]; then
    ok "console facility+ident token sequence matches the pinned oracle-derived shape exactly (vms-1fb/vms-32a)"
else
    bad "console token sequence diverges from the pinned oracle-derived shape"
    echo "  --- diff (expected vs actual) ---"
    diff <(printf '%s\n' "$EXPECTED_TOKENS") <(printf '%s\n' "$BOOT_TOKENS") | sed 's/^/    /' || true
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

# --- 4b. THE INTERACTIVE SESSION IS A DISTINCT EXECUTIVE PROCESS FROM
#         JOB_CONTROL (vms-d4ef). On real OpenVMS the job controller $CREPRCs
#         the console login as a genuinely NEW process with its OWN process id,
#         DISTINCT from JOB_CONTROL's (oracle
#         docs/oracle/vax73-show-system-process.md: JOB_CONTROL and the
#         interactive SYSTEM session appear at different pids). Previously
#         OVMX's login child called vms_kif_register_continue() and so SHARED
#         JOB_CONTROL's vms_pid -- SHOW SYSTEM listed a JOB_CONTROL row and a
#         SYSTEM row at the SAME pid. The child now registers FRESH (its own
#         vms_pid) and re-establishes the SYSTEM identity via
#         vms_kif_establish_system(), so the two are two processes at two pids.
#
#         This session logged in as SYSTEM, so its OWN row is named SYSTEM
#         (tools/vms_login.c $SETPRN's the account username, oracle-confirmed).
#         Its pid MUST differ from JOB_CONTROL's. This reads only the pid and
#         name columns, so it is unaffected by the accounting columns the VAX
#         lane restores to this table.
SYS_ROW=$(printf '%s\n' "$SHOW_SEG" | grep -E '^[0-9A-Fa-f]{8} SYSTEM ' | head -1)
if [ -n "$SYS_ROW" ]; then
    SYS_PID=$(printf '%s' "$SYS_ROW" | cut -c1-8)
    if [ "$SYS_PID" != "$JC_PID" ]; then
        ok "the interactive SYSTEM session ($SYS_PID) is a DISTINCT process from JOB_CONTROL ($JC_PID) -- login is not a JOB_CONTROL continuation (vms-d4ef)"
    else
        bad "the SYSTEM session and JOB_CONTROL both show pid $JC_PID -- the login is still continuing JOB_CONTROL's identity (vms-d4ef regression)"
    fi
else
    bad "SHOW SYSTEM listed no SYSTEM-named row for the logged-in interactive session (expected the \$SETPRN'd session row)"
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

# --- 6. THE /dev/kmsg BRIDGE'S LINES ACTUALLY LAND IN OPERATOR.LOG
# (vms-32a, round 3) -- the positive half of "console is silent, but the
# information is not lost." The bridge's reader thread captures every routed
# vms.ko/vmsfs.ko event to a durable append-only seed spool from the start of
# boot, and PROVISION.EXE seeds the on-volume OPERATOR.LOG from that spool
# (opcom_kmsg_seed_operator_log). The spool never wraps, so this is
# DETERMINISTIC -- every routed record is present in this TYPE dump (OPLOG_SEG,
# above), not "usually present". Before vms-98c2 the log was seeded by re-reading
# the fixed-size kernel ring buffer, which could evict early SYSID/LNM/MBX
# records before provision ran (worse under a slow CI boot), so these lines went
# intermittently missing -- that flake is what the seed-from-spool fix removed.
for MUST_HAVE in '^%OVMX-I-SYSID, ' '^%OVMX-I-LNM, ' '^%OVMX-I-MBX, ' \
                  '^%OVMX-I-VMSFS, '; do
    if printf '%s\n' "$OPLOG_SEG" | grep -qE "$MUST_HAVE"; then
        ok "OPERATOR.LOG contains the kmsg-bridge line matching '$MUST_HAVE' (vms-32a round 3)"
    else
        bad "OPERATOR.LOG is missing the kmsg-bridge line matching '$MUST_HAVE' -- the bridge did not reach the real file"
    fi
done

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
