#!/bin/sh
#
# test_opcom_record_body.sh - BEHAVIOURAL gate (rd vms-2d37): the OPCOM record
# OVMX writes to OPERATOR.LOG carries the MESSAGE, not the header bytes.
#
# WHAT WAS WRONG, MEASURED. $SNDOPR takes an OPC message BLOCK -- an opcdef
# header followed by text -- and every caller in this tree builds one. The
# service read it with dsc$strncpy(), i.e. treated the first byte of the BLOCK
# as the first byte of a C string, so it copied opc$b_ms_type and
# opc$b_ms_target and stopped at the first NUL inside opc$w_ms_rqstlen:
#
#   %%OPCOM, 02-AUG-2026 00:13:58.89, request 1 from user  on node OVMX
#   ^A^A
#
# Every OPCOM record OVMX wrote said that a request happened, by whom and when,
# and never what it was. That is an audit trail with the audit removed, which
# is why this is a gate and not a formatting preference.
#
# WHAT THIS GATE ASSERTS, and each check names the failure mode it exists to
# catch:
#
#   1. The body line is PRESENT and carries the text the caller sent. Presence,
#      not absence: "the control bytes are gone" is satisfied by a service that
#      writes no body at all, which is the same defect with a tidier symptom.
#   2. The body contains NO control characters. This is the specific corruption
#      that shipped, pinned so a regression cannot come back wearing different
#      bytes.
#   3. The header line is still well-formed. The fix moves where the body comes
#      from; if it also disturbed the %%OPCOM header -- which rd vms-cb5 round 3
#      fixed separately -- that is a regression this gate must not miss.
#
# WHAT THIS GATE DOES NOT CLAIM. It says nothing about whether OVMX's struct
# opcdef matches VSI's $OPCDEF byte for byte. That is unpinned and is rd
# vms-737. This gate is about the two halves of OVMX agreeing with each other,
# which is a different property and the one that was broken.
#
# THE USER FIELD IS DELIBERATELY NOT ASSERTED. With no /dev/vms the executive
# holds no name and the header honestly carries an empty user (Rule 9, see
# get_current_username in src/libvms/syssvc/sys_operator.c). Asserting a
# username here would make this gate pass or fail on whether the host has an
# executive, which is not what it is measuring.
#
# Usage: test_opcom_record_body.sh [PATH-TO-DCL.EXE]

set -u

DCL="${1:-${VMSDCL:-}}"
if [ -z "$DCL" ]; then
    for cand in "$(dirname "$0")/../../build/bin/DCL.EXE" \
                "$(dirname "$0")/../../build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

status=0
passed=0
failed=0

if [ -z "$DCL" ] || [ ! -x "$DCL" ]; then
    echo "FAIL: no DCL.EXE to exercise (looked at argv[1], \$VMSDCL, build/bin)"
    echo "  -> BEHAVIOURAL gate; with no binary it is FAILED, never skipped."
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-2d37: the OPCOM record body carries the message, not the header bytes"
echo "  DCL under test: $DCL"

# LOGOUT is the cheapest caller that emits a record, and its text is built by
# cmd_logout in src/vmsdcl/dcl_cmd_process.c as "logout: user X at DD-MON-YYYY".
#
# THE LOG PATH IS THE SERVICE'S TO CHOOSE, AND THIS GATE ASKS RATHER THAN
# GUESSES. open_operator_log() translates SYS$MANAGER:OPERATOR.LOG through
# vmsfs and falls back to /tmp/OPERATOR.LOG only when that cannot be opened.
# Neither is settable from the environment, and WHICH ONE IT USES DEPENDS ON THE
# HOST: on a dev seat with no populated /vms the fallback wins, and in CI --
# where an earlier test has already created /vms/SYS0 (rd vms-917) -- the vmsfs
# path resolves and the record lands there instead.
#
# An earlier revision of this gate hardcoded a guess at that second path and
# was WRONG about it, so the gate passed on the dev seat and reported "no
# OPERATOR.LOG was written at all" in CI on a build whose fix was working
# perfectly. Guessing a path the code computes is how a gate ends up measuring
# the host instead of the property.
#
# So: stamp the time, run the command, then find whichever OPERATOR.LOG this run
# actually touched. Anything older than the stamp is somebody else's record --
# a stale log must never be read as this run's output, or a broken build passes
# on a good record left behind by a previous one.
STAMP="$WORK/stamp"
: > "$STAMP"
# size of every OPERATOR.LOG that already exists, so the appended region can be
# isolated afterwards (see the note below the run)
find /vms /tmp -iname 'OPERATOR.LOG' 2>/dev/null | while read -r p; do
    printf '%s %s\n' "$(wc -c < "$p" 2>/dev/null || echo 0)" "$p"
done > "$WORK/sizes"
sleep 1   # filesystem timestamp granularity; without it a log written in the
          # same second as the stamp can compare as not-newer and be skipped

printf 'LOGOUT\n' | "$DCL" >"$WORK/dcl.out" 2>"$WORK/dcl.err"
DCL_RC=$?

# -iname, not -name: vmsfs_to_linux_path() LOWERCASES the translated filename,
# so SYS$MANAGER:OPERATOR.LOG lands on disk as
# /vms/SYS0/SYSCOMMON/SYSMGR/operator.log while the /tmp fallback keeps the
# upper-case name. Matching only the VMS spelling found the fallback on a dev
# seat and nothing at all in CI, measured with strace:
#   openat(AT_FDCWD, "/vms/SYS0/SYSCOMMON/SYSMGR/operator.log", ...) = 3
#
# READ ONLY THE BYTES THIS RUN APPENDED, AND THE REASON IS A CONTROL THAT
# CAUGHT THIS GATE CHEATING. OPERATOR.LOG is opened "a" -- it accumulates. An
# earlier revision took the first body line in the file, which on any log that
# already had a record in it was SOMEBODY ELSE'S record: the negative control
# then reported the defective build as PASSING, because the stale good line was
# still sitting at the top. A gate that reads a log it did not write is
# measuring history.
#
# So the size of every candidate is snapshotted BEFORE the command runs, and
# afterwards only the appended tail is examined.
LOG=$(find /vms /tmp -iname 'OPERATOR.LOG' -newer "$STAMP" 2>/dev/null | head -1)
if [ -n "$LOG" ]; then
    PREV=$(grep -F " $LOG" "$WORK/sizes" 2>/dev/null | cut -d' ' -f1)
    [ -n "$PREV" ] || PREV=0
    tail -c "+$((PREV + 1))" "$LOG" > "$WORK/appended" 2>/dev/null || : > "$WORK/appended"
fi

if [ -z "$LOG" ] || [ ! -s "$LOG" ]; then
    echo "  FAIL: no OPERATOR.LOG was written by this run, so there is no record to judge"
    echo "        catches: a service that stopped logging entirely -- which would"
    echo "        make every body assertion below pass by vacuity"
    echo ""
    echo "        DIAGNOSIS, printed because the first version of this gate could"
    echo "        not say WHY it found nothing and cost a CI round to work out:"
    echo "          DCL exit status: $DCL_RC"
    echo "          DCL stdout:"
    sed 's/^/            | /' "$WORK/dcl.out" 2>/dev/null | head -5
    echo "          DCL stderr:"
    sed 's/^/            | /' "$WORK/dcl.err" 2>/dev/null | head -5
    echo "          OPERATOR.LOG files present (any age):"
    find /vms /tmp -iname 'OPERATOR.LOG' 2>/dev/null | sed 's/^/            | /' | head -5
    echo ""
    echo "vms-2d37 OPCOM record body gate: 0 passed, 1 failed"
    exit 1
fi

RECORD="$WORK/appended"
if [ ! -s "$RECORD" ]; then
    echo "  FAIL: the log exists but this run appended nothing to it"
    echo "        catches: a service that stopped writing while an old record"
    echo "        left in the file would otherwise be read as this run's output"
    echo ""
    echo "vms-2d37 OPCOM record body gate: 0 passed, 1 failed"
    exit 1
fi
# The record now spans THREE lines (rd vms-32a, oracle-exact OPCOM format:
# docs/design-opcom-executive-logging.md): the boxed banner
# ("%%%%%%%%%%%  OPCOM  <ts>  %%%%%%%%%%%"), then "Request N, from user U
# on N", then the message body. Both header lines are skipped so BODY is
# the actual payload, not either header line.
BODY=$(grep -v '^%%%%%%%%%%%  OPCOM  ' "$RECORD" | grep -vE '^Request [0-9]+, from user ' | grep -v '^[[:space:]]*$' | head -1)

check() {
    if [ "$1" = "ok" ]; then
        echo "  PASS: $2"
        passed=$((passed + 1))
    else
        echo "  FAIL: $2"
        echo "        catches: $3"
        echo "        the record actually written:"
        cat -A "$RECORD" | head -4 | sed 's/^/          | /'
        failed=$((failed + 1))
        status=1
    fi
}

# 1. the body is present and is the caller's text
if printf '%s' "$BODY" | grep -q 'logout:'; then r=ok; else r=no; fi
check "$r" "the body carries the caller's message text" \
      "the header bytes reaching the log instead of the text, or no body at all"

# 2. no control characters survived into the body
if printf '%s' "$BODY" | LC_ALL=C grep -q '[[:cntrl:]]'; then r=no; else r=ok; fi
check "$r" "the body contains no control characters" \
      "the ^A^A corruption returning in any spelling -- the opcdef header being
                 copied as if it were text"

# 3. the header lines survived the change -- both the boxed banner (exactly
#    eleven '%', "OPCOM", the timestamp, eleven '%') and the "Request N,
#    from user U on N" body-line-2 that follows it immediately.
if grep -qE '^%%%%%%%%%%%  OPCOM  [0-9]{2}-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}  %%%%%%%%%%%$' "$LOG" \
    && grep -qE '^Request [0-9]+, from user .* on .+$' "$LOG"; then
    r=ok
else
    r=no
fi
check "$r" "the OPCOM header (banner + Request line) is still well-formed" \
      "a body fix that disturbed the header rd vms-cb5 round 3 fixed separately, or the oracle-exact format rd vms-32a introduced regressing"

echo ""
echo "vms-2d37 OPCOM record body gate: $passed passed, $failed failed"
exit $status
