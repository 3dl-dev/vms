#!/bin/sh
#
# test_terminal_identity.sh - standing gate (vms-fb9): a process does not
# name its own devices, and SHOW DEVICE does not invent rows.
#
# WHY THIS GATE EXISTS. On 2026-07-30 the operator rejected carrying a VMS
# process name across execve() in a VMS_PRCNAM environment variable: a
# process self-reporting an identity nothing else can see is a fake, however
# green it tests (CLAUDE.md rule 10, worked example 2). The SAME shape was
# live for terminals in three places at once --
#
#   src/ovmx_init/ovmx_init.c   setenv("VMS_TERMINAL", "_OPA0:")
#   src/vmsssh/vmssshd.c        setenv("VMS_TERMINAL") / setenv("VMS_DEVICE_TYPE")
#   src/vmsdcl/dcl_main.c       the matching getenv()s, plus a private
#                               "_FTA" name pool used when neither was set
#
# -- and SHOW DEVICE printed rows built from /proc/mounts, from a
# process-local table MOUNT keeps in this process's memory, and from a
# hardcoded stub when both produced nothing. vms-fb9 deleted all of it. This
# gate keeps it deleted.
#
# A VMS device is executive-resident (CLAUDE.md rule 11): it lives in the
# executive's device table (src/kernel/vms_devtab.c, reached via /dev/vms),
# every process on the node sees the same one, and a user-visible command is
# a READER of it -- never a thing that fabricates its own answer, and never a
# thing that is TOLD the answer by its parent.
#
# If you are here because this failed: do not add an exemption. The
# replacement for a name passed in the environment is $ASSIGN + $GETDVI. If
# the executive cannot answer the question yet, the honest output is no
# answer at all -- not a plausible one (rule 10).
#
# Tokens are matched against source with C comments STRIPPED, so the prose
# above and the deletion notes left at each site do not satisfy or trip any
# check. tests/integration/test_terminal_identity_negctl.sh proves each
# property below can actually go red, one minimal mutation at a time.
#
# WHAT THIS FILE DOES *NOT* ENFORCE, so nobody reads more into it than it
# says (vms-fb9 round 2). Every check below is TOKEN ABSENCE: it keeps a
# specific named fabrication deleted. It cannot see a NEW fabrication written
# in a spelling it has never met, and that is not a hypothetical -- an
# adversary reintroduced SHOW DEVICE's hardcoded stub row in the current
# oracle format and all ten checks here passed. The invariant "no row reaches
# the user that did not come from a /dev/vms read" is BEHAVIOURAL and is
# enforced by tests/integration/test_show_device_rows.sh, which runs DCL and
# requires stdout to be empty of rows. Do not add more token checks here in
# the belief that they cover it; they do not.
#
# Usage: test_terminal_identity.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

echo "vms-fb9 source gate: nothing names its own terminal (token absence only --"
echo "  the no-fabricated-rows PROPERTY is enforced by test_show_device_rows.sh)"

# Strip C comments (/* */ and //) so a token that appears only in prose is
# not mistaken for code. String literals containing "/*" are not handled and
# do not occur in the files scanned.
strip_comments() {
    awk '
    BEGIN { inc = 0 }
    {
        line = $0; out = ""; i = 1
        while (i <= length(line)) {
            two = substr(line, i, 2)
            if (inc) {
                if (two == "*/") { inc = 0; i += 2 } else { i++ }
            } else if (two == "/*") {
                inc = 1; i += 2
            } else if (two == "//") {
                break
            } else {
                out = out substr(line, i, 1); i++
            }
        }
        print out
    }' "$1"
}

# scan_absent <label> <fixed-token> <file...>
# Fails if the token appears in any file's CODE.
scan_absent() {
    label="$1"; token="$2"; shift 2
    hit=""
    for f in "$@"; do
        [ -f "$f" ] || continue
        if strip_comments "$f" | grep -qF "$token"; then
            hit="$hit $f"
        fi
    done
    if [ -n "$hit" ]; then
        echo "FAIL: $label"
        echo "  -> found '$token' in code:$hit"
        status=1
    else
        echo "  OK: $label"
    fi
}

SRC_FILES=$(find "$SRC_ROOT/src" -name '*.c' -o -name '*.h' | sort)

# --- 1. The terminal identity is never read out of the environment ------
scan_absent "no code reads VMS_TERMINAL from the environment" \
    'getenv("VMS_TERMINAL")' $SRC_FILES
scan_absent "no code reads VMS_DEVICE_TYPE from the environment" \
    'getenv("VMS_DEVICE_TYPE")' $SRC_FILES

# --- 2. ...and never written into it, which is where reading it starts --
scan_absent "no code hands VMS_TERMINAL down through the environment" \
    'setenv("VMS_TERMINAL"' $SRC_FILES
scan_absent "no code hands VMS_DEVICE_TYPE down through the environment" \
    'setenv("VMS_DEVICE_TYPE"' $SRC_FILES

# --- 3. Nothing hands itself a terminal name from a private pool --------
# TREE-WIDE ON THE SYMBOL, not "this call site in this file" (vms-fb9 round
# 2). The check used to name src/vmsdcl/dcl_main.c, and an adversary evaded
# it in one line by moving the call into src/vmsdcl/dcl_lexical.c -- the gate
# stayed green and the pool was still being used. A gate scoped to where a
# defect happened to live last time is a gate against history.
#
# vms_term_allocate() is now DELETED outright (Rule 10: do not keep a
# mechanism for a condition OVMX no longer has), so this scan covers its
# DEFINITION as well as any call -- re-adding it anywhere is what goes red.
scan_absent "no code allocates a terminal name from a private pool" \
    'vms_term_allocate' $SRC_FILES

# --- 3b. ...nor from a compiled-in default ------------------------------
# The fabrication had THREE sources (env, pool, default). Deleting two and
# leaving the third changes nothing observable, which is exactly how it
# survived: with no VMS_TERMINAL set and no pool file, vms_terminal_init()
# seeded "_FTA0:" and every DCL process claimed the same terminal.
#
# Scoped to vms_terminal_init()'s BODY, not the whole file: the same file
# legitimately READS term->device_name to print it (vms_terminal_show), and
# a whole-file token check would be satisfied by that read -- an assertion
# satisfiable by something other than the behaviour under test.
TERM_C="$SRC_ROOT/src/vmsdcl/dcl_terminal.c"
if [ -f "$TERM_C" ]; then
    init_body=$(strip_comments "$TERM_C" |
        awk '/^void vms_terminal_init/ { inf = 1 } inf { print } inf && /^}/ { exit }')
    if [ -z "$init_body" ]; then
        echo "FAIL: DCL does not seed a default terminal device name"
        echo "  -> vms_terminal_init() not found in $TERM_C; the check above"
        echo "     cannot have been evaluated, so it is reported as failed"
        status=1
    elif printf '%s\n' "$init_body" | grep -qF 'device_name'; then
        echo "FAIL: DCL does not seed a default terminal device name"
        echo "  -> vms_terminal_init() touches device_name. A terminal name is"
        echo "     looked up in the executive's device table, never defaulted."
        status=1
    else
        echo "  OK: DCL does not seed a default terminal device name"
    fi
fi

# --- 3c. No invented device-name literal anywhere in the tree -----------
# "_FTA0:" was the name every DCL process printed for its terminal when it
# did not know one -- from SHOW PROCESS, SHOW USERS, F$PROCESS(), vmssshd's
# PCB naming and F$DEVICE's hardcoded device list. Deleting the environment
# handoff while leaving those literals would have changed nothing a user can
# see: the real runtime's UAT still printed Terminal: _FTA0: with the handoff
# already gone. There is no allowlist here on purpose -- if a new site needs
# a device name, it reads one from the executive.
scan_absent "no invented terminal-device-name literal in the tree" \
    '"_FTA0:"' $SRC_FILES

# --- 4. SHOW DEVICE has no second source for a device row --------------
SHOW_C="$SRC_ROOT/src/vmsdcl/dcl_cmd_show.c"
scan_absent "SHOW DEVICE does not build device rows from /proc/mounts" \
    '/proc/mounts' "$SHOW_C"
scan_absent "SHOW DEVICE does not read the process-local MOUNT table" \
    'vms_device_table' "$SHOW_C"

# --- 5. ...and DOES read the executive's table -------------------------
# The absence checks above are all satisfiable by deleting the command
# outright. This is the paired positive: the row source must be the
# executive's $DEVICE_SCAN.
if [ -f "$SHOW_C" ] && strip_comments "$SHOW_C" | grep -qF 'vms_kif_devscan('; then
    echo "  OK: SHOW DEVICE reads the executive device table (\$DEVICE_SCAN)"
else
    echo "FAIL: SHOW DEVICE does not call vms_kif_devscan()"
    echo "  -> it must READ the executive's device table, not print nothing"
    echo "     and not find another source. See src/kernel/vms_devtab.c."
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "vms-fb9 gate: PASS"
else
    echo "vms-fb9 gate: FAIL"
fi
exit $status
