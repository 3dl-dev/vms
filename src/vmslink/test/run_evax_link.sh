#!/bin/sh
# run_evax_link.sh — integration test for LINK.EXE's EVAX/Alpha object path
# (bead vms-cbe, slices 3+4). Builds the real LINK.EXE (host tool), links the
# two-object EVAX fixture (link_main.obj + link_helper.obj, produced by
# binutils-2.43 alpha-dec-vms-as), and verifies the output image byte-for-byte:
# psects placed as sections, every relocation applied to the correct S+A value
# (including the Alpha LINKAGE pair), and a .vms$xfer transfer vector present.
#
# Architecture-independent: LINK.EXE's EVAX path and the verifier are pure byte
# manipulation, so this runs on the CI host with no Alpha toolchain. The .obj
# fixtures are checked in; regenerate with the alpha-dec-vms binutils if the .s
# files change (see the header of each .s). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-link-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
# evax_read.c is #included by link.c (single TU) — do NOT also compile it here,
# or its non-static entry points would be doubly defined at link time.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the EVAX image verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -o "$WORK/evax_link_verify" "$HERE/evax_link_verify.c"

echo
echo "== link the two-object EVAX fixture =="
"$WORK/LINK.EXE" --transfer MAIN_PROC -o "$WORK/link_sample.exe" \
    "$FIX/link_main.obj" "$FIX/link_helper.obj"

echo
echo "== verify the linked image (placement + relocations + .vms\$xfer) =="
"$WORK/evax_link_verify" "$WORK/link_sample.exe"
