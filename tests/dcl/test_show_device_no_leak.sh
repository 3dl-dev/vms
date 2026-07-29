#!/bin/bash
# TEST: SHOW DEVICE must list only OVMX VMS devices, never the host Linux mount table
# EXPECT: contains:LEAK_CHECK_COMPLETE
# EXPECT_NOT: contains:LEAK_DETECTED:
# EXPECT: contains:DKA0:
# EXPECT: contains:Mounted
# EXPECT: contains:OVMXSYS
#
# This is a HARD gate (unlike the informational tests/dcl/test_no_unix_leaks.sh): before the
# vms-b9f fix, this test FAILS the harness (EXPECT_NOT violated), not just logs a finding.
#
# POSITIVE assertions (vms-b9f C2 rework): the first fix attempt made the negative checks
# below pass by emptying SHOW DEVICE down to its two header lines -- gutted, not fixed. The
# three EXPECT lines above are positive: OVMX's boot-registered system disk (DKA0: --
# registered at src/vmsdcl/dcl_main.c setup_session(), see also dcl_main.c:347) must actually
# appear, shown as "Mounted" with its OVMXSYS volume label (the existing OVMX system-device
# convention already used by F$GETDVI's VOLNAM item, dcl_lexical.c). A SHOW DEVICE that prints
# only headers now fails this test.
# Root cause (vms-b9f / INV-4, docs/design-authenticity-roadmap.md §2.2): cmd_show_device()
# used to fopen("/proc/mounts") and print every host mount as a synthetic "$1$DGAn:" VMS
# disk, with the mount-point basename (uppercased) as the Volume Label. On a WSL host this
# printed the Linux kernel version ("5.15.167.4-MICR") and /usr/lib/wsl/drivers ("DRIVERS")
# as if they were VMS volumes -- a direct host-leak on a first-two-minutes command.
#
# This test is host-independent: it reads the CURRENT host's /proc/mounts at run time and
# asserts none of ITS mount-point basenames appear in SHOW DEVICE output, rather than
# hardcoding WSL-specific strings. It also checks for the "$1$DGA" naming pattern, which the
# leaked code path used exclusively -- a fresh SHOW DEVICE (nothing MOUNTed) must never
# produce it.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

LEAKS_FOUND=0

output=$(echo "SHOW DEVICE" | $VMSDCL 2>&1)
echo "$output"

# Marker of the old leak path: synthetic device names in the $1$DGAn: pattern were only
# ever produced by the /proc/mounts enumeration loop, never by MOUNT or the boot sequence.
if echo "$output" | grep -q '\$1\$DGA'; then
    echo "LEAK_DETECTED: SHOW DEVICE printed \$1\$DGAn: synthetic host-mount devices"
    echo "$output" | grep '\$1\$DGA' | head -5 | sed 's/^/    /'
    LEAKS_FOUND=$((LEAKS_FOUND + 1))
fi

# Cross-check against this host's actual mount table: no real mount-point basename should
# ever surface as a VMS Volume Label. Skip pseudo-filesystems the same way the (fixed) code
# should never have looked at in the first place.
if [ -r /proc/mounts ]; then
    while read -r _dev mntpt fstype _rest; do
        case "$fstype" in
            proc|sysfs|devtmpfs|tmpfs|cgroup|cgroup2|devpts|mqueue|hugetlbfs|pstore|securityfs|debugfs|bpf|tracefs)
                continue ;;
        esac
        [ "$mntpt" = "/" ] && continue
        base=$(basename "$mntpt" | tr '[:lower:]' '[:upper:]')
        [ -z "$base" ] && continue
        if echo "$output" | grep -qF "$base"; then
            echo "LEAK_DETECTED: host mount point '$mntpt' (label '$base') appeared in SHOW DEVICE"
            LEAKS_FOUND=$((LEAKS_FOUND + 1))
        fi
    done < /proc/mounts
fi

echo "LEAK_CHECK_COMPLETE ($LEAKS_FOUND leaks found)"
