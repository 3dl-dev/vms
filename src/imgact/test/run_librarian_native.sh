#!/bin/sh
# run_librarian_native.sh — self-host S3 spine proof (bead vms-1c2, epic
# vms-59a): LIBRARIAN.EXE runs AS an OVMX image. Builds the VMS-native producer
# graph + IMGACT.EXE, links librarian.c (+ the reused RMS I/O shim) into a
# NATIVE LIBRARIAN.EXE image (mk_librarian.sh: freestanding-musl,
# -DOVMX_OLB_RMS_IO, LINK.EXE --executable --use {6 producers},
# PT_INTERP=IMGACT.EXE), then ACTIVATES that native LIBRARIAN.EXE through
# IMGACT.EXE and has it /CREATE a real .OLB from two .OBJs — reading each .OBJ
# via RMS (sys$open/$get) and writing the .OLB via RMS (sys$create/$put) — with
# NO host LIBRARIAN anywhere in the create path. It then proves the .OLB is real
# three independent ways: a stock `ar t` oracle lists both members; the native
# LIBRARIAN.EXE /LIST reads the .OLB back via RMS; and a bootstrap LINK.EXE
# consumes it into a runnable image that activates and returns the right answer
# (LINK whole-archives the GST-less .OLB; selective 1-of-N pull is covered
# separately by librarian-olb-native). Mirrors run_link_native.sh.
#
# TWO ROLES (the anti-LARP crux, mirroring run_link_native.sh): a BOOTSTRAP
# LINK.EXE (an ordinary host tool, $WORK/LINK.EXE) BUILDS the native
# LIBRARIAN.EXE image and (separately) consumes the produced .OLB. What is
# PROVEN native here is LIBRARIAN.EXE itself: the IMGACT-activated
# $SYSEXE/LIBRARIAN.EXE does the real .OBJ->.OLB create through RMS. No host
# LIBRARIAN touches the .OLB that this proof builds.
#
# LIBRARIAN_EXPECT (default 1, BLOCKING — mirrors LINK_EXPECT): a failure of the
# native LIBRARIAN.EXE build or the activated /CREATE is a real regression. Set
# LIBRARIAN_EXPECT=0 to soften ONLY those steps to a SKIP (exit 2) while
# iterating; every other assertion stays a hard FAIL.
#
# Runs on aarch64 or x86_64 (librarian.c + ovmx_olb.h are arch-agnostic and
# IMGACT/the graph build both arches). musl Alpine container only. Needs root to
# create /vms.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
WORK=${WORK:-/tmp/librarian-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64)  ARCH=x86_64 ;;
    *) echo "SKIP-FAIL: run_librarian_native.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
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
# five OVMX shareables) — the same builder run_link_native.sh uses.
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build LIBRARIAN.EXE VMS-native (mk_librarian.sh: librarian.c + RMS shim, LINK.EXE --executable) =="
set +e
CC="$CC" ARCH="$ARCH" WORK="$WORK/mk-librarian" sh "$LINK_DIR/mk_librarian.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/LIBRARIAN.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SRC" 2>"$WORK/librarian-build.err"
BRC=$?
set -e
echo "-- mk_librarian.sh exit=$BRC; message: --"
tail -6 "$WORK/librarian-build.err" | sed 's/^/   /'

if [ "$BRC" -ne 0 ]; then
    if [ "${LIBRARIAN_EXPECT:-1}" = "1" ]; then
        echo "FAIL: native LIBRARIAN.EXE build failed (regression). See librarian-build.err above."
        exit 1
    fi
    echo "SKIP (LIBRARIAN_EXPECT=0): native LIBRARIAN.EXE build failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/LIBRARIAN.EXE" | grep -q 'INTERP' || { echo "FAIL: native LIBRARIAN.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/LIBRARIAN.EXE"
echo "-- native LIBRARIAN.EXE built: PT_INTERP=IMGACT.EXE, ready to activate --"

echo
echo "== input objects: compile mul3.o (referenced) + add.o (unreferenced) with gcc ($ARCH) =="
case "$ARCH" in
    aarch64) OBJ_ARCHFLAG="-mno-outline-atomics" ;;
    x86_64)  OBJ_ARCHFLAG="-mtls-dialect=gnu2" ;;
esac
OBJ_CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $OBJ_ARCHFLAG -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
printf 'int mul3(int x){return x*3;}\n' > "$WORK/mul3.c"
printf 'int addk(int a,int b){return a+b;}\n' > "$WORK/add.c"
# shellcheck disable=SC2086
$CC $OBJ_CFLAGS -c -o "$WORK/MUL3.OBJ" "$WORK/mul3.c"
# shellcheck disable=SC2086
$CC $OBJ_CFLAGS -c -o "$WORK/ADD.OBJ"  "$WORK/add.c"
[ -f "$WORK/MUL3.OBJ" ] && [ -f "$WORK/ADD.OBJ" ] || { echo "FAIL: could not compile seed objects"; exit 1; }
chmod 666 "$WORK/MUL3.OBJ" "$WORK/ADD.OBJ"
MUL3_BYTES=$(wc -c < "$WORK/MUL3.OBJ" | tr -d ' ')
ADD_BYTES=$(wc -c < "$WORK/ADD.OBJ" | tr -d ' ')
echo "-- MUL3.OBJ=$MUL3_BYTES bytes  ADD.OBJ=$ADD_BYTES bytes --"

echo
echo "== ACTIVATE native LIBRARIAN.EXE through IMGACT — /CREATE MATH.OLB from the two .OBJs via RMS =="
set +e
"$SYSEXE/LIBRARIAN.EXE" /CREATE "$WORK/MATH.OLB" "$WORK/MUL3.OBJ" "$WORK/ADD.OBJ" > "$WORK/create.out" 2>&1
CRC=$?
set -e
echo "-- activated LIBRARIAN.EXE /CREATE exit=$CRC; output (last 14 lines): --"
tail -14 "$WORK/create.out" | sed 's/^/   /'

if [ "$CRC" -ne 0 ]; then
    if [ "${LIBRARIAN_EXPECT:-1}" = "1" ]; then
        echo "FAIL: the IMGACT-activated native LIBRARIAN.EXE failed to /CREATE the .OLB (exit $CRC)."
        exit 1
    fi
    echo "SKIP (LIBRARIAN_EXPECT=0): activated /CREATE failed but the assertion is disabled."
    exit 2
fi

# sys$create mints a VMS version suffix, so the produced library is "MATH.OLB;1".
OLB="$WORK/MATH.OLB;1"
[ -f "$OLB" ] || { echo "FAIL: activated LIBRARIAN.EXE did not produce $OLB (RMS-versioned library)"; exit 1; }

# A VMS status is success iff odd (last hex digit in 1/3/5/7/9/B/D/F).
ovmx_hex_is_odd() { case "$1" in *[13579bBdDfF]) return 0 ;; *) return 1 ;; esac; }

# Assert the byte-exact RMS READ path ran for one input object (sys$open/$connect/
# $get-loop-to-EOF/$close) and the read total equals the object's on-disk size.
assert_rms_read() {
    _obj=$1; _bytes=$2; _log=$3
    grep -q "OVMX-RMS: sys\$open(\"$_obj\")" "$_log" \
        || { echo "FAIL: no OVMX-RMS sys\$open trace for $_obj — RMS read path not exercised"; exit 1; }
    _st=$(grep "OVMX-RMS: sys\$open(\"$_obj\")" "$_log" | sed -E 's/.*-> ([0-9A-Fa-f]+).*/\1/')
    ovmx_hex_is_odd "$_st" || { echo "FAIL: sys\$open($_obj) status $_st is not VMS-odd (success)"; exit 1; }
    grep -q "OVMX-RMS: sys\$connect(read \"$_obj\")" "$_log" \
        || { echo "FAIL: no OVMX-RMS sys\$connect(read) trace for $_obj"; exit 1; }
    _gl=$(grep "OVMX-RMS: sys\$get loop(\"$_obj\") done" "$_log")
    [ -n "$_gl" ] || { echo "FAIL: no sys\$get loop summary for $_obj"; exit 1; }
    echo "$_gl" | grep -q 'reached EOF' || { echo "FAIL: $_obj read loop did not terminate via RMS\$_EOF"; exit 1; }
    _rb=$(echo "$_gl" | sed -E 's/.*done, [0-9]+ gets, ([0-9]+) bytes.*/\1/')
    grep -q "OVMX-RMS: sys\$close(read \"$_obj\")" "$_log" \
        || { echo "FAIL: no OVMX-RMS sys\$close trace for $_obj"; exit 1; }
    echo "   $_obj: RMS-read total=$_rb  on-disk=$_bytes"
    [ "$_rb" = "$_bytes" ] || { echo "FAIL: RMS read total ($_rb) != $_obj on-disk size ($_bytes) — read not byte-exact"; exit 1; }
}

echo
echo "-- RMS READ-path assertions for the two input objects (LIBRARIAN read them via RMS) --"
assert_rms_read "$WORK/MUL3.OBJ" "$MUL3_BYTES" "$WORK/create.out"
assert_rms_read "$WORK/ADD.OBJ"  "$ADD_BYTES"  "$WORK/create.out"
echo "-- confirmed: both .OBJ read byte-exact via sys\$open/sys\$connect/sys\$get(loop-to-EOF)/sys\$close --"

echo
echo "-- RMS WRITE-path assertions for MATH.OLB (LIBRARIAN wrote it via RMS) --"
grep -q "OVMX-RMS: sys\$create(\"$WORK/MATH.OLB\")" "$WORK/create.out" \
    || { echo "FAIL: no OVMX-RMS sys\$create trace for MATH.OLB — RMS write path not exercised"; exit 1; }
CREATE_STATUS=$(grep "OVMX-RMS: sys\$create(\"$WORK/MATH.OLB\")" "$WORK/create.out" | sed -E 's/.*-> ([0-9A-Fa-f]+).*/\1/')
ovmx_hex_is_odd "$CREATE_STATUS" || { echo "FAIL: sys\$create(MATH.OLB) status $CREATE_STATUS is not VMS-odd (success)"; exit 1; }
grep -q "OVMX-RMS: sys\$connect(write \"$WORK/MATH.OLB\")" "$WORK/create.out" \
    || { echo "FAIL: no OVMX-RMS sys\$connect(write) trace for MATH.OLB"; exit 1; }
PUTLINE=$(grep "OVMX-RMS: sys\$put loop(\"$WORK/MATH.OLB\") done" "$WORK/create.out")
[ -n "$PUTLINE" ] || { echo "FAIL: no sys\$put loop summary for MATH.OLB"; exit 1; }
echo "   $PUTLINE"
NPUTS=$(echo "$PUTLINE" | sed -E 's/.*done, ([0-9]+) puts.*/\1/')
WRITE_BYTES=$(echo "$PUTLINE" | sed -E 's/.*puts, ([0-9]+) bytes total.*/\1/')
[ "$NPUTS" -ge 1 ] || { echo "FAIL: sys\$put never called for MATH.OLB"; exit 1; }
grep -q "OVMX-RMS: sys\$close(write \"$WORK/MATH.OLB\")" "$WORK/create.out" \
    || { echo "FAIL: no OVMX-RMS sys\$close trace for MATH.OLB"; exit 1; }
OLB_DISK=$(wc -c < "$OLB" | tr -d ' ')
echo "   MATH.OLB: RMS-write total=$WRITE_BYTES  on-disk=$OLB_DISK"
[ "$WRITE_BYTES" = "$OLB_DISK" ] \
    || { echo "FAIL: RMS write total ($WRITE_BYTES) != MATH.OLB on-disk size ($OLB_DISK) — library truncated/corrupt"; exit 1; }
echo "-- confirmed: MATH.OLB written byte-exact via sys\$create/sys\$connect/sys\$put(loop)/sys\$close --"

echo
echo "-- ORACLE: stock \`ar t\` lists the .OLB the native LIBRARIAN wrote (independent reader) --"
AR_LIST=$(ar t "$OLB" 2>/dev/null || true)
echo "$AR_LIST" | sed 's/^/   /'
echo "$AR_LIST" | grep -q '^MUL3' || { echo "FAIL: ar t does not list MUL3 member — .OLB is not a valid ar container"; exit 1; }
echo "$AR_LIST" | grep -q '^ADD'  || { echo "FAIL: ar t does not list ADD member"; exit 1; }
echo "-- confirmed: MATH.OLB is a valid ar container with both modules --"

echo
echo "== ACTIVATE native LIBRARIAN.EXE /LIST — read MATH.OLB back via RMS =="
set +e
"$SYSEXE/LIBRARIAN.EXE" /LIST "$OLB" > "$WORK/list.out" 2>&1
LRC=$?
set -e
echo "-- /LIST exit=$LRC; output: --"; sed 's/^/   /' "$WORK/list.out"
[ "$LRC" -eq 0 ] || { echo "FAIL: native LIBRARIAN.EXE /LIST exited non-zero ($LRC)"; exit 1; }
grep -q "OVMX-RMS: sys\$open(\"$OLB\")" "$WORK/list.out" \
    || { echo "FAIL: /LIST did not read the .OLB via RMS (no sys\$open trace)"; exit 1; }
grep -q '2 modules in library' "$WORK/list.out" \
    || { echo "FAIL: /LIST did not report the 2 modules read back from the .OLB"; exit 1; }
echo "-- confirmed: native LIBRARIAN.EXE /LIST read the .OLB back via RMS and saw both modules --"

echo
echo "== END-TO-END: a bootstrap LINK consumes the native-created .OLB into a runnable image =="
printf 'extern int mul3(int);\n#include <stdio.h>\nint main(void){int r=mul3(14);printf("mul3(14)=%%d\\n",r);return r==42?0:1;}\n' > "$WORK/main.c"
# shellcheck disable=SC2086
$CC $OBJ_CFLAGS -c -o "$WORK/main.o" "$WORK/main.c"
set +e
"$WORK/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$WORK/PROG.EXE" "$WORK/main.o" "$OLB" > "$WORK/link.out" 2>&1
PLRC=$?
set -e
echo "-- bootstrap LINK exit=$PLRC (last 8 lines): --"; tail -8 "$WORK/link.out" | sed 's/^/   /'
[ "$PLRC" -eq 0 ] || { echo "FAIL: bootstrap LINK could not consume the native-created .OLB"; exit 1; }
PROG="$WORK/PROG.EXE"
[ -f "$PROG" ] || PROG="$WORK/PROG.EXE;1"
[ -f "$PROG" ] || { echo "FAIL: LINK produced no PROG.EXE from the .OLB"; exit 1; }
chmod +x "$PROG"
set +e
"$PROG" > "$WORK/prog.out" 2>&1
PRC=$?
set -e
echo "-- PROG output: --"; sed 's/^/   /' "$WORK/prog.out"
echo "exit code = $PRC"
grep -q 'mul3(14)=42' "$WORK/prog.out" || { echo "FAIL: image built from the native-created .OLB did not compute mul3(14)=42"; exit 1; }
[ "$PRC" -eq 0 ] || { echo "FAIL: image from the .OLB did not exit clean (got $PRC)"; exit 1; }

echo
echo "================================================================================"
echo "MILESTONE (vms-1c2, self-host S3 spine): LIBRARIAN.EXE now runs AS an OVMX image"
echo "($ARCH). librarian.c + the reused RMS I/O shim are linked (LINK.EXE --executable"
echo "--use {DECC\$SHR + the five OVMX shareables}) into a PT_INTERP=IMGACT.EXE image,"
echo "IMGACT-activated with NO ld / NO ld.so, and — running INSIDE OVMX — it /CREATEs a"
echo "real .OLB from two .OBJs, reading each object via sys\$open/sys\$get (byte-exact to"
echo "EOF) and writing the .OLB via sys\$create/sys\$put (byte-exact), proven by a trace +"
echo "byte-count cross-check. A stock \`ar t\` lists both modules, the native LIBRARIAN"
echo "/LIST reads the .OLB back via RMS, and a bootstrap LINK consumes it, pulling MUL3 into"
echo "it into an image that activates and computes mul3(14)=42. The OVMX toolchain's"
echo "object librarian is now itself an OVMX image — self-host S3 spine."
echo "================================================================================"
