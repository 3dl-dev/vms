#!/bin/bash
# TEST: SHOW DEVICE must list only OVMX VMS devices, never the host Linux mount table
# EXPECT: contains:LEAK_CHECK_COMPLETE
# EXPECT_NOT: contains:LEAK_DETECTED:
# EXPECT: contains:DKA0:
# EXPECT: contains:Mounted
# EXPECT: contains:OVMXSYS
# EXPECT: contains:COLUMN_LAYOUT_OK
# EXPECT_NOT: contains:COLUMN_LAYOUT_BROKEN
# EXPECT: contains:DUA0_LAYOUT_OK
# EXPECT_NOT: contains:DUA0_LAYOUT_BROKEN
# EXPECT: contains:FREEBLOCKS_CONSISTENT
# EXPECT_NOT: contains:FREEBLOCKS_MISMATCH
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
#
# vms-b9f R4: those three EXPECT lines are bare substring checks -- a row emitted as
# "DKA0: Mounted OVMXSYS" with no column padding at all would pass all three, so they never
# actually verified the COLUMN LAYOUT was intact (and R3 found it wasn't: Status and Volume
# Label were both one column off from the oracle). The COLUMN_LAYOUT_OK/BROKEN check below
# slices the DKA0: data row at the exact byte offsets pinned live against the oracle (OpenVMS
# VAX 7.3, ~/vax/cluster/vax1, 2026-07-29: `SHOW DEVICE D` on a mounted disk put Status at
# column 24 and Volume Label at column 48) and asserts the actual substrings found there,
# not just that they appear SOMEWHERE in the line.
#
# vms-b9f R4, round 3 rework: the FIRST version of this column check only sliced offsets
# 24 and 48 -- Status and Volume Label, exactly the two fields the review's wording had
# named -- leaving Error Count, Free Blocks, Trans Count and Mnt Cnt unverified. Proven
# blind by injection: shifting Free Blocks and Trans Count off their oracle positions
# (while leaving 24/48 untouched) still passed COLUMN_LAYOUT_OK. It also could not see the
# real R3 defect it existed to catch: Trans Count was landing at column 73, not the
# oracle's 75 (masked because the 3-digit sample used to pin it, $2$DUA0:'s "250", spans
# 73-75 either way -- a second oracle row, $2$DUA1:, has a single-digit Trans Count that
# lands at 75 and disproves 73). The check below now slices EVERY field the row claims:
# Status@24, Error Count digit@45, Volume Label@48, Free Blocks last digit@69 (8-wide,
# right-aligned), Trans Count digit@75 (right-aligned), Mnt Cnt digit@79 (right-aligned) --
# all six pinned live against the oracle on 2026-07-29 (both the $2$DUA0:/$2$DUA1: mounted
# rows and a live MOUNT/DISMOUNT cycle on $2$DUA3: for the not-mounted row below).
#
# The not-mounted row (also R3, round 3): OVMX used to print "Dismounted" with a full row
# of zeroed fields and a stale volume label for a registered-but-unmounted device -- its
# own leak of internal state that isn't a real VMS status. Pinned live by actually driving
# a MOUNT/DISMOUNT cycle on the oracle (`MOUNT/OVERRIDE=IDENTIFICATION DUA3: SCRATCH3` then
# `DISMOUNT DUA3:`), not inferred from the never-mounted rows alone: both a never-mounted
# unit and a just-dismounted one print ONLY "Online" at column 24 and the Error Count digit
# at column 45, then the row ends -- exactly 46 bytes, no Volume Label/Free Blocks/Trans or
# Mnt Count at all. DUA0_LAYOUT_OK/BROKEN below mounts and dismounts DUA0: to produce that
# row and checks it byte-for-byte.
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

# Column-position check (vms-b9f R4, hardened round 3; reworked round 4 for S1).
# Status@24, Error Count@45 and Volume Label@48 are all BEFORE the Free Blocks field and
# their byte offsets never move, so they're still sliced at fixed offsets, pinned live
# against the oracle (~/vax/cluster/vax1, 2026-07-29).
#
# Free Blocks/Trans Count/Mnt Cnt CANNOT be sliced at fixed absolute offsets any more
# (vms-b9f S1, round 4): Free Blocks used to be a hardcoded literal "0" (always exactly
# 8 bytes wide via %8d), so 69/75/79 were constant. It is now a real statvfs()-derived
# free-block count for the device's own backing filesystem (dcl_cmd_show.c), which can
# be (and on this dev host, measurably is -- 10 digits) wider than the oracle-era 8-digit
# assumption; the printf field is a MINIMUM width and does not truncate, so Trans
# Count/Mnt Cnt's absolute column shifts right by however many extra digits Free Blocks
# printed. Hardcoding 69/75/79 here would make a CORRECT, non-fabricated Free Blocks
# implementation fail this gate for no reason other than this host's disk being bigger
# than a VAX-era one -- exactly the failure mode vms-b9f wave 3 hit and left STILL OPEN.
#
# Fixed layout invariants that DO still hold regardless of Free Blocks' width (both
# pinned against dcl_cmd_show.c's format string, which never changed shape, only the
# Free Blocks value itself): Trans Count and Mnt Cnt are always the LAST 10 bytes of a
# mounted row ("     1   1" -- 5 spaces, digit, 3 spaces, digit), and the Free Blocks
# field always starts immediately after Volume Label ends, at byte 62 (48 + 14-wide
# label field). Slice using those relative anchors instead of absolute end-columns.
# Positive cross-check (vms-b9f S1): SHOW DEVICE's Free Blocks and F$GETDVI's FREEBLOCKS
# item must report the IDENTICAL number for the SAME device -- the actual regression this
# round fixes (the branch previously hardcoded SHOW DEVICE's Free Blocks to 0 while
# F$GETDVI reported a real, different-source figure). Query both in ONE process
# invocation, back-to-back -- NOT the earlier standalone "$output" capture, which is a
# SEPARATE DCL.EXE run and can legitimately disagree by a few blocks with a live
# filesystem between two process launches (observed in dev: an 8-block drift with no
# code defect involved). Extract the DKA0: row from THIS SAME run too, so the column
# slice and the F$GETDVI comparison are reading literally the same statvfs() snapshot.
freedvi_run=$(printf 'SHOW DEVICE\nX = F$GETDVI("DKA0","FREEBLOCKS")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1)
dka0_line=$(echo "$freedvi_run" | grep '^DKA0:')
fgetdvi_freeblocks=$(echo "$freedvi_run" | grep -oE 'X = [0-9]+' | head -1 | awk '{print $3}')

status_field="${dka0_line:24:7}"
errcnt_field="${dka0_line:45:1}"
label_field="${dka0_line:48:7}"
dka0_len=${#dka0_line}
trailing10="${dka0_line: -10}"
freeblk_field="${dka0_line:62:$((dka0_len - 10 - 62))}"
freeblk_trimmed=$(echo "$freeblk_field" | tr -d ' ')

if [ "$status_field" = "Mounted" ] && [ "$errcnt_field" = "0" ] && \
   [ "$label_field" = "OVMXSYS" ] && [ "$trailing10" = "     1   1" ] && \
   [ -n "$freeblk_trimmed" ] && [ "$freeblk_trimmed" = "$fgetdvi_freeblocks" ]; then
    echo "COLUMN_LAYOUT_OK (Status@24='$status_field' ErrCnt@45='$errcnt_field' Label@48='$label_field' FreeBlk='$freeblk_trimmed' Trailing10='$trailing10')"
    echo "FREEBLOCKS_CONSISTENT (SHOW DEVICE='$freeblk_trimmed' F\$GETDVI='$fgetdvi_freeblocks')"
else
    echo "COLUMN_LAYOUT_BROKEN (Status@24='$status_field' ErrCnt@45='$errcnt_field' Label@48='$label_field' FreeBlk='$freeblk_trimmed' Trailing10='$trailing10', DKA0: line='$dka0_line')"
    echo "FREEBLOCKS_MISMATCH (SHOW DEVICE='$freeblk_trimmed' F\$GETDVI='$fgetdvi_freeblocks')"
fi

# Not-mounted row check (vms-b9f R3, round 3): a registered-but-unmounted device must
# print ONLY "Online" at column 24 and the Error Count digit at column 45, then end --
# exactly 46 bytes, no Volume Label/Free Blocks/Trans/Mnt Count -- pinned live by driving
# an actual MOUNT/DISMOUNT cycle on the oracle (~/vax/cluster/vax1, 2026-07-29). Drive the
# same cycle here on DUA0: (a fresh, real MOUNT/DISMOUNT round-trip, not a canned string).
mount_dismount_output=$(printf 'MOUNT DUA0: LAYOUTCHK\nSHOW DEVICE\nDISMOUNT DUA0:\nSHOW DEVICE\n' | $VMSDCL 2>&1)
dua0_line=$(echo "$mount_dismount_output" | grep '^DUA0:' | tail -1)
dua0_len=${#dua0_line}
dua0_status="${dua0_line:24:6}"
dua0_errcnt="${dua0_line:45:1}"
if [ "$dua0_len" -eq 46 ] && [ "$dua0_status" = "Online" ] && [ "$dua0_errcnt" = "0" ]; then
    echo "DUA0_LAYOUT_OK (len=$dua0_len Status@24='$dua0_status' ErrCnt@45='$dua0_errcnt')"
else
    echo "DUA0_LAYOUT_BROKEN (len=$dua0_len Status@24='$dua0_status' ErrCnt@45='$dua0_errcnt', DUA0: line='$dua0_line')"
fi
