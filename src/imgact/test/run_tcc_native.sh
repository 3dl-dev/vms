#!/bin/sh
# run_tcc_native.sh — the S2 ENDPOINT proof (bead vms-4ba.4, epic vms-4ba
# "self-host S2: a C compiler as an OVMX image"): build stock, UNMODIFIED
# tinycc (third-party/tcc) AS a VMS-native EXECUTABLE image (TCC.EXE), activate
# it through IMGACT.EXE (NO ld / NO ld.so — the operator's canonical "all VMS"
# path), have the ACTIVATED tcc compile hello.c to hello.o, then round-trip
# that object through the vms-4ba.3 path (LINK.EXE --executable --use {6
# producers} -> HELLO.EXE, IMGACT-activated, prints "hello", exit 0). Mirrors
# run_dcl_native.sh's shape exactly: build-to-/tmp, hard FAIL on any DONE-
# condition miss, milestone banner on success.
#
# DONE conditions (bead vms-4ba.4), all proven for REAL below, nothing mocked:
#   1. IMGACT.EXE activates TCC.EXE (mk_tcc.sh's LINK.EXE --executable output),
#      transitively pulling the 6-producer graph from .vms$imp; tcc runs
#      HOSTED (VMS-native, no ld/ld.so) — `tcc -v` prints its version banner.
#   2. The activated TCC.EXE opens hello.c (via DECC$SHR stdio/open) and
#      compiles it to hello.o (`TCC.EXE -c hello.c -o hello.o`) — a real
#      ELF64/REL/AArch64 object, not a canned artifact.
#   3. That hello.o links via the vms-4ba.3 path (LINK.EXE --executable --use
#      {DECC$SHR + the five OVMX shareables} -o HELLO.EXE) and IMGACT-
#      activates, printing 'hello' and exiting 0.
#
# --allow-undefined / environ: mk_tcc.sh defers exactly ONE cross-image
# reference (`environ`, tccrun.c's `-run` JIT path — dead code for `tcc -c`).
# This is NOT weakening the proof: mk_tcc.sh itself hard-asserts the deferred
# count is exactly 1 (LINK-I-DEFEXT) before returning success — see its header
# comment for the LINK.EXE BSS-universal-ordering gap this works around
# (link.c is out of Systems-Engineer file-domain; out of scope here).
#
# TCC_EXPECT_LINK (default 1, mirrors DCL_EXPECT_LINK / TCC_EXPECT_LINK in the
# other native harnesses): the vms-4ba.3 CRUX proof is closed (vms-9c1), so a
# LINK.EXE failure on EITHER the TCC.EXE build or the HELLO.EXE round-trip is a
# real regression (hard FAIL) by default. Set TCC_EXPECT_LINK=0 to soften ONLY
# the two LINK.EXE steps (and their downstream activations) to a SKIP (exit 2)
# while iterating on a new finding — every other assertion here stays a hard
# FAIL regardless.
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain — do NOT edit them here.
#
# arm64 musl Alpine container only (CLAUDE.md test loop). Needs root to create
# /vms. Run natively on an aarch64 host, or under arm64 emulation (binfmt/QEMU)
# on x86_64 (see .github/workflows/ci.yml).
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
TCC_SRC="$REPO/third-party/tcc/src"
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
WORK=${WORK:-/tmp/tcc-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ;;
    *) echo "SKIP-FAIL: run_tcc_native.sh needs a native aarch64 host (got $(uname -m)) — tinycc's configure auto-selects TCC_TARGET_ARM64 from uname -m, and LINK.EXE/IMGACT.EXE are aarch64-only today. Run in the arm64 musl Alpine container per CLAUDE.md / epic vms-4ba."; exit 1 ;;
esac

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }
[ -d "$TCC_SRC" ] || { echo "FAIL: third-party/tcc/src not found — vendoring bead vms-4ba.1 missing?"; exit 1; }

echo "== build IMGACT.EXE + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== DECC\$SHR.EXE (whole-archive musl; now +39 vms-4ba.4 universals for tcc) =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR no symbol vector"; exit 1; }

echo "== LIBVMSSYS\$SHR.EXE =="
SYSCFLAGS="-fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -I$LIBVMSSYS_DIR"
SYSOBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
    $CC $SYSCFLAGS -c -o "$WORK/sys_$c.o" "$LIBVMSSYS_DIR/$c.c"
    SYSOBJS="$SYSOBJS $WORK/sys_$c.o"
done
$CC -fPIC -mno-outline-atomics -c -o "$WORK/sys_syscall.o" "$LIBVMSSYS_DIR/arch/aarch64/syscall.S"
SYSOBJS="$SYSOBJS $WORK/sys_syscall.o"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_dclast=PROCEDURE,vms_kif_setast=PROCEDURE,vms_kif_deliverast=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBVMSSYS\$SHR.EXE" $SYSOBJS

echo "== LIBVMSPROCESS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsprocess_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "" "$VMSPROC_DIR" "$LIBVMS_INC"

echo "== LIBVMSLNM\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

echo "== LIBVMSFS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsfs_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$VMSFS_DIR" "$LIBVMS_INC" "$LNM_INC"

echo "== LIBVMS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_libvms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$LIBVMS_DIR"

echo "== LIBVMSRMS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsrms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$VMSRMS_DIR" "$LIBVMS_INC" "$VMSFS_INC"
echo "-- full six-library producer graph linked VMS-native (incl. tcc's DECC appends) --"

echo
echo "== build TCC.EXE VMS-native (mk_tcc.sh: 11 tcc TUs, LINK.EXE --executable) =="
set +e
CC="$CC" WORK="$WORK/mk-tcc" sh "$LINK_DIR/mk_tcc.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/TCC.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$TCC_SRC" 2>"$WORK/tcc-link.err"
LRC=$?
set -e
echo "-- mk_tcc.sh exit=$LRC; message: --"
tail -6 "$WORK/tcc-link.err" | sed 's/^/   /'

if [ "$LRC" -ne 0 ]; then
    if [ "${TCC_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: TCC.EXE build failed (regression — the vms-4ba.3 CRUX + vms-9c1"
        echo "  LOCAL-GOT fix are complete; tcc must build+link). See tcc-link.err above."
        exit 1
    fi
    echo "SKIP (TCC_EXPECT_LINK=0): TCC.EXE build failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/TCC.EXE" | grep -q 'INTERP' || { echo "FAIL: TCC.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/TCC.EXE"

echo
echo "== TCC.EXE linked — activate through IMGACT.EXE — DONE condition 1 =="
set +e
"$SYSEXE/TCC.EXE" -v > "$WORK/tcc-v.out" 2>&1
TVRC=$?
set -e
echo "-- tcc -v output: --"; sed 's/^/   /' "$WORK/tcc-v.out"
echo "exit code = $TVRC"
grep -qi 'tcc version' "$WORK/tcc-v.out" || { echo "FAIL: activated TCC.EXE did not print its version banner"; exit 1; }
[ "$TVRC" -eq 0 ] || { echo "FAIL: TCC.EXE -v did not exit clean (got $TVRC)"; exit 1; }
echo "-- confirmed: IMGACT.EXE activates TCC.EXE VMS-native, no ld / no ld.so --"

echo
echo "== activated TCC.EXE compiles hello.c -> hello.o — DONE condition 2 =="
cat > "$WORK/hello.c" <<'EOF'
#include <stdio.h>
int main(void)
{
    printf("hello\n");
    return 0;
}
EOF
# mk_tcc.sh now builds TCC.EXE with RMS file I/O (vms-4ba.5: OVMX_RMS_IO) —
# see run_tcc_rms.sh for the full RMS-path proof. Two side effects that also
# apply to THIS (vms-4ba.4) harness now:
#  (1) sys$open's protection check has a pre-existing, separate cross-module
#      bit-layout mismatch (vmsfs_mode_to_protection vs. sys_security.c's
#      vms$check_access — see run_tcc_rms.sh's header comment) that denies
#      root read access to a plain mode-644 file; chmod 666 sidesteps it
#      without touching that (unrelated, security-adjacent) code.
#  (2) sys$create always mints a literal VMS version suffix on disk
#      ("hello.o;1"), so the real produced artifact is no longer a bare
#      "hello.o".
chmod 666 "$WORK/hello.c"
set +e
"$SYSEXE/TCC.EXE" -c "$WORK/hello.c" -o "$WORK/hello.o" > "$WORK/tcc-compile.out" 2>&1
TCRC=$?
set -e
sed 's/^/   /' "$WORK/tcc-compile.out"
[ "$TCRC" -eq 0 ] || { echo "FAIL: activated TCC.EXE failed to compile hello.c (exit $TCRC)"; exit 1; }
HELLO_O="$WORK/hello.o;1"
[ -f "$HELLO_O" ] || { echo "FAIL: activated TCC.EXE did not produce $HELLO_O (RMS-versioned hello.o)"; exit 1; }
readelf -h "$HELLO_O" | tee "$WORK/hello.readelf.h" >/dev/null
grep -q 'Class:.*ELF64' "$WORK/hello.readelf.h" || { echo "FAIL: hello.o is not ELF64"; exit 1; }
grep -q 'Type:.*REL (Relocatable file)' "$WORK/hello.readelf.h" || { echo "FAIL: hello.o is not ET_REL"; exit 1; }
grep -q 'Machine:.*AArch64' "$WORK/hello.readelf.h" || { echo "FAIL: hello.o is not EM_AARCH64"; exit 1; }
echo "-- confirmed: hello.o is a real ELF64/REL/AArch64 object, compiled by the ACTIVATED tcc --"

echo
echo "== hello.o round-trip: LINK.EXE --executable --use {6 producers} (vms-4ba.3 path) -> HELLO.EXE — DONE condition 3 =="
set +e
"$WORK/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$SYSEXE/TCCHELLO.EXE" "$HELLO_O" 2>"$WORK/hello-link.err"
HLRC=$?
set -e
echo "-- LINK.EXE --executable exit=$HLRC; message: --"
tail -5 "$WORK/hello-link.err" | sed 's/^/   /'

if [ "$HLRC" -ne 0 ]; then
    if [ "${TCC_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: HELLO.EXE (compiled by our OWN TCC.EXE) failed to link — regression"
        echo "  against the vms-4ba.3 CRUX proof (a Linux-hosted stock tcc's hello.o links"
        echo "  clean via this exact path). See hello-link.err above."
        exit 1
    fi
    echo "SKIP (TCC_EXPECT_LINK=0): HELLO.EXE link failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/TCCHELLO.EXE" | grep -q 'INTERP' || { echo "FAIL: TCCHELLO.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/TCCHELLO.EXE"

set +e
"$SYSEXE/TCCHELLO.EXE" > "$WORK/tcchello.out" 2>&1
HRC=$?
set -e
echo "-- program output: --"; sed 's/^/   /' "$WORK/tcchello.out"
echo "exit code = $HRC"
grep -q 'hello' "$WORK/tcchello.out" || { echo "FAIL: TCCHELLO.EXE did not print 'hello'"; exit 1; }
[ "$HRC" -eq 0 ] || { echo "FAIL: TCCHELLO.EXE did not exit clean (got $HRC)"; exit 1; }

echo
echo "================================================================================"
echo "MILESTONE (vms-4ba.4, S2 ENDPOINT): stock, UNMODIFIED tinycc (third-party/tcc)"
echo "now runs AS an OVMX image. TCC.EXE — 11 objects (tcc.c/libtcc.c ONE_SOURCE=0 +"
echo "the 9 core/arm64-backend TUs), linked VMS-native via LINK.EXE --executable --use"
echo "{DECC\$SHR + the five OVMX shareables} (+39 DECC\$SHR universals for tcc's"
echo "long-double libgcc helpers + POSIX calls; one dead-for-'-c' reference, environ,"
echo "deferred via --allow-undefined around a real LINK.EXE BSS-universal-ordering gap"
echo "-- see mk_decc_shr.sh/mk_tcc.sh comments and the vms-4ba.4 follow-up item) --"
echo "activates through IMGACT.EXE with NO ld / NO ld.so, compiles a REAL hello.c to"
echo "hello.o, and that hello.o round-trips through the vms-4ba.3 LINK.EXE path into"
echo "HELLO.EXE, IMGACT-activates, prints 'hello', exits 0. The self-hosting S2 crux"
echo "(vms-116) has its compiler running natively inside OVMX."
echo "================================================================================"
