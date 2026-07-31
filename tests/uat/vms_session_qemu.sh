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
# regardless of what any real command printed. Where the two substrates
# genuinely differ, the real runtime is the authority.
#
# Sequencing waits on the actual DCL prompt rather than sleeping a guessed
# number of seconds. A UAT paced by fixed sleeps is a flaky test waiting to
# happen, and a flaky test is a broken test (CLAUDE.md Rule 11).

set -uo pipefail

KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
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
    'SHOW TERMINAL'
    'SHOW DEVICE'
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
truncate -s 64M "$DISK"
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

# --- Boot and log in -----------------------------------------------------
# The executive must come up first; if it does not, PID 1 aborts the boot and
# there is no session to test. Asserting it here (vms-0ff, ported by the
# vms-a35 PR #6 rebase) keeps a silent regression in the boot guarantee from
# surfacing as a confusing login timeout instead of a clear one.
wait_for '%OVMX-I-EXEC' "$BOOT_TIMEOUT" \
    || fail_with_console "ERROR: the executive never attached — the system did not come up"

wait_for 'Username:' "$BOOT_TIMEOUT" || fail_with_console "ERROR: no login prompt"
send 'SYSTEM'
wait_for 'Password:' || fail_with_console "ERROR: no password prompt"
send 'MANAGER'
wait_for 'Welcome to OVMX' || fail_with_console "ERROR: login did not succeed"

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
# for ('Username:', 'Password:', 'Welcome to OVMX') already appears earlier
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
wait_for 'Username:' "$STEP_TIMEOUT" "$LOGIN_OFFSET" \
    || fail_with_console "ERROR: no second login prompt after LOGOUT"
send 'GUEST'
wait_for 'Password:' "$STEP_TIMEOUT" "$LOGIN_OFFSET" \
    || fail_with_console "ERROR: no password prompt for GUEST"
send 'GUEST'
wait_for 'Welcome to OVMX' "$STEP_TIMEOUT" "$LOGIN_OFFSET" \
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
if grep -qF 'Welcome to OVMX' "$CONSOLE_LOG"; then
    WELCOME_OFFSET=$(grep -aboF 'Welcome to OVMX' "$CONSOLE_LOG" | head -1 | cut -d: -f1)
fi
SESSION_OUTPUT=$(tail -c "+$((WELCOME_OFFSET + 1))" "$CONSOLE_LOG")

echo "=== Session Output ==="
echo "$OUTPUT"
echo "=== End Session Output ==="

# --- Validation --------------------------------------------------------
PASS=0
FAIL=0
ERRORS=""

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

# THE PROCESS NAME IS STILL A REAL DIVERGENCE (vms-d0e, confirmed still
# open via `rd show vms-d0e` at the same 2026-07-31 measurement above).
# OVMX assigns no default process name at creation; VMS always has one.
# This is unrelated to the User: fix above -- LOGINOUT stamps a user name
# and UIC, not a process name, and $SETPRN was never called on this path.
check_known_divergence 'SHOW PROCESS' 'Process name: ""' \
    'vms-d0e' \
    'the executive row still carries no process name, so SHOW PROCESS prints Process name: "" (VMS has no nameless process -- oracle Section 1.3)'

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
# CMKRNL, CMEXEC, SETPRV, WORLD.
#
# THAT IS NOT THE SAME CLAIM AS "SYSUAF AUTHORIZES SYSTEM EXACTLY THAT SET",
# and round 3's comment here said the latter -- false, corrected round 4.
# MEASURED: distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT's SYSTEM row
# reads `SYSTEM||1|4|SYS$SYSDEVICE:[SYSMGR]||ALL` -- the seventh field,
# privileges, is the literal string ALL, not a 4-privilege
# list. The four names below are what SURVIVES THE VMS_PRV_M_ENFORCED
# INTERSECTION of that ALL mask, which is a strict subset -- SYSPRV,
# BYPASS, OPER, and 30 others (37 named rows in vms_priv_names[], minus
# the 4 shown, minus these 3 named here) are authorized by SYSUAF and correctly
# do NOT appear here, because nothing in vms.ko enforces them (see that
# constant's own comment). The old pattern would now silently pass on an
# EMPTY privilege block too (TMPMBX/NETMBX/OPER can't appear, but neither
# can anything else, and grep -qE against an empty capture only fails,
# which happens to be visible -- verified this round: the old pattern
# really did go red against the corrected output, it was not a silent
# pass). It is corrected here rather than widened to accept both, because
# accepting both would hide a future regression back to the unmasked
# display.
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
# command itself no longer claims a success it cannot deliver either: it
# prints %OVMX-I-NOSETPRV (informational, matching the SS$_NORMAL it
# returns -- round 5 fixed a W/success mismatch here too) and leaves
# ctx->privileges untouched, because actually reaching the executive is
# vms-pv1's job (vms_kif_setprv exists but is deliberately left
# OVMX-UNWIRED pending that item), not this one's.
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
check_response 'SHOW SYMBOL IDENT_CURPRIV' 'IDENT_CURPRIV = "CMKRNL,CMEXEC,SETPRV,WORLD"'
check_response 'SHOW SYMBOL IDENT_AUTHPRIV' 'IDENT_AUTHPRIV = "CMKRNL,CMEXEC,SETPRV,WORLD"'

# SEVERITY LETTER, ASSERTED (vms-2b8 round 5's own fix, uncovered until
# round 6): %OVMX-W-NOSETPRV printed as a Warning next to a function that
# falls through to `return SS$_NORMAL` -- a success status -- contradicted
# itself (W is DCL's "unsuccessful" severity, SS$_NORMAL is success). Round
# 5 changed the letter to I; nothing asserted it. 'SET
# PROCESS/PRIVILEGES=(OPER)' is unique text in SYSTEM_CMDS (unlike 'SHOW
# PROCESS /PRIVILEGES', it never repeats), so a plain check_response is
# safe here without the by-position anchor the surrounding checks need.
check_response 'SET PROCESS/PRIVILEGES=(OPER)' 'OVMX-I-NOSETPRV'
check_not_response 'SET PROCESS/PRIVILEGES=(OPER)' 'OVMX-W-NOSETPRV'

# DEFECT-1 POSITIVE CONTROL (vms-2b8 round 6): the GRANT path, not just
# the deny path. Every proof above this point (IDX_ALTPRI/IDX_PRIORITY_SET,
# SET TIME below) shows enforced_privs_held() REFUSING an operation; none
# shows it GRANTING one, and a gate that consults nothing and refuses
# unconditionally would pass every deny assertion in this file exactly as
# well as a correct gate does. This is the counterpart: SET
# PROCESS/PRIVILEGES's own gate (src/vmsdcl/dcl_cmd_set.c,
# cmd_set_process) reads the SAME enforced_privs_held() as SET
# PROCESS/PRIORITY, requiring SETPRV||SYSPRV||BYPASS -- and SETPRV IS one
# of the four bits VMS_PRV_M_ENFORCED names, so the SYSTEM session (which
# holds it, proven above by IDENT_SETPRV/IDENT_SETPRV2) must be let past
# this gate. The absence of %SET-E-NOPRIV here (checked negatively,
# because a command that failed the gate would print NOPRIV and not
# OVMX-I-NOSETPRV -- the two are mutually exclusive outcomes of the same
# branch) is the grant: the gate was consulted and it said yes.
#
# VERIFIED NON-VACUOUS BY MUTATION, one property at a time
# (src/vmsdcl/dcl_cmd_set.c's enforced_privs_held()), each rebuilt
# (podman build -f distro/Dockerfile.bootable) and re-booted under QEMU
# from a clean image, real runs, real counts:
#   BASELINE (this file's own fix, unmodified): 47 passed / 0 failed.
#   Body forced to `return 0;` (always-refuse): 45 passed / 2 failed --
#     BOTH failures on 'SET PROCESS/PRIVILEGES=(OPER)': the
#     'OVMX-I-NOSETPRV' presence check above and this grant check below
#     ('should NOT contain NOPRIV') -- the command now printed
#     %SET-E-NOPRIV instead. IDX_ALTPRI/IDX_PRIORITY_SET (the PRIORITY
#     deny pair) and the SET TIME deny assertion below stayed GREEN,
#     correctly: they were already refused, and still are.
#   Body forced to `return ~(uint64_t)0;` (always-grant): 45 passed / 2
#     failed -- IDX_PRIORITY_SET ('NOPRIV') went RED (SET
#     PROCESS/PRIORITY=6 was silently authorized), AND the SET TIME deny
#     assertion below went RED too (its gate passed on the strength of
#     the forced OPER bit, so cmd_set_time fell through to the REAL
#     settimeofday(2) call, which failed with a genuine OS-level EPERM --
#     this session runs as UID 4/SYSTEM post-drop, not root -- printing a
#     DIFFERENT message, "cannot set system time - insufficient OS
#     privilege", that no longer matches the gate-refusal text the
#     assertion looks for). Both grant checks on 'SET
#     PROCESS/PRIVILEGES=(OPER)' stayed GREEN (unsurprising: SETPRV
#     already granted this one before the mutation too; the point of
#     this run is that the DENY side is not vacuous, not that the grant
#     side moves).
# Each mutation flips exactly the assertion class it should (deny-only or
# grant-only) and no other -- the pairing is not satisfiable by a gate
# that always refuses or always grants, only by one that actually
# consults the mask.
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
# subprocess also registered holding CMKRNL|CMEXEC|SETPRV|WORLD, and
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

# THE AUTHENTICATED USER CAN ACTUALLY USE THE SYSTEM (vms-2b8 round 7).
#
# WHY THIS BLOCK EXISTS. Round 6 made LOGINOUT drop to the SYSUAF UIC, and
# nothing in the suite wrote a file, so nobody noticed that a user could log
# in and then not create one -- in their own login directory. `COPY LOGIN.COM
# ADVPROBE.TXT` answered `%RMS-E-CRE, cannot create - ADVPROBE.TXT` on the
# real bootable image. The cause was not the protection check: it was that
# PID 1 installed the whole VMS tree as Linux root, so once a session
# genuinely became UIC [1,4] there was nothing on the system that the SYSTEM
# account owned. src/ovmx_init/ovmx_init.c provision_ownership() now gives
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
# substrate-independent and is the thing that actually matters.
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

# SHOW TERMINAL should show terminal info in its own response. Anchored, not
# a whole-log scan: grep is case-insensitive, so 'Terminal' matches the echo
# of the command 'SHOW TERMINAL' itself, and '_[A-Z]' matches the echoes of
# 'DEFINE UAT_TEST ...' / 'SHOW LOGICAL UAT_TEST' / 'DEASSIGN UAT_TEST' (the
# '_T') as well as '_OPA0:' in SHOW PROCESS's output -- neither alternative
# needs SHOW TERMINAL to have run at all. Verified by mutation: prefixing
# the command with a bogus verb (%DCL-E-IVVERB, no terminal info printed)
# makes this assertion fail; the unmutated run still passes.
check_response 'SHOW TERMINAL' '(Terminal|Device|VT100)'

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

# HELP should produce output. Anchored to HELP SHOW's own response (not the
# whole log) because the command text itself contains 'SHOW' -- an unanchored
# scan would pass on the echo alone regardless of whether HELP.EXE printed
# anything real.
check_response 'HELP SHOW' '(SHOW|Additional information)'

# PID 1's identity is ESTABLISHED BY THE EXECUTIVE, not declared (vms-2b8).
#
# PID 1 used to call vms_pcb_init(0xFFFFFFFFFFFFFFFF) followed by
# vms_pcb_set_identity(1, [1,4], "SYSTEM", "SYSTEM") -- a process writing
# its own user name, UIC and every privilege bit into a private structure.
# It now reads the SYSTEM record from SYSUAF and asks the executive to
# stamp it (VMS_IOCTL_SETIDENT, which refuses any caller without SETPRV),
# then prints the row the executive holds back to it.
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
    ERRORS="${ERRORS}\n  FAIL: PID 1's identity was not established by the executive from SYSUAF"
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
