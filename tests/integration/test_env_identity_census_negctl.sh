#!/bin/sh
#
# test_env_identity_census_negctl.sh - the evasions (vms-cb5).
#
# test_env_identity_census.sh is a gate whose PASSING output is a list. A list
# that would look identical if the scanner were broken asserts nothing, and
# the defect this gate exists to fix was precisely a census that everyone
# believed and nobody had run. So every property it claims gets its own
# minimal mutation here, applied to a sandbox COPY of the tree, and each must
# turn the gate RED for its own reason.
#
# One mutation per property, and each mutation is the smallest edit that trips
# THAT property. In particular F and G exist because two of the gate's claims
# are about its own SCOPE and its own comment stripping -- the two things that
# silently failed in the gate this one replaces:
#
#   F. the scan really covers tools/ (the directory whose omission from
#      test_terminal_identity.sh let the refuted "written ONLY by vmssshd"
#      sentence survive review);
#   G. a variable named only in a COMMENT does not enter the census (if it
#      did, the census would be full of this repo's own deletion notes and
#      would be unfalsifiable).
#
# Usage: test_env_identity_census_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE_REL="tests/integration/test_env_identity_census.sh"
status=0

SANDBOX=$(mktemp -d) || exit 1
trap 'rm -rf "$SANDBOX"' EXIT INT TERM

echo "vms-cb5 census gate — evasions"
echo ""

# fresh_tree: a clean copy of just what the gate reads.
fresh_tree() {
    rm -rf "$SANDBOX/t"
    mkdir -p "$SANDBOX/t/tests/integration"
    cp -r "$SRC_ROOT/src"   "$SANDBOX/t/src"
    cp -r "$SRC_ROOT/tools" "$SANDBOX/t/tools"
    cp "$SRC_ROOT/$GATE_REL" "$SANDBOX/t/$GATE_REL"
    chmod +x "$SANDBOX/t/$GATE_REL"
}

# expect_red <label> — run the gate on the sandbox; it MUST fail.
expect_red() {
    label="$1"
    out=$("$SANDBOX/t/$GATE_REL" "$SANDBOX/t" 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  OK (went red): $label"
    else
        echo "FAIL: $label -- the gate stayed GREEN"
        printf '%s\n' "$out" | sed 's/^/      /'
        status=1
    fi
}

# expect_green <label> — the control: an UNmutated tree must pass, or every
# "went red" above proves nothing but that the sandbox is broken.
expect_green() {
    label="$1"
    out=$("$SANDBOX/t/$GATE_REL" "$SANDBOX/t" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  OK (stayed green): $label"
    else
        echo "FAIL: $label -- the gate went RED on an unmutated tree"
        printf '%s\n' "$out" | sed 's/^/      /'
        status=1
    fi
}

# --- CONTROL: the sandbox itself is green -----------------------------------
fresh_tree
expect_green "an unmutated copy of the tree passes"

# --- A. a new WRITER in src/ is caught --------------------------------------
fresh_tree
printf '%s\n' 'static void evade_a(void) { setenv("VMS_USERNAME", "SYSTEM", 1); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_main.c"
expect_red "A: a new setenv(\"VMS_USERNAME\") in src/ is an undeclared writer"

# --- B. a new READER in src/ is caught --------------------------------------
fresh_tree
printf '%s\n' 'static const char *evade_b(void) { return getenv("VMS_USERNAME"); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_main.c"
expect_red "B: a new getenv(\"VMS_USERNAME\") in src/ is an undeclared reader"

# --- C. a DECLARED site that disappears is caught ---------------------------
# The census must not survive its own subjects being deleted, or it becomes a
# monument. Deleting LOGINOUT's writer is the exact edit the security finding
# recommends, and it must fail LOUDLY until the declaration is removed too.
fresh_tree
sed -i 's|setenv("VMS_USERNAME",    rec->username,    1);|/* deleted */|' \
    "$SANDBOX/t/tools/vms_login.c"
expect_red "C: deleting a declared writer without undeclaring it is caught"

# --- D. the write-only claim goes red when a reader appears -----------------
# This is the claim that carries the security argument: VMS_PRIVILEGES et al.
# are harmless BECAUSE nothing reads them. A reader appearing anywhere must
# break that sentence, not merely add a census line.
fresh_tree
printf '%s\n' 'static const char *evade_d(void) { return getenv("VMS_PRIVILEGES"); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_cmd_show.c"
expect_red "D: a reader of VMS_PRIVILEGES breaks the write-only claim"

# --- E. VMS_TERMINAL is covered too -----------------------------------------
# It has neither writer nor reader today (vms-fb9 deleted them). The gate must
# still be watching it, not merely listing four variables it happens to find.
fresh_tree
printf '%s\n' 'static const char *evade_e(void) { return getenv("VMS_TERMINAL"); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_terminal.c"
expect_red "E: a reader of VMS_TERMINAL is caught, though it has no census entry"

# --- F. tools/ IS SCANNED ---------------------------------------------------
# THE POINT OF THIS WHOLE FILE. test_terminal_identity.sh scans src/ only, so
# this exact mutation is invisible to it -- and that blind spot is what let a
# reachable writer (tools/vms_login.c) sit under a review that said the only
# writer was unreachable. If F ever goes green, the gate has silently
# regressed to the scope that caused the defect.
fresh_tree
printf '%s\n' 'static const char *evade_f(void) { return getenv("VMS_UIC_GROUP"); }' \
    >> "$SANDBOX/t/tools/vms_authorize.c"
expect_red "F: a reader added under tools/ is caught -- tools/ really is scanned"

# --- G. a mention in a COMMENT does not enter the census --------------------
# The inverse failure, and the more insidious one: if prose counted, the
# census would be satisfied by this repo's own deletion notes and could never
# go red for a real site. tools/vms_login.c already carries the literal text
# setenv("VMS_UIC_GROUP"/... in a comment; this adds a getenv one too, and the
# gate must stay GREEN.
fresh_tree
printf '%s\n' '/* getenv("VMS_PRIVILEGES") and setenv("VMS_TERMINAL", x, 1) were deleted here */' \
    >> "$SANDBOX/t/tools/vms_mail.c"
expect_green "G: a variable named only in a comment does not enter the census"

# --- H. USER is watched (vms-b2e) -------------------------------------------
# The census went green for months while AUTHORIZE.EXE decided who could
# manage SYSUAF from getenv("USER"), because plain USER was not in VARS. It is
# now, and this is the case that keeps it there: if H goes green, the universe
# has silently narrowed back to the OVMX-prefixed names and the next
# identity-from-the-environment defect is invisible again.
fresh_tree
printf '%s\n' 'static const char *evade_h(void) { return getenv("USER"); }' \
    >> "$SANDBOX/t/tools/vms_authorize.c"
expect_red "H: a reader of USER is caught -- the vms-b2e blind spot is closed"

# --- I. LOGNAME is watched too ----------------------------------------------
# It has no census entry as a reader and vmssshd is its only writer; the gate
# must be WATCHING it rather than merely listing the names it happens to find.
fresh_tree
printf '%s\n' 'static const char *evade_i(void) { return getenv("LOGNAME"); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_main.c"
expect_red "I: a reader of LOGNAME is caught"

# --- J. USER does NOT over-fire on names that merely start with it ----------
# THE INVERSE FAILURE, and the one that gets a gate disabled. Adding a short
# name like USER to a substring-matched census risks it matching USERNAME,
# VMS_USERNAME or a USERNAME_SIZE macro, at which point the census fills with
# phantom sites, someone adds an exemption, and the gate stops being read. The
# closing quote in the grep patterns is what prevents it; this holds that
# apart. Both lines below are real environment reads of OTHER variables and
# neither is a declared site, so the gate must stay GREEN.
fresh_tree
printf '%s\n' 'static const char *evade_j1(void) { return getenv("USERNAME"); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_main.c"
printf '%s\n' 'static const char *evade_j2(void) { return getenv("LOGNAME_FILE"); }' \
    >> "$SANDBOX/t/src/vmsdcl/dcl_main.c"
expect_green "J: USER/LOGNAME do not over-fire on USERNAME or LOGNAME_FILE"

echo ""
if [ "$status" -eq 0 ]; then
    echo "PASS: every census property can be tripped, and prose cannot trip it"
else
    echo "FAIL: at least one census property is vacuous"
fi
exit $status
