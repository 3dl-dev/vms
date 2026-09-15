#!/bin/sh
# build-vax-shareable-graph.sh - build the REAL elf32-vax VMS-format (.vms$sv)
# OVMX shareable graph (rd vms-c7f7, epic vms-404 P3b;
# docs/design/vax-symbol-vector-imgact.md P3). VAX analog of the shipped Alpha
# packaging step (tools/cross-alpha/build-alpha-bootimage.sh step 1b, vms-410,
# PR #1075), scaled to what the VAX RTL actually provides.
#
# WHAT THIS PRODUCES: LIBVMS$SHR.EXE -- a genuine elf32-vax ET_DYN OVMX
# shareable (EM_VAX, ELF32, carrying `.vms$sv`), NOT an ordinary NetBSD `.so`.
# It is built from the REAL src/libvms sources (the same RTL every other arch
# ships), via a SEPARATE compile of src/vmslink/link.c with -DOVMX_LINK_ELF32
# (LINKVAX.EXE, vms-19b/P3a, already merged) -- exactly the mechanism P3a
# proved on a synthetic producer/consumer pair. This script runs it for real.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld/nm/ar/readelf + a NetBSD/vax sysroot
# (libc.a/libpthread.a/libm.a) + a host gcc for LINKVAX.EXE itself. Nothing
# here touches the host (Rule 9).
#
# ---------------------------------------------------------------------------
# WHY OVMX_LIB_TYPE stays STATIC (a corrected reading of the P3 design item):
#
# The design item's literal text says "drop OVMX_LIB_TYPE=STATIC ... the
# STATIC lever is the top CMakeLists.txt NetBSD branch". Building this script
# empirically found that flipping that lever to SHARED would be WRONG: on the
# NetBSD/vax toolchain, CMake's SHARED library type invokes the cross gcc's
# own `-shared` (an ORDINARY NetBSD ELF32 .so, ld.elf_so-shaped: PT_DYNAMIC,
# .dynsym, .hash, SONAME) -- exactly the Rule-8/R5 LARP the operator rejected
# ("not ld.elf_so, no LARP"). A genuine OVMX `.vms$sv` shareable (verified
# below) carries NO PT_DYNAMIC/.dynsym/.dynamic at all -- LINKVAX.EXE's output
# format is understood only by LINKVAX.EXE itself (--use) and IMGACT.EXE, not
# by a standard linker or ld.elf_so. So the "shareable-graph lever" is NOT
# OVMX_LIB_TYPE=SHARED; it is (a) building the RTL as POSITION-INDEPENDENT
# STATIC archives (an add_library(STATIC) target compiled -fPIC -- CMake
# already honors CMAKE_POSITION_INDEPENDENT_CODE on a STATIC target; no
# top-level CMakeLists.txt/toolchain-vax-netbsd.cmake edit is needed for
# this), then (b) whole-archive-feeding that .a into LINKVAX.EXE --shareable
# as this script does. OVMX_LIB_TYPE=STATIC on the NetBSD branch is therefore
# CORRECT and stays untouched -- it is the right INPUT format for step (b),
# not a blanket this script needs to lift.
#
# ---------------------------------------------------------------------------
# R4 (TLS) -- MEASURED, NOT A BLOCKER:
#
# src/libvms genuinely uses `_Thread_local` (lib_signal.c, arith_signal.c,
# lib_invo.c, sys_setexv.c). Empirically compiling it -fPIC for vax--netbsdelf
# (gcc 13.3.0) shows this target has NO native VAX ELF TLS ABI at all -- no
# R_VAX_TLS_* relocation type exists in this toolchain. `_Thread_local` lowers
# to gcc's portable EMUTLS model: an ordinary call to `__emutls_get_address`
# (libgcc, R_VAX_PLT32 -- the SAME import-call reloc class IMGACT/LINKVAX
# already handle) plus an ordinary R_VAX_32 pointer to a `__emutls_t.*`
# control block, which in turn calls the NetBSD libc pthread-key primitives
# (__libc_thr_get/setspecific, __libc_thr_keycreate) -- again ordinary
# R_VAX_JMP_SLOT imports. There are ZERO TLS-class relocations to resolve.
# Confirmed by inspecting the built shareable below (§ assertions): the design
# R4 constraint (imgact_arch.h defines NO IMGACT_R_TLSDESC/DTPMOD) is
# satisfied by construction, not by omission -- the ABI itself has nothing
# else to define.
#
# ---------------------------------------------------------------------------
# DEFERRED (cross-shareable / runtime) IMPORTS -- vms-61f, --allow-undefined:
#
# In standalone mode (LIBVMS_STANDALONE, src/libvms/CMakeLists.txt) libvms
# does NOT link vmssys/vmsfs/vmsprocess -- those are header-only compile deps
# for the handful of TUs that call into them (sys_lock.c/sys_qio.c ->
# vms_kif_*; sys_imgact.c -> vmsfs_*; ast.c -> vms_pcb_*). Those calls are
# genuine CROSS-SHAREABLE imports (satisfied by LIBVMSSYS$SHR/LIBVMSFS$SHR/
# LIBVMSPROCESS$SHR -- not yet packaged for vax, a later increment) and are
# left as LINK.EXE's documented "deferred import" mechanism
# (%LINK-I-DEFEXT, vms-61f) via --allow-undefined, exactly like `.vms$wimp`
# weak imports elsewhere. A small set of process-startup globals
# (__progname/__ps_strings/environ/_end, all referenced weakly by NetBSD libc
# internals) and one libm helper (CMPLX) are deferred the same way. The
# EXPECTED_DEFERRED_PREFIXES/EXPECTED_DEFERRED_NAMES allowlist below is a
# REGRESSION GATE, not a rubber stamp: any deferred name outside it fails the
# build loudly instead of silently widening scope.
set -eu

SRC="$(pwd)"
TARGET="${TARGET:-vax--netbsdelf}"
CROSS_PREFIX="${CROSS_PREFIX:-/opt/cross}"
SYSROOT="${SYSROOT:-$CROSS_PREFIX/sysroot}"
CC="${CC:-gcc}"
VAXNM="${VAXNM:-$TARGET-nm}"
VAXAR="${VAXAR:-$TARGET-ar}"
VAXREADELF="${VAXREADELF:-$TARGET-readelf}"
BUILD_DIR="${BUILD_DIR:-/tmp/build-vax-shareable-graph}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/out}"

TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$OUT_DIR"

# ---- 1. libvms.a, position-independent, standalone netbsd-vax cross -------
echo "=== configure+build: src/libvms standalone, -fPIC, vax--netbsdelf ==="
LIBVMS_BUILD="$BUILD_DIR/libvms"
cmake -S "$SRC/src/libvms" -B "$LIBVMS_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$LIBVMS_BUILD" -j"$(nproc)"
LIBVMS_A="$LIBVMS_BUILD/libvms.a"
[ -s "$LIBVMS_A" ] || { echo "FAIL: libvms.a did not build: $LIBVMS_A"; exit 1; }
echo "OK: $LIBVMS_A"
echo

# ---- 2. LINKVAX.EXE (host tool, -DOVMX_LINK_ELF32, vms-19b/P3a) -----------
echo "=== build LINKVAX.EXE (-DOVMX_LINK_ELF32, src/vmslink/link.c UNCHANGED) ==="
LINKVAX="$BUILD_DIR/LINKVAX.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -DOVMX_LINK_ELF32 -I"$SRC/src/vmslink/include" \
    -o "$LINKVAX" "$SRC/src/vmslink/link.c"
[ -x "$LINKVAX" ] || { echo "FAIL: LINKVAX.EXE did not build"; exit 1; }
echo "OK: $LINKVAX"
echo

# ---- 3. NetBSD/vax static libs, selective (.OLB) archive-member pull ------
# .a stays whole-archive in LINK.EXE's OVMX policy (load_archive); an .OLB
# extension routes through the SAME ar-container reader but with classic
# selective (needed-symbols-only) member pulling (resolve_olbs, vms-ca9) --
# an .OLB is byte-identical `ar`, selected by extension alone
# (src/vmslink/include/ovmx_olb.h). Renaming the sysroot's plain `ar`
# archives is therefore sufficient; no repackaging.
echo "=== stage NetBSD/vax static libs as selective .OLB inputs ==="
for lib in libc libpthread libm; do
    src="$SYSROOT/usr/lib/$lib.a"
    [ -s "$src" ] || { echo "FAIL: sysroot static lib missing: $src"; exit 1; }
    cp "$src" "$BUILD_DIR/$lib.OLB"
    echo "OK: staged $lib.OLB"
done

# libgcc.a is whole-archived at the WRONG grain (some members duplicate
# NetBSD libc's own compiler-support symbols, e.g. __fixdfdi, -> MULDEF).
# Extract only the specific compiler-runtime helpers the RTL's compiled
# output actually references (measured via OVMX_LINK_DUMP_UNDEF=1, see
# script header): the EMUTLS emulation entry points (the R4 TLS path) and
# the two soft-float DI<->DF conversion helpers VAX has no hardware op for.
LIBGCC_A="$($CC -print-libgcc-file-name 2>/dev/null || true)"
if [ -z "$LIBGCC_A" ] || command -v "$TARGET-gcc" >/dev/null 2>&1; then
    LIBGCC_A="$("$TARGET-gcc" -print-libgcc-file-name)"
fi
[ -s "$LIBGCC_A" ] || { echo "FAIL: vax--netbsdelf libgcc.a not found"; exit 1; }
mkdir -p "$BUILD_DIR/gccx"
( cd "$BUILD_DIR/gccx" && "$VAXAR" x "$LIBGCC_A" _floatdidf.o _floatundidf.o emutls.o )
for o in _floatdidf.o _floatundidf.o emutls.o; do
    [ -s "$BUILD_DIR/gccx/$o" ] || { echo "FAIL: libgcc helper missing after extraction: $o"; exit 1; }
done
echo "OK: extracted libgcc helpers (_floatdidf.o _floatundidf.o emutls.o) from $LIBGCC_A"
echo

# ---- 4. derive the symbol vector from libvms.a's real defined globals -----
# Every GLOBAL-bound defined symbol in libvms.a becomes a universal: T (text)
# -> PROCEDURE, everything else (D/B/R -- data/bss/rodata) -> DATA. This is
# the RTL's genuine public surface (lib$*/str$*/mth$*/ots$*/sys_*/dsc$*/cli$*
# etc, ~387 names), not a hand-picked subset.
echo "=== derive --symbol-vector from libvms.a (defined GLOBAL symbols) ==="
SYMLIST="$BUILD_DIR/symlist.txt"
"$VAXNM" --defined-only -g -P "$LIBVMS_A" 2>/dev/null \
    | awk 'NF>=2 && $1 !~ /:$/ {print $1, $2}' \
    | grep -v '^ovmx\$arith_signal_anchor ' \
    | grep -v '^ovmx\$accvio_signal_anchor ' \
    > "$SYMLIST"
# ovmx$arith_signal_anchor (src/libvms/rtl/arith_signal.c) and its sibling
# ovmx$accvio_signal_anchor (src/libvms/rtl/accvio_signal.c, added by vms-cc8)
# are `__attribute__((used))` FORCE-PULL anchors -- their only purpose is to be
# `extern`-referenced by a companion "*_bind.c" TU (not built by this
# standalone standalone target) so a *selective* (needed-symbols) static
# linker drags the .c.o into a -static image (the pattern
# src/vmslink/dcl_rms_bind.c documents). They are internal linker plumbing, not
# genuine RTL API, and LINKVAX.EXE's global-symbol resolver (measured:
# every other defined global in this same object, including its sibling
# ovmx$arith_signal_installed, resolves fine) does not resolve such an anchor as
# a universal in this whole-archive shareable build. Excluded rather than
# force-exported for symbols nothing is meant to consume as a universal.
# NOTE: any future ovmx$*_signal_anchor force-pull anchor must be excluded here
# the same way (it will otherwise red this gate as a %LINK-F unresolved universal).
NSYM=$(wc -l < "$SYMLIST")
[ "$NSYM" -gt 0 ] || { echo "FAIL: no defined global symbols found in libvms.a"; exit 1; }
VEC="$BUILD_DIR/vec.txt"
awk '{ if ($2=="T") t="PROCEDURE"; else t="DATA"; print $1"="t }' "$SYMLIST" \
    | paste -sd, - > "$VEC"
echo "OK: $NSYM universals derived -> $VEC"
echo

# ---- 5. link: LIBVMS$SHR.EXE, a genuine elf32-vax .vms$sv shareable -------
echo "=== LINKVAX.EXE --shareable : LIBVMS\$SHR.EXE (elf32-vax .vms\$sv) ==="
OUT_IMG="$OUT_DIR/LIBVMS\$SHR.EXE"
LINKLOG="$BUILD_DIR/link.log"
set +e
OVMX_LINK_DUMP_UNDEF=1 "$LINKVAX" --shareable --allow-undefined \
    --symbol-vector "$(cat "$VEC")" --gsmatch EQUAL,1,0 \
    -o "$OUT_IMG" \
    "$LIBVMS_A" \
    "$BUILD_DIR/gccx/_floatdidf.o" "$BUILD_DIR/gccx/_floatundidf.o" "$BUILD_DIR/gccx/emutls.o" \
    "$BUILD_DIR/libc.OLB" "$BUILD_DIR/libpthread.OLB" "$BUILD_DIR/libm.OLB" \
    >"$LINKLOG" 2>&1
RC=$?
set -e
cat "$LINKLOG"
[ "$RC" -eq 0 ] || { echo "FAIL: LINKVAX.EXE exited $RC building LIBVMS\$SHR.EXE"; exit 1; }
grep -q '%LINK-S-CREATED' "$LINKLOG" \
    || { echo "FAIL: no %LINK-S-CREATED banner -- LIBVMS\$SHR.EXE was not produced"; exit 1; }
[ -s "$OUT_IMG" ] || { echo "FAIL: $OUT_IMG missing/empty after a reported success"; exit 1; }
echo "OK: $OUT_IMG"
echo

# ---- 6. regression-gate the deferred-import set (no silent scope creep) ---
echo "=== assert: every deferred import is an EXPECTED cross-shareable/runtime dep ==="
DEFERRED="$BUILD_DIR/deferred.txt"
grep '^DEFERRED-UNDEF: ' "$LINKLOG" | awk '{print $2}' | sort -u > "$DEFERRED"
UNEXPECTED=0
while IFS= read -r name; do
    case "$name" in
        vms_kif_*|vmsfs_*|vms_pcb_*) ;;                      # LIBVMSSYS/LIBVMSFS/LIBVMSPROCESS (not yet packaged for vax)
        CMPLX|__progname|__ps_strings|environ|_end) ;;       # libm helper / NetBSD libc process-startup globals
        *)
            echo "FAIL: unexpected deferred import '$name' -- not on the allowlist (regression or a new real dependency; update the allowlist deliberately if it's the latter)"
            UNEXPECTED=1
            ;;
    esac
done < "$DEFERRED"
[ "$UNEXPECTED" -eq 0 ] || exit 1
echo "OK: $(wc -l < "$DEFERRED") deferred imports, all expected cross-shareable/runtime deps:"
sed 's/^/   /' "$DEFERRED"
echo

# ---- 7. readelf-shape assertions (the done-condition bar) -----------------
echo "=== ASSERT: LIBVMS\$SHR.EXE is a genuine elf32-vax .vms\$sv shareable ==="
"$VAXREADELF" -h "$OUT_IMG" | grep -E "Class:|Data:|Type:|Machine:" | sed 's/^/   /'
"$VAXREADELF" -h "$OUT_IMG" | grep -q "Class:.*ELF32" \
    || { echo "FAIL: not ELF32"; exit 1; }
"$VAXREADELF" -h "$OUT_IMG" | grep -qi "Digital VAX" \
    || { echo "FAIL: not EM_VAX (Digital VAX)"; exit 1; }
"$VAXREADELF" -h "$OUT_IMG" | grep -qE "Type:.*DYN" \
    || { echo "FAIL: not ET_DYN"; exit 1; }
"$VAXREADELF" -SW "$OUT_IMG" | grep -qE '\.vms\$sv' \
    || { echo "FAIL: missing .vms\$sv section"; exit 1; }
# Self-contained: an OVMX shareable carries NO ELF dynamic relocations (its
# load-bias fixups live in .vms$rel, applied by IMGACT, not ld.elf_so/a
# standard dynamic linker). A stray R_VAX_* here would mean a reloc was left
# unapplied -- and would also mean this is NOT an ordinary ld.elf_so .so
# (which would carry PT_DYNAMIC + a full .rela.dyn instead).
if "$VAXREADELF" -r "$OUT_IMG" 2>/dev/null | grep -q "R_VAX"; then
    echo "FAIL: LIBVMS\$SHR.EXE carries UNAPPLIED R_VAX_* dynamic relocations"
    exit 1
fi
"$VAXREADELF" -lW "$OUT_IMG" | grep -q "PT_DYNAMIC" \
    && { echo "FAIL: LIBVMS\$SHR.EXE carries a PT_DYNAMIC segment -- it is not a genuine self-contained OVMX .vms\$sv shareable (this would be the ld.elf_so-shaped LARP the operator rejected)"; exit 1; }
echo "   OK: self-contained elf32-vax ET_DYN (no PT_DYNAMIC, no unapplied R_VAX_* relocs)"
# Spot-check real RTL universals are actually exported (not a synthetic
# stand-in): a handful of genuine sys$/lib$ entry points. Extract the raw
# section bytes (objcopy) rather than grepping `readelf -x`'s hex dump --
# a name can straddle that dump's fixed 16-byte-per-line boundary and be
# missed by a naive line-grep even though it is genuinely present.
SVBIN="$BUILD_DIR/vms_sv.bin"
"$TARGET-objcopy" -O binary --only-section='.vms$sv' "$OUT_IMG" "$SVBIN"
for u in 'lib$sys_fao' 'lib$sys_getmsg' 'vms_status_string'; do
    strings "$SVBIN" | grep -qF "$u" \
        || { echo "FAIL: .vms\$sv does not export the real RTL universal '$u'"; exit 1; }
done
echo "   OK: .vms\$sv exports real RTL universals (lib\$sys_fao, lib\$sys_getmsg, vms_status_string, ...)"

echo
echo "ALL VAX SHAREABLE-GRAPH CHECKS PASSED: $OUT_IMG"
echo "  ($NSYM universals; $(wc -l < "$DEFERRED") deferred cross-shareable/runtime imports)"
