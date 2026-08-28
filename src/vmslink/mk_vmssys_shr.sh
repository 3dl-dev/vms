#!/bin/sh
# mk_vmssys_shr.sh — build recipe for LIBVMSSYS$SHR.EXE, the src/libvmssys
# (freestanding /dev/vms client + string/math/stdio/futex helpers) VMS-native
# shareable image (bead vms-b6a, pillar vms-ade). Extracted from the inline
# build steps `src/imgact/test/lib_build_graph.sh`'s build_producer_graph()
# carried directly (run_dcl_native.sh, run_login_native.sh) so there is ONE
# place that knows the LIBVMSSYS$SHR recipe instead of two copies drifting —
# the same drift risk mk_libvms_shr.sh's LIST comment calls out for its own
# translation-unit list.
#
# LIBVMSSYS$SHR.EXE exports the /dev/vms client entry points (vms_kif_*: open/
# enq/deq/convert/assign/dassgn/getdvi/setprn/getjpi/procscan/event-flag ops/
# AST ops (dclast/setast/deliverast)/
# devscan) plus vms_strlen — the FIRST producer in the b65/c39 graph (nothing
# else exports these; DCL's SHOW SYSTEM and LOGINOUT's identity establishment
# both import from it). Freestanding musl target: -fPIC -O2 -ffreestanding
# -fno-stack-protector -fno-builtin plus one target-specific codegen flag
# (see ARCH below); the syscall trampoline is compiled from
# arch/$ARCH/syscall.S. Both aarch64 and x86_64 (vms-6da; the mk_*_shr.sh
# recipes have taken ARCH/CFLAGS as env-overridable since vms-cb5f -- this
# is the one recipe vms-b6a extracted afterwards, so it hadn't picked up the
# convention yet).
#
# The vector always exports vms_kif_setident (identity establishment): the
# DCL-only test harness (run_dcl_native.sh) omits it via a narrower ad hoc
# vector because DCL never calls it, but any shared producer build (this
# script, used by both DCL.EXE and LOGINOUT.EXE) exports it unconditionally —
# an unused export is harmless (append-only vector, §3 docs/design-link-
# native-toolchain.md), an omitted one is a link failure for whichever
# consumer needed it.
#
# Composition (VMS-native, no hand TU-list — vms-71a3, epic vms-a90, Rung 3 of
# docs/design-vms-native-shareable-build.md, following the Rung-2 pilot
# e9512c76/mk_vmslnm_shr.sh): the libvmssys translation units are NOT re-typed
# here. They are DERIVED from the ONE CMake source set — the raw-freestanding
# (linux substrate) branch's set(VMSSYS_C_SOURCES ...) list that
# add_library(vmssys ...) compiles in src/libvmssys/CMakeLists.txt — so this
# recipe can no longer drift from the dev build. (libvmssys/CMakeLists.txt also
# has a NetBSD-substrate VMSSYS_C_SOURCES block for the link-libc vax port; this
# recipe only ever builds the raw-freestanding linux substrate — ARCH is
# x86_64/aarch64/alpha, never vax/netbsd — so derivation is anchored to that
# branch, not "whichever set(VMSSYS_C_SOURCES...) comes first".) Those TUs are
# compiled -fPIC musl into .OBJ, packed into an .OLB by LIBRARIAN.EXE, and
# LINK.EXE --shareable pulls exactly the members its --symbol-vector universals
# (+ transitive refs) require via selective library search (#659/vms-bf8):
# resolve_olbs seeds its unresolved set from the symbol vector, so a
# /SHAREABLE links from the .OLB ALONE with no explicit object TU list — the
# VMS way (Linker Utility Manual §1.2.3 selective search rooted at
# SYMBOL_VECTOR). arch/$ARCH/syscall.S stays a hand-named single file (it is
# not part of VMSSYS_C_SOURCES — it's ${VMSSYS_ASM_SOURCES}'s one member this
# shareable needs; crt0.S/sigreturn.S are CRT-only, never part of the
# shareable) and is packed into the same .OLB as an additional member.
#
# Usage:  mk_vmssys_shr.sh <LINK.EXE> <out-LIBVMSSYS$SHR.EXE> [libvmssys-src-dir] [extra-vec]
# Env:    CC (default gcc), GSMATCH (default LEQUAL,1,0), ARCH (default
#         aarch64; also x86_64 -- selects arch/$ARCH/syscall.S), CFLAGS
#         (env-overridable; default is the aarch64 flag set below so
#         standalone/aarch64 callers need not change -- the x86_64 caller
#         passes CFLAGS with -mtls-dialect=gnu2 in place of
#         -mno-outline-atomics, same convention as every other mk_*_shr.sh)
#
# Must run where the target musl toolchain + arch/$ARCH/syscall.S apply
# (CLAUDE.md test loop / the musl container for that arch).
set -e

LINK_EXE=${1:?usage: mk_vmssys_shr.sh <LINK.EXE> <out> [libvmssys-src] [extra-vec]}
OUT=${2:?usage: mk_vmssys_shr.sh <LINK.EXE> <out> [libvmssys-src] [extra-vec]}
HERE=$(cd "$(dirname "$0")" && pwd)                        # src/vmslink
SRC=${3:-$(cd "$HERE/../libvmssys" && pwd)}                # src/libvmssys
EXTRA_VEC=${4:-}
LIBVMS_INC=$(cd "$HERE/../libvms/include" && pwd)          # for ssdef.h (librarian.c)
CC=${CC:-gcc}
GSMATCH=${GSMATCH:-LEQUAL,1,0}
ARCH=${ARCH:-aarch64}

[ -d "$SRC" ] || { echo "mk_vmssys_shr: libvmssys src dir not found: $SRC"; exit 1; }
[ -f "$SRC/arch/$ARCH/syscall.S" ] || { echo "mk_vmssys_shr: unsupported ARCH=$ARCH (no $SRC/arch/$ARCH/syscall.S)"; exit 1; }

WORK=${WORK:-/tmp/mk-vmssys-shr}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -U_FORTIFY_SOURCE}"
CFLAGS="$CFLAGS -I$SRC"

echo "mk_vmssys_shr: LINK.EXE=$LINK_EXE  CC=$CC  GSMATCH=$GSMATCH  ARCH=$ARCH"
echo "mk_vmssys_shr: src=$SRC"

# ---------------------------------------------------------------------------
# TU set: DERIVED from the ONE CMake source list, never re-typed here
# (vms-71a3). Read the raw-freestanding (linux substrate) branch's
# set(VMSSYS_C_SOURCES ...) straight out of src/libvmssys/CMakeLists.txt — the
# same list add_library(vmssys ...) compiles for the dev build on this
# substrate — so the recipe and CMake cannot disagree. Anchored on the
# "OVMX/Linux (raw-freestanding) substrate" comment so this does not
# accidentally grab the earlier NetBSD-substrate VMSSYS_C_SOURCES block (the
# link-libc vax port, which this recipe never builds).
# ---------------------------------------------------------------------------
CMAKELISTS="$SRC/CMakeLists.txt"
[ -f "$CMAKELISTS" ] || { echo "mk_vmssys_shr: CMake source list not found: $CMAKELISTS"; exit 1; }
UNITS=$(awk '
    /OVMX\/Linux \(raw-freestanding\) substrate/ { seen=1 }
    seen && /set\(VMSSYS_C_SOURCES/ { f=1 }
    f { print }
    f && /\)/ { exit }
' "$CMAKELISTS" | grep -oE '[A-Za-z0-9_/]+\.c')
[ -n "$UNITS" ] || { echo "mk_vmssys_shr: could not derive VMSSYS_C_SOURCES from $CMAKELISTS"; exit 1; }
NUNITS=$(printf '%s\n' $UNITS | grep -c '.')
echo "mk_vmssys_shr: derived $NUNITS TU(s) from CMake VMSSYS_C_SOURCES: $(printf '%s ' $UNITS)"

# Build LIBRARIAN.EXE (the OVMX object-library utility, src/vmslink/librarian.c) —
# an ordinary host tool, same convention as the bootstrap LINK.EXE. It packs the
# .OBJ members into the .OLB that LINK.EXE then selectively searches.
echo "  cc LIBRARIAN.EXE"
$CC -std=gnu11 -O2 -I"$HERE/include" -I"$LIBVMS_INC" \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -o "$WORK/LIBRARIAN.EXE" "$HERE/librarian.c"

# Compile each derived TU to a .OBJ, plus the one hand-named ASM member
# (arch/$ARCH/syscall.S -- not part of VMSSYS_C_SOURCES; it is the single
# ${VMSSYS_ASM_SOURCES} member this shareable needs, crt0.S/sigreturn.S being
# CRT-only and never part of the shareable).
OBJS=""
for c in $UNITS; do
    u=$(basename "$c" .c)
    echo "  cc $c"
    $CC $CFLAGS -c -o "$WORK/$u.OBJ" "$SRC/$c"
    OBJS="$OBJS $WORK/$u.OBJ"
done
echo "  cc arch/$ARCH/syscall.S"
$CC -fPIC -c -o "$WORK/syscall.OBJ" "$SRC/arch/$ARCH/syscall.S"
OBJS="$OBJS $WORK/syscall.OBJ"
NMEMBERS=$((NUNITS + 1))

# LIBRARIAN /CREATE the libvmssys object library from every derived .OBJ member
# (the $NUNITS CMake-derived C TUs plus the one hand-named syscall.S member).
OLB="$WORK/VMSSYS.OLB"
rm -f "$OLB"
echo "mk_vmssys_shr: LIBRARIAN /CREATE $OLB from $NMEMBERS member(s)"
# shellcheck disable=SC2086
"$WORK/LIBRARIAN.EXE" /CREATE "$OLB" $OBJS

# The LIBVMSSYS$SHR universal set + ORDER is single-sourced from the frozen,
# append-only manifest src/vmslink/libvmssys_shr.vec (bead vms-bd1). This used
# to be an inline SYS_VEC literal here AND in six run_*_native.sh harnesses --
# seven hand-kept copies that had already drifted into two different orders. The
# manifest header documents the append-only GSMATCH LEQUAL contract; the parser
# (symvec_emit.sh) strips comments and joins entries in file order.
SYS_VEC=$(sh "$HERE/symvec_emit.sh" "$HERE/libvmssys_shr.vec")
if [ -n "$EXTRA_VEC" ]; then
    SYS_VEC="$SYS_VEC,$EXTRA_VEC"
fi

# ---------------------------------------------------------------------------
# Rung-0 reconcile PROBE (vms-71a3, INFO-only for this recipe -- see the FAIL
# NOTE below): try the pure SELECTIVE $OLB-alone link first, into a throwaway
# output, purely to measure how many of the $NMEMBERS derived members the
# --symbol-vector's reference closure actually reaches.
# ---------------------------------------------------------------------------
"$LINK_EXE" --shareable \
    --symbol-vector "$SYS_VEC" \
    --gsmatch "$GSMATCH" \
    -o "$WORK/probe.EXE" "$OLB" > "$WORK/link-selective-probe.out" 2>&1 || true
sed 's/^/   /' "$WORK/link-selective-probe.out"
PULL=$(grep -oE "$OLB: [0-9]+ of [0-9]+ member" "$WORK/link-selective-probe.out" | grep -oE '[0-9]+ of [0-9]+' | head -1)
GOT=${PULL%% of *}
TOT=${PULL##* of }

if [ -n "$PULL" ] && [ "$GOT" = "$TOT" ] && [ "$TOT" = "$NMEMBERS" ]; then
    echo "mk_vmssys_shr: Rung-0 reconcile OK: selective pull $PULL member(s) == the hand-list's whole set"
    echo "mk_vmssys_shr: LINK.EXE --shareable --symbol-vector ... SELECTIVE $OLB -> $OUT"
    "$LINK_EXE" --shareable \
        --symbol-vector "$SYS_VEC" \
        --gsmatch "$GSMATCH" \
        -o "$OUT" "$OLB"
else
    # -------------------------------------------------------------------
    # FAIL NOTE / DISCLOSED DEVIATION (vms-71a3, CLAUDE.md Rule 5 -- flag,
    # don't silently paper over): the pure SELECTIVE $OLB-alone link only
    # reaches $PULL of the $NMEMBERS derived members. Investigated: the 5
    # unreached members (vms_snprintf.c, vms_futex.c, vms_stdio.c,
    # vms_math.c, vms_runtime_init.c) define NO symbol in libvmssys_shr.vec
    # and are called by NEITHER vms_kif.c NOR vms_string.c NOR
    # kif_transport_linux.c NOR arch/$ARCH/syscall.S (confirmed by source
    # grep, not just the linker's report) -- unlike LIBVMSLNM$SHR (the Rung-2
    # pilot), where every TU directly implements a vector entry, libvmssys
    # bundles freestanding-runtime utility routines (snprintf/futex/stdio/
    # math/getenv) that OTHER images consume by linking libvmssys.a
    # STATICALLY (Library Build Order, CLAUDE.md), not through THIS
    # shareable's symbol vector. A pure SELECTIVE pull is therefore VMS-
    # authentic (Linker Utility Manual Sec 1.2.3: selective search includes
    # only what the reference graph needs) but produces a SMALLER image
    # (4 objects) than the pre-migration hand-list (9 objects) -- it fails
    # this task's byte-identical/same-object-count acceptance bar, and
    # LINK.EXE has no VMS-authentic /INCLUDE= force-list capability (yet)
    # to pull the other 5 without a reference. Rather than silently ship a
    # different image (dead-code prune) OR silently ship a broken assert
    # that reds the whole downstream graph (DCL.EXE/LOGINOUT.EXE never
    # build), this recipe falls back to the EXPLICIT object list for the
    # real $OUT link -- byte-identical to pre-migration -- and reports the
    # gap loudly instead of hiding it. Flagged for an operator call between
    # (a) pruning the 5 modules from LIBVMSSYS$SHR.EXE (functionally safe
    # per the grep above) or (b) a real LINK.EXE /INCLUDE= capability (its
    # own scoped Rung, not invented here). TU-set derivation from CMake and
    # the LIBRARIAN.EXE .OLB build stay migrated either way.
    # -------------------------------------------------------------------
    echo "mk_vmssys_shr: NOTE: pure SELECTIVE pull only reaches $PULL of $NMEMBERS members"
    echo "               (vms_snprintf/vms_futex/vms_stdio/vms_math/vms_runtime_init are"
    echo "               unreferenced by the vector's closure -- confirmed dead-in-this-image"
    echo "               by source inspection, not just the linker). Deferred: no LINK.EXE"
    echo "               /INCLUDE= force-list capability exists to pull them without a"
    echo "               reference. Falling back to the explicit full object list for $OUT"
    echo "               so the shipped image stays byte-identical to pre-migration; flag"
    echo "               this for an operator call (prune vs. add /INCLUDE=)."
    echo "mk_vmssys_shr: LINK.EXE --shareable -> $OUT (explicit $NMEMBERS-object list, SELECTIVE deferred)"
    # shellcheck disable=SC2086
    "$LINK_EXE" --shareable \
        --symbol-vector "$SYS_VEC" \
        --gsmatch "$GSMATCH" \
        -o "$OUT" $OBJS
fi

echo "mk_vmssys_shr: created $OUT"
