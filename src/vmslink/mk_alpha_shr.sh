#!/bin/sh
# mk_alpha_shr.sh (vms-a7a) — shared ALPHA/EVAX helper that builds ONE OVMX
# shareable image (LIBVMS*$SHR / LIBVMSRMS$SHR / ...) for the alpha-dec-vms LP64
# target, mirroring mk_decc_shr.sh's ALPHA branch mechanics for the case where
# the inputs are OVMX C translation units (not the musl libc.a/libgcc.a archives
# DECC$SHR whole-archives).
#
# WHY A SHARED HELPER. The six OVMX producer recipes (mk_vmssys_shr,
# mk_vmsprocess_shr, mk_vmslnm_shr, mk_vmsfs_shr, mk_libvms_shr, mk_vmsrms_shr)
# all need the IDENTICAL alpha mechanics: cross-compile a TU LIST with the
# alpha-dec-vms cc1 (LP64 freestanding musl), enumerate the DEFINED universals
# from the compiled .o objects, and LINK.EXE --shareable --use <producers>. On
# alpha the host `nm`/`ar` cannot read EVAX objects and the x86_64/aarch64
# selective-.OLB + host-nm symbol-vector machinery does not apply; the alpha
# path is uniform enough that each recipe's alpha branch is a thin call into
# this helper with its own LIST / --use set (rather than six copies of the same
# alpha shell). This is the OVMX-C analogue of mk_decc_shr.sh's ALPHA branch,
# which stays separate because it whole-archives libc.a/libgcc.a and enumerates
# only the decc$-decorated surface.
#
# SYMBOL-VECTOR ENUMERATION. The cross `alpha-dec-vms-nm --defined-only` DOES
# read a bare EVAX .o (DST-tolerant since #781 — the same reader mk_decc_shr.sh
# uses to ground-truth its bootstrap-surface objects), so we enumerate every
# GLOBAL/WEAK defined symbol and classify it: text/weak-text -> PROCEDURE,
# data/rodata/bss/common -> DATA. The nm weak-ALIAS-equate blind spot that made
# mk_decc_shr.sh switch to OVMX_LINK_DUMP_UNIVERSALS is musl-specific
# (weak_alias(__libc_free, free)); the OVMX producer TUs define their universals
# as ordinary functions, so nm reports them, and OVMX_LINK_DUMP_UNIVERSALS is
# hardwired to the decc$ surface only (link.c evax_dump_universals) and cannot be
# reused here. EVAX companion labels (..en/..ng/..lk/..lita) are dropped — the
# callable value is the bare descriptor, exactly as in mk_decc_shr.sh.
#
# Usage:  mk_alpha_shr.sh <LINK.EXE> <out.EXE> <src-root> "<tu-list>" [--use PATH]...
#   <tu-list>  space-separated relative paths under <src-root>, no .c suffix
#              (e.g. "rms_core rms_io" or "syssvc/sys_process rtl/lib_output").
# Env (required): ALPHA_CC (alpha-dec-vms-gcc), ALPHA_MUSL_SRC (the musl-1.2.5
#              src tree used to build the alpha libc.a — its PUBLIC headers only;
#              see build-musl.sh). The symbol vector is enumerated via LINK.EXE's
#              own OVMX_LINK_DUMP_UNIVERSALS=all view, so no cross nm is needed.
# Env (optional): ALPHA_INCS (extra -I flags), ALPHA_DEFS (extra -D), GSMATCH
#              (default LEQUAL,1,0), ALPHA_EXTRA_VEC (comma universals appended),
#              ALPHA_ALLOW_UNDEF=1 (pass --allow-undefined, first-light only).
set -e

LINK_EXE=${1:?usage: mk_alpha_shr.sh <LINK.EXE> <out> <src-root> "<tu-list>" [--use PATH]...}
OUT=${2:?usage: mk_alpha_shr.sh <LINK.EXE> <out> <src-root> "<tu-list>" [--use PATH]...}
SRC_ROOT=${3:?need <src-root>}
TU_LIST=${4:?need "<tu-list>" (space-separated relpaths, no .c)}
shift 4
# Remaining args are the --use <PATH> pairs, forwarded verbatim to LINK.EXE.
USE_ARGS="$*"

: "${ALPHA_CC:?mk_alpha_shr: set ALPHA_CC=alpha-dec-vms-gcc}"
: "${ALPHA_MUSL_SRC:?mk_alpha_shr: set ALPHA_MUSL_SRC=<musl-1.2.5 src tree used to build the alpha libc.a>}"
[ -d "$ALPHA_MUSL_SRC" ] || { echo "mk_alpha_shr: ALPHA_MUSL_SRC=$ALPHA_MUSL_SRC not a directory" >&2; exit 2; }
[ -d "$SRC_ROOT" ] || { echo "mk_alpha_shr: src-root $SRC_ROOT not a directory" >&2; exit 2; }
GSMATCH=${GSMATCH:-LEQUAL,1,0}

# PUBLIC musl headers only. The internal dirs ($ALPHA_MUSL_SRC/src/internal,
# .../src/include) define `#define weak __attribute__((__weak__))` (musl's own
# build convenience); leaking them makes an OVMX TU's `__attribute__((weak))`
# expand to a nested, malformed attribute and the cross cc1 rejects it. The
# public set is exactly what a normal musl sysroot exposes.
MUSL_INC="-I$ALPHA_MUSL_SRC/include -I$ALPHA_MUSL_SRC/arch/alpha-dec-vms -I$ALPHA_MUSL_SRC/arch/generic -I$ALPHA_MUSL_SRC/obj/include"

# LP64 freestanding musl CFLAGS (the proven alpha-dec-vms port shape): -fPIC for
# the shareable, -mpointer-size=64 for LP64, -ffreestanding/-fno-builtin so libc
# calls stay real cross-image imports to DECC$SHR, -g0 because the cross nm reads
# DST-stripped objects most reliably (the shipped image is byte-identical).
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mpointer-size=64 -g0 -U_FORTIFY_SOURCE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE ${ALPHA_DEFS:-}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "mk_alpha_shr: building $(basename "$OUT")  (alpha-dec-vms LP64)"
OBJS=""
for tu in $TU_LIST; do
    obj="$WORK/$(echo "$tu" | tr / _).o"
    if [ -f "$SRC_ROOT/$tu.c" ]; then
        # shellcheck disable=SC2086
        "$ALPHA_CC" $CFLAGS ${ALPHA_INCS:-} $MUSL_INC -c -o "$obj" "$SRC_ROOT/$tu.c"
    elif [ -f "$SRC_ROOT/$tu.S" ]; then
        # A .S trampoline (e.g. arch/alpha/syscall.S — the __vms_syscallN callsys
        # trampolines the kif transport calls). Assembled by the same cross cc1
        # driver (runs the EVAX assembler); -mpointer-size=64 kept for consistency.
        # shellcheck disable=SC2086
        "$ALPHA_CC" -fPIC -mpointer-size=64 ${ALPHA_INCS:-} $MUSL_INC -c -o "$obj" "$SRC_ROOT/$tu.S"
    else
        echo "mk_alpha_shr: TU not found: $SRC_ROOT/$tu.{c,S}" >&2; exit 2
    fi
    OBJS="$OBJS $obj"
done
# Pre-built objects (ALPHA_EXTRA_OBJS) join the link + the vector enumeration.
for o in ${ALPHA_EXTRA_OBJS:-}; do OBJS="$OBJS $o"; done

# Enumerate the DEFINED universals from the compiled EVAX objects using LINK.EXE's
# OWN evax_read view (OVMX_LINK_DUMP_UNIVERSALS=all, vms-a7a — the wider mode of
# the vms-614 dump mk_decc_shr.sh uses). Enumerating from the linker's reader,
# not a cross/host nm, keeps the vector in exact agreement with emit_shareable's
# universal resolution: a symbol nm calls "defined T" that evax_read does not
# treat as a universal-eligible global (a mount-visibility helper hit this) would
# otherwise fail %LINK-F-NOUNIV. The dump already emits `NAME=PROCEDURE|DATA` per
# line with EVAX companion labels (..en/..lk/...) dropped, so no classification
# here. The dummy --symbol-vector/-o only satisfy arg parsing; the dump exits
# before any emit.
VEC=$(OVMX_LINK_DUMP_UNIVERSALS=all "$LINK_EXE" --shareable \
        --symbol-vector "__ovmx_dump_probe=PROCEDURE" --gsmatch "$GSMATCH" \
        -o /dev/null $OBJS 2>/dev/null \
      | grep -E '=(PROCEDURE|DATA)$' | sort -u | paste -sd, -)
[ -n "$VEC" ] || { echo "mk_alpha_shr: FAIL empty symbol vector (LINK dump read no defined universals?)" >&2; exit 2; }
[ -n "${ALPHA_EXTRA_VEC:-}" ] && VEC="$VEC,$ALPHA_EXTRA_VEC"
NVEC=$(printf '%s' "$VEC" | tr ',' '\n' | grep -c '=' || true)
echo "mk_alpha_shr: $(basename "$OUT") symbol vector has $NVEC universals"

LINK_FLAGS="--shareable --symbol-vector $VEC --gsmatch $GSMATCH $USE_ARGS"
[ "${ALPHA_ALLOW_UNDEF:-0}" = 1 ] && LINK_FLAGS="$LINK_FLAGS --allow-undefined"
# shellcheck disable=SC2086
"$LINK_EXE" $LINK_FLAGS -o "$OUT" $OBJS
echo "mk_alpha_shr: created $OUT"
