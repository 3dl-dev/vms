#!/bin/sh
# run_link_native.sh — self-host S3.1 proof (bead vms-b5a): LINK.EXE runs AS an
# OVMX image. Builds the VMS-native producer graph + IMGACT.EXE, self-links
# link.c (+ its RMS I/O shim) into a NATIVE LINK.EXE image (mk_link.sh:
# freestanding-musl, -DOVMX_RMS_IO, LINK.EXE --executable --use {6 producers},
# PT_INTERP=IMGACT.EXE), then ACTIVATES that native LINK.EXE through IMGACT.EXE
# and has it link a real .o into a runnable .EXE — reading the .o via RMS
# (sys$open/$get) and writing the .EXE via RMS (sys$create/$put) — with NO host
# LINK.EXE anywhere in the linking path. Finally activates the produced .EXE and
# checks it prints 'hello', exit 0. Mirrors run_tcc_rms.sh / run_dcl_native.sh.
#
# TWO LINK.EXE ROLES (the crux of the anti-LARP bar): a BOOTSTRAP LINK.EXE (an
# ordinary host tool, $WORK/LINK.EXE) BUILDS the native LINK.EXE image. That is
# explicitly allowed (CLAUDE.md Rule 9: a build step is not an activation proof;
# the host tool building a native image is fine). What is PROVEN native here is
# the OUTPUT: the IMGACT-activated $SYSEXE/LINK.EXE does the real .o -> .EXE
# link. The bootstrap LINK.EXE never touches hello.o or HELLO.EXE.
#
# DONE conditions (bead vms-b5a), all proven for REAL below, nothing mocked:
#   1. IMGACT.EXE activates the native LINK.EXE (mk_link.sh output),
#      transitively pulling the producer graph from .vms$imp; it runs VMS-native
#      (no ld / no ld.so).
#   2. The ACTIVATED LINK.EXE reads a real ELF .o through RMS (sys$open/$connect/
#      a sys$get loop to EOF/sys$close) and writes a real ELF .EXE through RMS
#      (sys$create/$connect/a sys$put loop/sys$close), proven by a trace (VMS-odd
#      statuses) and a byte-count cross-check against both files' on-disk sizes.
#   3. The RMS-written .EXE IMGACT-activates and prints 'hello', exit 0 — a
#      LINK.EXE-produced image that actually runs, with no host LINK.EXE in the
#      path that produced it.
#
# LINK_EXPECT (default 1, BLOCKING — mirrors DCL_EXPECT_LINK / TCC_EXPECT_LINK):
# a failure of the native LINK.EXE build or the activated-link step is a real
# regression. Set LINK_EXPECT=0 to soften ONLY those steps to a SKIP (exit 2)
# while iterating; every other assertion stays a hard FAIL.
#
# Runs on aarch64 (native or arm64 binfmt/QEMU emulation) OR x86_64 (native) —
# link.c is arch-agnostic and IMGACT/the graph build both arches (unlike the tcc
# harnesses, which are aarch64-only because tinycc's configure keys off uname).
# musl Alpine container only (CLAUDE.md test loop). Needs root to create /vms.
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
WORK=${WORK:-/tmp/link-native}
rm -rf "$WORK"; mkdir -p "$WORK"

# ARCH: default from the running machine; CI passes it explicitly.
case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64)  ARCH=x86_64 ;;
    *) echo "SKIP-FAIL: run_link_native.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
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
# five OVMX shareables) — the same builder run_dcl_native.sh uses.
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build LINK.EXE VMS-native (mk_link.sh: link.c + RMS shim, LINK.EXE --executable) =="
set +e
CC="$CC" ARCH="$ARCH" WORK="$WORK/mk-link" sh "$LINK_DIR/mk_link.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/LINK.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SRC" 2>"$WORK/link-build.err"
LRC=$?
set -e
echo "-- mk_link.sh exit=$LRC; message: --"
tail -6 "$WORK/link-build.err" | sed 's/^/   /'

if [ "$LRC" -ne 0 ]; then
    if [ "${LINK_EXPECT:-1}" = "1" ]; then
        echo "FAIL: native LINK.EXE build failed (regression). See link-build.err above."
        exit 1
    fi
    echo "SKIP (LINK_EXPECT=0): native LINK.EXE build failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/LINK.EXE" | grep -q 'INTERP' || { echo "FAIL: native LINK.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/LINK.EXE"
echo "-- native LINK.EXE built: PT_INTERP=IMGACT.EXE, ready to activate --"

echo
echo "== input object: compile a real hello.c -> hello.o (gcc seed object, $ARCH) — DONE condition 2 input =="
cat > "$WORK/hello.c" <<'EOF'
#include <stdio.h>
int main(void)
{
    printf("hello\n");
    return 0;
}
EOF
case "$ARCH" in
    aarch64) OBJ_ARCHFLAG="-mno-outline-atomics" ;;
    x86_64)  OBJ_ARCHFLAG="-mtls-dialect=gnu2" ;;
esac
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $OBJ_ARCHFLAG \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -c -o "$WORK/hello.o" "$WORK/hello.c"
[ -f "$WORK/hello.o" ] || { echo "FAIL: could not compile hello.o seed object"; exit 1; }
# chmod 666 defensively (see run_tcc_rms.sh's long note: likely dead weight now
# that RMS has no protection pre-check and the process runs as root, but kept for
# parity with the proven RMS harness).
chmod 666 "$WORK/hello.o"
HELLO_O_BYTES=$(wc -c < "$WORK/hello.o" | tr -d ' ')
echo "-- hello.o compiled: $HELLO_O_BYTES bytes --"

echo
echo "== ACTIVATE native LINK.EXE through IMGACT — link hello.o -> HELLO.EXE via RMS — DONE conditions 1 & 2 =="
set +e
"$SYSEXE/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$WORK/HELLO.EXE" "$WORK/hello.o" > "$WORK/link-run.out" 2>&1
RRC=$?
set -e
echo "-- activated LINK.EXE exit=$RRC; output (last 12 lines): --"
tail -12 "$WORK/link-run.out" | sed 's/^/   /'

if [ "$RRC" -ne 0 ]; then
    if [ "${LINK_EXPECT:-1}" = "1" ]; then
        echo "FAIL: the IMGACT-activated native LINK.EXE failed to link hello.o (exit $RRC)."
        exit 1
    fi
    echo "SKIP (LINK_EXPECT=0): activated LINK.EXE link failed but the assertion is disabled."
    exit 2
fi

# sys$create mints a VMS version suffix, so the produced image is "HELLO.EXE;1".
HELLO_EXE="$WORK/HELLO.EXE;1"
[ -f "$HELLO_EXE" ] || { echo "FAIL: activated LINK.EXE did not produce $HELLO_EXE (RMS-versioned image)"; exit 1; }

# A VMS status is success iff odd (last hex digit in 1/3/5/7/9/B/D/F).
ovmx_hex_is_odd() { case "$1" in *[13579bBdDfF]) return 0 ;; *) return 1 ;; esac; }

echo
echo "-- RMS READ-path trace assertions (sys\$open/sys\$connect/sys\$get for hello.o) --"
grep -q "OVMX-RMS: sys\$open(\"$WORK/hello.o\")" "$WORK/link-run.out" \
    || { echo "FAIL: no OVMX-RMS sys\$open trace for hello.o — RMS read path not exercised"; exit 1; }
OPEN_STATUS=$(grep "OVMX-RMS: sys\$open(\"$WORK/hello.o\")" "$WORK/link-run.out" | sed -E 's/.*-> ([0-9A-Fa-f]+).*/\1/')
ovmx_hex_is_odd "$OPEN_STATUS" || { echo "FAIL: sys\$open(hello.o) status $OPEN_STATUS is not VMS-odd (success)"; exit 1; }
grep -q "OVMX-RMS: sys\$connect(read \"$WORK/hello.o\")" "$WORK/link-run.out" \
    || { echo "FAIL: no OVMX-RMS sys\$connect(read) trace for hello.o"; exit 1; }
GETLINE=$(grep "OVMX-RMS: sys\$get loop(\"$WORK/hello.o\") done" "$WORK/link-run.out")
echo "   $GETLINE"
[ -n "$GETLINE" ] || { echo "FAIL: no sys\$get loop summary for hello.o — RMS read path not exercised"; exit 1; }
echo "$GETLINE" | grep -q 'reached EOF' || { echo "FAIL: hello.o read loop did not terminate via RMS\$_EOF"; exit 1; }
READ_BYTES=$(echo "$GETLINE" | sed -E 's/.*done, [0-9]+ gets, ([0-9]+) bytes.*/\1/')
grep -q "OVMX-RMS: sys\$close(read \"$WORK/hello.o\")" "$WORK/link-run.out" \
    || { echo "FAIL: no OVMX-RMS sys\$close trace for hello.o"; exit 1; }
echo "   RMS-read byte total=$READ_BYTES  hello.o on-disk=$HELLO_O_BYTES"
[ "$READ_BYTES" = "$HELLO_O_BYTES" ] \
    || { echo "FAIL: RMS read byte total ($READ_BYTES) != hello.o on-disk size ($HELLO_O_BYTES) — read not byte-exact"; exit 1; }
echo "-- confirmed: hello.o read byte-exact via sys\$open/sys\$connect/sys\$get(loop-to-EOF)/sys\$close --"

echo
echo "-- RMS WRITE-path trace assertions (sys\$create/sys\$connect/sys\$put for HELLO.EXE) --"
grep -q "OVMX-RMS: sys\$create(\"$WORK/HELLO.EXE\")" "$WORK/link-run.out" \
    || { echo "FAIL: no OVMX-RMS sys\$create trace for HELLO.EXE — RMS write path not exercised"; exit 1; }
CREATE_STATUS=$(grep "OVMX-RMS: sys\$create(\"$WORK/HELLO.EXE\")" "$WORK/link-run.out" | sed -E 's/.*-> ([0-9A-Fa-f]+).*/\1/')
ovmx_hex_is_odd "$CREATE_STATUS" || { echo "FAIL: sys\$create(HELLO.EXE) status $CREATE_STATUS is not VMS-odd (success)"; exit 1; }
grep -q "OVMX-RMS: sys\$connect(write \"$WORK/HELLO.EXE\")" "$WORK/link-run.out" \
    || { echo "FAIL: no OVMX-RMS sys\$connect(write) trace for HELLO.EXE"; exit 1; }
PUTLINE=$(grep "OVMX-RMS: sys\$put loop(\"$WORK/HELLO.EXE\") done" "$WORK/link-run.out")
echo "   $PUTLINE"
[ -n "$PUTLINE" ] || { echo "FAIL: no sys\$put loop summary for HELLO.EXE"; exit 1; }
NPUTS=$(echo "$PUTLINE" | sed -E 's/.*done, ([0-9]+) puts.*/\1/')
WRITE_BYTES=$(echo "$PUTLINE" | sed -E 's/.*puts, ([0-9]+) bytes total.*/\1/')
[ "$NPUTS" -ge 1 ] || { echo "FAIL: sys\$put never called for HELLO.EXE"; exit 1; }
grep -q "OVMX-RMS: sys\$close(write \"$WORK/HELLO.EXE\")" "$WORK/link-run.out" \
    || { echo "FAIL: no OVMX-RMS sys\$close trace for HELLO.EXE"; exit 1; }
DISK_BYTES=$(wc -c < "$HELLO_EXE" | tr -d ' ')
echo "   RMS-write byte total=$WRITE_BYTES  HELLO.EXE;1 on-disk=$DISK_BYTES"
[ "$WRITE_BYTES" = "$DISK_BYTES" ] \
    || { echo "FAIL: RMS write byte total ($WRITE_BYTES) != HELLO.EXE on-disk size ($DISK_BYTES) — image truncated/corrupt"; exit 1; }
echo "-- confirmed: HELLO.EXE written byte-exact via sys\$create/sys\$connect/sys\$put(loop)/sys\$close --"

echo
echo "-- HELLO.EXE is a real OVMX image (ELF, PT_INTERP=IMGACT) built by the ACTIVATED LINK.EXE --"
readelf -lW "$HELLO_EXE" | grep -q 'INTERP' || { echo "FAIL: HELLO.EXE has no PT_INTERP (IMGACT)"; exit 1; }
readelf -hW "$HELLO_EXE" | grep -q 'DYN' || { echo "FAIL: HELLO.EXE is not ET_DYN"; exit 1; }

echo
echo "== activate HELLO.EXE (produced by the native LINK.EXE) through IMGACT — DONE condition 3 =="
chmod +x "$HELLO_EXE"
set +e
"$HELLO_EXE" > "$WORK/hello.out" 2>&1
HRC=$?
set -e
echo "-- program output: --"; sed 's/^/   /' "$WORK/hello.out"
echo "exit code = $HRC"
grep -q 'hello' "$WORK/hello.out" || { echo "FAIL: HELLO.EXE did not print 'hello'"; exit 1; }
[ "$HRC" -eq 0 ] || { echo "FAIL: HELLO.EXE did not exit clean (got $HRC)"; exit 1; }

echo
echo "================================================================================"
echo "MILESTONE (vms-b5a, self-host S3.1): LINK.EXE now runs AS an OVMX image ($ARCH)."
echo "link.c + its RMS I/O shim are self-linked (LINK.EXE --executable --use {DECC\$SHR"
echo "+ the five OVMX shareables}) into a PT_INTERP=IMGACT.EXE image, IMGACT-activated"
echo "with NO ld / NO ld.so, and — running INSIDE OVMX — it reads a real .o via"
echo "sys\$open/sys\$get (byte-exact to EOF) and writes a real .EXE via sys\$create/"
echo "sys\$put (byte-exact), proven by a trace + byte-count cross-check against both"
echo "files. The produced HELLO.EXE IMGACT-activates and prints 'hello', exit 0."
echo "No host LINK.EXE anywhere in the linking path. The OVMX toolchain's linker is"
echo "now itself an OVMX image — self-host S3.1."
echo "================================================================================"
