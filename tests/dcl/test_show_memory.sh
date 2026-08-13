#!/bin/bash
# TEST: SHOW MEMORY prints the authentic OpenVMS multi-section report from the
# system's REAL memory state, and does NOT fabricate the modified page list
# from Linux buffers+cached.
# EXPECT: contains:MEMORY_CROSSCHECK_OK
# EXPECT: contains:Physical Memory Usage (pages):
# EXPECT: contains:Main Memory
# EXPECT_NOT: contains:MEMORY_CROSSCHECK_FAIL
# EXPECT_NOT: contains:MemTotal
# EXPECT_NOT: contains:Modified List Size
#
# vms-31a: the report format changed from an invented one-section vertical
# list to the OpenVMS DCL Dictionary layout -- a "Physical Memory Usage
# (pages):" heading with Total/Free/In Use/Modified columns and a "Main
# Memory (NN.NNMb)" data row. The old body printed a "Modified List Size"
# line whose value was Linux Buffers+Cached (clean file cache), a masquerade
# that is now deleted; EXPECT_NOT above pins that it stays gone.
#
# vms-fe21 (still enforced): the Total column, converted back to KiB, must
# match /proc/meminfo MemTotal read independently BY THIS SCRIPT -- a
# genuine, product-independent invariant that a hardcoded/stubbed value
# would violate by an arbitrary, uncorrelated amount.
#
# Tolerance: 2% -- generous enough to absorb clock skew between the two reads
# while still catching a hardcoded or stubbed value.
VMSDCL="${VMSDCL:-vmsdcl}"

output=$(echo "SHOW MEMORY" | $VMSDCL 2>&1)
echo "$output"

real_total_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
# "  Main Memory (64.00Mb)   <Total> <Free> <In Use> <Modified>"
#   $1=Main $2=Memory $3=(NN.NNMb) $4=Total ...
shown_pages=$(echo "$output" | grep "Main Memory" | awk '{print $4}')

if [ -z "$real_total_kb" ] || [ -z "$shown_pages" ]; then
    echo "MEMORY_CROSSCHECK_FAIL (could not extract a figure to compare: real_total_kb='$real_total_kb' shown_pages='$shown_pages')"
    exit 1
fi

# shown_pages are 512-byte pages -> KiB
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
