#!/bin/bash
# build-libots.sh — build the OVMX LIBOTS$ shareable for alpha-dec-vms (vms-bfd6).
#
# Compiles the OTS$ runtime (tools/cross-alpha-vms/ots/ots_runtime.c +
# ots_home_args.s) with the alpha-dec-vms cross toolchain to genuine EVAX
# objects, then links them into a SEPARATE VMS shareable, LIBOTS$SHR.EXE, whose
# .vms$sv exports the OTS$ universals the GCC port emits calls to:
#
#   OTS$DIV_I  OTS$DIV_L  OTS$DIV_UI OTS$DIV_UL   (integer divide — Alpha has no
#   OTS$REM_I  OTS$REM_L  OTS$REM_UI OTS$REM_UL    integer-divide instruction)
#   OTS$HOME_ARGS   (varargs prologue register-homing)
#   OTS$MOVE OTS$ZERO   (block move / zero)
#
# This is the FAITHFUL OpenVMS shape: LIBOTS$ is a distinct image from DECC$SHR.
# mk_decc_shr.sh then links DECC$SHR with `--use LIBOTS$SHR.EXE` (via DECC_USE),
# so DECC$SHR's OTS$ references bind as real cross-image .vms$imp imports instead
# of deferring.
#
# Runs INSIDE the tools/cross-alpha-vms toolchain container. Build/oracle
# tooling, Rule-9-clean; writes only to $OUT (default /tmp), never into the repo.
#
# CLEAN-ROOM (Rule 8): the OTS$ interface is from public OpenVMS RTL docs +
# observed cc1 call-site codegen; the implementations are ours (see
# ots_runtime.c / ots_home_args.s headers). No VSI/HPE source was read.
#
# Env:  LINK_EXE (required — path to the OVMX LINK.EXE)
#       OTS_SRC  (dir holding ots_runtime.c/ots_home_args.s; default = this dir)
#       OUT      (output dir; default /tmp/libots-build)
#       PREFIX   (toolchain prefix; default /opt/cross-alpha-vms)
#
# KNOWN FIRST-LIGHT LIMIT (labelled, not hidden): a zero divisor returns
# quotient 0 rather than raising SS$_INTDIV — faithful VMS condition signalling
# needs the executive (LIB$SIGNAL), a later rung. No well-formed caller divides
# by zero; the correctness test (test_ots_div.c) exercises zero-numerator, not
# zero-divisor.
set -euxo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OTS_SRC=${OTS_SRC:-$HERE}
PREFIX=${PREFIX:-/opt/cross-alpha-vms}
OUT=${OUT:-/tmp/libots-build}
LINK_EXE=${LINK_EXE:?need LINK_EXE=<path to OVMX LINK.EXE>}
TARGET=alpha-dec-vms
export PATH="${PREFIX}/bin:${PATH}"
mkdir -p "$OUT"

# -g0 so the cross nm can read the objects to enumerate the symbol vector; the
# shipped shareable is byte-identical (LINK.EXE skips DST). -fno-builtin +
# -fno-tree-loop-distribute-patterns stop GCC re-synthesising a memcpy/memset/
# OTS$MOVE library call out of the C block loops (which would self-reference).
CF="-O2 -g0 -mpointer-size=64 -fno-builtin -fno-tree-loop-distribute-patterns"

"${TARGET}-gcc" $CF -c "$OTS_SRC/ots_runtime.c"  -o "$OUT/ots_runtime.o"
"${TARGET}-as"       "$OTS_SRC/ots_home_args.s"  -o "$OUT/ots_home_args.o"

# GUARD: the runtime objects must not reference any OTS$/decc$ symbol themselves
# (no self-recursion, no accidental CRTL dependency).
if "${TARGET}-nm" -u "$OUT/ots_runtime.o" "$OUT/ots_home_args.o" \
     | grep -qiE 'OTS\$|decc\$'; then
    echo "build-libots: FAIL — OTS runtime references an OTS\$/decc\$ symbol (self-dep)" >&2
    "${TARGET}-nm" -u "$OUT/ots_runtime.o" "$OUT/ots_home_args.o" | grep -iE 'OTS\$|decc\$' >&2
    exit 2
fi

# Symbol vector = every OTS$ PROCEDURE the objects define.
VEC=$("${TARGET}-nm" --defined-only "$OUT/ots_runtime.o" "$OUT/ots_home_args.o" \
      | awk '$NF ~ /^OTS\$/ && $(NF-1) ~ /^[Tt]$/ {print $NF"=PROCEDURE"}' \
      | sort | paste -sd,)
NVEC=$(printf '%s' "$VEC" | tr ',' '\n' | grep -c .)
echo "build-libots: symbol vector ($NVEC universals): $VEC"
[ "$NVEC" -eq 11 ] || { echo "build-libots: FAIL expected 11 OTS\$ universals, got $NVEC" >&2; exit 3; }

"$LINK_EXE" --shareable --symbol-vector "$VEC" --gsmatch LEQUAL,1,0 \
    -o "$OUT/LIBOTS_SHR.EXE" "$OUT/ots_runtime.o" "$OUT/ots_home_args.o"

echo "build-libots: created $OUT/LIBOTS_SHR.EXE (LIBOTS\$ shareable, $NVEC OTS\$ universals)"
