#!/bin/sh
# run_olb_symvec_native.sh — LINK.EXE --shareable ROOTED AT --symbol-vector,
# selective .OLB pull, ACTIVATED (Rung 1 of the VMS-native shareable-build
# migration, rd vms-bf8 / epic vms-a90; design-vms-native-shareable-build.md
# Part C, C.4.1). This is the sibling of run_olb_native.sh (vms-ca9), which
# rooted the selective pull at an explicit main.o TU list. Here the ONLY roots
# are the SYMBOL_VECTOR universals — the VMS way a /SHAREABLE is built, with no
# hand-listed object TUs:
#
#   1. LIBRARIAN.EXE builds MATH.OLB from THREE .OBJ members:
#        MUL3.OBJ    defines mul3(), which references mul_helper()  (transitive)
#        HELPER.OBJ  defines mul_helper()
#        UNUSED.OBJ  defines never_called()                        (unreferenced)
#   2. LINK.EXE --shareable --symbol-vector "mul3=PROCEDURE" MATH.OLB, with NO
#      explicit object on the command line. Before this rung LINK seeded its
#      unresolved set only from root objects, so a symbol-vector-only /SHAREABLE
#      pulled ZERO members and failed. Now the universal `mul3` seeds the pull:
#      LINK selectively pulls MUL3.OBJ (defines the universal) and then, to a
#      fixpoint, HELPER.OBJ (mul3's transitive reference) — 2 of 3 — and leaves
#      UNUSED.OBJ out. The result MATH$SHR.EXE carries a symbol vector.
#   3. IMGACT.EXE ACTIVATES a consumer PROG.EXE linked against MATH$SHR.EXE: it
#      calls mul3(14) and exits 0 iff the result is 42 — so exit 0 PROVES the
#      right members were pulled AND the shareable binds and runs VMS-native.
#
# Bootstrap LINK.EXE / LIBRARIAN.EXE are ordinary host tools that BUILD the image
# (CLAUDE.md Rule 9: a build step is not an activation proof). What is proven
# native is the OUTPUT: the IMGACT-activated PROG.EXE that reaches into MATH$SHR.
#
# SV_EXPECT (default 1, BLOCKING): a failure of the shareable link, the selective
# count, or the activation is a regression. Set SV_EXPECT=0 to soften those steps
# to a SKIP (exit 2) while iterating; every other assertion stays a hard FAIL.
#
# Runs on aarch64 (native or QEMU) OR x86_64 (native); musl Alpine container only
# (CLAUDE.md test loop). Needs root to create /vms. Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
RMS_INC="$VMSRMS_DIR/include"
WORK=${WORK:-/tmp/olb-symvec-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64; OBJ_ARCHFLAG="-mno-outline-atomics" ;;
    x86_64|amd64)  ARCH=x86_64;  OBJ_ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "SKIP-FAIL: run_olb_symvec_native.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
esac
export ARCH

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need a $ARCH musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

# Shared producer-graph build (IMGACT.EXE + bootstrap LINK.EXE + DECC$SHR + the
# five OVMX shareables) — the consumer executable in step 3 needs the C-RTL
# producer + crt0 + IMGACT to activate.
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build LIBRARIAN.EXE (host tool, same as the bootstrap LINK.EXE) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -I"$LIBVMS_INC" \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -o "$WORK/LIBRARIAN.EXE" "$LINK_DIR/librarian.c"
[ -x "$WORK/LIBRARIAN.EXE" ] || { echo "FAIL: could not build LIBRARIAN.EXE"; exit 1; }

echo
echo "== compile library members ($ARCH): mul3 -> mul_helper (transitive), unused =="
OCF="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $OBJ_ARCHFLAG"
# mul3 references mul_helper (defined in a DIFFERENT member) — proves the
# fixpoint iteration pulls the transitive dependency too, not just the universal.
printf 'int mul_helper(int);\nint mul3(int x) { return mul_helper(x); }\n' > "$WORK/mul3.c"
printf 'int mul_helper(int x) { return x * 3; }\n'                          > "$WORK/helper.c"
printf 'int never_called(void) { return -1; }\n'                           > "$WORK/unused.c"
$CC $OCF -c -o "$WORK/MUL3.OBJ"   "$WORK/mul3.c"
$CC $OCF -c -o "$WORK/HELPER.OBJ" "$WORK/helper.c"
$CC $OCF -c -o "$WORK/UNUSED.OBJ" "$WORK/unused.c"

echo
echo "== LIBRARIAN /CREATE MATH.OLB from 3 members =="
"$WORK/LIBRARIAN.EXE" /CREATE "$WORK/MATH.OLB" \
    "$WORK/MUL3.OBJ" "$WORK/HELPER.OBJ" "$WORK/UNUSED.OBJ"
command -v ar >/dev/null 2>&1 && { echo "-- ar oracle --"; ar t "$WORK/MATH.OLB"; }

echo
echo "== LINK.EXE --shareable ROOTED AT --symbol-vector (NO object TU list) =="
echo "   --symbol-vector mul3=PROCEDURE + MATH.OLB -> MATH\$SHR.EXE (selective)"
set +e
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "mul3=PROCEDURE" \
    --gsmatch "LEQUAL,1,0" \
    -o "$SYSLIB/MATH\$SHR.EXE" "$WORK/MATH.OLB" > "$WORK/link.out" 2>&1
LRC=$?
set -e
echo "-- LINK exit=$LRC; output: --"; sed 's/^/   /' "$WORK/link.out"
if [ "$LRC" -ne 0 ]; then
    if [ "${SV_EXPECT:-1}" = "1" ]; then
        echo "FAIL: symbol-vector-rooted /SHAREABLE link from .OLB alone failed (Rung 1 regression)"; exit 1
    fi
    echo "SKIP (SV_EXPECT=0): symbol-vector-rooted shareable link failed but the assertion is disabled."; exit 2
fi
# Selective + transitive: exactly MUL3 (defines the universal) and HELPER (mul3's
# transitive reference) pulled — 2 of 3 — with UNUSED left out. Before Rung 1 the
# count would have been "0 of 3" (empty root set) and the link would have failed
# on an undefined `mul3` universal.
grep -q "2 of 3 member" "$WORK/link.out" || {
    echo "FAIL: expected '2 of 3 members pulled (selective)' — symbol vector must root the pull of MUL3 + its transitive HELPER, and NOT UNUSED"; exit 1; }
[ -f "$SYSLIB/MATH\$SHR.EXE" ] || { echo "FAIL: LINK produced no MATH\$SHR.EXE"; exit 1; }
readelf -SW "$SYSLIB/MATH\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: MATH\$SHR.EXE has no symbol vector"; exit 1; }

echo
echo "== compile + LINK consumer PROG.EXE against MATH\$SHR.EXE, then ACTIVATE =="
printf 'int mul3(int);\nint main(void) { return mul3(14) == 42 ? 0 : 1; }\n' > "$WORK/main.c"
$CC $OCF -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -c -o "$WORK/main.o" "$WORK/main.c"
set +e
"$WORK/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    --use "$SYSLIB/MATH\$SHR.EXE" \
    -o "$WORK/PROG.EXE" "$WORK/main.o" > "$WORK/link_prog.out" 2>&1
PLRC=$?
set -e
echo "-- consumer LINK exit=$PLRC; output: --"; sed 's/^/   /' "$WORK/link_prog.out"
if [ "$PLRC" -ne 0 ]; then
    if [ "${SV_EXPECT:-1}" = "1" ]; then echo "FAIL: consumer link against MATH\$SHR.EXE failed (regression)"; exit 1; fi
    echo "SKIP (SV_EXPECT=0): consumer link failed but the assertion is disabled."; exit 2
fi
[ -f "$WORK/PROG.EXE" ] || { echo "FAIL: consumer LINK produced no PROG.EXE"; exit 1; }
readelf -lW "$WORK/PROG.EXE" | grep -q 'INTERP' || { echo "FAIL: PROG.EXE has no PT_INTERP (IMGACT)"; exit 1; }

echo
echo "== ACTIVATE PROG.EXE through IMGACT — it must run and exit 0 (mul3(14)==42) =="
chmod +x "$WORK/PROG.EXE"
set +e
"$WORK/PROG.EXE" > "$WORK/prog.out" 2>&1
PRC=$?
set -e
echo "-- PROG.EXE exit=$PRC; output: --"; sed 's/^/   /' "$WORK/prog.out"
if [ "$PRC" -ne 0 ]; then
    if [ "${SV_EXPECT:-1}" = "1" ]; then
        echo "FAIL: activated PROG.EXE exit $PRC (expected 0 — wrong members pulled or shareable did not bind)"; exit 1
    fi
    echo "SKIP (SV_EXPECT=0): activation failed but the assertion is disabled."; exit 2
fi

echo
echo "================================================================================"
echo "MILESTONE (Rung 1, vms-bf8): LINK.EXE --shareable roots its selective .OLB search"
echo "at the --symbol-vector universals. A /SHAREABLE built from MATH.OLB with NO explicit"
echo "object TU list (the VMS way) pulls exactly the module that defines the universal mul3"
echo "PLUS its transitive HELPER (2 of 3), leaves UNUSED out, carries a symbol vector, and"
echo "IMGACT-activates through a consumer — mul3(14)==42, exit 0, VMS-native ($ARCH)."
echo "================================================================================"
