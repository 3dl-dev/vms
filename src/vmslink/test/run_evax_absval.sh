#!/bin/sh
# run_evax_absval.sh — end-to-end test for an ABSOLUTE global symbol (a VMS
# "globalvalue") defined in an EVAX object and referenced across objects, in
# LINK.EXE's EVAX/Alpha path (bead vms-1bc). This is the concrete blocker to
# linking a real OpenVMS GCC-port main() object: the port emits
# `__gcc_main_flags = <flags>` (absolute; its ADDRESS is the flags word) and its
# crt0 reads `(unsigned __int64)&__gcc_main_flags`.
#
# Builds the real LINK.EXE (host tool) + a verifier, links the two-object EVAX
# fixture (absval_def.obj defines the absolute global; absval_ref.obj references
# it plus a normal psect-relative global), and asserts:
#   - the reader flagged __gcc_main_flags absolute (DEF set, REL clear), value 3;
#   - the linked image folds `&__gcc_main_flags` to the absolute constant 3
#     (no psect base, no load-bias), while `&REFUSER` still resolves to its
#     biased psect-relative image address (no regression).
#
# Architecture-independent: LINK.EXE's EVAX path, the reader, and the verifier
# are pure byte manipulation, so this runs on the CI host with no Alpha
# toolchain. The .obj fixtures are checked in; regenerate with alpha-dec-vms-as
# if the .s files change (see each .s header). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-absval-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
# evax_read.c is #included by link.c (single TU) — do NOT also compile it here,
# or its non-static entry points would be doubly defined at link time.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the absolute-globalvalue verifier (links evax_read.c) =="
$CC -std=gnu11 -O2 -Wall -Wextra \
    -o "$WORK/evax_absval_verify" "$HERE/evax_absval_verify.c" "$SRC/evax_read.c"

echo
echo "== link the two-object EVAX fixture (absolute def + cross-object ref) =="
"$WORK/LINK.EXE" --transfer MAIN -o "$WORK/absval.exe" \
    "$FIX/absval_def.obj" "$FIX/absval_ref.obj"

echo
echo "== verify: absolute fold + normal-symbol no-regression =="
"$WORK/evax_absval_verify" "$WORK/absval.exe" \
    "$FIX/absval_def.obj" "$FIX/absval_ref.obj"
