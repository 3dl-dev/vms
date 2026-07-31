#!/bin/sh
#
# test_show_device_rows.sh - BEHAVIOURAL gate (vms-fb9): SHOW DEVICE emits no
# row it did not read from /dev/vms, and never answers in VMS's voice for a
# question the executive did not answer.
#
# WHY THIS EXISTS, AND WHY THE SOURCE-SCAN GATE WAS NOT ENOUGH.
# tests/integration/test_terminal_identity.sh checks that specific TOKENS are
# absent: "/proc/mounts", "vms_device_table", "_FTA0:". Those are the
# vocabulary of the fabrication that was deleted. On 2026-07-30 an adversary
# reintroduced the identical DEFECT -- a hardcoded row emitted when the
# executive returns nothing -- written in the NEW oracle format instead
# (a memset'd struct vms_devinfo named "OPA0:" handed to show_device_row()).
# Every one of those token checks passed, all twelve negative controls
# passed, and full ctest was green. A gate that recognises the OLD SPELLING
# of a lie does not stop the lie.
#
# So this file asserts the PROPERTY instead, and does it by RUNNING DCL:
#
#   With no executive present, `SHOW DEVICE` must put NOTHING on stdout.
#
# Not "nothing that looks like the old format" -- nothing at all. The probe
# script appends one WRITE SYS$OUTPUT, so the whole of stdout must be exactly
# that one line. Any row, in ANY format anyone might invent, adds bytes and
# turns this red. It is also not satisfiable by DCL failing to start, because
# the marker line has to be there.
#
# The second property is Rule 10's: when the executive did not answer, DCL
# must not print the oracle-pinned "%SYSTEM-W-NOSUCHDEV, no such device
# available". That message was MEASURED for exactly one condition -- a named
# device that the executive says does not exist (docs/oracle/
# vax73-terminal-device.md section 6). Using it for "the executive rejected
# us" is a false statement wearing an oracle citation.
#
# REVISED 2026-07-31 (vms-fb9 r5), applying the general Rule 10 ruling
# already on the books for the identical condition, not a new local choice.
# This file used to ALSO require stderr be non-empty for the
# unanswered-read case ("an unanswered read must be reported, not
# swallowed"), and src/vmsdcl/dcl_cmd_show.c satisfied that with an invented
# "%OVMX-F-EXECDEV" message. That message was itself the illegal third
# answer under Rule 10 (CLAUDE.md): "the executive did not answer" is the
# SAME per-call executive-absent condition vms-a35/vms-0ff already deleted,
# product-wide, from src/libvms/syssvc/sys_lock.c and
# src/libvmssys/vms_kif.c -- made unreachable rather than handled, because
# PID 1 refuses to bring the system up without /dev/vms. Re-inventing a
# handler for it here was the identical mistake one level up. So the
# property is now the OPPOSITE: stderr must be EMPTY for this case, and
# "not swallowed" is satisfied a different way -- $STATUS still carries the
# raw status (see check_status_reports_failure below), which is exactly the
# treatment src/libvmssys/vms_kif.c's kif_bind() gives a bind it cannot
# complete.
#
# stdout and stderr are captured SEPARATELY and never merged: rows go to
# stdout via printf() and diagnostics to stderr via dcl_error(), and the two
# streams are buffered differently, so a merged capture would interleave
# non-deterministically. A test paced by luck is a flaky test.
#
# tests/integration/test_show_device_rows_negctl.sh builds mutant DCLs --
# including the exact adversary mutant above -- and requires each to drive
# this file RED.
#
# ADDED 2026-07-31 (vms-fb9 r6), PROPERTY 4: A STATUS ALONE IS NOT EVIDENCE.
# An adversary found that property 2b ($STATUS = 676) is satisfiable by
# something OTHER than the behaviour under test: `return SS$_BUGCHECK;` at
# the very top of cmd_show_device(), before vms_kif_open() or any ioctl,
# ALSO sets $STATUS = 676 and prints nothing -- so it passed every check in
# this file (mutation M3, tests/integration/test_show_device_rows_negctl.sh
# case D). "$STATUS carries the ioctl failure" was true of the REAL failure
# path and also true of a status simply assigned by hand; the gate could
# not tell them apart.
#
# check_executive_read_attempted() closes that gap with evidence a
# fabricated status cannot produce: a real openat("/dev/vms", O_RDWR)
# syscall, observed with strace. src/libvmssys/vms_kif.c's vms_kif_open()
# is a RAW KERNEL SYSCALL (src/libvmssys/vms_syscall.h, no libc wrapper,
# so LD_PRELOAD interposition could not see it -- strace attaches at the
# kernel syscall boundary and sees it regardless), issued unconditionally
# by kif_bind() before every /dev/vms ioctl, and RETRIED ON EVERY CALL
# because a failed open leaves vms_dev_fd negative
# (src/libvmssys/vms_kif.c vms_kif_open(): `if (vms_dev_fd >= 0) return
# vms_dev_fd;` never fires when the previous attempt failed). Measured: a
# real `SHOW DEVICE` against this build issues the syscall twice; mutation
# M3 issues it zero times. If strace is not available this property CANNOT
# be evaluated, so it is reported as FAILED, never silently skipped
# (CLAUDE.md Rule 10 -- the same convention run_facility_negctl.sh already
# uses for a missing cmake).
#
# NOT CLAIMED: that this is a complete defence against every possible
# vacuous handler. It closes the ONE hole M3 demonstrated (a status
# assigned without any executive interaction). A handler that DOES open
# /dev/vms for some unrelated reason and then still fabricates a row or a
# status would defeat this property too and would need a different anchor
# again -- this comment does not claim otherwise.
#
# UPDATED 2026-07-31 (vms-fb9 r7): check_executive_read_attempted() also
# verifies strace itself actually traced the tracee (its own exit status,
# plus the tracee's liveness marker in strace's captured stdout) before
# trusting an absent "/dev/vms" match as the real M3-shaped red. A container
# that denies ptrace produces the same "nothing observed" shape as M3 but is
# a broken fixture, not a property verdict, and is reported as such -- never
# as a pass.
#
# Usage: test_show_device_rows.sh [PATH_TO_DCL.EXE]

set -u

DCL="${1:-${VMSDCL:-}}"
if [ -z "$DCL" ]; then
    for cand in "$(dirname "$0")/../../build/bin/DCL.EXE" \
                "$(dirname "$0")/../../build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

status=0
MARK='OVMX-PROBE-ALIVE'
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
STRACE=$(command -v strace 2>/dev/null || true)

echo "vms-fb9 behavioural gate: SHOW DEVICE prints only rows it read from /dev/vms"

if [ ! -x "$DCL" ]; then
    echo "FAIL: DCL.EXE not found (looked at '$DCL')"
    echo "  -> this gate must RUN the command; it cannot be evaluated by reading"
    exit 1
fi

# run_show <command-line>  -> $WORK/out (stdout), $WORK/err (stderr)
run_show() {
    printf '%s\nWRITE SYS$OUTPUT "%s"\n' "$1" "$MARK" \
        | "$DCL" >"$WORK/out" 2>"$WORK/err"
}

# The executive is reached through /dev/vms and nowhere else, so its presence
# is what decides which half of the property is checkable here. ctest never
# runs anywhere that has one (CLAUDE.md Rule 9); the QEMU runtime always does.
if [ -c /dev/vms ]; then
    HAVE_EXEC=1
    echo "  (executive present: /dev/vms is a character device)"
else
    HAVE_EXEC=0
    echo "  (no executive: /dev/vms absent)"
fi

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

# --- Property 1: with no executive, not one byte of a device row ----------
check_no_rows() {
    label="$1"; cmdline="$2"
    run_show "$cmdline"
    actual=$(cat "$WORK/out")
    if [ "$actual" = "$MARK" ]; then
        echo "  OK: $label emitted no device row"
    else
        fail "$label put something other than the liveness marker on stdout" \
             "with no executive there is no row to print, so stdout must be" \
             "exactly '$MARK'. It was:" "$(sed 's/^/       | /' "$WORK/out")"
    fi
}

# --- Property 2: no VMS device verdict for a question VMS never answered --
check_no_vms_verdict() {
    label="$1"; cmdline="$2"
    run_show "$cmdline"
    if grep -q 'NOSUCHDEV\|IVDEVNAM' "$WORK/err"; then
        fail "$label answered with a VMS device verdict the executive never gave" \
             "the executive did not answer at all, so NOSUCHDEV / IVDEVNAM --" \
             "both measured on the oracle for conditions the executive DID" \
             "answer -- are false statements in VMS's own voice (Rule 10)." \
             "stderr was:" "$(sed 's/^/       | /' "$WORK/err")"
    else
        echo "  OK: $label did not borrow a VMS device verdict"
    fi
    # vms-fb9 r5: the unanswered-read case is SILENT by design now, not an
    # oversight -- see the file header. stderr must be EMPTY here; a
    # non-empty stderr means something invented a message for a condition
    # Rule 10 says must be unreachable, not handled (the deleted
    # %OVMX-F-EXECDEV was exactly this).
    if [ -s "$WORK/err" ]; then
        fail "$label wrote to stderr for an unanswered read" \
             "vms-fb9 r5: this is silent by design -- \$STATUS carries the" \
             "failure instead (see check_status_reports_failure). stderr was:" \
             "$(sed 's/^/       | /' "$WORK/err")"
    else
        echo "  OK: $label was silent (not swallowed -- \$STATUS still carries it)"
    fi
}

# --- Property 2b: silent is not swallowed -- $STATUS still carries the
# ioctl-level failure even though nothing is printed for it. -----------------
check_status_reports_failure() {
    label="$1"; cmdline="$2"
    printf '%s\nSHOW SYMBOL $STATUS\n' "$cmdline" \
        | "$DCL" >"$WORK/out2" 2>"$WORK/err2"
    # 676 = SS$_BUGCHECK, the default of vms_kif_kerr_to_ss()'s closed
    # errno-mapping switch (src/libvmssys/vms_kif.c) for the EBADF an ioctl
    # on the never-opened /dev/vms descriptor produces. Measured, not
    # assumed: with only 'MOUNT DUA0: TESTDISK' and no SHOW DEVICE at all,
    # $STATUS reads 1 (SS$_NORMAL, MOUNT's own status) -- so 676 here can
    # only come from this command's own executive read failing, not from
    # something upstream.
    if grep -qF '$STATUS = 676' "$WORK/out2"; then
        echo "  OK: $label set \$STATUS = 676 (SS\$_BUGCHECK) though it printed nothing"
    else
        fail "$label did not leave \$STATUS carrying the ioctl failure" \
             "expected '\$STATUS = 676' in SHOW SYMBOL \$STATUS output; got:" \
             "$(sed 's/^/       | /' "$WORK/out2")"
    fi
}

# --- Property 4: the executive was ACTUALLY read, not merely a status ----
# fabricated without touching it. See the file header (added vms-fb9 r6)
# for why $STATUS alone (property 2b) is not enough.
#
# RE-ANCHORED (vms-2b8, 2026-07-31): this used to check for ANY
# openat("/dev/vms", ...) syscall. vms-2b8 made src/vmsdcl/dcl_main.c's
# dcl_context_init() call vms_kif_getjpi_self() UNCONDITIONALLY at DCL
# startup (reading the executive's identity row instead of the
# environment) -- so as of that change EVERY DCL invocation opens
# /dev/vms before cmd_show_device() ever runs, mutation or not. Measured
# directly (strace -e trace=openat,ioctl over a real `SHOW DEVICE`, no
# executive present): the openat("/dev/vms", ...) attempt now appears in
# BOTH the mutated (M3) and unmutated runs, so it stopped discriminating
# the property it exists to catch -- it would have passed mutation D
# silently, exactly the vacuity class this property was added to close.
#
# The re-anchor is one of the two device-read ioctls src/kernel/vms_ioctl.h
# defines (VMS_IOC_MAGIC='V'=0x56): VMS_IOCTL_DEVSCAN, cmd 0x53
# (_IOWR(..., 0x53, struct vms_devscan_args), pinned by its own
# _Static_assert against 0xC0505653), issued by vms_kif_devscan() for a
# BARE "SHOW DEVICE"; and VMS_IOCTL_GETDVI, cmd 0x52
# (_Static_assert-pinned against 0xC0585652 -- vms_ioctl.h's own encoding
# of _IOWR(0x56, 0x52, struct vms_getdvi_args); an earlier round of this
# comment transposed two digits and cited 0xC0505752, a value with no
# matching _Static_assert anywhere in the tree), issued by
# vms_kif_getdvi_devnam() for a NAMED "SHOW DEVICE <dev>" -- measured
# directly, the two forms do not share an ioctl. Neither is the identity
# read dcl_context_init() now issues at startup (VMS_IOCTL_GETJPI, cmd
# 0x42), so either one is proof that cmd_show_device() itself, not merely
# DCL's startup, reached the executive. kif_call() issues its ioctl
# UNCONDITIONALLY, on whatever descriptor kif_bind() produced (even -1/-2
# when /dev/vms could not be opened), so it is observable by strace
# whether or not the executive answers -- exactly the same "issued
# regardless of success" property the openat anchor used to provide, one
# door further in. Measured: strace shows a `0x56, 0x53` (bare) or
# `0x56, 0x52` (named) ioctl for a real SHOW DEVICE and NEITHER for
# mutation D (which returns before either vms_kif_* call is made).
R_NOREAD='no VMS_IOCTL_DEVSCAN/GETDVI ioctl (0x56, 0x53 / 0x56, 0x52) was observed by strace'
# BROKEN FIXTURE vs. real red (vms-fb9 r7, corrected r8): a container that
# denies ptrace (common -- Docker's default seccomp profile blocks ptrace(2)
# without --cap-add=SYS_PTRACE) makes strace's OWN PTRACE_TRACEME fail. That
# is fatal to strace -- it never execs the tracee at all (verified against a
# real denied-ptrace sandbox: a seccomp filter that errno's ptrace(2)
# reproduces "strace: ptrace(PTRACE_TRACEME, ...): Operation not permitted",
# strace exits nonzero, and the tracee's own stdout never appears -- so
# $WORK/strace_stdout has no liveness marker and $WORK/strace.out is empty).
#
# STRACE'S OWN EXIT STATUS IS NOT EVIDENCE OF THIS. When tracing succeeds,
# strace exits with the TRACEE's own exit status (documented strace
# behaviour), so `SHOW DEVICE` piped into a script that later does
# `EXIT <nonzero>` makes a perfectly healthy trace exit nonzero too -- r7
# read that single number as "strace itself failed" and reported BROKEN
# FIXTURE for a trace that worked, inverting the property it exists to
# protect. Measured directly (see the round 8 progress note for the pasted
# commands): a real DCL run with `EXIT 44` appended exits 44 under strace
# while still writing the liveness marker and still showing two
# openat(.., "/dev/vms", ..) records in strace.out.
#
# So the two statuses are read SEPARATELY and neither one substitutes for
# the other:
#   - whether the tracee ran far enough to write the liveness marker decides
#     BROKEN FIXTURE: a trace that never got that far recorded nothing usable.
#   - the openat("/dev/vms", ...) record in strace.out, unconditional on the
#     tracee's own exit code, decides the actual property.
# Neither state is ever reported as a pass when it cannot be evaluated.
R_BROKEN='strace could not trace the tracee at all (ptrace denied or unusable) -- BROKEN FIXTURE, not a property verdict'
check_executive_read_attempted() {
    label="$1"; cmdline="$2"
    if [ -z "$STRACE" ]; then
        fail "$label: strace is not available" \
             "this property cannot be evaluated without it, so it is" \
             "reported as FAILED, never silently skipped (Rule 10)"
        return
    fi
    printf '%s\nWRITE SYS$OUTPUT "%s"\n' "$cmdline" "$MARK" \
        | "$STRACE" -f -e trace=openat,ioctl -o "$WORK/strace.out" "$DCL" \
              >"$WORK/strace_stdout" 2>"$WORK/strace_stderr"
    strace_rc=$?
    # strace_rc mirrors the TRACEE's own exit status once tracing succeeds
    # (see the comment block above) -- it is deliberately NOT consulted to
    # decide broken-vs-real below. The liveness marker is the only signal.
    if ! grep -qF "$MARK" "$WORK/strace_stdout" 2>/dev/null; then
        fail "$label: $R_BROKEN" \
             "strace exited $strace_rc and the tracee's liveness marker" \
             "'$MARK' never appeared in its stdout, so the tracee did not" \
             "run to completion under trace." \
             "This property CANNOT be evaluated in this state, so it is" \
             "reported as FAILED, never a silent pass (Rule 10; same" \
             "convention as tests/integration/test_runtime_target_negctl.sh's" \
             "expect_red() and tests/qemu/run_facility_negctl.sh)." \
             "strace stderr was:" "$(sed 's/^/       | /' "$WORK/strace_stderr" 2>/dev/null)"
        return
    fi
    if grep -E 'ioctl\(.*0x56, 0x5[23]' "$WORK/strace.out" >/dev/null 2>&1 \
        || grep -iE '0xc0585652|0xc0505653' "$WORK/strace.out" >/dev/null 2>&1; then
        echo "  OK: $label really issued a device-read ioctl (strace-observed)"
    else
        fail "$label did not prove the executive was actually read" \
             "$R_NOREAD" \
             "\$STATUS alone is not evidence -- a handler can fabricate it" \
             "without ever calling vms_kif_devscan()/vms_kif_getdvi_devnam()" \
             "(vms-fb9 r6 mutation M3; re-anchored from openat to these ioctls" \
             "under vms-2b8, see the comment above -- DCL's own startup now" \
             "opens /dev/vms unconditionally, so openat alone no longer" \
             "discriminates)." \
             "ioctl() calls strace saw:" \
             "$(grep -F 'ioctl(' "$WORK/strace.out" 2>/dev/null | tail -8 | sed 's/^/       | /')"
    fi
}

# --- Property 3: with an executive, the console IS listed ----------------
check_lists_console() {
    label="$1"; cmdline="$2"
    run_show "$cmdline"
    if grep -q '^OPA0: ' "$WORK/out"; then
        echo "  OK: $label listed the executive's console device"
    else
        fail "$label did not list OPA0: from the executive device table" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    fi
}

if [ "$HAVE_EXEC" -eq 0 ]; then
    check_no_rows "bare SHOW DEVICE" 'SHOW DEVICE'
    check_no_rows "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
    check_no_rows "SHOW DEVICE ZZA0:" 'SHOW DEVICE ZZA0:'
    check_no_rows "SHOW DEVICE DUA0:" 'SHOW DEVICE DUA0:'
    check_no_vms_verdict "bare SHOW DEVICE" 'SHOW DEVICE'
    check_no_vms_verdict "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
    check_status_reports_failure "bare SHOW DEVICE" 'SHOW DEVICE'
    check_status_reports_failure "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
    check_executive_read_attempted "bare SHOW DEVICE" 'SHOW DEVICE'
    check_executive_read_attempted "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
else
    check_lists_console "bare SHOW DEVICE" 'SHOW DEVICE'
    check_lists_console "SHOW DEVICE OPA0:" 'SHOW DEVICE OPA0:'
    # A device the executive does not have must still be refused with the
    # oracle's own message -- this half of Rule 10 is "match VMS".
    run_show 'SHOW DEVICE ZZA0:'
    if grep -q 'NOSUCHDEV, no such device available' "$WORK/err"; then
        echo "  OK: an absent device gets the oracle's NOSUCHDEV"
    else
        fail "SHOW DEVICE ZZA0: did not report the oracle's NOSUCHDEV" \
             "stderr was:" "$(sed 's/^/       | /' "$WORK/err")"
    fi
    if [ "$(cat "$WORK/out")" != "$MARK" ]; then
        fail "SHOW DEVICE ZZA0: printed a row for a device that does not exist" \
             "stdout was:" "$(sed 's/^/       | /' "$WORK/out")"
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "vms-fb9 behavioural gate: PASS"
else
    echo "vms-fb9 behavioural gate: FAIL"
fi
exit $status
