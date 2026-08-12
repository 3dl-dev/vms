#!/bin/bash
# TEST: SHOW MEMORY reports the REAL host memory (converted to 512-byte VMS
# pages), not a hardcoded/faked figure
# EXPECT: contains:MEMORY_CROSSCHECK_OK
# EXPECT_NOT: contains:MEMORY_CROSSCHECK_FAIL
# EXPECT_NOT: contains:MemTotal
#
# vms-fe21: re-armed. The old test only asserted that ONE of the words
# Memory/Pages/pagelets/bytes appeared -- true of essentially any output,
# including a fixed dummy string, so it could never fail regardless of
# whether SHOW MEMORY read real data. src/vmsdcl/dcl_cmd_show.c's
# cmd_show_memory() DOES read /proc/meminfo and converts KB to VMS's
# 512-byte pages (page_size_bytes = 512; total_pages = total_kb*1024/512),
# so there is a genuine, checkable, product-independent invariant here: SHOW
# MEMORY's "Total Physical Pages" figure, converted back to KB, must match
# /proc/meminfo's MemTotal read independently BY THIS SCRIPT.
#
# Tolerance: 2% -- generous enough to absorb the few seconds of clock skew
# between the two reads (memory pressure can shift MemTotal-adjacent fields,
# though MemTotal itself is static; the margin is for robustness, not
# because MemTotal is expected to move) while still catching a hardcoded or
# stubbed value, which would be off by an arbitrary, uncorrelated amount.
VMSDCL="${VMSDCL:-vmsdcl}"

output=$(echo "SHOW MEMORY" | $VMSDCL 2>&1)
echo "$output"

real_total_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
shown_pages=$(echo "$output" | grep "Total Physical Pages" | awk '{print $4}')

if [ -z "$real_total_kb" ] || [ -z "$shown_pages" ]; then
    echo "MEMORY_CROSSCHECK_FAIL (could not extract a figure to compare: real_total_kb='$real_total_kb' shown_pages='$shown_pages')"
    exit 1
fi

# shown_pages are 512-byte pages -> KB
shown_kb=$(( shown_pages / 2 ))

diff=$(( shown_kb > real_total_kb ? shown_kb - real_total_kb : real_total_kb - shown_kb ))
# 2% tolerance
tolerance=$(( real_total_kb / 50 ))

if [ "$diff" -le "$tolerance" ]; then
    echo "MEMORY_CROSSCHECK_OK (shown ${shown_kb}KB vs /proc/meminfo ${real_total_kb}KB, diff ${diff}KB, tolerance ${tolerance}KB)"
else
    echo "MEMORY_CROSSCHECK_FAIL (shown ${shown_kb}KB vs /proc/meminfo ${real_total_kb}KB, diff ${diff}KB exceeds tolerance ${tolerance}KB)"
    exit 1
fi
