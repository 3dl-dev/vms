#!/bin/bash
# OVMX User Acceptance Test — scripted VMS session on THE REAL RUNTIME
#
# Runs inside the ovmx-boot image (QEMU + kernel + initramfs):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/uat/vms_session_qemu.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# WHY THIS EXISTS (vms-71a, epic vms-6b8)
#
# The UAT used to SSH into the dead-legacy Docker container on port 2222. That
# container has no /dev/vms, so what the UAT was really certifying was a full
# interactive VMS session running with no executive backing -- not what the
# real runtime is. CLAUDE.md Rule 9 (operator ruling 2026-07-28) retired that
# container as a runtime target, so the UAT moves onto the runtime OVMX
# actually has: boot the real kernel under QEMU, log in over the console, and
# drive DCL there.
#
# PROVENANCE (disclosed per vms-71a re-dispatch ruling, completed by the
# vms-a35 PR #6 rebase): this script started as a near-verbatim copy of
# origin/vms-0ff-executive-fatal:tests/uat/vms_session_qemu.sh, with the
# `wait_for '%STARTUP-I-EXEC'` assertion that branch has deliberately NOT
# ported (main's src/ovmx_init/ovmx_init.c did not print that string yet, so
# waiting for it would have hung forever). vms-0ff has since merged, so
# ovmx_init.c prints an executive-attached line once the kernel executive
# attaches, and the wait is ported below (see "Boot and log in"). Everything
# else in the harness (assertion anchoring, prompt sync, timeout split) is
# unique to this file and was never present on vms-0ff-executive-fatal.
#
# vms-a35 round 2: the string itself moved from `%STARTUP-I-EXEC` to
# `%OVMX-I-EXEC` (see src/ovmx_init/ovmx_init.c, executive_attach) --
# a message that names a Linux device node (/dev/vms) is an OVMX event,
# not a VMS one, and may not wear the STARTUP facility (Rule 10). This
# wait was updated to match.
#
# NEARLY THE SAME COMMANDS as tests/uat/vms_session_test.sh (the retired SSH
# UAT), with ONE deliberate change: `SET DEFAULT SYS$MANAGER` (no trailing
# colon) is sent here as `SET DEFAULT SYS$MANAGER:`. Empirically verified on
# this runtime (2026-07-30): the bare form fails --
# `%DCL-E-DIRECT, invalid directory - \SYS$MANAGER\` -- for EVERY bare
# logical name tried, including SYS$LOGIN, while the same name with a
# trailing colon (`SYS$MANAGER:`) succeeds and SHOW DEFAULT correctly
# reflects it. Whether OpenVMS itself requires the colon here, or OVMX's
# SET DEFAULT has a real gap handling a bare logical name (src/vmsdcl/
# dcl_cmd_set.c:cmd_set_default), is a VMS-semantics question this
# CI-migration item is not scoped to answer -- filed separately (see the item
# this script's commit references). This script uses the syntax proven to
# work so the assertion is exercising real DCL behavior instead of masking
# an unrelated, undiagnosed gap.
#
# The ASSERTIONS are NOT a verbatim carryover from the SSH UAT either: this
# harness anchors each echo-sensitive check to the specific command's own
# response segment (see "Assertion anchoring" below) rather than grepping
# the whole console log, because the whole log also contains the guest tty's
# echo of every command this script sends -- a naive `grep` for e.g. 'SYSTEM'
# would be satisfied by the echo of `send 'SYSTEM'` at the login prompt
# regardless of what any real command printed. Where the two runtimes
# genuinely differ, the real runtime is the authority.
#
# Sequencing waits on the actual DCL prompt rather than sleeping a guessed
# number of seconds. A UAT paced by fixed sleeps is a flaky test waiting to
# happen, and a flaky test is a broken test (CLAUDE.md Rule 11).

set -uo pipefail

KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# PRE-INSTALLED distribution disk (vms-8ab): PID 1 no longer installs on a blank
# disk (vms-2f0, operator ruling 2026-08-10 "STRIP ALL OF IT"), so the UAT seeds
# its disk from the mastered image and boots an already-installed system.
DISTRIB_IMG=/boot/ovmx-distrib.img
DISK=/tmp/uat-sysdisk.img
CONSOLE_LOG=/tmp/uat-console.log
FIFO=/tmp/uat-console.in

# Timeouts are split so a slow boot cannot silently eat the budget for the
# rest of the session (or vice versa) and surface as an opaque mid-session
# guest death instead of a diagnosable timeout at the right stage.
BOOT_TIMEOUT=120     # budget for EACH of the two boot-phase waits below
                      # (executive attach, then the login prompt) -- see
                      # SESSION_TIMEOUT's comment for why it is counted twice
STEP_TIMEOUT=60      # budget for each named login-flow wait (password/welcome/logout)
COMMAND_TIMEOUT=10   # budget for the DCL prompt to return after one command
                      # (observed on this host: the whole session completes
                      # in well under 1s per command under TCG -- 10s is a >10x margin)

# --- The two sessions this UAT drives ------------------------------------
# Declared here rather than at the loops so SESSION_TIMEOUT below can be
# computed from their real lengths instead of a hand-maintained count that
# drifts every time a command is added.
#
# SESSION 1 is the SYSTEM account. SESSION 2 (vms-2b8 round 7) logs in AGAIN
# as an ordinary user, because after LOGINOUT began dropping to the
# authenticated user's real credentials there is a whole class of behaviour
# that only a second, unprivileged account can show: what SYSTEM may do to
# the VMS system tree, and what an ordinary user may not.
SYSTEM_CMDS=(
    'SHOW TIME'
    'SHOW SYSTEM'
    'SHOW MEMORY'
    'SHOW DEFAULT'
    'SET DEFAULT SYS$MANAGER:'
    'SHOW DEFAULT'
    'DIRECTORY'
    'SHOW PROCESS'
    'SHOW PROCESS /PRIVILEGES'
    'IDENT_OPER = F$PRIVILEGE("OPER")'
    'SHOW SYMBOL IDENT_OPER'
    'IDENT_SETPRV = F$PRIVILEGE("SETPRV")'
    'SHOW SYMBOL IDENT_SETPRV'
    'SPAWN SHOW PROCESS'
    'SPAWN SHOW TIME'
    'COPY LOGIN.COM UATWRITE.TXT'
    'TYPE UATWRITE.TXT'
    'COPY LOGIN.COM SYS$SYSTEM:UATSYS.TXT'
    'TYPE SYS$SYSTEM:UATSYS.TXT'
    'DELETE SYS$SYSTEM:UATSYS.TXT;1'
    'DIRECTORY SYS$SYSTEM:'
    'SHOW LOGICAL SYS$LOGIN'
    'DEFINE UAT_TEST "session_test_passed"'
    'SHOW LOGICAL UAT_TEST'
    'DEASSIGN UAT_TEST'
    'SHOW USERS'
    'SHOW USERS/FULL'
    'SHOW TERMINAL'
    'SHOW DEVICE'
    'SHOW DEVICE VDA0:'
    'SHOW DEVICE/FULL VDA0:'
    'HELP SHOW'
)

# Index of the ORIGINAL 'SHOW PROCESS /PRIVILEGES' occurrence above, found
# programmatically (not hand-counted) BEFORE the round-4 block below adds a
# SECOND occurrence of the same text. Needed for the SAME reason
# check_response_at exists (see its own comment): once a second occurrence
# of this exact command text exists anywhere in SYSTEM_CMDS,
# CMD_OUTPUT['SHOW PROCESS /PRIVILEGES'] holds whichever one ran LAST, so
# even the check against the ORIGINAL, pre-SET occurrence must stop relying
# on the text-keyed lookup once a duplicate is introduced -- checking it by
# text after this point would (silently, if the two happened to answer the
# same way) or would not (if they diverged) be checking the right one by
# accident, not by construction.
IDX_PRIV_ORIGINAL=-1
for __i in "${!SYSTEM_CMDS[@]}"; do
    if [ "${SYSTEM_CMDS[$__i]}" = "SHOW PROCESS /PRIVILEGES" ]; then
        IDX_PRIV_ORIGINAL=$__i
        break
    fi
done
unset __i

# LOUD FAILURE, NOT A SILENT RETARGET, if the anchor above was not found
# (vms-2b8 round 5). Bash arrays accept NEGATIVE indices (>= 4.3) as
# offsets from the end, so a left-at-default IDX_PRIV_ORIGINAL=-1 would
# make check_response_at() below silently check the LAST command in
# CMD_OUTPUT_SEQ instead of erroring -- a check that cannot fail by
# construction whenever someone renames or removes the original 'SHOW
# PROCESS /PRIVILEGES' entry above, which is exactly the defect class
# this harness exists to hunt. Fail the whole run here, before any
# session is even started, rather than let that happen silently.
if [ "$IDX_PRIV_ORIGINAL" -lt 0 ]; then
    echo "UAT HARNESS ERROR: 'SHOW PROCESS /PRIVILEGES' anchor not found" \
         "in SYSTEM_CMDS -- IDX_PRIV_ORIGINAL would silently retarget to" \
         "the last command instead of failing. Fix the harness, do not" \
         "let this run." >&2
    exit 1
fi

# DEFECT-1 PROOF (vms-2b8 round 4): SET PROCESS/PRIVILEGES must not corrupt
# what F$PRIVILEGE or SHOW PROCESS/PRIVILEGES report for privileges the
# executive actually holds. MEASURED before this fix, on this exact runtime:
#   $ SHOW PROCESS/PRIVILEGES        -> Authorized: CMEXEC CMKRNL SETPRV WORLD
#   $ SET PROCESS/PRIVILEGES=(OPER)
#   $ SHOW PROCESS/PRIVILEGES        -> unchanged (reads the executive)
#   $ F$PRIVILEGE("SETPRV")          -> "FALSE"   <-- was "TRUE" before the SET
# 'SHOW PROCESS /PRIVILEGES' and the F$PRIVILEGE pattern both already
# appear earlier in SYSTEM_CMDS above, so CMD_OUTPUT (keyed by command
# TEXT -- see run_cmd) cannot be used to check these SECOND occurrences:
# the array slot for that text is already claimed by the FIRST
# occurrence's answer, and a second run_cmd() for the same text would
# silently take over the same slot with no signal either way. That
# exact hazard is check_response_at()'s reason to exist (see its comment
# below) -- these are checked BY POSITION, not by re-using the command
# text as a key. Indices are computed from ${#SYSTEM_CMDS[@]} at append
# time, not hand-counted, so inserting or removing an earlier command
# cannot silently desynchronize them.
SYSTEM_CMDS+=('SET PROCESS/PRIVILEGES=(OPER)')
SYSTEM_CMDS+=('SHOW PROCESS /PRIVILEGES')
IDX_PRIV_AFTER_SET=$(( ${#SYSTEM_CMDS[@]} - 1 ))
SYSTEM_CMDS+=('IDENT_SETPRV2 = F$PRIVILEGE("SETPRV")')
SYSTEM_CMDS+=('SHOW SYMBOL IDENT_SETPRV2')
IDX_SETPRV2_AFTER_SET=$(( ${#SYSTEM_CMDS[@]} - 1 ))
SYSTEM_CMDS+=('IDENT_WORLD2 = F$PRIVILEGE("WORLD")')
SYSTEM_CMDS+=('SHOW SYMBOL IDENT_WORLD2')
IDX_WORLD2_AFTER_SET=$(( ${#SYSTEM_CMDS[@]} - 1 ))

# DEFECT-1 PROOF, ROUND 5: the report (F$PRIVILEGE) and the GATE (SET
# PROCESS's own privilege check) must agree about the SAME process at the
# SAME instant -- round 4 fixed the REPORT (F$PRIVILEGE/SHOW PROCESS/
# PRIVILEGES all read the executive fresh) but left every DCL privilege
# GATE reading the RAW, unmasked ctx->privileges the session's identity
# was given at VMS_IOCTL_SETIDENT time. MEASURED before this fix, on this
# exact runtime, in this exact SYSTEM session:
#   $ IDENT_ALTPRI = F$PRIVILEGE("ALTPRI")
#   $ SHOW SYMBOL IDENT_ALTPRI        -> IDENT_ALTPRI = "FALSE"
#   $ SET PROCESS/PRIORITY=6          -> AUTHORIZED (no error) -- SAME
#                                         session, SAME moment the report
#                                         above said ALTPRI was not held.
# Root cause: SYSUAF's SYSTEM record authorizes ALL privileges, and
# VMS_IOCTL_SETIDENT sets cur_privs = the full authorized mask verbatim
# (OVMX design choice, vms_proctab.c) for a caller with SETPRV -- so
# ctx->privileges (dcl_main.c, filled from that same read) genuinely
# contains ALTPRI, even though F$PRIVILEGE and SHOW PROCESS/PRIVILEGES
# correctly mask it out of what they REPORT (VMS_PRV_M_ENFORCED). The
# GATE in cmd_set_process (src/vmsdcl/dcl_cmd_set.c) read the unmasked
# value, so it granted what the masked report had just denied.
# Fix: every gate in dcl_cmd_set.c now asks enforced_privs_held(), the
# SAME masked, freshly-read source F$PRIVILEGE and SHOW PROCESS/
# PRIVILEGES use -- so SET PROCESS/PRIORITY is refused whenever
# F$PRIVILEGE("ALTPRI") says FALSE, on this SYSTEM session included
# (ALTPRI is authorized by SYSUAF but not in VMS_PRV_M_ENFORCED, so it is
# never granted by strength alone until vms-pv1 gives it real
# enforcement -- Rule 10's HIDE answer, applied to the gate, not just the
# display).
# Checked BY POSITION (see the DEFECT-1 round-4 block above for why):
# IDENT_ALTPRI's text is unique so far in SYSTEM_CMDS, but SET PROCESS is
# not, and the discipline of anchoring every assertion in this block to a
# position rather than text is kept uniform rather than mixed per-command.
SYSTEM_CMDS+=('IDENT_ALTPRI = F$PRIVILEGE("ALTPRI")')
SYSTEM_CMDS+=('SHOW SYMBOL IDENT_ALTPRI')
IDX_ALTPRI=$(( ${#SYSTEM_CMDS[@]} - 1 ))
SYSTEM_CMDS+=('SET PROCESS/PRIORITY=6')
IDX_PRIORITY_SET=$(( ${#SYSTEM_CMDS[@]} - 1 ))

# F$GETJPI CURPRIV / AUTHPRIV FORMAT (vms-2b8 round 5). Before this round
# CURPRIV returned a decimal integer (never pinned to any VMS source) and
# AUTHPRIV silently returned "0" -- indistinguishable from "holds no
# privileges", false for this SYSTEM session. Both are now the
# comma-separated privilege-NAME string the public OpenVMS DCL Dictionary
# and the VSI Wiki's own F$GETJPI example document (see dcl_lexical.c's
# lex_getjpi() CURPRIV/AUTHPRIV comment for the two citations), masked to
# VMS_PRV_M_ENFORCED the same as every other surface in this session --
# so for this SYSTEM session, whose Authorized/Process privileges block
# above already reads CMEXEC CMKRNL SETPRV WORLD, both items must render
# the SAME four names, in ascending bit-position order (CMKRNL before
# CMEXEC, matching the oracle's own CURPRIV example order, not the
# alphabetical order SHOW PROCESS/PRIVILEGES uses).
SYSTEM_CMDS+=('IDENT_CURPRIV = F$GETJPI("","CURPRIV")')
SYSTEM_CMDS+=('SHOW SYMBOL IDENT_CURPRIV')
SYSTEM_CMDS+=('IDENT_AUTHPRIV = F$GETJPI("","AUTHPRIV")')
SYSTEM_CMDS+=('SHOW SYMBOL IDENT_AUTHPRIV')

# DEFECT-1 REGRESSION, DISCLOSED (vms-2b8 round 6): SET TIME's gate
# (OPER||SYSPRV||BYPASS, none of which VMS_PRV_M_ENFORCED names) can no
# longer be passed by ANY identity, including this SYSTEM session, whose
# SYSUAF record authorizes privilege ALL. This is the same shape as
# the ALTPRI/PRIORITY proof above -- a capability that worked before round
# 5's fix and cannot work now, for anyone, until vms-pv1 -- but SET TIME
# had no dedicated assertion of its own until this round. It gets one now,
# for the same reason PRIORITY did: an undisclosed behaviour change is not
# the same defect as an unenforced privilege, and hiding it from the test
# suite is exactly the silence Rule 10 forbids. The date is fictional and
# far enough in the future to not collide with the host clock; if this
# command somehow succeeded it would visibly jump SHOW TIME on the NEXT
# command, which nothing here reads, so no side effect is exercised or
# needed -- the check is purely on this command's own refusal.
SYSTEM_CMDS+=('SET TIME 1-JAN-2030:00:00:00')
IDX_TIME_SET=$(( ${#SYSTEM_CMDS[@]} - 1 ))

USER_CMDS=(
    'TYPE SYS$MANAGER:LOGIN.COM'
    'COPY SYS$MANAGER:LOGIN.COM UATUSER.TXT'
    'TYPE UATUSER.TXT'
    'COPY SYS$MANAGER:LOGIN.COM SYS$SYSTEM:UATDENY.TXT'
    'TYPE SYS$SYSTEM:UATDENY.TXT'
)
CMD_COUNT=$(( ${#SYSTEM_CMDS[@]} + ${#USER_CMDS[@]} ))

# Overall wall-clock kill switch for the whole QEMU process: two boot-phase
# waits (executive attach, then login prompt -- each at BOOT_TIMEOUT) + the
# named login/logout steps of BOTH sessions + every command, each at its own
# budget, plus slack. This
# is a safety net (the guest should return far faster than this in the normal
# case, observed 11-14s end to end on this host) so a genuinely wedged QEMU
# process cannot run forever -- it is NOT the pacing budget for any single
# stage (that would let a slow boot silently eat the rest of the session's
# time, CLAUDE.md Rule 11 / vms-71a re-dispatch finding). Kept comfortably
# under the CI step's `timeout-minutes: 10` (600s) in ci.yml's uat-session
# job so THIS timeout fires first with a diagnosis, instead of GH Actions
# SIGKILLing the job with no console log captured.
SESSION_TIMEOUT=$((BOOT_TIMEOUT * 2 + STEP_TIMEOUT * 6 + COMMAND_TIMEOUT * CMD_COUNT))

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

rm -f "$DISK" "$CONSOLE_LOG" "$FIFO"
if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG not found — run this inside the ovmx-boot image;"
    echo "       a stripped PID 1 cannot install a blank disk (vms-2f0)."
    exit 1
fi
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

echo "=== OVMX UAT — scripted VMS session (real kernel, QEMU) ==="
echo "Architecture: $ARCH / QEMU: $QEMU"
echo ""

# shellcheck disable=SC2086
timeout "$SESSION_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -nographic \
    -append "$CONSOLE loglevel=3 quiet" \
    -m 256M \
    -smp 1 \
    -nic none \
    -nodefaults \
    -serial stdio \
    -drive file="$DISK",format=raw,if=virtio \
    -no-reboot \
    <"$FIFO" >"$CONSOLE_LOG" 2>&1 &
QEMU_PID=$!

# Hold the FIFO open for the life of the session: if every writer closes, the
# guest console sees EOF and the login loop tears down mid-test.
exec 3>"$FIFO"

cleanup() {
    exec 3>&- 2>/dev/null || true
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT

send() { printf '%s\r' "$1" >&3; }

# Wait for text to appear in $CONSOLE_LOG. By default scans the whole file
# (byte offset 0); callers driving the command loop pass $since (a byte
# offset captured before send()) so they synchronise on output APPENDED
# after that point rather than on anything anywhere in the log -- which
# matters because the log also contains the tty echo of what was just sent,
# and (before login) the boot log. Returns 1 on timeout or if the guest
# died, so a hung boot or a wedged command fails fast with a diagnostic
# instead of silently feeding more input into a dead or stuck machine.
wait_for() {
    local pattern="$1" limit="${2:-$STEP_TIMEOUT}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$CONSOLE_LOG" 2>/dev/null | grep -qF "$pattern"; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "ERROR: guest exited while waiting for '$pattern'" >&2
            return 1
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    echo "ERROR: timed out after ${limit}s waiting for '$pattern'" >&2
    return 1
}

fail_with_console() {
    echo "$1" >&2
    echo "--- console output so far ---" >&2
    tail -60 "$CONSOLE_LOG" >&2
    exit 1
}

# --- Validation counters (vms-72c) -----------------------------------------
# Declared HERE, not down at "Validation" below where they used to live,
# because the bad-password probe immediately below this comment is a real
# assertion (PASS/FAIL/ERRORS-mutating) that runs during the boot/login
# phase, before the "Drive the session" block. `set -u` (nounset, top of
# this file) turns a reference to an undeclared PASS/FAIL/ERRORS into a hard
# script error, not a silent 0 -- which is what caught this the first time:
# moving the assertion up without moving the declaration failed the whole
# run with "PASS: unbound variable" before a single check_response() below
# ever got a chance to run.
PASS=0
FAIL=0
ERRORS=""

# --- Boot and log in -----------------------------------------------------
# The executive must come up first; if it does not, PID 1 aborts the boot and
# there is no session to test. Asserting it here (vms-0ff, ported by the
# vms-a35 PR #6 rebase) keeps a silent regression in the boot guarantee from
# surfacing as a confusing login timeout instead of a clear one.
wait_for '%OVMX-I-EXEC' "$BOOT_TIMEOUT" \
    || fail_with_console "ERROR: the executive never attached — the system did not come up"

send ''  # vms-2213: wake OPA0: — LOGINOUT waits for RETURN before Username:
wait_for 'Username:' "$BOOT_TIMEOUT" || fail_with_console "ERROR: no login prompt"

# --- NEGATIVE LOGIN: a wrong password is refused (vms-72c) -----------------
#
# WHY THIS IS HERE AND WHY IT WAS NOT PROVABLE BEFORE THIS ITEM. Every
# account in distro/rootfs/.../SYSUAF.DAT used to ship with an EMPTY
# password hash, and sysuaf_authenticate() (src/libvms/rtl/sysuaf.c)
# treats an empty hash as "no password required" -- so ANY string typed at
# Password: authenticated successfully, including a deliberately wrong
# one. MEASURED directly against a real QEMU boot of this exact image,
# before SYSTEM's SYSUAF row gained a real hash: sending SYSTEM /
# TOTALLY_WRONG_PASSWORD reached "Welcome to OpenVMX" and a DCL prompt. That
# is precisely the veracity gap this item's DONE CONDITION names ("a bad
# password is refused") and precisely what "a login that prints a banner
# and reaches a DCL prompt... passes today against the facade" (this
# item's own dispatch text) warns against accepting as proof. SYSTEM now
# carries SHA256("MANAGER") in SYSUAF.DAT, so this is a REAL refusal, not
# a string this script types being echoed back.
#
# Anchored to BADPW_OFFSET, captured before 'SYSTEM' is sent a second
# time: 'Username:' and 'Password:' both already appear earlier in the
# log (the executive-attach boot messages do not contain them, but this
# is the FIRST login attempt of the whole run, so there is nothing
# upstream to collide with here -- the anchor is taken anyway, on
# principle, for the same reason every other wait_for in this script that
# can run more than once takes one).
BADPW_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'SYSTEM'
wait_for 'Password:' "$STEP_TIMEOUT" "$BADPW_OFFSET" \
    || fail_with_console "ERROR: no password prompt for the bad-password probe"
send 'TOTALLY_WRONG_PASSWORD'
wait_for 'Username:' "$STEP_TIMEOUT" "$BADPW_OFFSET" \
    || fail_with_console "ERROR: no reprompt after a wrong password -- did it succeed?"
BADPW_SEGMENT=$(tail -c "+$((BADPW_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')

if printf '%s' "$BADPW_SEGMENT" | grep -qF 'User authorization failure'; then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: a wrong password was not refused with 'User authorization failure' (got: $(printf '%s' "$BADPW_SEGMENT" | tr '\n' ' '))"
fi
# The refusal must be a REFUSAL, not incidental text alongside a session
# that was granted anyway -- checked on the SAME segment as the positive
# check above so this cannot pass by printing both.
if printf '%s' "$BADPW_SEGMENT" | grep -qF 'Welcome to OpenVMX'; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: a wrong password reached a session anyway ('Welcome to OpenVMX' present)"
else
    PASS=$((PASS + 1))
fi

# --- Real login --------------------------------------------------------
# A FRESH OFFSET, NOT $BADPW_OFFSET: the bad-password attempt's own
# "Password:" prompt already sits in the log after $BADPW_OFFSET, so
# reusing it here would let wait_for's byte-offset scan match that STALE
# prompt instantly instead of waiting for the real one below -- 'MANAGER'
# would then be sent before the second Username:/Password: exchange had
# even happened. MEASURED, not merely reasoned: this exact reuse produced
# "Username: SYSTEM\nMANAGER\nPassword:" in a real run (MANAGER landing
# between the two prompts, not after either of them) before this offset
# was split from BADPW_OFFSET.
REALLOGIN_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'SYSTEM'
wait_for 'Password:' "$STEP_TIMEOUT" "$REALLOGIN_OFFSET" || fail_with_console "ERROR: no password prompt"
send 'MANAGER'
wait_for 'Welcome to OpenVMX' "$STEP_TIMEOUT" "$REALLOGIN_OFFSET" || fail_with_console "ERROR: login did not succeed"

# --- Drive the session -----------------------------------------------------
# Same command list as the SSH UAT, except SET DEFAULT SYS$MANAGER: (see the
# header note above for why the trailing colon was added).
#
# Assertion anchoring: for each command, record the console log's byte
# offset immediately before sending it, then wait for the DCL prompt ('$ ')
# to reappear IN THE BYTES APPENDED SINCE THAT OFFSET. That segment is
# [echoed input line][command's real output][next prompt]. The first line is
# always the guest tty's echo of what we typed (readline echoes each
# character as it's read, so the whole line lands before any output does);
# stripping it leaves only the command's own response, which is what
# CMD_OUTPUT holds. This is what lets check_response() below assert against
# real DCL output instead of being satisfiable by the fact that we typed the
# string we're checking for.
declare -A CMD_OUTPUT

# PARALLEL, POSITION-INDEXED CAPTURE (vms-2b8 round 4), alongside
# CMD_OUTPUT above. CMD_OUTPUT is keyed by command TEXT, so the SAME text
# run twice -- in this session or the other one -- makes the SECOND run's
# output silently take over the associative-array slot; a check written
# against the FIRST occurrence would then read the SECOND run's answer
# with no signal that it happened. That is exploited ONCE, deliberately
# and with a comment ('SHOW DEFAULT' below, checked only at its second,
# post-SET-DEFAULT occurrence -- nothing ever checks the first), but nothing
# stops a future addition from doing it BY ACCIDENT. CMD_OUTPUT_SEQ /
# CMD_SEQ_LABEL give every run_cmd() call its own permanent slot by call
# order, so a check that must distinguish two occurrences of the same
# command text (see check_response_at() below) can, without renaming the
# command or restructuring CMD_OUTPUT's existing keyed-by-text callers,
# all of which are left exactly as they were.
CMD_OUTPUT_SEQ=()
CMD_SEQ_LABEL=()

run_cmd() {
    local cmd="$1" offset segment out
    offset=$(wc -c <"$CONSOLE_LOG")
    send "$cmd"
    wait_for '$ ' "$COMMAND_TIMEOUT" "$offset" \
        || fail_with_console "ERROR: no DCL prompt after '$cmd'"
    segment=$(tail -c "+$((offset + 1))" "$CONSOLE_LOG" | tr -d '\r')
    out=$(printf '%s\n' "$segment" | tail -n +2)
    CMD_OUTPUT["$cmd"]="$out"
    CMD_OUTPUT_SEQ+=("$out")
    CMD_SEQ_LABEL+=("$cmd")
}

for cmd in "${SYSTEM_CMDS[@]}"; do
    run_cmd "$cmd"
done

LOGIN_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'LOGOUT'
wait_for 'logged out' "$STEP_TIMEOUT" "$LOGIN_OFFSET" || fail_with_console "ERROR: session never logged out"

# --- Session 2: an ORDINARY user, on the same running system --------------
#
# WHY A SECOND LOGIN (vms-2b8 round 7). Session 1 proves what the SYSTEM
# account can do; on its own that is not evidence of an access control
# system, because a system where EVERYONE can write SYS$SYSTEM: passes every
# assertion session 1 makes. The refusal has to be shown too, and it can
# only be shown by an account that is not SYSTEM. GUEST is the least
# privileged account SYSUAF ships (UIC [200,201], TMPMBX only).
#
# Every wait below is anchored to LOGIN_OFFSET -- the byte offset captured
# immediately BEFORE 'LOGOUT' was sent -- because every string being waited
# for ('Username:', 'Password:', 'Welcome to OpenVMX') already appears earlier
# in the log from session 1, and an unanchored wait_for would return
# instantly on session 1's output and then type GUEST's commands into a
# dead session.
#
# BEFORE the LOGOUT and not after it, which is not a detail: PID 1 prints
# the next 'Username:' in the same breath as the logout message, so an
# offset taken after wait_for 'logged out' returns (it polls at 250ms) has
# already skipped past the prompt being waited for, and the wait times out
# with the prompt sitting in plain sight on the console. Measured, not
# reasoned: that is exactly how this failed the first time it ran.
# vms-2213: LOGOUT caused PID 1/JOB_CONTROL to spawn a BRAND NEW LOGINOUT on
# the console, which waits for RETURN before presenting Username: (the
# "press RETURN to log in" behaviour). Wake it -- as a real operator would --
# so the second login prompt appears and the OPERATOR probe below is not
# swallowed as the wake keystroke.
send ''
wait_for 'Username:' "$STEP_TIMEOUT" "$LOGIN_OFFSET" \
    || fail_with_console "ERROR: no second login prompt after LOGOUT"

# --- NEGATIVE LOGIN: OPERATOR refuses a wrong password too (vms-08f) -------
#
# vms-72c's probe above only drove SYSTEM. vms-08f found the SAME bypass
# vms-72c fixed for SYSTEM/GUEST still live for OPERATOR/DEFAULT/USER1/
# USER2 -- SYSUAF rows that shipped (and, after vms-08f, still ship) with
# NO password hash on file. sysuaf_authenticate() no longer treats an
# unset hash as "no password required"; it refuses every password for
# that account, which is the correct MATCH-VMS behaviour for an account
# with no password on record (see the Rule 10 disposition comment on
# sysuaf_authenticate() in src/libvms/rtl/sysuaf.c) -- not a passable
# login, and not the account-specific SYSTEM/GUEST fix vms-72c shipped.
# This probes OPERATOR specifically because it is the one of the four
# still-empty-hash accounts with real privileges (OPER,SYSPRV) -- the
# highest-value account vms-72c's own account-shaped scope note named as
# still open.
#
# WHY IT RUNS HERE AND NOT NEXT TO THE SYSTEM PROBE ABOVE (moved in this
# round). tools/vms_login.c gives every LOGINOUT instance its own budget
# of MAX_ATTEMPTS (3) failed password attempts before it disconnects the
# line without reprompting -- confirmed by reading console_login() there,
# not assumed. Stacking both negative probes (SYSTEM bad, OPERATOR bad) in
# front of the FIRST real login already spends 2 of that instance's 3
# attempts, leaving zero headroom for anything added later in this same
# session; a third negative probe placed there would push attempts to 3,
# and the instance disconnects instead of reprompting 'Username:', which
# is a different, untested code path this script does not drive. PID 1
# execs a BRAND NEW vms_login per console session (see the fork+execl of
# LOGINOUT.EXE in src/ovmx_init/ovmx_init.c, run once per login prompt),
# so the 'Username:' just reached above -- printed by the fresh instance
# LOGOUT caused PID 1 to spawn -- carries its own fresh budget of 3, none
# of it spent yet. Probing OPERATOR here instead spends 1 of THIS
# instance's 3 (GUEST's real login below spends 0, since only failures
# count), leaving 2 free in this instance and 2 free in the first one:
# headroom in both, not zero in one.
OPBADPW_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'OPERATOR'
wait_for 'Password:' "$STEP_TIMEOUT" "$OPBADPW_OFFSET" \
    || fail_with_console "ERROR: no password prompt for the OPERATOR bad-password probe"
send 'TOTALLY_WRONG_PASSWORD'
wait_for 'Username:' "$STEP_TIMEOUT" "$OPBADPW_OFFSET" \
    || fail_with_console "ERROR: no reprompt after OPERATOR + a wrong password -- did it succeed?"
OPBADPW_SEGMENT=$(tail -c "+$((OPBADPW_OFFSET + 1))" "$CONSOLE_LOG" | tr -d '\r')

if printf '%s' "$OPBADPW_SEGMENT" | grep -qF 'User authorization failure'; then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: OPERATOR + a wrong password was not refused with 'User authorization failure' (got: $(printf '%s' "$OPBADPW_SEGMENT" | tr '\n' ' '))"
fi
# Same shape as the SYSTEM check above: the refusal must be a REFUSAL, not
# incidental text alongside a session granted anyway -- checked on the
# SAME segment so this cannot pass by printing both.
if printf '%s' "$OPBADPW_SEGMENT" | grep -qF 'Welcome to OpenVMX'; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: OPERATOR + a wrong password reached a session anyway ('Welcome to OpenVMX' present)"
else
    PASS=$((PASS + 1))
fi

# A FRESH OFFSET, NOT $LOGIN_OFFSET: the OPERATOR probe just above already
# put its own 'Password:' text after $LOGIN_OFFSET, so reusing that anchor
# here would let wait_for's byte-offset scan match THAT stale prompt
# instantly instead of waiting for the real one GUEST's send triggers --
# the identical hazard, and identical fix, already called out above for
# $BADPW_OFFSET vs. $REALLOGIN_OFFSET on the SYSTEM login.
GUEST_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'GUEST'
wait_for 'Password:' "$STEP_TIMEOUT" "$GUEST_OFFSET" \
    || fail_with_console "ERROR: no password prompt for GUEST"
send 'GUEST'
wait_for 'Welcome to OpenVMX' "$STEP_TIMEOUT" "$GUEST_OFFSET" \
    || fail_with_console "ERROR: GUEST login did not succeed"

for cmd in "${USER_CMDS[@]}"; do
    run_cmd "$cmd"
done

LOGOUT2_OFFSET=$(wc -c <"$CONSOLE_LOG")
send 'LOGOUT'
wait_for 'logged out' "$STEP_TIMEOUT" "$LOGOUT2_OFFSET" \
    || fail_with_console "ERROR: GUEST session never logged out"

# Whole-session output, for the human-readable transcript below and for the
# negative Unix-leak checks (which are safe to scan broadly -- none of the
# strings they look for are ones this script ever sends).
OUTPUT=$(cat "$CONSOLE_LOG")

# Post-login output only: excludes the boot log (kernel/init messages this
# harness never validated before, so scanning it for the negative checks
# would be new, unvalidated surface) and excludes the login-prompt echo of
# the username/password we sent.
WELCOME_OFFSET=0
if grep -qF 'Welcome to OpenVMX' "$CONSOLE_LOG"; then
    WELCOME_OFFSET=$(grep -aboF 'Welcome to OpenVMX' "$CONSOLE_LOG" | head -1 | cut -d: -f1)
fi
SESSION_OUTPUT=$(tail -c "+$((WELCOME_OFFSET + 1))" "$CONSOLE_LOG")

echo "=== Session Output ==="
echo "$OUTPUT"
echo "=== End Session Output ==="

# --- Validation --------------------------------------------------------
# PASS/FAIL/ERRORS are declared ABOVE, before the boot/login section, not
# here -- see the comment at their declaration for why (vms-72c: the
# bad-password probe needs them before this point in the script runs).

# Whole-log substring check. Used ONLY for the negative Unix-leak checks
# below, whose patterns cannot be satisfied by something this script itself
# typed (see check_response() below for the positive assertions, which can
# be echo-satisfied and so are anchored to one command's own response
# instead of scanning the whole log). Round-3 mutation testing found and
# removed the two whole-log positive-assertion helpers this file used to
# have (check_contains/check_regex): every positive check that used them
# was provably satisfiable by a command's own echo or by unrelated output
# elsewhere in the log (e.g. LOGOUT's timestamp satisfying a "SHOW TIME
# printed a date" check even when SHOW TIME itself was rejected by DCL).
check_not_contains() {
    if echo "$OUTPUT" | grep -qi "$1"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should NOT contain '$1'"
    else
        PASS=$((PASS + 1))
    fi
}

# Anchored check: asserts against ONE command's own captured response
# (CMD_OUTPUT[cmd]), not the whole log -- so it cannot be satisfied by the
# echo of any command's input, including its own.
check_response() {
    local cmd="$1" pattern="$2"
    if printf '%s' "${CMD_OUTPUT[$cmd]}" | grep -qiE "$pattern"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: response to '$cmd' should contain '$pattern' (got: $(printf '%s' "${CMD_OUTPUT[$cmd]}" | tr '\n' ' '))"
    fi
}

# Assert a KNOWN DIVERGENCE from VMS, deliberately, so that it is visible in
# the run rather than merely absent from it. It passes while the divergence
# holds and goes RED the moment OVMX starts matching VMS -- at which point the
# fix is to delete the tripwire and assert the VMS behaviour, not to relax it.
#
# WHY A TRIPWIRE AND NOT SILENCE (vms-6a7 round 2): the alternative was to keep
# asserting the fields that happen to be right and say nothing about the field
# that is wrong. That is "asserting around" the defect: the suite reports 14/14
# green while the runtime prints something VMS never prints, and the only
# record of it is a comment. Rule 10 allows two answers -- match VMS, or do not
# expose the thing -- and a rendering of a state VMS never reaches is neither.
# The condition is not vms-6a7's to fix, so it is DISCLOSED here instead, with
# the owning item named in the failure message.
check_known_divergence() {
    local cmd="$1" pattern="$2" item="$3" what="$4"
    if printf '%s' "${CMD_OUTPUT[$cmd]}" | grep -qiE "$pattern"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: known divergence ($item) no longer reproduces in '$cmd': $what.\n        If $item has landed this is GOOD NEWS -- delete this tripwire and assert the VMS behaviour instead. Do NOT relax the pattern.\n        (got: $(printf '%s' "${CMD_OUTPUT[$cmd]}" | tr '\n' ' '))"
    fi
}

# Anchored negative: asserts a pattern is ABSENT from ONE command's own
# captured response. Same anchoring rationale as check_response -- a
# whole-log negative would be defeated by any other command that happens
# to print the string.
check_not_response() {
    local cmd="$1" pattern="$2"
    if printf '%s' "${CMD_OUTPUT[$cmd]}" | grep -qiE "$pattern"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: response to '$cmd' should NOT contain '$pattern' (got: $(printf '%s' "${CMD_OUTPUT[$cmd]}" | tr '\n' ' '))"
    else
        PASS=$((PASS + 1))
    fi
}

# ANCHORED CHECK BY CALL POSITION, NOT BY COMMAND TEXT (vms-2b8 round 4).
# For asserting on the SECOND (or Nth) time the same command text runs in
# this script -- where check_response()'s CMD_OUTPUT[$cmd] lookup would
# silently return whichever occurrence wrote last, not necessarily the one
# the caller means. Takes the index run_cmd() assigned (CMD_OUTPUT_SEQ),
# computed at the call site from ${#SYSTEM_CMDS[@]} rather than hand-counted,
# so it tracks the command list even if entries are added or removed above
# it. The label in failure output comes from CMD_SEQ_LABEL, so a failure
# still names the actual command, not just a bare index.
check_response_at() {
    local idx="$1" pattern="$2" label out
    label="${CMD_SEQ_LABEL[$idx]}"
    out="${CMD_OUTPUT_SEQ[$idx]}"
    if printf '%s' "$out" | grep -qiE "$pattern"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: response to command #$idx ('$label') should contain '$pattern' (got: $(printf '%s' "$out" | tr '\n' ' '))"
    fi
}

# SHOW TIME should print a VMS date (DD-MMM-YYYY) in its own response.
# Anchored, not a whole-log scan: LOGOUT's own message ("logged out at
# 1-JAN-1970 00:00:09.95") independently matches this exact date-format
# regex, so a whole-log check_regex here would still pass even if SHOW TIME
# were rejected outright and printed no date at all. Verified by mutation:
# prefixing SHOW TIME with a bogus verb (%DCL-E-IVVERB, no date printed by
# that command) makes this assertion fail only once it is anchored to SHOW
# TIME's own response; the unmutated run still passes.
check_response 'SHOW TIME' '[0-9]{1,2}-(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)-[0-9]{4}'

# SHOW DEFAULT should show SYS$MANAGER after SET DEFAULT. Anchored to the
# SECOND 'SHOW DEFAULT' response (associative array key => last write wins,
# i.e. the one issued after SET DEFAULT), not the whole log -- the whole log
# also contains the echo of `SET DEFAULT SYS$MANAGER` itself, which contains
# the literal substring 'SYS$MANAGER' regardless of whether SET DEFAULT
# actually took effect.
check_response 'SHOW DEFAULT' 'SYS\$MANAGER'

# SHOW LOGICAL UAT_TEST should show the value. Anchored to that command's own
# response, not the whole log -- the whole log also contains the echo of
# `DEFINE UAT_TEST "session_test_passed"`.
check_response 'SHOW LOGICAL UAT_TEST' 'session_test_passed'

# SHOW PROCESS should show process info. Anchored to SHOW PROCESS's own
# response -- the whole log also contains the echo of `send 'SYSTEM'` at the
# login-username prompt, which would satisfy a plain substring scan on its
# own regardless of what SHOW PROCESS printed.
#
# THE ASSERTION CHANGED FROM 'SYSTEM' TO THE PROCESS ID, AND IT IS STRONGER,
# NOT WEAKER (vms-6a7). READ THIS BEFORE CHANGING IT BACK.
#
# 'SYSTEM' matched because SHOW PROCESS printed the STRING LITERAL "SYSTEM":
#     const char *upper_user = ctx->username[0] ? ctx->username : "SYSTEM";
# a hardcoded fallback in src/vmsdcl/dcl_cmd_show.c. The command now reads the
# target's row out of the executive through $GETJPI and prints what that row
# holds, and on this runtime the row holds NO user name at all -- so the old
# assertion was satisfied by a constant in the source and nothing else. It
# would have passed on a machine with no executive, no login and no process.
#
# WHAT THIS RUN ACTUALLY REVEALED, verbatim, once the literal was gone:
#     1-JAN-1970 00:00:06.94   User:                  Process ID:   10000001
#                              Node: OVMX             Process name: ""
#     User Identifier:    [000,000]
#     Default file spec:  SYS$MANAGER:
# The console DCL session has no authenticated identity stamped on its
# executive row (no user name), and no process name -- because nothing on the
# console boot path calls $SETPRN or the SYSUAF authentication that stamps one
# (vms-2b8's vms_kif_setident). Those are REAL GAPS the fallback was hiding;
# they are reported by vms-6a7 rather than papered over again, and they are not
# vms-6a7's to fix (identity: vms-afd/vms-2b8/vms-d0b; the missing default
# process name: vms-d0e). They are now ASSERTED as known divergences further
# down this file (check_known_divergence), not merely described here -- a
# defect that lives only in a comment is a defect the suite reports as green.
#
# The Process ID: field is the honest assertion available today: it is an
# EXECUTIVE-ASSIGNED value (src/kernel/vms_module.c assign_vms_pid), not
# getpid() and not a literal, so it cannot be produced without a working
# /dev/vms round trip -- which is exactly what this UAT exists to certify.
#
# THE END ANCHOR IS LOAD-BEARING, measured not assumed. Without it the pattern
# was 'Process ID:   [0-9A-F]{8}', and a mutation printing the pid as decimal
# (%u instead of %08X) did NOT redden it -- 268435457 contains eight
# consecutive hex-legal characters immediately after the label, so the
# assertion could not see its own defect. It was rebuilt and rerun on this
# runtime with the anchor in place: the decimal form is rejected and the
# unmutated %08X still passes.
check_response 'SHOW PROCESS' 'Process ID:   [0-9A-F]{8}[[:space:]]*$'
check_response 'SHOW PROCESS' 'Node: +OVMX'

# THE USER FIELD IS NOW POPULATED FOR THE INTERACTIVE SESSION (vms-2b8
# round 3), MEASURED, NOT ASSUMED FROM THE COMMENT THIS REPLACES.
#
# This used to be a check_known_divergence tripwire pinned to vms-afd. It
# fired RED on a real QEMU run of this exact script (2026-07-31): the
# session's SHOW PROCESS now prints `User: SYSTEM`, not an empty field. But
# `rd show vms-afd` at the same moment shows it is STILL OPEN (status
# inbox) -- so the tripwire's own causal story ("$CREPRC does not
# propagate identity ... nothing on the console boot path calls $SETPRN")
# was not what changed. What changed is narrower: tools/vms_login.c
# (LOGINOUT) now calls vms_kif_setident() directly for ITS OWN session
# (this item's central mechanism), which stamps the executive row before
# DCL ever reads it -- independent of $CREPRC inheritance entirely.
# vms-afd is about a DIFFERENT code path (sys$creprc-created subprocesses,
# e.g. SPAWN), and that path is UNCHANGED: see the SPAWN assertions below,
# which still correctly pin the empty-user state for a spawned subprocess
# on this same run. So this is the VMS-matching assertion the old
# tripwire's comment said to install once its pattern went red, but it is
# NOT evidence vms-afd has landed -- do not close vms-afd from this.
check_response 'SHOW PROCESS' 'User: +SYSTEM +Process ID:'

# THE PROCESS NAME IS NOW POPULATED FOR THE LOGIN SESSION (vms-72c),
# MEASURED, NOT ASSUMED FROM THE COMMENT THIS REPLACES.
#
# This used to be a check_known_divergence tripwire pinned to vms-d0e
# ("OVMX assigns no default process name at creation"). It fired RED on a
# real QEMU run of this exact script the moment vms-72c wired
# tools/vms_login.c to call vms_kif_setprn(rec->username) after
# authentication -- exactly the outcome the tripwire's own comment named as
# the condition to delete it under ("If vms-d0e has landed this is GOOD
# NEWS -- delete this tripwire and assert the VMS behaviour instead").
#
# NOT EVIDENCE vms-d0e HAS LANDED, and the distinction matters: vms-d0e asks
# what the executive should name a process created with NO explicit prcnam
# at all (e.g. a bare $CREPRC/RUN with no /PROCESS_NAME -- SPAWN's own
# subprocess below is exactly that case, and it is STILL unnamed, on
# purpose, unchanged by this item). vms-72c answers a narrower, already-
# oracle-pinned question: LOGINOUT itself supplies an EXPLICIT name (the
# SYSUAF username -- VAX1, OpenVMS VAX V7.3, docs/oracle/
# vax73-show-system-process.md Section 1: the interactive SYSTEM session's
# own SHOW SYSTEM row is named literally SYSTEM). Do not close vms-d0e from
# this passing.
check_response 'SHOW PROCESS' 'Process name: "SYSTEM"'

# Privilege names should appear in SHOW PROCESS /PRIVILEGES's own response.
# Anchored, not a whole-log scan: the whole log also contains the echo of
# the command itself, and 'PRIV' is a literal substring of 'SHOW PROCESS
# /PRIVILEGES' -- a check_regex('...|PRIV') on SESSION_OUTPUT would pass on
# the echo alone even if DCL rejected the command outright. Verified by
# mutation: prefixing the command with a bogus verb so DCL returns
# %DCL-E-IVVERB and no privilege list is ever printed makes this assertion
# fail; the unmutated run still passes.
#
# PATTERN CORRECTED (vms-2b8 round 3), measured on a real QEMU run: the old
# pattern here, '(TMPMBX|NETMBX|OPER)', predates the operator ruling that
# masks this display to VMS_PRV_M_ENFORCED (src/kernel/vms_ioctl.h) --
# privileges OVMX stores but does not enforce are no longer printed at all
# (see src/vmsdcl/dcl_cmd_show.c's cmd_show_process_privileges). SYSTEM's
# DISPLAYED authorized mask on this runtime -- after masking -- is exactly
# CMKRNL, CMEXEC, SYSNAM, GRPNAM, SETPRV, WORLD (SYSNAM/GRPNAM joined the
# enforced set in vms-5b7, LNM$SYSTEM/LNM$GROUP privilege enforcement).
#
# THAT IS NOT THE SAME CLAIM AS "SYSUAF AUTHORIZES SYSTEM EXACTLY THAT SET",
# and round 3's comment here said the latter -- false, corrected round 4.
# MEASURED: distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT's SYSTEM row
# reads `SYSTEM||1|4|SYS$SYSDEVICE:[SYSMGR]||ALL` -- the seventh field,
# privileges, is the literal string ALL, not a short list. The six names
# below are what SURVIVES THE VMS_PRV_M_ENFORCED INTERSECTION of that ALL
# mask, which is a strict subset -- SYSPRV, BYPASS, OPER, and the rest of
# the 37 named rows in vms_priv_names[] (minus the 6 shown) are authorized
# by SYSUAF and correctly do NOT appear here, because nothing in vms.ko
# enforces them for THIS surface (see that constant's own comment; SYSNAM/
# GRPNAM are the exception vms-5b7 adds, enforced narrowly by vms_lnm.c
# rather than by this DCL display). The old pattern would now silently
# pass on an EMPTY privilege block too (TMPMBX/NETMBX/OPER can't appear,
# but neither can anything else, and grep -qE against an empty capture
# only fails, which happens to be visible -- verified this round: the old
# pattern really did go red against the corrected output, it was not a
# silent pass). It is corrected here rather than widened to accept both,
# because accepting both would hide a future regression back to the
# unmasked display.
check_response_at "$IDX_PRIV_ORIGINAL" '(CMKRNL|CMEXEC|SETPRV|WORLD)'

# F$PRIVILEGE MUST AGREE WITH SHOW PROCESS/PRIVILEGES ABOUT THE SAME
# PROCESS AT THE SAME MOMENT (vms-2b8 round 3).
#
# WHY THIS EXISTS. Measured before this round's fix, on the real runtime:
# SYSTEM's SYSUAF record authorizes OPER, so SHOW PROCESS /PRIVILEGES
# above -- masked to VMS_PRV_M_ENFORCED -- correctly showed no OPER
# anywhere, while `F$PRIVILEGE("OPER")` on the SAME session answered
# "TRUE", because src/vmsdcl/dcl_lexical.c's lex_privilege() read the
# executive's RAW cur_privs, unmasked. That is OVMX advertising a
# privilege it cannot enforce -- Rule 10's illegal third answer -- through
# a surface DCL scripts branch on (`IF F$PRIVILEGE(...) THEN`), which is
# worse than a display line: code takes the wrong path, not just a human
# reading the wrong text. lex_privilege() now masks to
# VMS_PRV_M_ENFORCED, same as the display above, so OPER (authorized but
# unenforced) must read FALSE and SETPRV (authorized AND enforced) must
# read TRUE for the identical session.
check_response 'SHOW SYMBOL IDENT_OPER' 'IDENT_OPER = "FALSE"'
check_response 'SHOW SYMBOL IDENT_SETPRV' 'IDENT_SETPRV = "TRUE"'

# SET PROCESS/PRIVILEGES MUST NOT BE ABLE TO DESYNCHRONIZE F$PRIVILEGE FROM
# SHOW PROCESS/PRIVILEGES (vms-2b8 round 4 -- the part of defect 1 round 3's
# fix missed).
#
# WHY THIS EXISTS. Round 3 masked F$PRIVILEGE to VMS_PRV_M_ENFORCED, which
# fixed the OVER-claim direction (F$PRIVILEGE saying TRUE for an unenforced
# privilege SHOW PROCESS/PRIVILEGES correctly omits) but left an UNDER-claim
# direction open: cmd_set_process() (src/vmsdcl/dcl_cmd_set.c) used to
# REPLACE ctx->privileges outright with whatever SET PROCESS/PRIVILEGES was
# asked for, and F$PRIVILEGE used to read that same ctx->privileges.
# MEASURED, before this round's fix, on this exact runtime:
#   $ SHOW PROCESS/PRIVILEGES        -> Authorized: CMEXEC CMKRNL SETPRV WORLD
#   $ SET PROCESS/PRIVILEGES=(OPER)
#   $ SHOW PROCESS/PRIVILEGES        -> UNCHANGED (reads the executive directly)
#   $ F$PRIVILEGE("SETPRV")          -> "FALSE"  <-- was "TRUE" one command earlier
# Same process, same moment, two surfaces disagreeing about a privilege
# (SETPRV) the executive never stopped enforcing. The fix: F$PRIVILEGE
# (dcl_lexical.c's lex_privilege()) now asks the executive fresh on every
# call, the same source SHOW PROCESS/PRIVILEGES reads, instead of the local,
# SET-PROCESS-mutable ctx->privileges -- so the two cannot disagree by
# construction, regardless of what SET PROCESS/PRIVILEGES does locally. That
# command now REACHES the executive (vms-e5d7): it routes the mutation through
# sys$setprv -> vms_kif_setprv and leaves ctx->privileges untouched (sys$setprv
# mirrors the executive's resulting mask into pcb->cur_privs for the in-process
# readers; F$PRIVILEGE and SHOW both read the executive fresh). A successful
# enable is silent, matching VMS (docs/oracle/vax73-privileges.md §3/§8).
#
# Checked BY POSITION (check_response_at), not by command text: 'SHOW
# PROCESS /PRIVILEGES' and the F$PRIVILEGE pattern both already ran once
# above in this same session, so CMD_OUTPUT[$cmd] would return whichever
# occurrence ran LAST, not necessarily the one meant -- see
# check_response_at's own comment for why that is unsafe to rely on here.
check_response_at "$IDX_PRIV_AFTER_SET" '(CMKRNL|CMEXEC|SETPRV|WORLD)'
check_response_at "$IDX_SETPRV2_AFTER_SET" 'IDENT_SETPRV2 = "TRUE"'
check_response_at "$IDX_WORLD2_AFTER_SET" 'IDENT_WORLD2 = "TRUE"'

# DEFECT-1 PROOF, ROUND 5 (see the SYSTEM_CMDS block above for the full
# measured-before/fixed-after account): the REPORT and the GATE must
# agree. IDENT_ALTPRI's text is unique in this session so a plain
# check_response would be safe for it alone, but SET PROCESS/PRIORITY=6
# prints nothing on success (silently would look identical to "ran and
# printed nothing yet") and only %SET-E-NOPRIV on refusal, so the
# by-position anchor matters for THAT check specifically; both are
# checked the same way for consistency with the rest of this block.
check_response_at "$IDX_ALTPRI" 'IDENT_ALTPRI = "FALSE"'
check_response_at "$IDX_PRIORITY_SET" 'NOPRIV'

# F$GETJPI CURPRIV/AUTHPRIV FORMAT (vms-2b8 round 5, see the SYSTEM_CMDS
# block above). Ascending bit-position order, matching the oracle's own
# CURPRIV example (CMKRNL before CMEXEC) -- NOT alphabetical, which is
# SHOW PROCESS/PRIVILEGES's own, different, VMS display convention.
# SYSNAM/GRPNAM (bit positions 2/3, between CMEXEC and SETPRV) joined
# VMS_PRV_M_ENFORCED in vms-5b7 (LNM$SYSTEM/LNM$GROUP privilege
# enforcement); MOUNT (bit position 17, right after WORLD) joined it in
# vms-651 (cmd_mount/cmd_dismount really gate on it) -- this literal is
# DERIVED from that mask's current definition, not a number owned by this
# test; update it again whenever VMS_PRV_M_ENFORCED (src/kernel/
# vms_ioctl.h) changes.
check_response 'SHOW SYMBOL IDENT_CURPRIV' 'IDENT_CURPRIV = "CMKRNL,CMEXEC,SYSNAM,GRPNAM,SETPRV,WORLD,MOUNT"'
check_response 'SHOW SYMBOL IDENT_AUTHPRIV' 'IDENT_AUTHPRIV = "CMKRNL,CMEXEC,SYSNAM,GRPNAM,SETPRV,WORLD,MOUNT"'

# SET PROCESS/PRIVILEGES IS NOW WIRED TO THE EXECUTIVE (vms-e5d7). It used to
# be a HIDE stub that printed %OVMX-I-NOSETPRV and changed nothing; it now
# routes the mutation through sys$setprv -> the executive
# (docs/oracle/vax73-privileges.md §8), and there is no DCL pre-gate. For THIS
# session -- SYSTEM, which holds SETPRV and is authorized for privilege ALL --
# enabling OPER is granted, and (matching VMS, oracle §3) a SUCCESSFUL SET
# PROCESS/PRIVILEGE prints NOTHING. So the old %OVMX-*-NOSETPRV facade must be
# GONE in either severity. 'SET PROCESS/PRIVILEGES=(OPER)' is unique text in
# SYSTEM_CMDS, so a plain check_not_response is safe. The genuine grant/deny
# behaviour (authorized ALL enables; an unauthorized request yields
# %SYSTEM-W-NOTALLPRIV and stays unheld; SHOW reflects the executive mask) is
# proven end-to-end against a real /dev/vms in
# tests/qemu/test_syssvc_setprv_dcl.c.
check_not_response 'SET PROCESS/PRIVILEGES=(OPER)' 'NOSETPRV'

# POSITIVE CONTROL -- the GRANT path. SET PROCESS/PRIVILEGES=(OPER) for this
# SYSTEM session must SUCCEED: OPER is within SYSTEM's authorization and it
# holds SETPRV, so the executive grants it and DCL prints no error. A refusal
# would print %SET-E-NOPRIV or %SYSTEM-*, so its ABSENCE here is the grant.
# (OPER is outside VMS_PRV_M_ENFORCED, so SHOW PROCESS/PRIVILEGES does not
# display it -- which is why the desync assertions above read SHOW's ENFORCED
# privileges, and the executive-visible grant/deny cycle is proven directly in
# tests/qemu/test_syssvc_setprv_dcl.c.)
#
# NOTE (vms-e5d7): SET PROCESS/PRIVILEGES no longer consults
# enforced_privs_held() -- the executive authorizes it now, and the old DCL
# pre-gate was removed (it wrongly refused enabling an already-authorized
# privilege, docs/oracle/vax73-privileges.md §3/§8). The remaining
# enforced_privs_held() gates are SET PROCESS/PRIORITY and SET TIME, both
# deny-only on this build (ALTPRI/OPER/SYSPRV/BYPASS are all outside
# VMS_PRV_M_ENFORCED). VERIFIED NON-VACUOUS BY MUTATION
# (src/vmsdcl/dcl_cmd_set.c's enforced_privs_held(), rebuilt + re-booted under
# QEMU): forcing the body to `return ~(uint64_t)0;` (always-grant) reddens
# IDX_PRIORITY_SET ('NOPRIV' -- SET PROCESS/PRIORITY=6 becomes silently
# authorized) AND the SET TIME deny assertion below (its gate passes on the
# forced OPER bit, so cmd_set_time falls through to the real settimeofday(2),
# which fails with a genuine OS EPERM printing a DIFFERENT message than the
# gate-refusal text). So a gate that always grants is caught by the deny pair.
check_not_response 'SET PROCESS/PRIVILEGES=(OPER)' 'NOPRIV'

# DEFECT-1 REGRESSION PROOF, SET TIME (see the SYSTEM_CMDS block above for
# the full account): OPER/SYSPRV/BYPASS are all outside VMS_PRV_M_ENFORCED,
# so this SYSTEM session -- SYSUAF-authorized for privilege ALL -- must
# still be refused. 'SET TIME 1-JAN-2030:00:00:00' is unique text, so a
# plain check_response is safe.
#
# NOT matched on the bare substring 'NOPRIV': cmd_set_time has a SECOND,
# unrelated failure path below the privilege gate (settimeofday(2)
# returning EPERM, e.g. if this ever ran as a non-root OS user with the
# gate somehow passed) that ALSO renders as "%SET-E-NOPRIV, ..." -- same
# facility/severity/ident, different text. A bare 'NOPRIV' match would be
# satisfied by either branch and could not tell "the executive-mask gate
# refused" from "the gate passed and the OS syscall refused instead",
# which is exactly the kind of assertion Method Requirement 3 rules out.
# FOUND BY MUTATION, not by inspection: the always-grant mutation
# described above (enforced_privs_held() forced to `return ~(uint64_t)0`)
# initially showed this check passing for the wrong reason -- against a
# bare 'NOPRIV' pattern it was satisfied by the OS EPERM message instead
# of going red -- before the pattern was narrowed to the gate's own
# disclosure text, which only the privilege-check branch prints.
check_response 'SET TIME 1-JAN-2030:00:00:00' 'no privilege for SET TIME'

# THE SESSION REALLY IS THE AUTHENTICATED USER AT THE OS LEVEL (vms-2b8
# round 6). This is the only externally observable proof that
# tools/vms_login.c's credential drop happened, and it is why the
# assertion is on a SPAWNed subprocess rather than on the session itself.
#
# The session's own UIC comes from the row LOGINOUT stamped out of
# SYSUAF, so SHOW PROCESS reports [001,004] whether or not the drop
# occurred -- it cannot distinguish. A SUBPROCESS is different: it
# registers with the executive on its own, and vms_proc_register() in
# src/kernel/vms_module.c derives its UIC from the task's REAL Linux
# credentials, inheriting nothing. So the subprocess's UIC is a direct
# readout of what the session is running as:
#   drop performed  -> [001,004]   (setgid(1), setuid(4) from SYSUAF)
#   drop absent     -> [000,000]   (root, as every session used to be)
#
# That difference is not cosmetic. While sessions ran as root, every
# subprocess also registered holding CMKRNL|CMEXEC|SYSNAM|GRPNAM|SETPRV|WORLD, and
# SETPRV is what VMS_IOCTL_SETIDENT requires to claim any identity at
# all -- an ordinary user's subprocess could stamp itself SYSTEM with
# SYSUAF's privilege ALL (reproduced against a real /dev/vms; the refusal is
# asserted in tests/qemu/test_syssvc_ident.c scenario D).
#
# Anchored to SPAWN's own response segment, not the whole log: '[001,004]'
# also appears in the SHOW PROCESS response earlier in the same session.
check_response 'SPAWN SHOW PROCESS' '\[001,004\]'
check_not_response 'SPAWN SHOW PROCESS' '\[000,000\]'

# KNOWN DIVERGENCE FROM VMS, ASSERTED OUT LOUD RATHER THAN STEPPED AROUND
# (vms-afd).
#
# The display the two assertions above read is, on this line, WRONG: OVMX
# prints 'User:' followed by nothing for a spawned subprocess. VMS has no
# process without a user name -- measured on the oracle in this item's own
# session (VAX1, OpenVMS VAX V7.3): SPAWN there answers '%DCL-S-SPAWNED,
# process SYSTEM_1 spawned', a subprocess inheriting the creator's
# username. OVMX's src/kernel/vms_module.c zeroes proc->username at
# registration and inherits nothing, so every SPAWNed process in the
# product reports blank. The credential drop this item lands does not
# cause it, but it makes it reachable for every spawned process rather
# than theoretical.
#
# The fix is $CREPRC identity propagation, filed as vms-afd and entangled
# with vms-8019's in-flight work on the executive process table, so it is
# not built here. What is NOT acceptable is asserting around it in silence
# -- that is how a facade survives. This assertion PINS the blank, so the
# day vms-afd makes SPAWN inherit a user name this line goes red and
# whoever lands it has to come and delete it.
check_response 'SPAWN SHOW PROCESS' 'User: +Process ID:'

# SPAWN WORKS MORE THAN ONCE PER SESSION (rd vms-00e).
#
# This is the SECOND SPAWN in this session. Before vms-00e it answered
# '%DCL-E-CREPRC, cannot create subprocess' every time -- deterministically,
# for any command, on both the static and the VMS-native DCL.EXE (the
# VMS-native one failed on the FIRST spawn too). The cause was vmsfs.ko's
# ->d_revalidate unhashing the running image's own dentry, which made
# readlink("/proc/self/exe") -- how cmd_spawn() finds the image to re-exec --
# return a path with " (deleted)" glued on. See the block above.
#
# TWO assertions, because either alone is weak: the negative one alone would
# be satisfied by SPAWN printing nothing at all, and the positive one alone
# would not distinguish "subprocess ran SHOW TIME" from "SPAWN failed and the
# error text happened to match". Together they say the subprocess was created
# AND it produced the VMS date/time SHOW TIME is supposed to produce.
check_not_response 'SPAWN SHOW TIME' 'CREPRC'
check_response 'SPAWN SHOW TIME' '[0-9]{1,2}-(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)-[0-9]{4}'

# A-WRITES / B-READS FOR THE LOGIN SESSION'S PROCESS NAME -- WHERE THE
# PROOF ACTUALLY LIVES, AND WHY NOT HERE (vms-72c).
#
# The natural second assertion here would be a SECOND spawned subprocess
# doing 'SHOW PROCESS SYSTEM' (a bare name parameter -- src/vmsdcl/
# dcl_cmd_show.c's cmd_show_process() resolves it via sys$getjpi with a
# name descriptor, cross-process) to prove that A DIFFERENT, independently
# registered process can look up the login session BY THE NAME THIS ITEM
# gives it. That assertion was written, built, and run against a real QEMU
# boot -- and found a PRE-EXISTING, UNRELATED DCL DEFECT instead: the
# SECOND 'SPAWN' in one session, ANY command, fails outright with
# '%DCL-E-CREPRC, cannot create subprocess' (cmd_spawn(), src/vmsdcl/
# dcl_cmd_process.c -- its execl() of vmsdcl's own /proc/self/exe returns
# instead of replacing the image). REPRODUCED THREE WAYS: (1) 'SPAWN SHOW
# PROCESS SYSTEM' immediately after the existing 'SPAWN SHOW PROCESS' above,
# twice, deterministically; (2) a minimal standalone probe running 'SPAWN
# SHOW TIME' three times in a row -- attempt 1 succeeds, attempts 2 and 3
# both fail identically, proving the argument content is irrelevant and the
# SECOND spawn in a session is what fails. Not this item's to fix (SPAWN's
# re-exec mechanism, not console login), and forcing an assertion through a
# bug in an unrelated command would itself violate Method Requirement 3 (an
# assertion whose failure is explained by something OTHER than the property
# under test is vacuous either way it goes). Reported as a finding instead.
#
# THAT DEFECT IS FIXED (rd vms-00e) -- and it was never in DCL. It was
# vmsfs.ko's ->d_revalidate answering "invalid" for every positive
# regular-file dentry, which makes the VFS d_invalidate() (== UNHASH) the
# dentry of the running DCL.EXE; d_path() renders an unhashed dentry with a
# " (deleted)" suffix, so readlink("/proc/self/exe") handed cmd_spawn() a
# path that could not be exec'd. The FIRST walk of DCL.EXE's path after exec
# is what unhashed it -- which is why the static image survived one SPAWN
# (its own execl() was that walk) and the VMS-native, IMGACT-activated image
# survived none (IMGACT re-opens the image by AT_EXECFN during activation,
# before main()). The 'SPAWN SHOW TIME' entry added to SYSTEM_CMDS above is
# a SECOND spawn in the same session, and the two assertions below pin it.
#
# THE PROOF THIS ITEM RELIES ON INSTEAD:
#   - cross-process $GETJPI-BY-NAME, generically, at the executive layer,
#     with NO DCL/SPAWN involved: tests/qemu/test_syssvc_procnam.c forks a
#     real second process and asserts "sys$getjpi resolved another process
#     BY NAME" against a real /dev/vms (pre-existing, vms-8019's proof, not
#     re-derived here).
#   - THIS ITEM'S OWN CONTRIBUTION -- that the login session specifically
#     HAS a resolvable name at all -- is what 'SHOW PROCESS' above already
#     proves ('Process name: "SYSTEM"', oracle-pinned) and what SHOW USERS
#     below proves a SECOND TIME over, from the executive's terminal-binding
#     table rather than the process table: two independent executive-backed
#     readers naming the SAME session the SAME way is itself evidence
#     neither one is a per-reader fabrication.

# THE AUTHENTICATED USER CAN ACTUALLY USE THE SYSTEM (vms-2b8 round 7).
#
# WHY THIS BLOCK EXISTS. Round 6 made LOGINOUT drop to the SYSUAF UIC, and
# nothing in the suite wrote a file, so nobody noticed that a user could log
# in and then not create one -- in their own login directory. `COPY LOGIN.COM
# ADVPROBE.TXT` answered `%RMS-E-CRE, cannot create - ADVPROBE.TXT` on the
# real bootable image. The cause was not the protection check: it was that
# PID 1 installed the whole VMS tree as Linux root, so once a session
# genuinely became UIC [1,4] there was nothing on the system that the SYSTEM
# account owned. SYS$SYSTEM:PROVISION.EXE provision_ownership() now gives
# the tree to SYSTEM, which is what the oracle says VMS does
# (DIRECTORY/OWNER SYS$COMMON:[000000]SYSEXE.DIR -> [SYSTEM]).
#
# EXISTENCE IS PROVEN BY READING THE FILE BACK (TYPE), not by DIRECTORY.
# Measured on this runtime while writing these assertions: `DIRECTORY <one
# named file>` answers 'Total of 0 files' even for a file that certainly
# exists (`DIRECTORY SYS$MANAGER:LOGIN.COM` -> 0 files, while `TYPE
# SYS$MANAGER:LOGIN.COM` prints it). That is a real DCL defect, reported
# separately; it is not this item's, and a test that leaned on it would be
# asserting against a broken observer.
#
# Nor are these assertions on an error message. OVMX's COPY prints
# %RMS-E-CRE where VMS prints %COPY-E-OPENOUT with -RMS- and -SYSTEM-
# secondaries; asserting the OVMX text would be certifying a message this
# item never measured (CLAUDE.md Rule 10). Whether the bytes are THERE is
# platform-independent and is the thing that actually matters.
check_not_response 'COPY LOGIN.COM UATWRITE.TXT' 'RMS-E'
check_response 'TYPE UATWRITE.TXT' 'Per-user login command procedure'

# ... including in SYS$SYSTEM:, which on VMS is owned by SYSTEM and gives
# world R+E and no write (oracle: SYSEXE.DIR;1 [SYSTEM] (RWE,RWE,RE,RE)).
check_not_response 'COPY LOGIN.COM SYS$SYSTEM:UATSYS.TXT' 'RMS-E'
check_response 'TYPE SYS$SYSTEM:UATSYS.TXT' 'Per-user login command procedure'

# ... and can delete what it created again. The listing assertion is paired
# with a positive one on the same response so it cannot pass vacuously: if
# `DIRECTORY SYS$SYSTEM:` listed nothing at all, the DCL.EXE check fails.
check_not_response 'DELETE SYS$SYSTEM:UATSYS.TXT;1' 'RMS-E'
check_response 'DIRECTORY SYS$SYSTEM:' 'DCL\.EXE'
check_not_response 'DIRECTORY SYS$SYSTEM:' 'UATSYS\.TXT'

# --- and the refusal, which is the half that makes it access control -----
#
# GUEST [200,201] is not in a system UIC group and does not own the system
# tree. It must be able to read the system tree and write its OWN login
# directory, and must NOT be able to write SYS$SYSTEM:. All three are
# asserted, because any one alone is satisfied by a broken system: "GUEST
# can write everywhere" passes the first two, "GUEST can do nothing" passes
# the last.
check_response 'TYPE SYS$MANAGER:LOGIN.COM' 'Per-user login command procedure'
check_not_response 'COPY SYS$MANAGER:LOGIN.COM UATUSER.TXT' 'RMS-E'
check_response 'TYPE UATUSER.TXT' 'Per-user login command procedure'
check_response 'COPY SYS$MANAGER:LOGIN.COM SYS$SYSTEM:UATDENY.TXT' 'RMS-E-CRE'
check_not_response 'TYPE SYS$SYSTEM:UATDENY.TXT' 'Per-user login command procedure'

# The second session really is GUEST and not another SYSTEM login. Whole-log
# grep and safe as one: this is DCL's OWN logout line ('  GUEST      logged
# out at ...'), not the echo of the username we typed at the prompt -- the
# echo is the bare word, and no command in this script ever sends the string
# 'logged out'.
if echo "$OUTPUT" | grep -qE 'GUEST +logged out at'; then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: the second session was not a GUEST session"
fi

# SHOW USERS MUST NAME THE REAL, EXECUTIVE-ASSIGNED SESSION (vms-72c, Rule
# 11 corollary) -- NOT A FABRICATED ROW ABOUT ITSELF.
#
# cmd_show_users() used to read a file-based terminal-allocation table whose
# only writer vms-fb9 had already deleted, so the table was permanently
# empty and its "no entries" branch fabricated a single row out of the
# CALLING process's own context -- ctx->username, getpid() (a LINUX pid),
# and whatever the caller's own environment happened to hold for a
# terminal. MEASURED on a real QEMU boot of this exact image before
# vms-72c: SHOW USERS reported PID 00000049 for the SAME session whose
# SHOW PROCESS, one command earlier in the identical transcript, reported
# the executive-assigned VMS pid 10000003 -- two different numbers for one
# process, because SHOW USERS was never actually looking at it.
#
# THE DISCRIMINATING CHECK: the PID in SHOW USERS/FULL's row must equal
# SYSPID, the SAME executive-assigned pid this session's own 'SHOW PROCESS'
# response already printed earlier in the transcript (extracted here,
# straight out of CMD_OUTPUT -- not re-typed as a literal, which would just
# be trusting this script's own arithmetic instead of the product's). A
# fabricated getpid()-based row could still coincidentally print 8 hex
# digits in the right column; it could not print the SAME 8 hex digits SHOW
# PROCESS already proved were the VMS pid.
#
# vms-eaa (#555) SPLIT bare SHOW USERS from SHOW USERS/FULL into the two
# DIFFERENT tables the DCL Dictionary actually specifies (cmd_show_users()
# header comment, src/vmsdcl/dcl_cmd_show.c): the default form is per-USER
# counts and carries no PID at all -- only /FULL is per-PROCESS and carries
# one. This PID check now runs against 'SHOW USERS/FULL', not bare
# 'SHOW USERS'; tests/dcl/test_show_users_full.sh /
# test_show_users_terminal.sh pin the same split and are the ground truth
# this block was re-aligned against.
SYSPID=$(printf '%s' "${CMD_OUTPUT['SHOW PROCESS']}" | grep -oE 'Process ID:   [0-9A-F]{8}' | grep -oE '[0-9A-F]{8}$')
if [ -n "$SYSPID" ]; then
    check_response 'SHOW USERS/FULL' "$SYSPID"
else
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: could not extract A's own VMS pid to check SHOW USERS/FULL against"
fi

# vms-086 gave SHOW USERS/FULL a Node column (Username, Node, Process Name,
# PID, Terminal -- VSI OpenVMS DCL Dictionary SHOW USERS /FULL entry,
# https://www0.mi.infn.it/~calcolo/OpenVMS/ssb71/9996/9996p060.htm), sitting
# between username and process name -- the same fact SHOW PROCESS's own
# 'Node: +OVMX' check above already proves. vms-eaa (#555) kept the Node
# column but dropped the Type column (an always-"Interactive" OVMX addition
# absent from the DCL Dictionary) from /FULL's row set -- see
# test_show_users_full.sh's EXPECT_NOT:Interactive and the cmd_show_users()
# header comment. This session's process name is its own username
# (tools/vms_login.c's vms_kif_setprn(rec->username) call), so the /FULL row
# reads Username, Node, Process-Name all "SYSTEM", in that column order.
check_response 'SHOW USERS/FULL' 'SYSTEM +OVMX +SYSTEM'

# vms-eaa (#555): the default form is per-USER counts (Username Node
# Interactive Subprocess Batch), NOT the /FULL per-process table above --
# and #555 also dropped the pre-#555 "(interactive = N, subprocess = N,
# batch = N)" parenthetical summary line as a non-VMS addition (the DCL
# Dictionary has no such line). What's left to assert is the real header
# and the real count row: the header carries all three named columns, and
# the one SYSTEM session -- one distinct user, one process -- rolls up to
# an Interactive count of 1 next to its Username/Node columns (Subprocess
# and Batch render blank at zero, per the cmd_show_users() header comment).
# The "Total number of users"/"number of processes" line is unchanged by
# #555 and still computed from two distinct counts (vms-086; independently
# confirmed by three captures, ibid.), so it is re-asserted as-is.
check_response 'SHOW USERS' 'Username +Node +Interactive +Subprocess +Batch'
check_response 'SHOW USERS' 'Total number of users = 1,  number of processes = 1'
check_response 'SHOW USERS' 'SYSTEM +OVMX +1'

# ... and the default form must NOT be the /FULL per-process table (vms-eaa,
# #555's whole fidelity point) -- this session's process name must not leak
# into the default row the way it legitimately does in /FULL above.
check_not_response 'SHOW USERS' 'Process Name'

# vms-eaa (#555): SHOW USERS/FULL's row set is exactly Username, Node,
# Process Name, PID, Terminal (VSI OpenVMS DCL Dictionary SHOW USERS /FULL
# entry, ibid.). The vms-086 "Login Time" column asserted below until now
# was an unsourced OVMX extension -- absent from the DCL Dictionary, nothing
# populated JPI$_LOGINTIM into it -- and #555 dropped it as a fidelity tell
# alongside Type (cmd_show_users() header comment; confirmed against the
# current /FULL printf, which emits no timestamp field at all). Assert it
# stays gone rather than merely stop asserting it is there: a regression
# that resurrected an unsourced timestamp on the process row, specifically,
# should fail this test. (The banner line above the table legitimately
# carries this same DD-MMM-YYYY timestamp format for "at <now>" -- this
# check is anchored to the SYSTEM/OVMX/SYSTEM process row itself, a
# different line, so it cannot be satisfied/defeated by the banner.)
check_not_response 'SHOW USERS/FULL' 'SYSTEM +OVMX +SYSTEM.*[0-9]{1,2}-(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}'

# SHOW TERMINAL must name the terminal THIS LOGIN SESSION is on, read out of
# the executive (vms-d0b).
#
# TIGHTENED, and the old pattern is worth recording because it was weak in a
# way that mattered. It was '(Terminal|Device|VT100)', matched
# case-insensitively against SHOW TERMINAL's own response -- so it was
# satisfied by the string "Device_Type:" in a header, by the word "terminal"
# in a diagnostic, and (before vms-fb9) by a terminal name DCL had invented
# for itself out of a VMS_TERMINAL environment variable. Every one of those
# satisfies "SHOW TERMINAL printed something", which is not the property.
#
# The property is that the name comes from the EXECUTIVE. On this runtime
# PID 1's login child takes a channel to the console and records it
# (src/ovmx_init/ovmx_init.c), so the executive's process row for this job
# says OPA0: and SHOW TERMINAL reads it back with the physical-name
# underscore the oracle prints (docs/oracle/vax73-terminal-device.md §1).
# Nothing in DCL can produce that string on its own: the environment handoff
# and the invented "_FTA0:" default are both deleted and gated
# (tests/integration/test_terminal_identity.sh), and with no binding in the
# executive this command prints nothing at all -- which is the case
# tests/qemu/test_syssvc_showterm.c runs beside this one.
check_response 'SHOW TERMINAL' '^Terminal: _OPA0:'

# SHOW DEVICE must list the console the EXECUTIVE created (vms-fb9). This is
# the one assertion in this file that cannot be satisfied by anything inside
# DCL: OPA0: is created by vms.ko at module init (src/kernel/vms_devtab.c),
# nothing in userspace registers it, and vms-fb9 deleted every source DCL
# used to invent device names from (/proc/mounts, a process-local MOUNT
# table, a hardcoded stub row, the "_OPA0:"/"_FTA0:" arrays). So DCL can
# only print this by having read /dev/vms.
#
# Anchored to SHOW DEVICE's own response, and matched at the start of a line
# with the trailing colon, because 'OPA0' also appears in this session's
# SHOW PROCESS output; an unanchored scan would pass on that alone.
# tests/qemu/test_syssvc_showdev.c carries the A-writes/B-reads half (a
# second process allocates the console and SHOW DEVICE observes it) -- what
# this adds is that it holds in the PRODUCT boot, through a real login, not
# only in the kernel-test initramfs.
check_response 'SHOW DEVICE' '^OPA0: +Online'

# THE MOUNTED SYSTEM DISK REFLECTS ITS REAL MOUNT STATE (vms-e6f). The boot
# $MOUNTs VDA0: (the OVMXSYS ODS-2 volume) into the executive-global ACP
# mounted-volume table; SHOW DEVICE reads that table through VMS_IOCTL_GETVOL and
# must report VDA0: as Mounted with its volume label and free-block count -- not
# the bare "Online" it printed before this fix, when it read only the device
# table (which carries no mount state). Anchored to each command's own response.
# The label and the free count are the executive/ACP's genuine readings (the
# OVMXSYS home block + a live BITMAP.SYS scan), never fabricated (INV-6).
#
# Bare SHOW DEVICE now groups by class: the disk section (VDA0: Mounted) then the
# terminal section (OPA0: Online, asserted above) -- so the same listing proves
# both a mounted disk and the console terminal read from the executive.
check_response 'SHOW DEVICE'            '^VDA0: +Mounted'
check_response 'SHOW DEVICE VDA0:'      '^VDA0: +Mounted'
check_response 'SHOW DEVICE VDA0:'      'OVMXSYS'
check_response 'SHOW DEVICE/FULL VDA0:' 'is online, mounted'
check_response 'SHOW DEVICE/FULL VDA0:' 'Volume label +OVMXSYS'
check_response 'SHOW DEVICE/FULL VDA0:' 'Free blocks +[0-9]'
check_response 'SHOW DEVICE/FULL VDA0:' 'Maximum blocks +[0-9]'

# HELP should produce output. Anchored to HELP SHOW's own response (not the
# whole log) because the command text itself contains 'SHOW' -- an unanchored
# scan would pass on the echo alone regardless of whether HELP.EXE printed
# anything real.
check_response 'HELP SHOW' '(SHOW|Additional information)'

# THE SYSTEM IDENTITY IS ESTABLISHED BY THE EXECUTIVE, not declared
# (vms-2b8; the process it is established ON changed in vms-9b7).
#
# PID 1 used to call vms_pcb_init(0xFFFFFFFFFFFFFFFF) followed by
# vms_pcb_set_identity(1, [1,4], "SYSTEM", "SYSTEM") -- a process writing
# its own user name, UIC and every privilege bit into a private structure.
# vms-2b8 replaced that with a real establishment: read the SYSTEM record
# from SYSUAF, ask the executive to stamp it (VMS_IOCTL_SETIDENT, which
# refuses any caller without SETPRV), then print the row the executive
# holds back.
#
# WHAT vms-9b7 CHANGED, and what it did NOT. The read-and-stamp now happens
# in SYS$SYSTEM:PROVISION.EXE -- the process PID 1 execs where it used to
# exec DCL.EXE, and which execs DCL.EXE on STARTUP.COM afterwards, so the
# identity carries into the DCL that runs system startup. That is what
# OpenVMS does (STARTUP runs under username SYSTEM), and it is what let PID 1
# stop parsing SYSUAF with two 512-byte parsers of its own -- the divergence
# that could leave the system with no readable SYSTEM record and no boot.
#
# THE ASSERTION IS UNCHANGED AND IS NOT WEAKER: the same line, with the same
# values, printed at boot time before the login prompt exists, from a row read
# back out of the executive rather than from what the caller asked for.
#
# WHOLE-LOG grep, and safe as one: this is a BOOT-TIME diagnostic, printed
# before the login prompt exists. The script cannot have typed it -- the
# session has not started -- so unlike the command-response assertions
# above it is not echo-satisfiable. The values are asserted, not just the
# line: deleting the SETIDENT call leaves the user name empty and the UIC
# [0,0] (root's derived credentials), and this goes red.
if echo "$OUTPUT" | grep -qF 'system identity SYSTEM [1,4] established by the executive'; then
    PASS=$((PASS + 1))
else
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: the system identity was not established by the executive from SYSUAF"
fi

# Unix leak checks -- scanned against the WHOLE log (including the boot log
# and command echoes), which is a strictly BROADER surface than before.
# Safe: none of these strings are ones this script ever sends, so widening
# the scan can only catch more real leaks, never manufacture a false one
# from our own input.
check_not_contains '/bin/bash'
check_not_contains 'Permission denied'
check_not_contains 'No such file or directory'
check_not_contains 'Segmentation fault'
check_not_contains '/home/'
check_not_contains 'errno'

echo ""
echo "=== UAT Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
if [ -n "$ERRORS" ]; then
    echo -e "Errors:$ERRORS"
fi

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
echo "All checks passed!"
