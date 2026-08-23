#!/bin/sh
# mk_decc_shr.sh — build recipe for DECC$SHR.EXE, the OVMX C run-time library
# shareable image (bead vms-61f.1, pillar vms-ade).
#
# DECC$SHR.EXE is the whole musl `libc.a` linked by LINK.EXE (the OVMX VMS-native
# linker — NO ld/ld.so) into a single OVMX shareable image (ELF ET_DYN) that
# carries a `.vms$sv` symbol vector exposing the C run-time universals
# (malloc/free/memcpy/strlen/printf/...) as PROCEDURE universals. Consumers bind
# to it through the symbol vector at activation, exactly as VMS images bind to
# DECC$SHR on OpenVMS.
#
# Composition (VMS-native, whole-archive, in-process — no `ld -r`):
#   libc.a   — musl C library (all 1345 members pulled)
#   libgcc.a — the compiler runtime: soft-float / long-double / complex builtins
#              (__addtf3, __trunctfdf2, __fixtfsi, __multc3, ...). musl's stdio /
#              printf reference these internally; they are resolved WITHIN the
#              image and kept INTERNAL — a C-RTL consumer never calls them, so
#              they are NOT exported as universals.
#
# Not exported / linker-defined (resolve to 0, the correct empty value): musl's
# libc.a + libgcc.a carry NO .init_array/.fini_array sections and no dynamic
# section, so __init_array_start/end, __fini_array_start/end and _DYNAMIC are
# empty/null — LINK.EXE resolves these WEAK-undefined symbols to 0 (standard ELF
# semantics). The image therefore links with ZERO deferred externals.
#
# THIS RECIPE PRODUCES THE SHAREABLE ONLY. Runtime init (musl bootstrap, thread-
# pointer/TCB setup, __libc_start_main, running constructors) is vms-61f.2.
#
# Usage:  mk_decc_shr.sh <LINK.EXE> <out-DECC$SHR.EXE> [libc.a] [libgcc.a]
# Env:    GSMATCH (default LEQUAL,1,0)
#
# Must run where an aarch64 musl `libc.a` + `libgcc.a` exist (the arm64 musl
# container — see CLAUDE.md test loop). Otherwise arch-neutral: LINK.EXE
# derives e_machine from the input libc.a/libgcc.a members, so pointing $2/$3
# at an x86_64 musl libc.a + libgcc.a (vms-cb5f) produces an x86_64 DECC$SHR --
# MODULO the TLS filter below, which the x86_64 build actually needs.
set -e

LINK_EXE=${1:?usage: mk_decc_shr.sh <LINK.EXE> <out> [libc.a] [libgcc.a]}
OUT=${2:?usage: mk_decc_shr.sh <LINK.EXE> <out> [libc.a] [libgcc.a]}
LIBC=${3:-/usr/lib/libc.a}
LIBGCC=${4:-$(gcc -print-libgcc-file-name)}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -f "$LIBC" ]   || { echo "mk_decc_shr: libc.a not found: $LIBC (need the arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "mk_decc_shr: libgcc.a not found: $LIBGCC"; exit 1; }

# ==========================================================================
# ALPHA / EVAX BRANCH (vms-7b96 RUNG-1) — a DEDICATED path for alpha-dec-vms
# input archives, taken BEFORE any of the x86_64/aarch64 (ELF) machinery below.
#
# WHY A SEPARATE PATH. On alpha the GCC port itself DECORATES the C-RTL surface
# at codegen: musl-alpha DEFINES `decc$strlen`, `decc$_malloc64` (malloc under
# -mpointer-size=64), `decc$_memcpy64`, `decc$tprintf` (printf's IEEE T_float
# variant), ... — it does NOT define the plain names. A port client imports those
# DECORATED names directly. So DECC$SHR must export the decc$* names DIRECTLY
# (universal == the defined symbol), NOT the x86_64 `decc$<name>/<name>`
# alias-to-plain vector (there is no plain <name> to alias to), and the static
# plain-name VEC below would resolve to nothing. The x86_64 TLS/fortify
# filter_tls_members and the decc_crtl_map alias loop are likewise inapplicable
# (alpha long double is IEEE binary64 = 8 bytes, so no 128-bit tf soft-float; no
# GD-TLS members). And bare nm/ar/readelf are HOST binutils, which cannot read
# VMS-native EVAX objects at all — the caller passes the cross tools via NM/AR.
#
# The vector is GROUND-TRUTHED from the archives: enumerate every decc$* symbol
# libc.a + libgcc.a actually DEFINE and export each as a universal (text ->
# PROCEDURE, data -> DATA), skipping the EVAX companion labels (..en/..ng/..lk/
# ..lita — the callable value is the bare descriptor). A name the port did not
# emit is simply ABSENT (a visible gap for a later rung), never faked. LINK.EXE's
# EVAX archive front end (link.c load_archive_evax) whole-archives both inputs.
#
# THE OVMX BOOTSTRAP SURFACE (vms-864). musl-alpha's own decc$ decoration
# covers everything musl DEFINES, but a port crt0 also needs OVMX-authored
# glue no C library defines: decc$main (the DEC C RTL image-startup routine)
# and C$_EXIT1 (a linker globalvalue, not code at all). Below (after the
# libc.a/libgcc.a enumeration) this branch cross-compiles the SAME
# ovmx_decc_crtl.c / ovmx_libc_stub.c the generic tail uses, with the
# alpha-dec-vms cc1 — whose own CRTL auto-decoration turns their plain
# _malloc32/_malloc64/get_errno_addr/get_vms_errno_addr definitions into the
# decc$-decorated universals a port image actually imports, for free.
# ==========================================================================
NM=${NM:-nm}
OVMX_DECC_ARCH=${OVMX_DECC_ARCH:-auto}
if [ "$OVMX_DECC_ARCH" = auto ]; then
    if "$NM" --defined-only "$LIBC" 2>/dev/null | grep -q 'decc\$'; then
        OVMX_DECC_ARCH=alpha
    else
        OVMX_DECC_ARCH=generic
    fi
fi

if [ "$OVMX_DECC_ARCH" = alpha ]; then
    echo "mk_decc_shr: ALPHA/EVAX branch (vms-7b96)"
    echo "mk_decc_shr: LINK.EXE=$LINK_EXE  libc.a=$LIBC  libgcc.a=$LIBGCC  GSMATCH=$GSMATCH"
    ALPHA_VEC=$(mktemp)
    # ENUMERATE decc$ defs. The cross nm/ar's vms-alpha BFD rejects a System V ar
    # container ("file format not recognized"), and bare host nm cannot read EVAX
    # objects at all — but host `ar` extracts the (format-agnostic) container fine,
    # and the cross nm reads a bare EVAX .o. So extract with host ar, nm each member
    # with the cross nm. libc.a MUST be built -g0 for this (the cross nm cannot read
    # DST members, vms-7b96); the shipped DECC$SHR is byte-identical either way
    # (LINK.EXE skips DST). text (T) -> PROCEDURE, data (D/G/R/B) -> DATA; drop the
    # ..-suffixed EVAX companion labels (the callable value is the bare descriptor).
    AR_HOST=${AR_HOST:-ar}
    EXDIR=$(mktemp -d)
    ( cd "$EXDIR" && "$AR_HOST" x "$LIBC" && [ -s "$LIBGCC" ] && "$AR_HOST" x "$LIBGCC" 2>/dev/null || true )
    "$NM" --defined-only "$EXDIR"/*.o 2>/dev/null \
      | awk '
          $NF ~ /^decc\$/ && $NF !~ /\.\.[a-z]+$/ {
              t=$(NF-1); n=$NF;
              # text (incl. weak text W) -> PROCEDURE; data (incl. weak data V) -> DATA
              if (t=="T"||t=="t"||t=="W"||t=="w") print n"=PROCEDURE";
              else if (t~/^[DdGgRrBbVv]$/) print n"=DATA";
          }' \
      | sort -u > "$ALPHA_VEC"
    rm -rf "$EXDIR"
    NVEC=$(wc -l < "$ALPHA_VEC")
    echo "mk_decc_shr: enumerated $NVEC decc\$ universals from libc.a + libgcc.a"
    [ "$NVEC" -ge 50 ] || { echo "mk_decc_shr: FAIL only $NVEC decc\$ symbols found — is NM the alpha-dec-vms cross nm and libc.a built -g0 (nm cannot read DST, vms-7b96)?" >&2; exit 2; }
    VEC=$(paste -sd, "$ALPHA_VEC")
    rm -f "$ALPHA_VEC"

    # ----------------------------------------------------------------------
    # OVMX BOOTSTRAP SURFACE ON ALPHA (vms-864). A real alpha-dec-vms port
    # crt0 (libgcc/config/vms/vms-ucrt0.c) references decc$main + C$_EXIT1
    # UNCONDITIONALLY — musl-alpha defines neither (decc$main is not a musl
    # function at all; C$_EXIT1 is a linker globalvalue, not code). Without
    # them every port image fails "%LINK-F-UNDEF" against an otherwise-
    # complete genuine alpha DECC$SHR. Mirror the generic tail's OVMX-authored
    # bootstrap objects (ovmx_decc_crtl.c / ovmx_libc_stub.c) here, cross-
    # compiled to real EVAX objects by the alpha-dec-vms cc1 (tools/cross-
    # alpha-vms), and whole-archived alongside libc.a/libgcc.a.
    #
    # WHY NOT A PLAIN PORT OF THE GENERIC TAIL. The alpha-dec-vms GCC port
    # cc1 ITSELF auto-decorates any function whose SOURCE NAME matches a
    # recognized DEC C RTL surface name — transparently, at every definition
    # AND call site (gcc/config/vms/vms.cc's crtlmap; verified empirically
    # against the cached toolchain, vms-864). Confirmed by direct compile:
    #   _malloc32(int)          -> decc$malloc      (the crtlmap "_X32->X" rule)
    #   _malloc64(unsigned long)-> decc$_malloc64    (the "_X64->X" rule)
    #   get_errno_addr(void)    -> decc$get_errno_addr    (real DEC C RTL name)
    #   get_vms_errno_addr(void)-> decc$get_vms_errno_addr (real DEC C RTL name)
    # ovmx_decc_crtl.c ALREADY defines _malloc32/_malloc64/get_errno_addr/
    # get_vms_errno_addr under those exact plain names for the generic
    # (x86_64/aarch64, non-VMS-native-compiler) branch — so compiling that
    # SAME file with the alpha cc1 makes those four decorated universals fall
    # out for free, with no source change. The file's OWN decc$malloc /
    # decc$_malloc64 / decc$tprintf WRAPPER functions (asm-relabeled explicitly)
    # are the generic branch's substitute for that auto-decoration and are
    # NOT needed on alpha — compiling them too is a genuine duplicate
    # definition (confirmed: GAS rejects the .s with "symbol 'decc$malloc..en'
    # is already defined"), so they are '#ifndef __VMS__'-guarded out in
    # ovmx_decc_crtl.c (the alpha-dec-vms cc1 predefines __VMS__; a host gcc
    # building the generic branch does not) — this is not "#ifdef away needed
    # functionality" (Rule: adapt minimally/honestly): the functionality
    # (decc$malloc/decc$_malloc64) is still produced, by the port's own
    # codegen from the SAME _malloc32/_malloc64 source, not omitted.
    # decc$tprintf is likewise already produced by musl-alpha's OWN printf.c
    # (the "printf FLOAT64" crtlmap rule prepends 't', then decc$-decorates —
    # confirmed present in the plain libc.a+libgcc.a enumeration above, no
    # ovmx_decc_crtl.c involvement needed for it at all).
    #
    # ovmx_decc_crtl.c needs real musl-alpha headers (pthread.h/stdio.h/
    # errno.h/sys/mman.h) for correct struct/ABI layouts, not host ones —
    # ALPHA_MUSL_SRC must point at the musl-1.2.5 source tree used to build
    # $LIBC (tools/cross-alpha-vms/musl-arch/build-musl.sh's $WORK/musl-*).
    # Fail honestly (no silent skip, INV-6) if it is not given: the bootstrap
    # surface would otherwise be silently absent from DECC$SHR and every port
    # image would defer decc$main, hiding the real gap this bead closes.
    ALPHA_CC=${ALPHA_CC:-alpha-dec-vms-gcc}
    : "${ALPHA_MUSL_SRC:?mk_decc_shr: alpha branch needs ALPHA_MUSL_SRC=<musl-1.2.5 src tree used to build \$LIBC> to compile ovmx_decc_crtl.c's real DEC C RTL headers (get_errno_addr/pthread/stdio) -- see tools/cross-alpha-vms/musl-arch/build-musl.sh}"
    [ -d "$ALPHA_MUSL_SRC" ] || { echo "mk_decc_shr: FAIL ALPHA_MUSL_SRC=$ALPHA_MUSL_SRC is not a directory" >&2; exit 2; }
    ALPHA_MUSL_INC="-I$ALPHA_MUSL_SRC/arch/alpha-dec-vms -I$ALPHA_MUSL_SRC/arch/generic -I$ALPHA_MUSL_SRC/obj/src/internal -I$ALPHA_MUSL_SRC/src/include -I$ALPHA_MUSL_SRC/src/internal -I$ALPHA_MUSL_SRC/obj/include -I$ALPHA_MUSL_SRC/include"
    IMGACT_INC="$(CDPATH= cd "$(dirname "$0")/../imgact/include" && pwd)"   # ovmx_activation.h (vms-8c8)

    ALPHA_BOOT_DIR=$(mktemp -d)
    ALPHA_STUB_OBJ="$ALPHA_BOOT_DIR/ovmx_libc_stub.o"
    ALPHA_CRTL_OBJ="$ALPHA_BOOT_DIR/ovmx_decc_crtl.o"
    STUB_SRC="$(CDPATH= cd "$(dirname "$0")" && pwd)/ovmx_libc_stub.c"
    CRTL_SRC="$(CDPATH= cd "$(dirname "$0")" && pwd)/ovmx_decc_crtl.c"

    # -g0: same reason as libc.a/libgcc.a above — the cross nm cannot read
    # DST, and WE must nm these two objects ourselves next to ground-truth
    # their decc$ universals (never hand-guessed).
    "$ALPHA_CC" -c -fPIC -ffreestanding -mpointer-size=64 -g0 \
        -o "$ALPHA_STUB_OBJ" "$STUB_SRC"
    # shellcheck disable=SC2086
    "$ALPHA_CC" -c -fPIC -ffreestanding -mpointer-size=64 -g0 \
        $ALPHA_MUSL_INC -I"$IMGACT_INC" -o "$ALPHA_CRTL_OBJ" "$CRTL_SRC"

    # Ground-truth the decc$ universals these two objects ACTUALLY define —
    # same enumeration as libc.a/libgcc.a above, never a hardcoded name list.
    ALPHA_BOOT_VEC=$(mktemp)
    "$NM" --defined-only "$ALPHA_STUB_OBJ" "$ALPHA_CRTL_OBJ" 2>/dev/null \
      | awk '
          $NF ~ /^decc\$/ && $NF !~ /\.\.[a-z]+$/ {
              t=$(NF-1); n=$NF;
              if (t=="T"||t=="t"||t=="W"||t=="w") print n"=PROCEDURE";
              else if (t~/^[DdGgRrBbVv]$/) print n"=DATA";
          }' | sort -u > "$ALPHA_BOOT_VEC"
    NBOOT=$(wc -l < "$ALPHA_BOOT_VEC")
    echo "mk_decc_shr: enumerated $NBOOT decc\$ universals from the OVMX bootstrap surface (ovmx_decc_crtl.c/ovmx_libc_stub.c)"
    [ "$NBOOT" -ge 1 ] || { echo "mk_decc_shr: FAIL 0 decc\$ universals from the bootstrap surface -- did the alpha-dec-vms cc1's CRTL auto-decoration not fire (wrong ALPHA_CC)?" >&2; exit 2; }
    VEC="$VEC,$(paste -sd, "$ALPHA_BOOT_VEC")"
    rm -f "$ALPHA_BOOT_VEC"

    # Plain (non-decc$-decorated) names the decc$ filter above cannot catch,
    # each verified present by direct nm before being claimed here:
    #   __copy_tls / __init_tp -- musl-alpha's OWN TLS bootstrap entry points
    #     (plain C names; "musl-internal", not a recognized DEC C RTL surface
    #     name, so the cc1 does not decorate them). IMGACT's activation-time
    #     TLS setup (vms-c07) resolves these BY NAME, same as the generic
    #     (x86_64/aarch64) DECC$SHR.
    #   ovmx_get_libc -- OVMX loader glue (ovmx_libc_stub.c), likewise a
    #     plain OVMX-original name never decorated.
    VEC="$VEC,__copy_tls=PROCEDURE,__init_tp=PROCEDURE,ovmx_get_libc=PROCEDURE"

    # ---- vms-864 (mirrors vms-954 R1b-2b on the generic tail): C$_EXIT1 ----
    # Same architecture-independent VMS C-facility globalvalue, same oracle
    # grounding as the generic tail above (measured on OpenVMS Alpha V8.4,
    # lab-Alpha, 2026-08-22): C$_EXIT1 = 0x0035A009. LINK.EXE folds
    # `&C$_EXIT1` to this constant at link time -- never an activation import.
    VEC="$VEC,C\$_EXIT1=GLOBALVALUE:0x0035A009"

    # Strict whole-archive link (no --allow-undefined) unless the caller opts into
    # first-light deferral for the VMS runtime surface musl-alpha references.
    #
    # OTS$ runtime (vms-bfd6): the VMS integer-divide / block family the compiler
    # emits (OTS$DIV_*, OTS$REM_*, OTS$HOME_ARGS, OTS$MOVE, OTS$ZERO) is provided
    # by a SEPARATE shareable, LIBOTS$ — the faithful OpenVMS shape (LIBOTS$ is a
    # distinct image from DECC$SHR). Point DECC_USE at it (space-separated for
    # several) and its OTS$ references bind as real cross-image .vms$imp imports
    # instead of deferring. Build LIBOTS$ first with tools/cross-alpha-vms/ots/
    # build-libots.sh, then pass DECC_USE=<path-to-LIBOTS$SHR.EXE>.
    #
    # DECC_ALLOW_UNDEF=1 still records any remaining first-light residual (e.g.
    # the setjmp/cancellation surface — decc$longjmp, __cp_*, __syscall_cp_asm —
    # and the linker-defined _DYNAMIC/__init_array bounds) as deferred imports.
    ALPHA_LINK_FLAGS="--shareable --symbol-vector $VEC --gsmatch $GSMATCH"
    for p in ${DECC_USE:-}; do ALPHA_LINK_FLAGS="$ALPHA_LINK_FLAGS --use $p"; done
    [ "${DECC_ALLOW_UNDEF:-0}" = 1 ] && ALPHA_LINK_FLAGS="$ALPHA_LINK_FLAGS --allow-undefined"
    # shellcheck disable=SC2086
    "$LINK_EXE" $ALPHA_LINK_FLAGS -o "$OUT" "$LIBC" "$LIBGCC" "$ALPHA_STUB_OBJ" "$ALPHA_CRTL_OBJ"
    rm -rf "$ALPHA_BOOT_DIR"
    echo "mk_decc_shr: created $OUT (alpha/EVAX)"
    exit 0
fi

# DECC$SHR must stay a NON-TLS-producer: it whole-archives libc.a/libgcc.a for
# C-runtime support routines only and carries no __thread state of its own.
# LINK.EXE enforces "one TLS object per image" (general multi-module TLS is
# vms-212, not yet built) -- fine on aarch64, whose libgcc.a empirically
# carries ZERO TLS-defining members (366 members / 0 TLS, checked directly
# with readelf), but x86_64's libgcc.a whole-archives an entire dead-for-OVMX
# subsystem built around __thread state: GCC's bundled IEEE 754-2008 decimal
# floating-point library (bid_decimal_globals.o DEFINES two __thread globals,
# __bid_IDEC_glbflags/glbround; ~20 bid128_*/bid32_*/bid64_* arithmetic
# objects REFERENCE them via R_X86_64_TLSGD, the general-dynamic TLS model --
# OVMX/musl never uses GCC's _Decimal32/64/128 types, so none of this is ever
# called) plus gcc -fsplit-stack support (generic-morestack.o DEFINES a
# __thread stack-guard var; generic-morestack-thread.o REFERENCES it, also
# via TLSGD -- OVMX never builds with -fsplit-stack). Without this filter,
# mk_decc_shr's x86_64 build hits "%LINK-F-ERROR, multi-module TLS not
# supported yet" from the TLS-defining half, or "%LINK-F-ERROR, unsupported
# .text relocation" (TLSGD is not a reloc type LINK.EXE's x86_64 path
# resolves -- OVMX standardizes on the gnu2/TLSDESC model throughout, per
# docs/design-link-x86_64-relocs.md) from the referencing half, if only the
# definers are dropped and the referencing members are left dangling
# (discovered building this bead's DCL.EXE proof, vms-cb5f). Filtered
# architecture-generically -- any member DEFINING TLS storage (.tdata/.tbss)
# or REFERENCING it via the GD model (TLSGD) is dropped as one unit, not by
# hardcoding the four x86_64 object names -- so it is a no-op on an archive
# with neither, like aarch64's, and covers a future libgcc revision (or a
# different musl/libgcc build carrying the same dead subsystems) on either
# architecture without edits here.
#
# SECOND, INDEPENDENT taint class filtered by the SAME function (vms-0b8, the
# "Unix-leaky toolchain jobs" quarantine item; blocks vms-db2's in-process
# activation): a glibc `_FORTIFY_SOURCE` artifact. Empirically found running
# this recipe with a HOST (non-Alpine) toolchain's libgcc.a: Ubuntu/Debian
# gcc packages are themselves BUILT with `_FORTIFY_SOURCE` hardening on, so
# libgcc.a ships a member -- `_eprintf.o`, the ancient `__eprintf()` helper
# for old-style `assert()` failure messages, dead code no OVMX source calls
# -- whose OWN fprintf() call got rewritten to `__fprintf_chk` when THAT
# member was compiled, decades ago, by the distro. Nothing in musl's libc.a
# or the rest of libgcc.a defines `__fprintf_chk` (it is a glibc-only ABI
# symbol; musl's headers never rewrite fprintf() in the first place -- an
# Alpine-native (musl-target) gcc's own libgcc.a carries no such member, so
# the CI jobs that run inside the Alpine container never hit this), so
# whole-archiving an unfiltered host libgcc.a fails STRICT with "%LINK-F-
# ERROR, unresolved external symbol '__fprintf_chk'" -- a real Unix/glibc
# leak by the letter of vms-0b8, just entering through the host toolchain's
# own prebuilt libgcc.a rather than through any OVMX-compiled object. Any
# member with an unresolved `__*_chk` reference is exactly this class of
# dead weight (DECC$SHR is a musl C-RTL; it can never legitimately need a
# glibc fortify entry point), so it is seeded into the SAME tainted-member
# set below and removed by the SAME fixed-point dead-code closure as the
# TLS case -- no second closure implementation, no hardcoded object names.
#
# The direct TLS-tainted set is not the whole dead subsystem, though: gcc's
# decimal-float library has non-TLS "glue" objects that call INTO it without
# touching TLS themselves -- e.g. libgcc's _addsub_dd.o/_addsub_sd.o (the
# __bid_adddd3/__bid_subdd3/__bid_addsd3/__bid_subsd3 entry points) reference
# __bid64_add via a plain GOT call, and __bid64_add is defined ONLY by
# bid64_add.o -- which IS TLS-tainted (it references the rounding-mode
# globals via TLSGD) and gets dropped above. Left in the archive, _addsub_*.o
# would whole-archive into DECC$SHR with a GOT cell that resolve_named()
# cannot satisfy -- die()'s "GOT symbol undefined" is CORRECT (there really
# is no definition left), not a link.c gap; the gap is that the filter above
# only removes the directly-tainted half of a connected dead-code component
# (found completing vms-cb5f's x86_64 DCL.EXE proof, once vms-e5d unblocked
# the GOTPCRELX reloc that had masked this further in). Fixed by a
# reference-graph fixed-point closure below: after the direct TLS-tainted
# set is seeded, repeatedly pull in any SURVIVING member whose undefined
# reference is satisfied ONLY by an already-removed member (i.e. the symbol
# has no definition left among survivors) -- not by name, by the actual
# def/ref graph nm reports, so it generalizes to any future libgcc/musl
# revision with the same shape on either architecture, and is a no-op archive
# whose direct-tainted set is empty (aarch64's, and libc.a on both).
filter_tls_members() {
    src=$1
    dir=$(mktemp -d)
    ( cd "$dir" && ar x "$src" )

    # One nm pass over the whole archive -> "<member> <symbol>" for defined
    # (non-U) and undefined (U) symbols, split into two lookup files. Doing
    # this once up front (instead of re-invoking nm per member per iteration)
    # is what keeps the fixed-point loop below cheap on a 1345-member archive.
    defs="$dir/.defs"; refs="$dir/.refs"
    : > "$defs"; : > "$refs"
    ( cd "$dir" && nm -A ./*.o 2>/dev/null ) | awk -v defs="$defs" -v refs="$refs" '
        {
            c = index($0, ":"); if (!c) next;
            file = substr($0, 1, c-1); sub(/^\.\//, "", file);
            n = split(substr($0, c+1), a, " "); if (n < 2) next;
            type = a[n-1]; sym = a[n];
            if (type == "U") print file, sym >> refs; else print file, sym >> defs;
        }'

    removed="$dir/.removed"
    : > "$removed"
    for o in "$dir"/*.o; do
        [ -f "$o" ] || continue
        if readelf -SW "$o" 2>/dev/null | grep -qE '\.tdata|\.tbss' || \
           readelf -rW "$o" 2>/dev/null | grep -q 'TLSGD' || \
           nm "$o" 2>/dev/null | grep -qE '^[0-9a-f ]*U __[A-Za-z0-9_]*_chk$'; then
            basename "$o" >> "$removed"
        fi
    done

    # Fixed-point: keep pulling in surviving members that reference a symbol
    # only a removed member defines, until nothing new is added.
    while :; do
        new="$dir/.new_removed"
        awk '
            NR==FNR { removed[$1]=1; next }
            FILENAME==defs { if ($1 in removed) rdef[$2]=1; else sdef[$2]=1; next }
            { if (!($1 in removed) && ($2 in rdef) && !($2 in sdef) && !($1 in seen)) { print $1; seen[$1]=1 } }
        ' defs="$defs" "$removed" "$defs" "$refs" > "$new"
        [ -s "$new" ] || { rm -f "$new"; break; }
        cat "$new" >> "$removed"
        sort -u "$removed" -o "$removed"
        rm -f "$new"
    done

    if [ -s "$removed" ]; then
        echo "mk_decc_shr: filtered TLS- and/or glibc-fortify-tainted member(s) [direct + dead-code callers pulled in by the reference-graph closure] out of $(basename "$src") (DECC\$SHR stays a non-TLS, non-glibc producer, vms-212/vms-0b8): $(tr '\n' ' ' < "$removed")" >&2
        while read -r b; do rm -f "$dir/$b"; done < "$removed"
        out="$dir/filtered.a"
        ( cd "$dir" && ar rcs "$out" ./*.o )
        echo "$out"
    else
        rm -rf "$dir"
        echo "$src"
    fi
}

LIBC=$(filter_tls_members "$LIBC")
LIBGCC=$(filter_tls_members "$LIBGCC")

# The C run-time universals DECC$SHR exports. Every name is defined by musl's
# libc.a; each becomes a PROCEDURE universal in .vms$sv. (The stdin/stdout/stderr
# DATA universals are appended at the END for vms-b65.2 — musl statically
# initializes these `FILE *const` objects at image build, so a consumer's GOT
# data-import binds to the real object address via vms-e65's =DATA path. `environ`
# and other runtime-populated DATA objects still wait on vms-61f.2 runtime init.)
#
# __init_libc is musl's C-RTL bootstrap (programs the thread pointer, builds the
# TCB/TLS, sets the stack guard, makes malloc usable). It is not a consumer-
# callable universal, but IMGACT's activation bootstrap (vms-61f.2) resolves it
# BY NAME from the .vms$sv name blob to drive runtime init before transferring
# control — so the production vector MUST export it. (vms-36a)
#
# The pthread_{mutex,cond}_* + raise/sigaction/sigemptyset + getpid universals at
# the END are APPENDED (never inserted): the symbol vector is append-only, so
# existing consumers' bound vector indices stay valid — a GSMATCH-compatible
# additive change (LEQUAL). They let OVMX libs that migrate onto DECC$SHR bind
# pthread / signal / getpid (the b65 lib-migration chain; the import-binding path
# itself is vms-e65). getpid was appended for vms-b65.1 (vmsprocess's
# vms_get_current_process needs it). (calloc/close are already exported above, so
# they are not re-listed here.) pthread_once/toupper/ttyname were appended for
# vms-b65.3 (vmslnm's lnm_get_manager singleton + lnm_setup_defaults terminal
# logicals). All appended at the END — indices for prior consumers are unchanged.
# __errno_location + the ctype (isalnum/tolower) + strncasecmp + the dirent
# (opendir/readdir/closedir) + stat/realpath/unlink universals at the very END
# were appended for vms-b65.4 (vmsfs's filespec translation, ODS-2 version scan,
# case-insensitive path resolution, and device-table ops need them). Appended at
# the END — indices for prior consumers are unchanged (GSMATCH LEQUAL-compatible).
#
# The FINAL block (the libm transcendentals + the process/time/glob/pwd libc
# universals, and the stdin/stdout/stderr DATA universals) was appended for
# vms-b65.2 — the LIBVMS$SHR migration, the largest OVMX runtime. libvms's
# system services + lib$/str$/mth$/ots$ RTL import them: mth_routines.c /
# sys_float.c pull the libm transcendentals (sin/cos/tan/exp/log*/pow/sqrt/
# floor/ceil/round/fmod/fabs* + f-variants), sys_process.c / lib_misc.c the
# fork/exec*/kill/pause/*priority/get{uid,gid,pwuid,rusage} process controls,
# lib_datetime.c the time/*time_r/timegm/timer_* clocks, sys_time.c
# clock_gettime, sys_uring.c syscall/mmap/munmap, lib_logical.c glob/globfree,
# lib_output.c fwrite(...,stdout)/fprintf(stderr,...) (the stdin/stdout/stderr
# DATA universals — musl statically initializes these `FILE *const` objects, so
# a consumer's GOT data-import binds to DECC$SHR's real object, vms-e65's
# =DATA path, link.c:400). The rest (dup/dup2/mkdir/rename/socketpair/statvfs/
# sysconf/uname/strtok_r/fscanf/freopen/gmtime_r/localtime*/__libc_current_sigrtmin/
# _exit) are scattered across the RTL. ALL appended at the END (GSMATCH LEQUAL-
# compatible): prior consumers' bound vector indices are unchanged. DATA
# universals sit at the very end; only stdin/stdout/stderr are cross-image DATA
# imports for libvms (every other import is a PROCEDURE) — no producer-pointer
# ABS64 case (vms-212).
#
# 32 more PROCEDURE universals (access..utimes) were APPENDED for vms-b65.6 (DCL,
# the executable consumer): the DCL shell's 114 libc imports include these POSIX
# calls (file access/mode, cwd/chdir, terminal ioctl/tcgetattr/tcsetattr/tcflush/isatty,
# pty pipe, network inet_*/ntohl/socket for SHOW NETWORK-style probes, rlimit,
# time gettimeofday/settimeofday/mktime/strptime/utimes, getpwnam, system/execvp,
# strerror/rewind/fileno/flock/fstat/readlink/sleep/isxdigit) not previously pulled
# by any library consumer. Enumerated empirically: `nm` the 22 compiled vmsdcl
# objects for U symbols minus intra-image T defs minus the OVMX-universal set, then
# comm against this vector. All appended at the END -> prior consumers' indices
# unchanged (GSMATCH LEQUAL-compatible).
#
# 39 PROCEDURE universals APPENDED for vms-4ba.4 (tcc itself running AS an OVMX
# image, TCC.EXE): enumerated empirically the same way as the vms-b65.6 block
# above — `nm` the 11 compiled tcc TUs (tcc.o, libtcc.o, tccpp.o, tccgen.o,
# tccdbg.o, tccasm.o, tccelf.o, tccrun.o, arm64-{gen,link,asm}.o) for U symbols,
# subtract intra-tcc-image T/D/B defs (tcc's own ~584 internal symbols), comm
# against this vector. Two groups:
#   (a) 18 libgcc IEEE-quad ("tf"/128-bit long double) soft-float helpers
#       (__addtf3/__subtf3/__multf3/__divtf3, __*tf2 compares, __extend*tf2/
#       __trunctf*2 conversions, __fixtfdi/__fixunstfdi/__floatun{di,si}tf) +
#       __clear_cache. (vms-4ba.6 APPENDED a 19th quad helper, __negtf2, the
#       long-double unary-negation soft-float routine: gcc INLINES -(long
#       double) as a sign-bit flip so the gcc-built gen-1 TCC.EXE never
#       referenced it, but a SELF-HOSTED tcc — tcc compiling tcc, vms-4ba.6 —
#       emits a real __negtf2 CALL for the same expression, so the second-
#       generation TCC.EXE needs it as a cross-image import. Same
#       whole-archived/hidden-in-libgcc status as its 18 siblings.)
#       libgcc.a is ALREADY whole-archived into DECC$SHR (see the
#       header comment above — "resolved WITHIN the image and kept INTERNAL");
#       tcc's own long-double constant-folding (tccgen.c) and its arm64 JIT
#       icache flush (`-run` mode) call these as CROSS-IMAGE imports, so they
#       must now ALSO be exported, not just internally resolved.
#   (b) 20 more musl libc.a POSIX/libc calls tcc references that no prior
#       consumer needed: __assert_fail, atoi, dlclose/dlopen/dlsym (tcc's -l
#       dynamic-load path), fdopen, ldexpl, longjmp/setjmp (tcc's own
#       error-recovery jmp_buf), mprotect (tcc's -run JIT page permissions),
#       remove, sem_init/sem_post/sem_wait, sigaddset/sigprocmask, strpbrk,
#       strtof/strtold/strtoull. All confirmed present in musl libc.a as WEAK
#       ('W') or GLOBAL defined symbols via nm (dlopen/fdopen/mprotect are musl
#       weak stubs — 'nm | grep -w' must match W, not just T/D/B, or they look
#       falsely absent).
#
#   NOT exported: `environ` (tcc's ONLY other cross-image reference, tccrun.c —
#   the `-run` JIT execve-argv-passthrough path, never exercised by `tcc -c`).
#   `environ` is musl's genuinely-zero-initialized (`.bss`) global — exporting
#   ANY BSS-bucket DATA universal hits a REAL LINK.EXE gap discovered while
#   bringing up this bead: emit_shareable()'s universal-symbol-vector resolution
#   loop (resolve_named() over the --symbol-vector list) runs BEFORE the BSS
#   section-placement loop that assigns sec_va[] for B_BSS-bucket sections, so
#   placed_addr() reads sec_va[]==0 (not yet laid out) and resolve_named() wrongly
#   dies with "%LINK-F-ERROR, universal symbol not defined in any input object"
#   even though `environ` IS defined (just not yet placed). Confirmed via direct
#   debug instrumentation (temporary, reverted) tracing resolve_named("environ")
#   -> bucket=B_BSS, sec_va=0, da=0. Every PRIOR DATA universal (stdin/stdout/
#   stderr) is B_DATA (statically initialized `FILE *const`, placed earlier in
#   program order), so this is the FIRST BSS-bucket universal any consumer has
#   needed — link.c/imgact.c are out of Systems-Engineer file-domain for this
#   bead, so this is NOT patched here (see the vms-4ba.4 escalation / follow-up
#   item). TCC.EXE's build (mk_tcc.sh) instead passes --allow-undefined so its
#   `environ` GOT reference defers to a null cell rather than failing the whole
#   link — safe because tccrun.c's `-run` path (the only reader) is dead code
#   for the `tcc -c` proof this bead requires.
# All appended at the END -> prior consumers' indices unchanged (GSMATCH
# LEQUAL-compatible).
VEC="\
__init_libc=PROCEDURE,\
malloc=PROCEDURE,free=PROCEDURE,calloc=PROCEDURE,realloc=PROCEDURE,\
aligned_alloc=PROCEDURE,posix_memalign=PROCEDURE,\
memcpy=PROCEDURE,memmove=PROCEDURE,memset=PROCEDURE,memcmp=PROCEDURE,memchr=PROCEDURE,\
strlen=PROCEDURE,strnlen=PROCEDURE,strcmp=PROCEDURE,strncmp=PROCEDURE,strcasecmp=PROCEDURE,\
strcpy=PROCEDURE,strncpy=PROCEDURE,strcat=PROCEDURE,strncat=PROCEDURE,strdup=PROCEDURE,\
strchr=PROCEDURE,strrchr=PROCEDURE,strstr=PROCEDURE,strtok=PROCEDURE,\
strtol=PROCEDURE,strtoul=PROCEDURE,strtod=PROCEDURE,atoi=PROCEDURE,atol=PROCEDURE,\
snprintf=PROCEDURE,vsnprintf=PROCEDURE,sprintf=PROCEDURE,vsprintf=PROCEDURE,\
printf=PROCEDURE,fprintf=PROCEDURE,vfprintf=PROCEDURE,sscanf=PROCEDURE,\
puts=PROCEDURE,putchar=PROCEDURE,fputs=PROCEDURE,fputc=PROCEDURE,\
fwrite=PROCEDURE,fread=PROCEDURE,fopen=PROCEDURE,fclose=PROCEDURE,fflush=PROCEDURE,\
fseek=PROCEDURE,ftell=PROCEDURE,fgets=PROCEDURE,fgetc=PROCEDURE,getchar=PROCEDURE,\
perror=PROCEDURE,\
qsort=PROCEDURE,bsearch=PROCEDURE,abs=PROCEDURE,labs=PROCEDURE,\
exit=PROCEDURE,abort=PROCEDURE,atexit=PROCEDURE,getenv=PROCEDURE,setenv=PROCEDURE,\
open=PROCEDURE,close=PROCEDURE,read=PROCEDURE,write=PROCEDURE,lseek=PROCEDURE,\
\
pthread_mutex_init=PROCEDURE,pthread_mutex_lock=PROCEDURE,\
pthread_mutex_unlock=PROCEDURE,pthread_mutex_destroy=PROCEDURE,\
pthread_cond_init=PROCEDURE,pthread_cond_wait=PROCEDURE,\
pthread_cond_broadcast=PROCEDURE,pthread_cond_destroy=PROCEDURE,\
raise=PROCEDURE,sigaction=PROCEDURE,sigemptyset=PROCEDURE,\
getpid=PROCEDURE,\
pthread_once=PROCEDURE,toupper=PROCEDURE,ttyname=PROCEDURE,\
__errno_location=PROCEDURE,isalnum=PROCEDURE,tolower=PROCEDURE,\
strncasecmp=PROCEDURE,opendir=PROCEDURE,readdir=PROCEDURE,closedir=PROCEDURE,\
stat=PROCEDURE,realpath=PROCEDURE,unlink=PROCEDURE,\
\
acos=PROCEDURE,acosf=PROCEDURE,asin=PROCEDURE,asinf=PROCEDURE,\
atan=PROCEDURE,atan2=PROCEDURE,atan2f=PROCEDURE,atanf=PROCEDURE,\
ceil=PROCEDURE,cos=PROCEDURE,cosf=PROCEDURE,cosh=PROCEDURE,\
exp=PROCEDURE,expf=PROCEDURE,fabs=PROCEDURE,fabsf=PROCEDURE,\
floor=PROCEDURE,fmod=PROCEDURE,log=PROCEDURE,log10=PROCEDURE,\
log10f=PROCEDURE,log2=PROCEDURE,logf=PROCEDURE,pow=PROCEDURE,\
round=PROCEDURE,sin=PROCEDURE,sinf=PROCEDURE,sinh=PROCEDURE,\
sqrt=PROCEDURE,sqrtf=PROCEDURE,tan=PROCEDURE,tanf=PROCEDURE,tanh=PROCEDURE,\
__libc_current_sigrtmin=PROCEDURE,_exit=PROCEDURE,clock_gettime=PROCEDURE,\
dup=PROCEDURE,dup2=PROCEDURE,execl=PROCEDURE,execv=PROCEDURE,fork=PROCEDURE,\
freopen=PROCEDURE,fscanf=PROCEDURE,getgid=PROCEDURE,getpriority=PROCEDURE,\
getpwuid=PROCEDURE,getrusage=PROCEDURE,getuid=PROCEDURE,glob=PROCEDURE,\
globfree=PROCEDURE,gmtime_r=PROCEDURE,kill=PROCEDURE,localtime=PROCEDURE,\
localtime_r=PROCEDURE,mkdir=PROCEDURE,mmap=PROCEDURE,munmap=PROCEDURE,\
pause=PROCEDURE,rename=PROCEDURE,setpriority=PROCEDURE,socketpair=PROCEDURE,\
statvfs=PROCEDURE,strtok_r=PROCEDURE,syscall=PROCEDURE,sysconf=PROCEDURE,\
time=PROCEDURE,timegm=PROCEDURE,timer_create=PROCEDURE,timer_delete=PROCEDURE,\
timer_settime=PROCEDURE,uname=PROCEDURE,waitpid=PROCEDURE,\
\
stdin=DATA,stdout=DATA,stderr=DATA,\
fnmatch=PROCEDURE,fsync=PROCEDURE,ftruncate=PROCEDURE,\
access=PROCEDURE,chdir=PROCEDURE,chmod=PROCEDURE,execvp=PROCEDURE,\
fileno=PROCEDURE,flock=PROCEDURE,fstat=PROCEDURE,getcwd=PROCEDURE,\
geteuid=PROCEDURE,getpwnam=PROCEDURE,getrlimit=PROCEDURE,gettimeofday=PROCEDURE,\
inet_ntop=PROCEDURE,inet_pton=PROCEDURE,ioctl=PROCEDURE,isatty=PROCEDURE,\
isxdigit=PROCEDURE,mktime=PROCEDURE,ntohl=PROCEDURE,pipe=PROCEDURE,\
readlink=PROCEDURE,rewind=PROCEDURE,setrlimit=PROCEDURE,settimeofday=PROCEDURE,\
sleep=PROCEDURE,socket=PROCEDURE,strerror=PROCEDURE,strptime=PROCEDURE,\
strftime=PROCEDURE,\
system=PROCEDURE,tcgetattr=PROCEDURE,tcsetattr=PROCEDURE,tcflush=PROCEDURE,utimes=PROCEDURE,\
\
__addtf3=PROCEDURE,__divtf3=PROCEDURE,__eqtf2=PROCEDURE,__extenddftf2=PROCEDURE,\
__extendsftf2=PROCEDURE,__fixtfdi=PROCEDURE,__fixunstfdi=PROCEDURE,\
__floatunditf=PROCEDURE,__floatunsitf=PROCEDURE,__getf2=PROCEDURE,\
__gttf2=PROCEDURE,__letf2=PROCEDURE,__lttf2=PROCEDURE,__multf3=PROCEDURE,\
__netf2=PROCEDURE,__subtf3=PROCEDURE,__trunctfdf2=PROCEDURE,\
__trunctfsf2=PROCEDURE,__negtf2=PROCEDURE,__clear_cache=PROCEDURE,\
__assert_fail=PROCEDURE,atoi=PROCEDURE,dlclose=PROCEDURE,dlopen=PROCEDURE,\
dlsym=PROCEDURE,fdopen=PROCEDURE,ldexpl=PROCEDURE,longjmp=PROCEDURE,\
mprotect=PROCEDURE,remove=PROCEDURE,sem_init=PROCEDURE,sem_post=PROCEDURE,\
sem_wait=PROCEDURE,setjmp=PROCEDURE,sigaddset=PROCEDURE,sigprocmask=PROCEDURE,\
strpbrk=PROCEDURE,strtof=PROCEDURE,strtold=PROCEDURE,strtoull=PROCEDURE,\
\
fcntl=PROCEDURE,\
\
setsid=PROCEDURE,\
\
setvbuf=PROCEDURE,setgid=PROCEDURE,setuid=PROCEDURE,setgroups=PROCEDURE,\
getegid=PROCEDURE,\
\
nanosleep=PROCEDURE,\
\
tmpfile=PROCEDURE,clearerr=PROCEDURE,\
\
unsetenv=PROCEDURE,\
\
pthread_create=PROCEDURE,pthread_detach=PROCEDURE,pthread_join=PROCEDURE,\
\
pread=PROCEDURE,pwrite=PROCEDURE,ferror=PROCEDURE,\
\
chown=PROCEDURE,\
__ctype_get_mb_cur_max=PROCEDURE,__cxa_atexit=PROCEDURE,__fsetlocking=PROCEDURE,__stack_chk_fail=PROCEDURE,\
asprintf=PROCEDURE,btowc=PROCEDURE,dirfd=PROCEDURE,dl_iterate_phdr=PROCEDURE,dlerror=PROCEDURE,\
fchmod=PROCEDURE,fchmodat=PROCEDURE,fdopendir=PROCEDURE,fegetround=PROCEDURE,feof=PROCEDURE,\
fesetround=PROCEDURE,ffs=PROCEDURE,freelocale=PROCEDURE,frexp=PROCEDURE,frexpl=PROCEDURE,\
fseeko=PROCEDURE,ftello=PROCEDURE,get_nprocs=PROCEDURE,getc=PROCEDURE,getentropy=PROCEDURE,\
getpagesize=PROCEDURE,getwc=PROCEDURE,isalpha=PROCEDURE,isblank=PROCEDURE,iscntrl=PROCEDURE,\
islower=PROCEDURE,isprint=PROCEDURE,ispunct=PROCEDURE,isspace=PROCEDURE,isupper=PROCEDURE,\
iswctype=PROCEDURE,ldexp=PROCEDURE,link=PROCEDURE,lstat=PROCEDURE,mbrtowc=PROCEDURE,\
mbstowcs=PROCEDURE,mempcpy=PROCEDURE,mkstemps=PROCEDURE,mktemp=PROCEDURE,newlocale=PROCEDURE,\
nl_langinfo=PROCEDURE,openat=PROCEDURE,pathconf=PROCEDURE,poll=PROCEDURE,posix_spawn=PROCEDURE,\
posix_spawn_file_actions_addclose=PROCEDURE,posix_spawn_file_actions_adddup2=PROCEDURE,posix_spawn_file_actions_destroy=PROCEDURE,posix_spawn_file_actions_init=PROCEDURE,posix_spawnattr_destroy=PROCEDURE,\
posix_spawnattr_init=PROCEDURE,posix_spawnattr_setflags=PROCEDURE,posix_spawnp=PROCEDURE,prctl=PROCEDURE,pthread_cond_signal=PROCEDURE,\
pthread_getspecific=PROCEDURE,pthread_key_create=PROCEDURE,pthread_key_delete=PROCEDURE,pthread_rwlock_rdlock=PROCEDURE,pthread_rwlock_unlock=PROCEDURE,\
pthread_rwlock_wrlock=PROCEDURE,pthread_setspecific=PROCEDURE,putc=PROCEDURE,putwc=PROCEDURE,sbrk=PROCEDURE,\
sched_yield=PROCEDURE,secure_getenv=PROCEDURE,sendfile=PROCEDURE,setlocale=PROCEDURE,sigfillset=PROCEDURE,\
signal=PROCEDURE,stpcpy=PROCEDURE,strcoll=PROCEDURE,strerror_r=PROCEDURE,strsignal=PROCEDURE,\
strspn=PROCEDURE,strxfrm=PROCEDURE,symlink=PROCEDURE,towlower=PROCEDURE,towupper=PROCEDURE,\
truncate=PROCEDURE,umask=PROCEDURE,ungetc=PROCEDURE,ungetwc=PROCEDURE,unlinkat=PROCEDURE,\
uselocale=PROCEDURE,utimensat=PROCEDURE,vasprintf=PROCEDURE,vprintf=PROCEDURE,wait4=PROCEDURE,\
wcrtomb=PROCEDURE,wcscoll=PROCEDURE,wcsftime=PROCEDURE,wcslen=PROCEDURE,wcsxfrm=PROCEDURE,\
wctob=PROCEDURE,wctype=PROCEDURE,wmemchr=PROCEDURE,wmemcmp=PROCEDURE,wmemcpy=PROCEDURE,\
wmemmove=PROCEDURE,wmemset=PROCEDURE,writev=PROCEDURE,\
\
__copy_tls=PROCEDURE,__init_tp=PROCEDURE,ovmx_get_libc=PROCEDURE"

# fcntl APPENDED for vms-8019 (append-only -> prior consumers' vector indices
# unchanged, GSMATCH LEQUAL-compatible). $CREPRC's creation handshake sets
# FD_CLOEXEC on its report pipe so a concurrent exec in another thread of the
# CALLING process cannot leak the write end and leave the creator blocked in
# read() forever. pipe2(O_CLOEXEC) would need _GNU_SOURCE and is not in the
# vector either; fcntl() is a plain C-RTL entry point (OpenVMS's own DECC$SHR
# exports it) that musl's libc.a defines, so DECC$SHR is the right producer.
#
# setsid APPENDED for vms-47b (append-only -> prior consumers' vector indices
# unchanged, GSMATCH LEQUAL-compatible). PRC$M_DETACH's double-fork idiom in
# $CREPRC (sys_process.c) calls setsid() in the intermediate task to leave the
# creator's session and controlling terminal before the grandchild registers
# as a detached process. setsid() is a plain C-RTL entry point musl's libc.a
# defines, so DECC$SHR is the right producer.
#
# setvbuf/setgid/setuid/setgroups/getegid APPENDED for vms-c39 (append-only
# -> prior consumers' vector indices unchanged, GSMATCH LEQUAL-compatible).
# LOGINOUT.EXE (tools/vms_login.c, mk_loginout.sh) is the first VMS-native
# EXECUTABLE consumer that needs them, found by the STRICT link actually
# failing (link.c's unresolved-symbol diagnostic names the symbol one at a
# time; `nm -u` of the compiled object against this vector gave the rest in
# one pass rather than one link-fail-fix-relink cycle per name):
#   - setvbuf: unbuffers stdin so credential input typed at Username:/
#     Password: survives past LOGINOUT's own execl() into DCL.EXE (the
#     unread SHOW-TIME/EXIT lines a scripted session pipes ahead of time
#     stay in the kernel pipe buffer, not libc's).
#   - setgid/setuid/setgroups: LOGINOUT drops Linux credentials from root to
#     the authenticated SYSUAF UIC before exec'ing the user's session (the
#     vms-2b8 round-6 fix cited at length in tools/vms_login.c) so a forked
#     child cannot re-register with the executive and re-escalate.
#   - getegid: part of the same block's post-drop verification
#     (getuid/geteuid/getgid/getegid all compared against the target UIC);
#     getuid/geteuid/getgid were already exported for earlier consumers,
#     getegid was not. (strtok_r, also referenced by LOGINOUT via privs.h's
#     parse_privilege_string(), was ALREADY exported -- appended earlier for
#     a prior consumer -- so it is not part of this block.)
#
# nanosleep APPENDED for vms-801 R2.2 batch 4 (append-only -> prior consumers'
# vector indices unchanged, GSMATCH LEQUAL-compatible). lib$wait (rtl/lib_timer.c)
# is the first consumer: it suspends the caller for a sub-second real-time
# interval, which sleep() (whole seconds, already exported) cannot express.
# nanosleep() is a plain C-RTL entry point musl's libc.a defines, so DECC$SHR is
# the right producer.
#
# tmpfile/clearerr APPENDED for vms-1a9 (append-only -> prior consumers' vector
# indices unchanged, GSMATCH LEQUAL-compatible). DCL's SYS$INPUT-from-procedure
# wiring (dcl_sysinput_setup/_restore, dcl_cmd_process.c) buffers an image's
# SYS$INPUT data-line block in an anonymous auto-deleted temp stream via
# tmpfile(), and clears the residual EOF/error state on stdin with clearerr()
# once fd 0 is restored. Both are C89 standard stdio entry points that real
# OpenVMS DECC$SHR exports, and musl's libc.a defines them (nm: tmpfile.lo -> T
# tmpfile, whose __randname helper is also defined and whole-archived in;
# clearerr.lo -> T clearerr), so DECC$SHR is the right producer -- the faithful
# fix, not avoiding the standard calls. DCL.EXE is the first consumer.
#
# unsetenv APPENDED for vms-54e (append-only -> prior consumers' vector indices
# unchanged, GSMATCH LEQUAL-compatible). LIB$GET_FOREIGN (libvms rtl/lib_output.c)
# consumes the CLI foreign command line on first read by clearing the
# VMS_FOREIGN_CMD environment variable DCL publishes for it (dcl_cmd_process.c
# also unsetenv()s it after activation as a safety net), so a second call falls
# through to SYS$INPUT exactly as OpenVMS's one-shot CLI foreign line does.
# getenv/setenv were already exported above; unsetenv is the POSIX/C-RTL
# companion that real OpenVMS DECC$SHR exports and musl's libc.a defines, so
# DECC$SHR is the right producer. libvms (LIBVMS$SHR) and DCL.EXE are consumers.
#
# pthread_create/pthread_detach/pthread_join APPENDED for vms-786 (append-only
# -> prior consumers' vector indices unchanged, GSMATCH LEQUAL-compatible).
# DCL's mailbox-backed SYS$INPUT/SYS$OUTPUT (src/vmsdcl/dcl_mbx.c) runs a reader
# thread that blocks in IO$_READVBLK on the input mailbox and a writer thread
# that drains DCL's output to the output mailbox, so DCL.EXE is the first
# VMS-native consumer to CREATE a thread (the pthread_mutex_*/pthread_cond_*
# family was already exported for lock/condition use, but nothing had needed
# pthread_create/detach/join yet). All three are POSIX threading entry points
# real OpenVMS DECC$SHR exports (DECthreads) and musl's libc.a defines, so
# DECC$SHR is the right producer.
#
# pread/pwrite APPENDED for vms-5f0 (append-only -> prior consumers' vector
# indices unchanged, GSMATCH LEQUAL-compatible). The Files-11 atomic-flip's RMS
# legacy-POSIX defer (A2a, rms_io.c) reads and writes an ODS-2 block at an
# explicit byte offset without disturbing the file position -- the POSIX backend
# of rms_io_read/write_at (rms_io_{read,write}_posix) uses pread()/pwrite() for
# exactly the positioned I/O the ACP backend expresses via IO$_READVBLK's VBN.
# open/close/read/write/lseek were already exported above; pread/pwrite are the
# POSIX positioned-I/O companions real OpenVMS DECC$SHR exports and musl's libc.a
# defines, so DECC$SHR is the right producer. LIBVMSRMS$SHR is the consumer.
#
# chown APPENDED for vms-a86f (append-only -> prior consumers' vector indices
# unchanged, GSMATCH LEQUAL-compatible). LOGINOUT.EXE (tools/vms_login.c),
# still privileged before its credential drop, creates the session's per-user
# private image-staging directory OVMX_BOOT_STAGE_DIR "/<uid>/" 0700 and chowns
# it to the authenticated UIC, so a non-root session can stage an image the
# boot bridge did not pre-stage (SYS$SYSTEM:PARTS.EXE on first `$ PARTS`) into a
# directory it owns rather than the root-owned shared parent (whose EACCES was
# the red PARTS gate). mkdir/setgid/setuid/setgroups were already exported for
# LOGINOUT; chown is the POSIX ownership companion real OpenVMS DECC$SHR exports
# and musl's libc.a defines, so DECC$SHR is the right producer.
#
# THE GENERAL RULE, because this is the commonest way to break the VMS-native
# toolchain jobs: EVERY libc call added to an OVMX library is a claim that
# DECC$SHR provides it. The claim is checked by LINK.EXE at link time — the
# STRICT (no --allow-undefined) libvms/vmsrms/DCL/tcc links fail if it is
# false. --allow-undefined is NOT the fix; it records the symbol as a deferred
# import and hides the breakage.
echo "mk_decc_shr: LINK.EXE=$LINK_EXE"
echo "mk_decc_shr: libc.a=$LIBC  libgcc.a=$LIBGCC  GSMATCH=$GSMATCH"

# __copy_tls/__init_tp/ovmx_get_libc APPENDED for vms-c07 (append-only -> prior
# consumers' vector indices unchanged, GSMATCH LEQUAL-compatible). IMGACT (the
# PT_INTERP for LINK.EXE images) drives musl's OWN TLS helpers to set up an OVMX
# executable's main-program TLS, which musl-as-a-shareable structurally cannot
# relocate (static_init_tls derives the bias from PT_DYNAMIC = DECC$SHR's own,
# bias 0). __copy_tls (builds the TCB + copies the RELOCATED .tdata + DTV,
# returns TP) and __init_tp (writes the self-ptr/canary/tid) are musl libc.a
# entry points; ovmx_get_libc is OVMX loader glue (ovmx_libc_stub.c) exposing
# musl's hidden `__libc` so IMGACT can poke its tls_head/tls_cnt/tls_size/
# tls_align fields. A getter PROCEDURE, not a `__libc=DATA` universal: `__libc`
# is BSS and a BSS DATA universal hits the emit_shareable() resolve-before-place
# gap (vms-910). See src/imgact/imgact.c (reserve_exe_main_tls_over_crtl).
#
# Compile the loader glue for the SAME ABI as libc.a (the recipe runs in the
# musl container whose cc matches the target archive; CC overridable for cross).
# -ffreestanding: no libc dependency of its own (it only takes __libc's address).
STUB_SRC="$(CDPATH= cd "$(dirname "$0")" && pwd)/ovmx_libc_stub.c"
STUB_OBJ="$(mktemp -d)/ovmx_libc_stub.o"
"${CC:-cc}" -c -fPIC -ffreestanding -o "$STUB_OBJ" "$STUB_SRC"

# ovmx_decc_crtl.c (vms-3e4 R1b-2a): the DEC C RTL special-symbol impls that are
# NOT plain musl aliases — the per-thread errno accessors (get_errno_addr /
# get_vms_errno_addr) and the 32/64-bit dual-pointer allocators (_malloc32 /
# _malloc64), REAL impls grounded to the VSI C RTL manual + the port crt0 contract
# (see the file header). Compiled + linked in like ovmx_libc_stub.c; the four
# symbols are appended to the vector below (append-only, GSMATCH LEQUAL-compatible).
# Non-TLS (pthread-key per-thread state, never __thread) so DECC$SHR stays a
# non-TLS producer.
CRTL_SRC="$(CDPATH= cd "$(dirname "$0")" && pwd)/ovmx_decc_crtl.c"
CRTL_OBJ="$(mktemp -d)/ovmx_decc_crtl.o"
IMGACT_INC="$(CDPATH= cd "$(dirname "$0")/../imgact/include" && pwd)"   # ovmx_activation.h (vms-8c8)
"${CC:-cc}" -c -fPIC -ffreestanding -I"$IMGACT_INC" -o "$CRTL_OBJ" "$CRTL_SRC"
VEC="$VEC,get_errno_addr=PROCEDURE,get_vms_errno_addr=PROCEDURE,_malloc32=PROCEDURE,_malloc64=PROCEDURE"

# decc$malloc / decc$_malloc64 / decc$tprintf APPENDED for vms-981 (append-only ->
# prior consumers' vector indices unchanged, GSMATCH LEQUAL-compatible). The three
# DECORATED CRTL entries a real alpha-dec-vms GCC-port program actually references
# that the R1b-1 no-decoration alias vector deliberately did NOT generate — CONFIRMED
# undefined by linking the staged port objects (crt0.obj + app.obj, the cc1-compiled
# crt0 + printf/malloc/strlen program) against a REAL DECC$SHR.EXE with
# --allow-undefined (the other decc$ refs — decc$main, decc$strlen, C$_EXIT1 — already
# bind). Real impls in ovmx_decc_crtl.c (same object), grounded to the GPL port's
# name-decoration (gcc/config/vms/vms.cc + vms-crtlmap.map) + the VSI OpenVMS C RTL
# Reference Manual (Rule 8):
#   decc$malloc    -> _malloc32  (the 32-bit-pointer malloc: < 4 GB-addressable,
#                    the crt0's widened argv/envp array; NOT musl's 64-bit malloc)
#   decc$_malloc64 -> _malloc64  (the 64-bit-pointer malloc: full 64-bit heap;
#                    app.c's plain malloc lowers here under -mpointer-size=64)
#   decc$tprintf   -> vprintf    (printf's IEEE T_FLOAT variant; OVMX/musl doubles
#                    ARE IEEE-754, so it is musl's native printf model — real
#                    formatted output to stdout, correct char-count return)
VEC="$VEC,decc\$malloc=PROCEDURE,decc\$_malloc64=PROCEDURE,decc\$tprintf=PROCEDURE"

# decc$main (vms-954, §4b.5): the DEC C RTL image-startup routine the alpha-dec-vms
# crt0's __main forwards its six-arg VMS activation context to; it PRODUCES
# argc/argv/envp (non-CLI path implemented, CLI path a grounded follow-up). Real
# impl in ovmx_decc_crtl.c (same object), exported as a PROCEDURE universal the
# port image binds via .vms$imp at activation. Append-only.
VEC="$VEC,decc\$main=PROCEDURE"

# ---- vms-954 R1b-2b: C$_EXIT1 as a C-RTL globalvalue ----------------------
# The alpha-dec-vms crt0 (libgcc/config/vms/vms-ucrt0.c) computes a POSIX exit
# status as `(__int64)&C$_EXIT1 + ((status-1) << STS$V_MSG_NO)`: C$_EXIT1 is a
# VMS GLOBALVALUE (the symbol names NO storage — its ADDRESS is a link-time
# constant, the base VMS condition value of the C facility's EXIT1 message). On
# real VMS it resolves from the C RTL's default libraries; OVMX exports it from
# DECC$SHR, the C RTL surface, as an absolute globalvalue universal. LINK.EXE
# folds `&C$_EXIT1` to this constant at link time (never load-biased, never an
# activation import — see link.c collect_globalvalues / gval_find).
#
# VALUE GROUNDED TO THE ORACLE (Rule 8, clean-room): C$_EXIT1 = 0x0035A009,
# measured 2026-08-22 on OpenVMS Alpha V8.4 (lab-Alpha) via a MACRO `.LONG
# C$_EXIT1` + LINK/MAP/FULL resolved from the default libraries, cross-checked
# with F$MESSAGE -> %C-S-NOMSG (C facility, Success severity). Not invented.
VEC="$VEC,C\$_EXIT1=GLOBALVALUE:0x0035A009"

# ---- vms-3e4 R1b-1: the decc$-prefixed CRTL alias vector -------------------
# The alpha-dec-vms GCC port references every C-RTL entry as `decc$<name>`
# (gcc/config/vms/vms.c), so a port object imports `decc$fprintf`, not `fprintf`.
# Export each C-RTL entry as a universal ALIASED to the musl implementation via the
# VSI Linker SYMBOL_VECTOR=(universal/internal) form (link.c R1a): the exported
# universal `decc$<name>` is bound to the internal defining symbol `<name>` that
# musl's whole-archived libc.a provides. Rule-based from decc_crtl_map.txt (OVMX's
# DEC C-RTL surface map), nm-filtered to what THIS libc.a actually defines — a name
# musl lacks is a visible GAP for a later rung (reported), never a silent success.
# No-decoration-flag entries only (R1b-1); the decorated names (malloc64 / float
# model / 64-bit _X32/_X64 / DPML MATH$) and the special bootstrap symbols
# (decc$main, _malloc32/64, vaxc$errno, get_vms_errno_addr, C$_EXIT1) are R1b-2.
# Semantics are musl/POSIX until R2 routes the file entries through RMS/ACP
# (vms-dfb, ROUTE-THROUGH): this rung provides the linkable CRTL surface; R2
# upgrades the file semantics. Append-only -> prior consumers' vector indices are
# unchanged (GSMATCH LEQUAL-compatible).
DECC_MAP="$(CDPATH= cd "$(dirname "$0")" && pwd)/decc_crtl_map.txt"
if [ -f "$DECC_MAP" ]; then
    LIBC_DEFS="$(mktemp)"
    # Defined (non-undefined) symbols of the (TLS-filtered) libc.a, one pass. `nm
    # --defined-only` prints member-header lines (NF==1, "member.o:") we drop and
    # symbol lines "<addr> <type> <name>" whose $NF is the name.
    nm --defined-only "$LIBC" 2>/dev/null | awk 'NF>=2{print $NF}' | sort -u > "$LIBC_DEFS"
    n_add=0; n_skip=0
    while read -r nm_name; do
        case "$nm_name" in ''|\#*) continue;; esac
        if grep -qxF "$nm_name" "$LIBC_DEFS"; then
            VEC="$VEC,decc\$$nm_name/$nm_name=PROCEDURE"
            n_add=$((n_add + 1))
        else
            n_skip=$((n_skip + 1))
        fi
    done < "$DECC_MAP"
    rm -f "$LIBC_DEFS"
    echo "mk_decc_shr: decc\$ CRTL alias vector: exported $n_add, skipped $n_skip (musl-undefined — gap for a later rung, not faked)"
fi

# Whole-archive, strict (NO --allow-undefined): a complete C-RTL shareable must
# link with zero deferred externals. libc.a first so its strong defs win; the
# loader-glue object last (it only REFERENCES __libc, which libc.a defines).
"$LINK_EXE" --shareable \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" "$LIBC" "$LIBGCC" "$STUB_OBJ" "$CRTL_OBJ"

echo "mk_decc_shr: created $OUT"
