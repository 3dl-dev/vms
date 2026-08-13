#!/bin/sh
# run_olb_native.sh — LIBRARIAN.EXE + .OLB + LINK.EXE END-TO-END, ACTIVATED
# (bead vms-ca9, self-host spine #3). Proves the object-library toolchain path
# all the way to "activates and runs":
#
#   1. LIBRARIAN.EXE builds an .OLB (ar container) from >=2 real .OBJ members.
#   2. LINK.EXE --executable resolves an undefined symbol by SELECTIVELY pulling
#      the one member that defines it (leaving the unreferenced member out), then
#      binds the C-RTL producer graph and synthesizes crt0 — a real program image
#      (PT_INTERP=IMGACT.EXE).
#   3. IMGACT.EXE ACTIVATES the produced image and it RUNS: main calls the pulled
#      library function (mul3(14) == 42) and exits 0 iff it computed 42 — so exit
#      0 PROVES the right member was pulled AND the image runs VMS-native (no ld).
#
# Bootstrap LINK.EXE / LIBRARIAN.EXE are ordinary host tools that BUILD the image
# (CLAUDE.md Rule 9: a build step is not an activation proof). What is proven
# native is the OUTPUT: the IMGACT-activated PROG.EXE.
#
# OLB_EXPECT (default 1, BLOCKING): a failure of the link or activation is a
# regression. Set OLB_EXPECT=0 to soften those steps to a SKIP (exit 2) while
# iterating; every other assertion stays a hard FAIL.
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
WORK=${WORK:-/tmp/olb-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64; OBJ_ARCHFLAG="-mno-outline-atomics" ;;
    x86_64|amd64)  ARCH=x86_64;  OBJ_ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "SKIP-FAIL: run_olb_native.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
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
# five OVMX shareables).
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build LIBRARIAN.EXE (host tool, same as the bootstrap LINK.EXE) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -I"$LIBVMS_INC" \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -o "$WORK/LIBRARIAN.EXE" "$LINK_DIR/librarian.c"
[ -x "$WORK/LIBRARIAN.EXE" ] || { echo "FAIL: could not build LIBRARIAN.EXE"; exit 1; }

echo
echo "== compile library members + main ($ARCH) =="
OCF="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $OBJ_ARCHFLAG"
printf 'int mul3(int x) { return x * 3; }\n'          > "$WORK/mul3.c"
printf 'int never_called(void) { return -1; }\n'      > "$WORK/unused.c"
printf 'int mul3(int);\nint main(void) { return mul3(14) == 42 ? 0 : 1; }\n' > "$WORK/main.c"
$CC $OCF -c -o "$WORK/MUL3.OBJ"   "$WORK/mul3.c"
$CC $OCF -c -o "$WORK/UNUSED.OBJ" "$WORK/unused.c"
$CC $OCF -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -c -o "$WORK/main.o" "$WORK/main.c"

echo
echo "== LIBRARIAN /CREATE MATH.OLB from 2 members =="
"$WORK/LIBRARIAN.EXE" /CREATE "$WORK/MATH.OLB" "$WORK/MUL3.OBJ" "$WORK/UNUSED.OBJ"
command -v ar >/dev/null 2>&1 && { echo "-- ar oracle --"; ar t "$WORK/MATH.OLB"; }

echo
echo "== LINK.EXE --executable: main.o + MATH.OLB (selective pull) -> PROG.EXE =="
set +e
"$WORK/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$WORK/PROG.EXE" "$WORK/main.o" "$WORK/MATH.OLB" > "$WORK/link.out" 2>&1
LRC=$?
set -e
echo "-- LINK exit=$LRC; output: --"; sed 's/^/   /' "$WORK/link.out"
if [ "$LRC" -ne 0 ]; then
    if [ "${OLB_EXPECT:-1}" = "1" ]; then echo "FAIL: LINK with .OLB failed (regression)"; exit 1; fi
    echo "SKIP (OLB_EXPECT=0): LINK with .OLB failed but the assertion is disabled."; exit 2
fi
# Selective: exactly MUL3 pulled (1 of 2), UNUSED left out.
grep -q "1 of 2 member" "$WORK/link.out" || { echo "FAIL: expected '1 of 2 members pulled (selective)'"; exit 1; }
[ -f "$WORK/PROG.EXE" ] || { echo "FAIL: LINK produced no PROG.EXE"; exit 1; }
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
    if [ "${OLB_EXPECT:-1}" = "1" ]; then
        echo "FAIL: activated PROG.EXE exit $PRC (expected 0 — mul3 not pulled or wrong result)"; exit 1
    fi
    echo "SKIP (OLB_EXPECT=0): activation failed but the assertion is disabled."; exit 2
fi

echo
echo "================================================================================"
echo "MILESTONE (vms-ca9, self-host spine #3): LIBRARIAN.EXE builds an .OLB object"
echo "library, LINK.EXE resolves an undefined symbol by SELECTIVELY pulling the member"
echo "that defines it (1 of 2), and the produced image IMGACT-activates and RUNS ($ARCH)"
echo "— mul3(14)==42, exit 0, VMS-native (no ld / no ld.so)."
echo "================================================================================"
