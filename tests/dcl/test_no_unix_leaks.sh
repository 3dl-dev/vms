#!/bin/bash
# TEST: Error messages and output must not leak Unix paths or terminology
# EXPECT: contains:LEAK_CHECK_COMPLETE
# EXPECT_NOT: contains:UNIX_LEAK_DETECTED
#
# HARD GATE (vms-b9f C5 rework): this suite used to have its EXPECT_NOT commented out as
# "FUTURE_EXPECT_NOT" and could never fail -- the whole Unix-leak class was ungated. It is
# now a real EXPECT_NOT: if any check below finds a leak, the script prints
# "UNIX_LEAK_DETECTED" and the harness fails. Proven to actually catch a regression by
# temporarily reintroducing the vms-b9f F$DEVICE()/proc-mounts leak in dcl_lexical.c and
# re-running this test -- it went from PASS to FAIL (see vms-b9f return notes); reverted
# before commit.
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

# Test 11: F$DEVICE() must not enumerate the host's real Linux block devices as VMS device
# names (vms-b9f C4 sibling leak: populate_device_list() in dcl_lexical.c used to read
# /proc/mounts directly and turn e.g. /dev/sda into "_SDA:", independently of SHOW DEVICE's
# own leak through the SAME root cause). Host-independent: derive this host's actual block
# device basenames from /proc/mounts at run time, walk F$DEVICE("*") to enumerate everything
# OVMX reports, and assert none of the host's basenames appear.
devlist_input=""
for _i in $(seq 1 20); do
    devlist_input="${devlist_input}D = F\$DEVICE(\"*\")
SHOW SYMBOL D
"
done
output=$(printf '%s' "$devlist_input" | $VMSDCL 2>&1)
if [ -r /proc/mounts ]; then
    while read -r dev _mnt fstype _rest; do
        case "$fstype" in
            proc|sysfs|devtmpfs|tmpfs|cgroup|cgroup2|devpts|mqueue|hugetlbfs|pstore|securityfs|debugfs|bpf|tracefs)
                continue ;;
        esac
        case "$dev" in
            /dev/*) ;;
            *) continue ;;
        esac
        base=$(basename "$dev" | tr '[:lower:]' '[:upper:]')
        [ -z "$base" ] && continue
        if echo "$output" | grep -qF "_${base}:"; then
            echo "  LEAK in F\$DEVICE(): host block device '$dev' appeared as _${base}:"
            LEAKS_FOUND=$((LEAKS_FOUND + 1))
        fi
    done < /proc/mounts
fi

# Test 12: MOUNT-ing one of OVMX's fixed scratch devices (DUA0:/DJA0:, dcl_builtin.c
# known_devices[]) must not leak the CALLING PROCESS'S current working directory into the
# mounted device's DIRECTORY listing (vms-b9f, round 3 finding: cmd_mount() used
# getcwd() as the device's backing path, so DIRECTORY DUA0:[000000] dumped whatever host
# directory the shell happened to be in when MOUNT ran -- observed live: run from a temp
# directory containing a marker file, DIRECTORY DUA0:[000000] listed that marker file).
# This is a NEGATIVE/POSITIVE pair per the standing rule: the negative check below (no
# marker file) is worthless on its own -- a DIRECTORY command gutted to print nothing
# would also show no marker -- so it is paired with a positive assertion that DIRECTORY
# still produces real output (a "Directory" header and a "Total of" summary line).
mount_leak_dir=$(mktemp -d)
mount_leak_marker="OVMX_MOUNT_HOST_LEAK_MARKER_$$"
touch "$mount_leak_dir/$mount_leak_marker"
output=$(cd "$mount_leak_dir" && printf 'MOUNT DUA0: LEAKCHK\nDIRECTORY DUA0:[000000]\nDISMOUNT DUA0:\n' | $VMSDCL 2>&1)
rm -rf "$mount_leak_dir"
if echo "$output" | grep -qF "$mount_leak_marker"; then
    echo "  LEAK in MOUNT/DIRECTORY: host cwd marker file '$mount_leak_marker' appeared in DIRECTORY DUA0:[000000]"
    LEAKS_FOUND=$((LEAKS_FOUND + 1))
fi
if ! echo "$output" | grep -q "^Directory " || ! echo "$output" | grep -q "^Total of "; then
    echo "  LEAK in MOUNT/DIRECTORY: DIRECTORY DUA0:[000000] did not produce a real directory listing (gutted, not fixed)"
    LEAKS_FOUND=$((LEAKS_FOUND + 1))
fi

if [ $LEAKS_FOUND -gt 0 ]; then
    echo "UNIX_LEAK_DETECTED: $LEAKS_FOUND commands leaked Unix paths/errors"
fi
echo "LEAK_CHECK_COMPLETE ($LEAKS_FOUND leaks found)"
