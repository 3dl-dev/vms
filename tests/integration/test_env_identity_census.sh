#!/bin/sh
#
# test_env_identity_census.sh - standing gate (vms-cb5): the complete,
# DERIVED census of every place the identity environment variables are
# written or read.
#
# WHY THIS GATE EXISTS, AND WHAT IT IS FIXING. The Phase 3 security review
# (vms-cb5) concluded that no escalation path remained, and the load-bearing
# sentence in its report was:
#
#     "the VMS_USERNAME / VMS_UIC_* / VMS_PRIVILEGES env facades are written
#      ONLY by vmssshd, which is never launched"
#
# That sentence is FALSE. tools/vms_login.c -- LOGINOUT, on the console login
# path, which runs for every interactive session there is -- writes
# VMS_USERNAME too, and its own source comment says so. The review's whole
# argument was "the only writer is unreachable", so a second, REACHABLE
# writer invalidated the argument even though it did not invalidate the
# conclusion.
#
# The defect was not the reviewer's carelessness. It was that the census was
# PROSE. Nothing derived it, nothing printed it, and nothing failed when it
# went stale. tests/integration/test_terminal_identity.sh is the closest
# existing gate and it could not have caught this, because it scans
# "$SRC_ROOT/src" ONLY -- and BOTH the writer (tools/vms_login.c) and the one
# surviving reader (tools/vms_mail.c) live in tools/, which no source gate in
# this repo covered. A gate scoped to one directory is a gate against the
# directory a defect last happened to live in.
#
# SO THIS GATE DOES NOT ASSERT ABSENCE. Absence is the wrong shape here:
# there ARE legitimate writers, and there is one legitimate (if dead) reader.
# It DERIVES the census from the source, PRINTS it, and requires it to match
# a declared set. A new writer or reader of any of these five variables --
# anywhere in src/ or tools/, in any file, by any author -- fails this test
# with the new site named, instead of silently joining a list nobody rereads.
# Deleting a declared site fails it too, so the declarations cannot rot.
#
# WHAT THE CENSUS MEANS FOR SECURITY (measured 2026-08-01 on the real runtime,
# CLAUDE.md Rule 9 -- QEMU, console login, aarch64 TCG):
#
#   - The executive reads NONE of these. It is a kernel module; it has no
#     environment. That is proved behaviourally, not by this file, in
#     tests/qemu/test_syssvc_ident.c and test_kmod_ident.c, which exec DCL
#     with all five planted at their most privileged values and require
#     SHOW PROCESS, SHOW PROCESS/PRIVILEGES and F$GETJPI to report the
#     executive's answer instead, from two different processes.
#
#   - VMS_UIC_GROUP, VMS_UIC_MEMBER, VMS_PRIVILEGES and VMS_TERMINAL have
#     ZERO readers in the trees this gate scans (src/ and tools/ -- see the
#     scope note at the bottom before reading that as "anywhere"). They are
#     write-only. Their sole writer, vmssshd, is not merely unlaunched --
#     MEASURED: it is not in the runtime image at all (the fat initramfs
#     contains exactly 8 executables and vmssshd is not one of them). Per
#     Rule 10 those four setenv() calls should be DELETED rather than
#     documented; that is filed, not done here, because src/vmsssh/ belongs
#     to vms-475.
#
#   - VMS_USERNAME has exactly ONE reader, tools/vms_mail.c, which uses it to
#     choose whose mailbox to open. That is a real identity decision, so it is
#     the one entry below that could carry an escalation. It does not, for two
#     independent measured reasons.
#
#     (1) No way was found for a console user to write the process
#         environment. DCL contains no setenv/putenv at all, and its exec
#         paths (dcl_exec_utility's execv/execvp, cmd_run's execl, cmd_spawn's
#         execl) hand the child environ verbatim. The mechanisms a user could
#         actually reach were TRIED, on the runtime, not reasoned about:
#         DEFINE, ASSIGN and a DCL symbol assignment, each in its own fresh
#         login session, each followed by SPAWN of a fresh DCL -- and the
#         subprocess reported the LOGINOUT-set value every time. The
#         observation channel was proved live in the same run by moving the
#         cwd with SET DEFAULT and watching the subprocess still report the
#         environment's value over it. This is a "none found by these
#         probes", not a proof of impossibility: a mechanism nobody thought
#         to try is exactly what a gate is for, which is why the census
#         below fails on a new READER as well as a new writer.
#
#     (2) MAIL opened no mailbox at all on this runtime -- not another
#         user's, and not its own. It fails with "%MAIL-E-NOMAIL, cannot
#         create mail directory: No such file or directory" because
#         get_user_homedir() returns the raw VMS filespec from SYSUAF
#         ("SYS$SYSDEVICE:[USERS.GUEST]") and build_maildir() then uses it
#         as a Linux path. Measured for GUEST and for SYSTEM, which is both
#         accounts rather than a sample: the other four SYSUAF rows carry no
#         password hash and cannot authenticate at all (vms-08f).
#
# IF YOU ARE HERE BECAUSE THIS FAILED: adding your new site to the declared
# set below is the WRONG first move. Ask Rule 10's question first -- does VMS
# carry this in the environment? It does not; VMS identity is executive-
# resident (Rule 11). The right answer is almost always to delete the site.
#
# WHAT THIS GATE DOES *NOT* ENFORCE, stated here so nobody reads the census as
# broader than it is (the same disclosure test_terminal_identity.sh makes
# about itself, and for the same reason -- an undisclosed limit is how the
# refuted sentence got written in the first place):
#
#   - It matches the getenv/setenv/putenv SPELLING. Code that walks `environ`
#     or `vms_environ` directly (src/libvmssys/vms_runtime_init.c exports
#     both, plus vms_getenv/vms_setenv wrappers -- those wrappers ARE caught,
#     since their names contain the matched substrings, but a raw environ
#     walk is not) would read these variables invisibly to this file.
#   - It scans src/ and tools/. Shell and DCL procedures under distro/ are
#     not scanned. At the time of writing none of them mention any of these
#     five variables; that is a fact about today's tree, not a property this
#     gate maintains.
#   - It says nothing about whether a value, once read, is USED for anything.
#     tools/vms_mail.c is a declared reader precisely because it is one, and
#     the census being green does not mean nobody acts on what they read.
#
# The BEHAVIOURAL invariant -- that no identity OVMX reports comes from the
# environment -- is not enforced here and cannot be. It is enforced by
# tests/qemu/test_syssvc_ident.c and tests/qemu/test_kmod_ident.c, which run
# the real DCL against a real /dev/vms with all five variables planted at
# their most privileged values and require the executive's answer instead.
#
# Usage: test_env_identity_census.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

echo "vms-cb5 census gate: every writer and reader of the identity environment"
echo "  variables, DERIVED from src/ AND tools/ (C comments stripped)"
echo ""

# ---------------------------------------------------------------------------
# THE DECLARED CENSUS.
#
# One line per site: <kind> <file> <variable>
#   kind = WRITE (setenv/putenv) or READ (getenv)
# Order is irrelevant; the comparison sorts both sides.
#
# Line numbers are DELIBERATELY NOT recorded. A census pinned to line numbers
# fails on every unrelated edit above it, and a gate that cries wolf gets an
# exemption added and then gets ignored. The file + variable + direction is
# what carries the security meaning.
# ---------------------------------------------------------------------------
DECLARED=$(cat <<'EOF'
WRITE src/vmsssh/vmssshd.c VMS_PRIVILEGES
WRITE src/vmsssh/vmssshd.c VMS_UIC_GROUP
WRITE src/vmsssh/vmssshd.c VMS_UIC_MEMBER
WRITE src/vmsssh/vmssshd.c VMS_USERNAME
WRITE tools/vms_login.c VMS_USERNAME
READ tools/vms_mail.c VMS_USERNAME
EOF
)

# Strip C comments (/* */ and //) so a variable named only in prose -- and
# every one of them is named in prose somewhere, including in this repo's
# deletion notes and in the long comment above -- cannot enter the census.
# Same awk as tests/integration/test_terminal_identity.sh; string literals
# containing "/*" are not handled and do not occur in the files scanned.
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

VARS="VMS_USERNAME VMS_UIC_GROUP VMS_UIC_MEMBER VMS_PRIVILEGES VMS_TERMINAL"

# BOTH TREES. src/ alone is what let the refuted sentence stand.
FILES=$(find "$SRC_ROOT/src" "$SRC_ROOT/tools" \( -name '*.c' -o -name '*.h' \) | sort)

OBSERVED=""
for f in $FILES; do
    [ -f "$f" ] || continue
    rel=${f#"$SRC_ROOT"/}
    code=$(strip_comments "$f")
    for v in $VARS; do
        # VMS_USERNAME_SIZE is a buffer-size macro, not the variable. The
        # trailing '"' in the patterns below already excludes it (the macro is
        # never written as VMS_USERNAME_SIZE" ), but the getenv/setenv verb is
        # what actually pins these to the environment API rather than to any
        # identifier that merely starts with the same characters.
        if printf '%s\n' "$code" | grep -q "getenv( *\"$v\"\|getenv(\"$v\""; then
            OBSERVED="$OBSERVED
READ $rel $v"
        fi
        if printf '%s\n' "$code" | grep -q "setenv( *\"$v\"\|setenv(\"$v\"\|putenv( *\"$v\|putenv(\"$v"; then
            OBSERVED="$OBSERVED
WRITE $rel $v"
        fi
    done
done

OBS_SORTED=$(printf '%s\n' "$OBSERVED" | grep -v '^$' | sort)
DEC_SORTED=$(printf '%s\n' "$DECLARED"  | grep -v '^$' | sort)

echo "OBSERVED CENSUS ($(printf '%s\n' "$OBS_SORTED" | grep -c . ) sites):"
printf '%s\n' "$OBS_SORTED" | sed 's/^/  /'
echo ""

# TEMP FILES, NOT PROCESS SUBSTITUTION. This was written as
# `comm -23 - <(printf ...)`, which is a bashism: under dash -- which IS
# /bin/sh on Debian, and this file's shebang is #!/bin/sh -- it dies with
# 'Syntax error: "(" unexpected'. It did so INSIDE a pipeline, so `$?` was
# the exit status of the last stage and the script still reported success.
# Caught by running the file under dash on purpose; do not reintroduce.
TMPD=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM
printf '%s\n' "$OBS_SORTED" > "$TMPD/obs"
printf '%s\n' "$DEC_SORTED" > "$TMPD/dec"
UNDECLARED=$(comm -23 "$TMPD/obs" "$TMPD/dec")
MISSING=$(comm -13 "$TMPD/obs" "$TMPD/dec")

if [ -n "$UNDECLARED" ]; then
    echo "FAIL: a writer or reader of an identity environment variable is not declared"
    printf '%s\n' "$UNDECLARED" | sed 's/^/  NEW -> /'
    echo "  Read the header of this file before declaring it. Rule 10's answer"
    echo "  for an identity carried in the environment is to delete the site."
    status=1
else
    echo "  OK: no undeclared writer or reader"
fi

if [ -n "$MISSING" ]; then
    echo "FAIL: a declared site no longer exists -- the census has rotted"
    printf '%s\n' "$MISSING" | sed 's/^/  GONE -> /'
    echo "  If you deleted it (good), delete its line from DECLARED above too."
    status=1
else
    echo "  OK: every declared site still exists"
fi

# ---------------------------------------------------------------------------
# THE ONE PROPERTY THE CENSUS IMPLIES THAT IS WORTH STATING SEPARATELY.
#
# Four of the five variables are WRITE-ONLY: nothing reads them. That is the
# fact that makes the writer count harmless, and it is a stronger and more
# durable claim than any statement about which writers exist -- it survives
# any number of new writers. It is DERIVED from the census above, not
# declared, so it cannot be true here and false in the tree.
# ---------------------------------------------------------------------------
echo ""
for v in VMS_UIC_GROUP VMS_UIC_MEMBER VMS_PRIVILEGES VMS_TERMINAL; do
    n=$(printf '%s\n' "$OBS_SORTED" | grep -c "^READ .* $v$")
    if [ "$n" -eq 0 ]; then
        echo "  OK: $v has no reader in src/ or tools/ -- writing it decides nothing"
    else
        echo "FAIL: $v is now READ by $n site(s); it used to be write-only, so any"
        echo "      argument that its writers are harmless no longer holds"
        printf '%s\n' "$OBS_SORTED" | grep "^READ .* $v$" | sed 's/^/  -> /'
        status=1
    fi
done

echo ""
if [ "$status" -eq 0 ]; then
    echo "PASS: identity environment census matches"
else
    echo "FAIL: identity environment census does not match"
fi
exit $status
