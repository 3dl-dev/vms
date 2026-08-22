#!/bin/sh
# run_evax_read.sh — unit test for the EVAX (Alpha/VMS) object reader front end
# (bead vms-cbe). Builds evax_read.c + the driver and parses a real EVAX object
# (evax-fixtures/sample.obj, produced by binutils-2.43 alpha-dec-vms-as from
# sample.s) asserting the sections + symbols match alpha-dec-vms-objdump.
#
# Pure byte parsing — arch-independent, no toolchain needed at test time (the
# fixture is checked in; regenerate with the alpha-dec-vms binutils if the .s
# changes). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/evax-read-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build evax_read + test =="
$CC -std=gnu11 -O2 -Wall -Wextra -o "$WORK/evax_read_test" \
    "$SRC/evax_read.c" "$HERE/evax_read_test.c"

echo
echo "== parse the real EVAX fixture =="
"$WORK/evax_read_test" "$HERE/evax-fixtures/sample.obj"
