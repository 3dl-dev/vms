#!/bin/sh
# run_tcc_selfhost.sh — the S2 FIXPOINT proof (bead vms-4ba.6, epic vms-4ba
# "self-host S2: a C compiler as an OVMX image"). The summit of the epic:
# prove tcc COMPILES tcc INSIDE OVMX — the compiler reproduces itself with NO
# Unix compiler in the loop.
#
# THE CHAIN (all real, nothing mocked; NO ld / NO ld.so anywhere):
#   gen-1  gcc compiles tcc's 12 TUs -> LINK.EXE --executable -> TCC.EXE (seed).
#          This is the ONLY step that touches a Unix compiler; it is the
#          bootstrap seed, identical to run_tcc_native.sh (vms-4ba.4).
#   gen-2  gen-1 TCC.EXE, ACTIVATED THROUGH IMGACT.EXE (no gcc, no ld.so),
#          compiles EVERY one of tcc's 12 TUs to an object -> LINK.EXE
#          --executable -> a SECOND TCC.EXE. Every gen-2 object is produced by
#          the VMS-native tcc, reading its .c through RMS and writing its .o
#          through RMS (OVMX_RMS_IO), exactly as tcc does for any input.
#   proof  gen-2 TCC.EXE, ACTIVATED, compiles a fresh hello.c to an object that
#          LINK.EXE + IMGACT round-trip into a running program printing 'hello',
#          exit 0 — the second-generation compiler is a WORKING compiler.
#   gen-3  (reproducibility) gen-2 TCC.EXE recompiles the SAME 12 TUs; the
#          resulting objects are asserted BYTE-IDENTICAL to gen-2's — the
#          classic self-host fixpoint check (a tcc-built tcc and the tcc IT
#          builds emit identical code, so the compiler has converged).
#
# WHY THIS IS A REAL SELF-HOST (what a veracity adversary re-runs to confirm):
#   * gen-2's objects are compiled by /vms/.../SYSEXE/TCC.EXE under IMGACT — the
#     script NEVER invokes $CC to make them. gcc builds ONLY the gen-1 seed.
#   * gen-2 TCC.EXE is LINK.EXE-linked from those gen-2 objects and IMGACT-
#     activatable (PT_INTERP), then itself compiles hello.c to a working image.
#   * gen-2==gen-3 byte-identity proves the reproduction is a true fixpoint, not
#     a lucky one-off.
#
# THE ONE OVMX ADAPTATION THAT MAKES tcc-BUILT-tcc WORK (vms-4ba.6):
#   tcc emits its pointer-initializer tables (tcc_options[] — the command-line
#   option table — and friends) into a section named `.data.ro`, created
#   SHF_ALLOC-only because upstream tcc's OWN linker adds SHF_WRITE at final
#   link (GNU_RELRO). For a `tcc -c` OBJECT that fixup never runs, so `.data.ro`
#   ships read-only while still carrying R_AARCH64_ABS64 pointer relocations.
#   OVMX's LINK.EXE (like most linkers) applies data-pointer relocations only to
#   WRITABLE sections; gcc never trips this because it emits such tables into
#   `.data.rel.ro` WITH SHF_WRITE. Left unfixed, EVERY `.data.ro` pointer in a
#   tcc-built binary lands NULL — the gen-2 tcc then matches no option ("tcc:
#   error: invalid option"). The fix is a single OVMX-tagged, define-gated seam
#   in the vendored tcc (third-party/tcc/src/tccelf.c `shf_RELRO`, activated by
#   -DOVMX_DATA_RO_WRITABLE in src/vmslink/mk_tcc.sh): emit `.data.ro` writable
#   from creation, functionally identical to gcc's pre-RELRO `.data.rel.ro`.
#   This lives entirely in tcc/mk_tcc.sh scope — NOT a link.c change. It is a
#   no-op for the gcc-built gen-1 (gcc doesn't create `.data.ro`); it matters
#   only once tcc is compiling tcc.
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain — do NOT edit them here. This harness is a pure CONSUMER
# of the existing LINK.EXE/IMGACT.EXE.
#
# TCC_EXPECT_LINK (default 1, mirrors run_tcc_native.sh / run_tcc_rms.sh): the
# vms-4ba.3/.4/.5 crux+endpoint proofs are closed, so a LINK.EXE failure on ANY
# generation's build is a real regression (hard FAIL). Set TCC_EXPECT_LINK=0 to
# soften ONLY the LINK.EXE steps (and their downstream activations) to a SKIP
# (exit 2) while iterating on a new finding — every other assertion stays a hard
# FAIL regardless.
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
OVMX_DIR="$REPO/third-party/tcc/ovmx"
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
VMSRMS_INC="$VMSRMS_DIR/include"
WORK=${WORK:-/tmp/tcc-selfhost}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ;;
    *) echo "SKIP-FAIL: run_tcc_selfhost.sh needs a native aarch64 host (got $(uname -m)) — tinycc's configure auto-selects TCC_TARGET_ARM64 from uname -m, and LINK.EXE/IMGACT.EXE are aarch64-only today. Run in the arm64 musl Alpine container per CLAUDE.md / epic vms-4ba."; exit 1 ;;
esac

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }
[ -d "$TCC_SRC" ] || { echo "FAIL: third-party/tcc/src not found — vendoring bead vms-4ba.1 missing?"; exit 1; }

# ---------------------------------------------------------------------------
# PRELUDE — build the producer toolchain + gen-1 TCC.EXE (the bootstrap seed).
# Identical to run_tcc_native.sh's build (mirrors run_dcl_native.sh): IMGACT.EXE
# + LINK.EXE, the six OVMX shareables, then mk_tcc.sh's gcc-compiled TCC.EXE.
# ---------------------------------------------------------------------------
echo "== build IMGACT.EXE + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== DECC\$SHR.EXE (whole-archive musl; +tcc universals incl. __negtf2 for self-host) =="
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
    --symbol-vector "vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_getdvi_devnam=PROCEDURE,vms_kif_devscan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_setprv=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_dclast=PROCEDURE,vms_kif_setast=PROCEDURE,vms_kif_deliverast=PROCEDURE,vms_kif_lnm_define=PROCEDURE,vms_kif_lnm_delete=PROCEDURE,vms_kif_lnm_translate=PROCEDURE,vms_kif_mbx_create=PROCEDURE,vms_kif_mbx_assign=PROCEDURE,vms_kif_mbx_delmbx=PROCEDURE,vms_kif_mbx_write=PROCEDURE,vms_kif_mbx_read=PROCEDURE,vms_kif_p0_map=PROCEDURE,vms_kif_p0_unmap=PROCEDURE,vms_kif_p1_map=PROCEDURE,vms_kif_enter_image=PROCEDURE,vms_kif_image_rundown=PROCEDURE,vms_kif_p1_protect=PROCEDURE,vms_kif_lnm_enumerate=PROCEDURE,vms_kif_disk_resolve=PROCEDURE,vms_kif_chkpriv=PROCEDURE,vms_kif_alloc=PROCEDURE,vms_kif_dalloc=PROCEDURE" \
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
echo "-- full six-library producer graph linked VMS-native --"

PRODUCERS="--use $SYSLIB/DECC\$SHR.EXE --use $SYSLIB/LIBVMS\$SHR.EXE --use $SYSLIB/LIBVMSPROCESS\$SHR.EXE --use $SYSLIB/LIBVMSFS\$SHR.EXE --use $SYSLIB/LIBVMSLNM\$SHR.EXE --use $SYSLIB/LIBVMSRMS\$SHR.EXE"

echo
echo "== gen-1: gcc builds the SEED TCC.EXE (mk_tcc.sh — the ONLY Unix-compiler step) =="
MKTCC_WORK="$WORK/mk-tcc"        # holds config.h + tccdefs_.h (generated by mk_tcc.sh)
set +e
CC="$CC" WORK="$MKTCC_WORK" sh "$LINK_DIR/mk_tcc.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/TCC.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$TCC_SRC" 2>"$WORK/gen1-link.err"
G1RC=$?
set -e
tail -4 "$WORK/gen1-link.err" | sed 's/^/   /'
if [ "$G1RC" -ne 0 ]; then
    if [ "${TCC_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: gen-1 TCC.EXE (seed) build failed — regression against vms-4ba.4."; exit 1
    fi
    echo "SKIP (TCC_EXPECT_LINK=0): gen-1 TCC.EXE build failed but the assertion is disabled."; exit 2
fi
readelf -lW "$SYSEXE/TCC.EXE" | grep -q 'INTERP' || { echo "FAIL: gen-1 TCC.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/TCC.EXE"
cp "$SYSEXE/TCC.EXE" "$SYSEXE/TCC1.EXE"     # keep the seed distinct from gen-2
"$SYSEXE/TCC1.EXE" -v > "$WORK/gen1-v.out" 2>&1 || true
grep -qi 'tcc version' "$WORK/gen1-v.out" || { echo "FAIL: gen-1 TCC.EXE did not activate / print version"; exit 1; }
echo "-- gen-1 seed TCC.EXE activates VMS-native: $(grep -i 'tcc version' "$WORK/gen1-v.out") --"

# ---------------------------------------------------------------------------
# The exact per-TU recipe mk_tcc.sh uses — replicated so gen-2/gen-3 compile
# with IDENTICAL flags, but with the ACTIVATED TCC.EXE as the compiler instead
# of gcc. The 12-object native shape (mk_tcc.sh header): 9 core TUs, 2 wrapper
# TUs (-DONE_SOURCE=0), + the OVMX RMS I/O shim.
# ---------------------------------------------------------------------------
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_RMS_IO -DOVMX_DATA_RO_WRITABLE"
INCS="-I$MKTCC_WORK -I$TCC_SRC -I$OVMX_DIR"
CORE_TUS="tccpp tccgen tccdbg tccasm tccelf tccrun arm64-gen arm64-link arm64-asm"
WRAPPER_TUS="tcc libtcc"

# Canonical copy of every TU source, chmod 666 so gen-N tcc can read it through
# RMS (sys$open's protection check — the same pre-existing, unrelated cross-
# module nibble mismatch run_tcc_native.sh/run_tcc_rms.sh document — denies root
# read on a mode-644 file; 666 forces the fully-permissive nibble). BOTH gen-2
# and gen-3 compile from THESE identical paths so tcc's embedded STT_FILE source-
# path symbol matches, which is a precondition for the byte-identity check.
TUSRC="$WORK/tus"
mkdir -p "$TUSRC"
for t in $CORE_TUS $WRAPPER_TUS; do cp "$TCC_SRC/$t.c" "$TUSRC/$t.c"; chmod 666 "$TUSRC/$t.c"; done
cp "$OVMX_DIR/ovmx_rms_io.c" "$TUSRC/ovmx_rms_io.c"; chmod 666 "$TUSRC/ovmx_rms_io.c"

# compile_all <compiler-EXE> <out-obj-dir> <log-prefix>
#   Runs the ACTIVATED tcc (IMGACT) once per TU. RMS delivers each object as a
#   VMS-versioned "<name>.o;1"; normalise to a plain "<name>.o" for the link.
compile_all() {
    _cc="$1"; _od="$2"; _lp="$3"
    rm -rf "$_od"; mkdir -p "$_od"
    _objs=""
    for t in $CORE_TUS; do
        rm -f "$_od/$t.o" "$_od/$t.o;1"
        set +e
        "$_cc" -c "$TUSRC/$t.c" -o "$_od/$t.o" $DEFS $INCS > "$_od/$t.log" 2>&1
        _rc=$?
        set -e
        [ "$_rc" -eq 0 ] || { echo "FAIL: $_lp tcc could not compile $t.c (exit $_rc):"; grep -v 'OVMX-RMS:' "$_od/$t.log" | head -20; exit 1; }
        [ -f "$_od/$t.o;1" ] && cp "$_od/$t.o;1" "$_od/$t.o"
        [ -f "$_od/$t.o" ] || { echo "FAIL: $_lp produced no object for $t"; exit 1; }
        _objs="$_objs $_od/$t.o"
    done
    for t in $WRAPPER_TUS; do
        rm -f "$_od/$t.o" "$_od/$t.o;1"
        set +e
        "$_cc" -c "$TUSRC/$t.c" -o "$_od/$t.o" $DEFS -DONE_SOURCE=0 $INCS > "$_od/$t.log" 2>&1
        _rc=$?
        set -e
        [ "$_rc" -eq 0 ] || { echo "FAIL: $_lp tcc could not compile $t.c (exit $_rc):"; grep -v 'OVMX-RMS:' "$_od/$t.log" | head -20; exit 1; }
        [ -f "$_od/$t.o;1" ] && cp "$_od/$t.o;1" "$_od/$t.o"
        _objs="$_objs $_od/$t.o"
    done
    rm -f "$_od/ovmx_rms_io.o" "$_od/ovmx_rms_io.o;1"
    set +e
    "$_cc" -c "$TUSRC/ovmx_rms_io.c" -o "$_od/ovmx_rms_io.o" $DEFS $INCS -I"$VMSRMS_INC" -I"$LIBVMS_INC" > "$_od/ovmx_rms_io.log" 2>&1
    _rc=$?
    set -e
    [ "$_rc" -eq 0 ] || { echo "FAIL: $_lp tcc could not compile ovmx_rms_io.c (exit $_rc):"; grep -v 'OVMX-RMS:' "$_od/ovmx_rms_io.log" | head -20; exit 1; }
    [ -f "$_od/ovmx_rms_io.o;1" ] && cp "$_od/ovmx_rms_io.o;1" "$_od/ovmx_rms_io.o"
    _objs="$_objs $_od/ovmx_rms_io.o"
    GEN_OBJS="$_objs"
}

# link_tcc <out-EXE> <objs...>  — mirrors mk_tcc.sh's final link exactly
#   (--executable --use {6 producers} --allow-undefined). Asserts the deferred-
#   import set is bounded and known (see the DEFEXT guard below).
link_tcc() {
    _out="$1"; shift
    set +e
    # shellcheck disable=SC2086
    "$WORK/LINK.EXE" --executable --allow-undefined $PRODUCERS -o "$_out" "$@" 2>"$WORK/$(basename "$_out").link.err"
    _lrc=$?
    set -e
    tail -3 "$WORK/$(basename "$_out").link.err" | sed 's/^/   /'
    if [ "$_lrc" -ne 0 ]; then
        if [ "${TCC_EXPECT_LINK:-1}" = "1" ]; then
            echo "FAIL: LINK.EXE --executable failed building $_out (exit $_lrc) — regression."; exit 1
        fi
        echo "SKIP (TCC_EXPECT_LINK=0): $_out link failed but the assertion is disabled."; exit 2
    fi
    readelf -lW "$_out" | grep -q 'INTERP' || { echo "FAIL: $_out has no PT_INTERP (IMGACT)"; exit 1; }
    chmod +x "$_out"
    # DEFEXT guard: with __negtf2 now a DECC$SHR universal, a self-hosted
    # TCC.EXE's ONLY remaining deferred cross-image reference is `environ`
    # (tccrun.c's `-run` execve-argv path — dead code for `tcc -c`, and a
    # genuine LINK.EXE BSS-universal-ordering gap that keeps it un-exportable;
    # see mk_decc_shr.sh / mk_tcc.sh). Assert EXACTLY 1 deferred import, same as
    # the gcc-built gen-1 — proof no OTHER symbol silently went unresolved.
    _nd=$(grep -oE 'LINK-I-DEFEXT, [0-9]+' "$WORK/$(basename "$_out").link.err" | grep -oE '[0-9]+')
    if [ "${_nd:-0}" -ne 1 ]; then
        echo "FAIL: expected exactly 1 deferred import (environ) for $_out, LINK.EXE reported ${_nd:-0}"; exit 1
    fi
}

echo
echo "================================================================================"
echo "== gen-2: the ACTIVATED gen-1 TCC.EXE (IMGACT, no gcc/no ld.so) compiles ALL   =="
echo "==        12 tcc TUs, then LINK.EXE links a SECOND TCC.EXE                      =="
echo "================================================================================"
compile_all "$SYSEXE/TCC1.EXE" "$WORK/gen2-obj" "gen-2"
G2_OBJS="$GEN_OBJS"
NOBJ=$(echo $G2_OBJS | wc -w)
[ "$NOBJ" -eq 12 ] || { echo "FAIL: expected 12 gen-2 objects, got $NOBJ"; exit 1; }
echo "-- gen-1 TCC.EXE compiled all 12 tcc TUs VMS-native (no Unix compiler touched them) --"
# shellcheck disable=SC2086
link_tcc "$SYSEXE/TCC2.EXE" $G2_OBJS
echo "-- gen-2 TCC.EXE linked + has PT_INTERP; exactly 1 deferred import (environ) --"

echo
echo "== gen-2 TCC.EXE activates through IMGACT and reports its version =="
set +e
"$SYSEXE/TCC2.EXE" -v > "$WORK/gen2-v.out" 2>&1
G2VRC=$?
set -e
grep -v 'OVMX-RMS:' "$WORK/gen2-v.out" | sed 's/^/   /'
grep -qi 'tcc version' "$WORK/gen2-v.out" || { echo "FAIL: gen-2 TCC.EXE did not print its version banner (self-host produced a broken compiler)"; exit 1; }
[ "$G2VRC" -eq 0 ] || { echo "FAIL: gen-2 TCC.EXE -v did not exit clean (got $G2VRC)"; exit 1; }
echo "-- confirmed: the tcc-BUILT tcc activates VMS-native and runs --"

echo
echo "== PROOF: gen-2 TCC.EXE compiles hello.c -> object -> LINK.EXE -> IMGACT -> 'hello' =="
cat > "$WORK/hello.c" <<'EOF'
#include <stdio.h>
int main(void)
{
    printf("hello\n");
    return 0;
}
EOF
chmod 666 "$WORK/hello.c"
rm -f "$WORK/hello.o" "$WORK/hello.o;1"
set +e
"$SYSEXE/TCC2.EXE" -c "$WORK/hello.c" -o "$WORK/hello.o" > "$WORK/gen2-hello.out" 2>&1
G2HC=$?
set -e
grep -v 'OVMX-RMS:' "$WORK/gen2-hello.out" | sed 's/^/   /'
[ "$G2HC" -eq 0 ] || { echo "FAIL: gen-2 TCC.EXE failed to compile hello.c (exit $G2HC)"; exit 1; }
HELLO_O="$WORK/hello.o;1"        # RMS-versioned (OVMX_RMS_IO always mints ;1)
[ -f "$HELLO_O" ] || { echo "FAIL: gen-2 TCC.EXE produced no $HELLO_O"; exit 1; }
readelf -h "$HELLO_O" > "$WORK/hello.readelf.h"
grep -q 'Class:.*ELF64'                "$WORK/hello.readelf.h" || { echo "FAIL: gen-2 hello.o not ELF64"; exit 1; }
grep -q 'Type:.*REL (Relocatable file)' "$WORK/hello.readelf.h" || { echo "FAIL: gen-2 hello.o not ET_REL"; exit 1; }
grep -q 'Machine:.*AArch64'            "$WORK/hello.readelf.h" || { echo "FAIL: gen-2 hello.o not EM_AARCH64"; exit 1; }
set +e
"$WORK/LINK.EXE" --executable $PRODUCERS -o "$SYSEXE/SELFHELLO.EXE" "$HELLO_O" 2>"$WORK/selfhello-link.err"
SHLRC=$?
set -e
tail -3 "$WORK/selfhello-link.err" | sed 's/^/   /'
if [ "$SHLRC" -ne 0 ]; then
    if [ "${TCC_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: SELFHELLO.EXE (compiled by the gen-2 tcc) failed to link — regression."; exit 1
    fi
    echo "SKIP (TCC_EXPECT_LINK=0): SELFHELLO.EXE link failed but the assertion is disabled."; exit 2
fi
chmod +x "$SYSEXE/SELFHELLO.EXE"
set +e
"$SYSEXE/SELFHELLO.EXE" > "$WORK/selfhello.out" 2>&1
SHRC=$?
set -e
echo "   program output: $(cat "$WORK/selfhello.out")   exit=$SHRC"
grep -q 'hello' "$WORK/selfhello.out" || { echo "FAIL: SELFHELLO.EXE did not print 'hello'"; exit 1; }
[ "$SHRC" -eq 0 ] || { echo "FAIL: SELFHELLO.EXE did not exit clean (got $SHRC)"; exit 1; }
echo "-- confirmed: the SECOND-generation tcc is a WORKING compiler --"

echo
echo "== gen-3 (reproducibility): gen-2 TCC.EXE recompiles the SAME 12 TUs; assert byte-identical to gen-2 =="
compile_all "$SYSEXE/TCC2.EXE" "$WORK/gen3-obj" "gen-3"
G3_OBJS="$GEN_OBJS"
IDENT=0; DIFF=0; DIFFLIST=""
for t in $CORE_TUS $WRAPPER_TUS ovmx_rms_io; do
    if cmp -s "$WORK/gen2-obj/$t.o" "$WORK/gen3-obj/$t.o"; then
        IDENT=$((IDENT+1))
    else
        DIFF=$((DIFF+1)); DIFFLIST="$DIFFLIST $t"
    fi
done
echo "   gen-2 vs gen-3 objects: $IDENT identical, $DIFF differing"
[ "$DIFF" -eq 0 ] || { echo "FAIL: gen-2 != gen-3 objects differ:$DIFFLIST — self-host has NOT reached a fixpoint"; exit 1; }
echo "-- confirmed: all 12 gen-2 == gen-3 objects BYTE-IDENTICAL — the compiler has converged --"

echo
echo "================================================================================"
echo "MILESTONE (vms-4ba.6, S2 FIXPOINT): tcc COMPILES tcc INSIDE OVMX. A gcc-built"
echo "seed TCC.EXE, activated through IMGACT (NO ld / NO ld.so), compiled every one of"
echo "tcc's 12 translation units — reading each .c and writing each .o through RMS —"
echo "into a SECOND, fully VMS-native TCC.EXE. That second-generation compiler then"
echo "compiled a fresh hello.c into a working image ('hello', exit 0), AND recompiled"
echo "tcc a THIRD time producing objects BYTE-IDENTICAL to the second — the classic"
echo "self-host fixpoint. The OVMX C toolchain now reproduces its own compiler with no"
echo "Unix compiler in the loop. S2 (vms-4ba) is proven; the self-hosting north star"
echo "(vms-116) has a compiler that builds itself natively inside OVMX."
echo "================================================================================"
