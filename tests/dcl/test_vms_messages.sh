#!/bin/bash
# TEST: Error messages use VMS %FAC-SEV-IDENT format, AND the specific
# facility/ident VMS itself uses for each condition
# EXPECT: regex:%[A-Z]+-[WSEIF]-[A-Z]+,
# EXPECT: contains:VMS_MSG_FORMAT_OK
# EXPECT_NOT: contains:VMS_MSG_FORMAT_FAIL
#
# vms-fe21: re-armed. Before this, check_vms_format() returned OK whenever a
# command produced NO output starting with "%" -- so a command whose error
# path silently regressed to printing nothing (or to printing prose with no
# "%" prefix at all) passed exactly like a correctly-formatted one. Absence
# of output was indistinguishable from correct output.
#
# Each scenario below now also asserts the SPECIFIC %FACILITY-SEV-IDENT VMS
# actually emits for that condition, not just "whatever we got happened to
# be well-formed". The idents are grounded in the source that raises them
# (src/vmsdcl/dcl_exec.c, dcl_cmd_set.c, dcl_cmd_file.c, dcl_cmd_show.c),
# which is itself modeled on the DCL Dictionary's documented condition names
# (IVVERB, NOKEYW, DIRECT, NOIFBLK, NOLAB, NOGOSUB, IVKEYW, RMS FNF/RNF). If
# any of these error paths regresses -- wrong ident, wrong facility, or no
# error at all -- assert_ident below fails and FAILURES becomes nonzero,
# which flips the VMS_MSG_FORMAT_OK/_FAIL token the harness gates on.
VMSDCL="${VMSDCL:-vmsdcl}"
FAILURES=0

check_vms_format() {
    local desc="$1"
    local output="$2"
    # Extract lines that look like error messages (start with %)
    local err_lines
    err_lines=$(echo "$output" | grep '^%' || true)
    if [ -z "$err_lines" ]; then
        return 0
    fi
    # Every line starting with % must match %FAC-SEV-IDENT, format
    while IFS= read -r line; do
        if ! echo "$line" | grep -qE '^%[A-Z]+-[WSEIF]-[A-Z]+, '; then
            echo "  BAD FORMAT in $desc: $line"
            FAILURES=$((FAILURES + 1))
        fi
    done <<< "$err_lines"
}

# assert_ident: the condition MUST produce this exact %FACILITY-S-IDENT.
# Unlike check_vms_format, this cannot be satisfied by silence -- a command
# that emits nothing, or emits a well-formed but WRONG ident, fails here.
assert_ident() {
    local desc="$1"
    local expected="$2"
    local output="$3"
    if echo "$output" | grep -qF "$expected"; then
        # Echo the actual raw VMS-format line (not just our own summary
        # prose) so the file-level EXPECT regex above -- which checks the
        # real %FACILITY-SEV-IDENT, comma-terminated shape -- has genuine
        # product output to match, not just this script's own commentary.
        echo "$output" | grep '^%'
        echo "PASS: $desc -> $expected"
    else
        echo "FAIL: $desc -- expected $expected, got:"
        echo "$output" | grep '^%' | sed 's/^/    /'
        FAILURES=$((FAILURES + 1))
    fi
}

# Test 1: Invalid command verb -> %DCL-E-IVVERB
output=$(echo "XYZZY_INVALID_CMD" | $VMSDCL 2>&1)
check_vms_format "invalid verb" "$output"
assert_ident "invalid verb" "%DCL-E-IVVERB" "$output"

# Test 2: Missing SHOW keyword -> %DCL-E-NOKEYW
output=$(echo "SHOW" | $VMSDCL 2>&1)
check_vms_format "show no keyword" "$output"
assert_ident "show no keyword" "%DCL-E-NOKEYW" "$output"

# Test 3: SET DEFAULT to invalid dir -> %DCL-E-DIRECT (dcl_cmd_set.c
# cmd_set_default: stat() fails or isn't a dir -> dcl_error("DCL",2,"DIRECT",...))
output=$(echo "SET DEFAULT [.NONEXISTENT_QWERTY_DIR]" | $VMSDCL 2>&1)
check_vms_format "set default invalid" "$output"
assert_ident "set default invalid" "%DCL-E-DIRECT" "$output"

# Test 4: DELETE nonexistent file -> %RMS-E-FNF (dcl_cmd_file.c cmd_delete)
output=$(echo "DELETE NONEXISTENT_FILE_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "delete nonexistent" "$output"
assert_ident "delete nonexistent" "%RMS-E-FNF" "$output"

# Test 5: TYPE nonexistent file -> %RMS-E-FNF (dcl_cmd_file.c cmd_type)
output=$(echo "TYPE NONEXISTENT_FILE_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "type nonexistent" "$output"
assert_ident "type nonexistent" "%RMS-E-FNF" "$output"

# Test 6: RENAME nonexistent file -> %RMS-E-RNF (dcl_cmd_file.c cmd_rename)
output=$(echo "RENAME NONEXISTENT_FILE_QWERTY.TXT NEW_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "rename nonexistent" "$output"
assert_ident "rename nonexistent" "%RMS-E-RNF" "$output"

# Test 7: ENDIF without IF -> %DCL-E-NOIFBLK (dcl_exec.c: distinct call site
# from Test 8's ELSE-without-IF, same ident, "ENDIF without IF" text).
# (Replaces the old "IF without ENDIF" case, which asserted nothing: an
# unterminated multi-line IF block is accepted silently at EOF and has no
# corresponding VMS error path in this implementation, so it could never
# have a real failure mode.)
output=$(echo "ENDIF" | $VMSDCL 2>&1)
check_vms_format "endif without if" "$output"
assert_ident "endif without if" "%DCL-E-NOIFBLK" "$output"

# Test 8: ELSE without IF -> %DCL-E-NOIFBLK
output=$(echo "ELSE" | $VMSDCL 2>&1)
check_vms_format "else without if" "$output"
assert_ident "else without if" "%DCL-E-NOIFBLK" "$output"

# Test 9: GOTO with no label -> %DCL-E-NOLAB
output=$(echo "GOTO" | $VMSDCL 2>&1)
check_vms_format "goto no label" "$output"
assert_ident "goto no label" "%DCL-E-NOLAB" "$output"

# Test 10: RETURN without GOSUB -> %DCL-E-NOGOSUB
output=$(echo "RETURN" | $VMSDCL 2>&1)
check_vms_format "return without gosub" "$output"
assert_ident "return without gosub" "%DCL-E-NOGOSUB" "$output"

# Test 11: Unrecognized SHOW keyword -> %DCL-E-IVKEYW
output=$(echo "SHOW XYZZY_INVALID" | $VMSDCL 2>&1)
check_vms_format "show invalid keyword" "$output"
assert_ident "show invalid keyword" "%DCL-E-IVKEYW" "$output"

# Test 12 (vms-a10): SET PROTECTION on a nonexistent file -> %RMS-E-PRV
# (dcl_cmd_set.c cmd_set_protection: chmod() fails with ENOENT and the error
# path formats vms_strerror(errno)). This exercises one of the three
# vms_strerror() call sites in dcl_cmd_set.c that had NO prototype in scope
# because the file never included dcl/vms_messages.h -- so vms_strerror() was
# assumed to return int, its 64-bit char* was truncated to 32 bits, and the
# %s dereference in dcl_error() SEGFAULTED (same defect class as the RENAME
# crash fixed for dcl_cmd_file.c in vms-fe21/#375). With the include, this
# path returns a clean %RMS-E-PRV instead of crashing. Pre-fix, $VMSDCL dies
# on SIGSEGV, emits no %-line, and assert_ident fails -> VMS_MSG_FORMAT_FAIL.
output=$(echo "SET PROTECTION (S:RWED,O:RW,G:R,W:) NONEXISTENT_FILE_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "set protection nonexistent" "$output"
assert_ident "set protection nonexistent" "%RMS-E-PRV" "$output"

if [ $FAILURES -eq 0 ]; then
    echo "VMS_MSG_FORMAT_OK"
else
    echo "VMS_MSG_FORMAT_FAIL ($FAILURES bad format/ident messages)"
fi
