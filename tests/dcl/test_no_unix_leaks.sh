#!/bin/bash
# TEST: Error messages and output must not leak Unix paths or terminology
# EXPECT: contains:LEAK_CHECK_COMPLETE
# EXPECT_NOT: contains:UNIX_LEAK_DETECTED
#
# vms-fe21: re-armed. This test's only assertion used to be
# LEAK_CHECK_COMPLETE, a token the script printed unconditionally on every
# run -- so it passed whether or not any leak was found. The real guard
# (this EXPECT_NOT) was written but commented out as FUTURE_EXPECT_NOT, and
# a detected leak was downgraded to a printed WARNING instead of a failure.
# That is the no-lies defect this test suite exists to catch, in the test
# suite itself: a test that cannot fail is not testing anything.
#
# The guard is real now: any leak sets UNIX_LEAK_DETECTED in the output (the
# harness's EXPECT_NOT gates on it directly) and the script exits non-zero.
# Proof this can actually go red: pointing VMSDCL at a stub that echoes a
# Unix leak for one of the probed commands makes this script exit 1 with
# UNIX_LEAK_DETECTED in its output; pointing it at the real, clean vmsdcl
# exits 0 with no such token. See the PR description for the transcript.
VMSDCL="${VMSDCL:-vmsdcl}"

# Patterns that indicate Unix leakage in VMS output
UNIX_PATTERNS="(/home/|/tmp/|/var/|/etc/|/usr/|/dev/|strerror|Permission denied|No such file or directory|bash:|/bin/sh|/bin/bash)"

LEAKS_FOUND=0
check_output() {
    local desc="$1"
    local output="$2"
    if echo "$output" | grep -qE "$UNIX_PATTERNS"; then
        echo "  LEAK in $desc:"
        echo "$output" | grep -E "$UNIX_PATTERNS" | head -3 | sed 's/^/    /'
        LEAKS_FOUND=$((LEAKS_FOUND + 1))
    fi
}

# Test 1: DELETE nonexistent file — should get VMS error, not Unix
output=$(echo "DELETE NONEXISTENT_FILE.TXT" | $VMSDCL 2>&1)
check_output "DELETE nonexistent" "$output"

# Test 2: COPY nonexistent file
output=$(echo "COPY NONEXISTENT_SRC.TXT NONEXISTENT_DST.TXT" | $VMSDCL 2>&1)
check_output "COPY nonexistent" "$output"

# Test 3: TYPE nonexistent file
output=$(echo "TYPE NONEXISTENT_FILE.TXT" | $VMSDCL 2>&1)
check_output "TYPE nonexistent" "$output"

# Test 4: SET DEFAULT to invalid directory
output=$(echo "SET DEFAULT [.NONEXISTENT_DIR]" | $VMSDCL 2>&1)
check_output "SET DEFAULT invalid" "$output"

# Test 5: SHOW SYSTEM should not show Linux process names
output=$(echo "SHOW SYSTEM" | $VMSDCL 2>&1)
if echo "$output" | grep -qE "(kworker|systemd|/usr/|/sbin/)"; then
    echo "  LEAK in SHOW SYSTEM: Linux process names visible"
    echo "$output" | grep -E "(kworker|systemd|/usr/|/sbin/)" | head -3 | sed 's/^/    /'
    LEAKS_FOUND=$((LEAKS_FOUND + 1))
fi

# Test 6: SHOW DEFAULT should not show Unix paths
output=$(echo "SHOW DEFAULT" | $VMSDCL 2>&1)
check_output "SHOW DEFAULT" "$output"

# Test 7: DIRECTORY should not show Unix paths
output=$(echo "DIRECTORY" | $VMSDCL 2>&1)
check_output "DIRECTORY" "$output"

# Test 8: SEARCH nonexistent file
output=$(printf 'SEARCH NONEXISTENT_FILE.TXT "pattern"\n' | $VMSDCL 2>&1)
check_output "SEARCH nonexistent" "$output"

# Test 9: OPEN nonexistent file
output=$(echo "OPEN/READ F NONEXISTENT_FILE.TXT" | $VMSDCL 2>&1)
check_output "OPEN nonexistent" "$output"

# Test 10: RENAME nonexistent file
output=$(echo "RENAME NONEXISTENT_FILE.TXT NEWNAME.TXT" | $VMSDCL 2>&1)
check_output "RENAME nonexistent" "$output"

# Test 11: SHOW DEVICE must not surface the host Linux mount table (vms-b9f).
#
# The original vms-b9f leak walked /proc/mounts and presented each Linux
# mount as a VMS disk: invented "$1$DGAn:" names, "Mounted" status, and the
# mount-point basename (or the truncated kernel version) as the Volume Label
# -- e.g. "5.15.167.4-MICR" (5.15.167.4-microsoft-standard-WSL2), "DRIVERS"
# (/usr/lib/wsl/drivers), "BINFMT_MISC" (/proc/sys/fs/binfmt_misc). vms-fb9
# rewrote SHOW DEVICE to READ the executive device table instead, so with no
# /dev/vms here it prints nothing at all. This probe re-arms that fix: it can
# only go red if the /proc/mounts fabricator returns, and the fabricator
# needs no executive -- so it WOULD produce rows in exactly this environment.
#
# None of the leak tells below are Unix paths, so the generic UNIX_PATTERNS
# above cannot catch them; they are matched explicitly. All are
# format-independent of the current oracle-pinned 3-column listing (which,
# with no executive, is never emitted here anyway):
#   - the 7-column mount-derived header ("Error  Volume" / "Blocks Count Cnt")
#   - invented "$1$DGAn:" unit names derived from mount points
#   - a "NAME: Mounted" device row printed with no executive present
#   - host-mount volume-label tells: a kernel-version-shaped label, and the
#     WSL / binfmt_misc mount basenames the real capture showed
HOST_MOUNT_TELLS='(Error +Volume|Blocks +Count +Cnt|\$1\$DGA[0-9]|^[A-Z0-9$_]+: +Mounted|[0-9]+\.[0-9]+\.[0-9]+.*(microsoft|MICR|WSL)|binfmt_misc|BINFMT_MISC|/usr/lib/wsl)'
output=$(echo "SHOW DEVICE" | $VMSDCL 2>&1)
check_output "SHOW DEVICE (generic)" "$output"
if echo "$output" | grep -qE "$HOST_MOUNT_TELLS"; then
    echo "  LEAK in SHOW DEVICE: host mount table surfaced as VMS devices"
    echo "$output" | grep -E "$HOST_MOUNT_TELLS" | head -3 | sed 's/^/    /'
    LEAKS_FOUND=$((LEAKS_FOUND + 1))
fi

if [ $LEAKS_FOUND -gt 0 ]; then
    echo "UNIX_LEAK_DETECTED: $LEAKS_FOUND command(s) leaked Unix paths/errors"
    echo "LEAK_CHECK_COMPLETE ($LEAKS_FOUND leaks found)"
    exit 1
fi
echo "LEAK_CHECK_COMPLETE (0 leaks found)"
