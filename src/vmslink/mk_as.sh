#!/bin/sh
# mk_as.sh — build recipe for AS.EXE, GNU binutils' assembler (gas,
# third-party/binutils) built as a VMS-native EXECUTABLE image (bead
# vms-0b6b, epic vms-da0 "GCC-as-VMS-oracle" — F1: the assembler
# forcing-function). Mirrors mk_tcc.sh's SHAPE (freestanding musl CFLAGS,
# then LINK.EXE --executable --use {DECC$SHR + the five OVMX shareables} -o
# AS.EXE) but NOT its per-TU enumeration: gas + the BFD/opcodes/libiberty it
# links is ~150 TUs across four sub-trees with generated headers (bfd's
# bfd-in2.h, opcodes' i386-tbl.h, gas's config.h/targ-cpu.h/...) that
# binutils' own top-level configure + recursive `make all-gas` already knows
# how to produce correctly. Re-deriving that dependency graph by hand (the
# tcc approach) would be a second, drifting copy of binutils' own build
# system. Instead: let `./configure` + `make all-gas` do the compiling (with
# CC/CFLAGS overridden to the same freestanding-musl profile mk_tcc.sh
# uses), then LINK.EXE the resulting objects + static archives directly —
# LINK.EXE's producer side explicitly supports ingesting whole `ar` archives
# in addition to loose .o objects (src/vmslink/link.c's header comment).
#
# RMS FILE I/O (vms-0b6b, mirrors vms-4ba.5): AS.EXE's own primary .s source
# read and output .o object write are routed through OVMX's RMS system
# services instead of raw fopen()/fread()/fwrite() for those two files. This
# is a small, clearly-labeled OVMX addition: a new companion TU
# (third-party/binutils/ovmx/ovmx_rms_io.c, compiled in below) plus seams in
# the otherwise-stock vendored gas source (gas/input-file.c's
# input_file_open/input_file_get/input_file_close/input_file_give_next_buffer,
# gas/output-file.c's output_file_create/output_file_close) — every edit is
# tagged "OVMX (vms-0b6b)" in the vendored files themselves so a diff against
# upstream binutils stays self-documenting. See
# third-party/binutils/ovmx/ovmx_rms_io.h for exactly what is and is not
# routed through RMS (gas's own #include search / any file OTHER than the
# named primary source is deliberately left on the stock path, same
# narrowing tcc's shim uses for header search).
#
# CONFIG: gas is configured --target=x86_64-elf (its own codegen target — the
# GNU as/bfd/opcodes tree is not itself "VMS-hosted" in any upstream sense;
# there is no x86_64-*-vms* target upstream, and this bead does not invent
# one. What makes AS.EXE VMS-native is how it is BUILT and LINKED — a musl
# freestanding compile + LINK.EXE --executable against the OVMX shareable
# graph, same as TCC.EXE — and, per the design doc's oracle framing, that its
# own file I/O for the two files it touches on the command line goes through
# native RMS rather than the CRTL Unix-shim path upstream GCC's VMS host
# layer would otherwise take). Its OUTPUT is standard ELF64 ET_REL x86_64,
# which is what LINK.EXE's producer side already consumes (src/vmslink/link.c)
# and what the GCC-as-VMS-oracle lane's endgame (an in-guest GNU ld/BFD for
# kernel builds) also expects.
#
# HOST-SIDE PROOF (this bead, before attempting the freestanding OVMX build):
# `./configure --target=x86_64-elf --disable-nls --disable-werror
# --disable-shared --disable-gdb --disable-sim --disable-ld --disable-gold
# --disable-binutils --disable-gprof --disable-gprofng --disable-libctf` +
# `make all-gas` with a PLAIN host CC (no freestanding flags, no OVMX_RMS_IO)
# produces a working `as-new` that assembles a trivial .s to a real ELF64
# ET_REL x86_64 object (confirmed via `readelf -h`) — see the vms-0b6b report
# for the exact vendoring corrections (po/Make-in, libiberty/testsuite/
# Makefile.in, libctf's swap.h) this proof surfaced and third-party/binutils/
# VENDOR-REV records. THIS script reruns that same configure, then rebuilds
# with the OVMX freestanding CFLAGS + OVMX_RMS_IO, then LINK.EXEs the result.
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain — do NOT edit them here.
#
# Usage:  mk_as.sh <LINK.EXE> <out-AS.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             [binutils-src-dir]
# Env:    CC (default gcc), ARCH (default x86_64 — also accepts aarch64,
#         selecting the same TLS-dialect/outline-atomics ARCHFLAG convention
#         lib_build_graph.sh uses), WORK (default /tmp/mk-as)
# Must run in the musl container where the producer .EXE already exist (see
# epic vms-da0 / CLAUDE.md test loop) — same environment shape as mk_tcc.sh.
set -e

LINK_EXE=${1:?usage: mk_as.sh <LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> [binutils-src]}
OUT=${2:?need output AS.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)                                     # src/vmslink
BINUTILS_SRC=${9:-$(cd "$HERE/../../third-party/binutils/src" && pwd)}  # third-party/binutils/src
OVMX_DIR=$(cd "$HERE/../../third-party/binutils/ovmx" && pwd)           # third-party/binutils/ovmx (vms-0b6b shim)
VMSRMS_INC=$(cd "$HERE/../vmsrms/include" && pwd)                       # rms/{fab,rab,rms}.h
LIBVMS_INC=$(cd "$HERE/../libvms/include" && pwd)                       # rmsdef.h/ssdef.h (rms.h deps)
CC=${CC:-gcc}
ARCH=${ARCH:-x86_64}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR"; do
    [ -f "$f" ] || { echo "mk_as: producer image not found: $f"; exit 1; }
done
[ -d "$BINUTILS_SRC" ] || { echo "mk_as: third-party/binutils/src not found: $BINUTILS_SRC"; exit 1; }
[ -d "$OVMX_DIR" ] || { echo "mk_as: OVMX shim dir not found: $OVMX_DIR"; exit 1; }

case "$ARCH" in
    aarch64) ARCHFLAG="-mno-outline-atomics" ;;
    x86_64)  ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "mk_as: unsupported ARCH=$ARCH (expected aarch64 or x86_64)"; exit 1 ;;
esac
case "$ARCH" in
    aarch64) GAS_TARGET=aarch64-elf ;;
    x86_64)  GAS_TARGET=x86_64-elf ;;
esac

WORK=${WORK:-/tmp/mk-as}
rm -rf "$WORK"
mkdir -p "$WORK"

echo "mk_as: ./configure --target=$GAS_TARGET (host tool, generates config.h + the Makefile graph)"
( cd "$WORK" && sh "$BINUTILS_SRC/configure" \
      --target="$GAS_TARGET" --disable-nls --disable-werror --disable-shared \
      --disable-gdb --disable-sim --disable-ld --disable-gold --disable-binutils \
      --disable-gprof --disable-gprofng --disable-libctf \
      CC="$CC" ) >"$WORK/configure.log" 2>&1 \
    || { echo "mk_as: configure failed, see $WORK/configure.log"; tail -40 "$WORK/configure.log"; exit 1; }
[ -f "$WORK/gas/Makefile" ] || { echo "mk_as: configure did not produce gas/Makefile"; exit 1; }

# -DOVMX_RMS_IO (vms-0b6b): activates the seams in gas/input-file.c and
# gas/output-file.c that route the primary .s read + the output .o's final
# delivery through RMS instead of raw fopen()/fread()/fwrite(). Applied
# uniformly across bfd/opcodes/libiberty/gas via the CFLAGS override below —
# harmless no-op for every TU that doesn't reference it (same convention as
# mk_tcc.sh's -DOVMX_RMS_IO).
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector $ARCHFLAG -U_FORTIFY_SOURCE"
CFLAGS="$CFLAGS -DOVMX_RMS_IO -I$OVMX_DIR -I$VMSRMS_INC -I$LIBVMS_INC"

echo "mk_as: make all-gas CC=$CC (freestanding musl CFLAGS + OVMX_RMS_IO, pulls bfd/opcodes/libiberty as deps)"
MAKE_LOG="$WORK/make.log"
set +e
( cd "$WORK" && make all-gas -j"$(nproc)" CC="$CC" CFLAGS="$CFLAGS" ) >"$MAKE_LOG" 2>&1
MRC=$?
set -e
echo "-- make all-gas exit=$MRC; tail: --"
tail -40 "$MAKE_LOG" | sed 's/^/   /'
[ "$MRC" -eq 0 ] || { echo "mk_as: FAIL: make all-gas (freestanding OVMX_RMS_IO build) exited $MRC — see $MAKE_LOG"; exit 1; }
[ -f "$WORK/gas/as-new" ] || { echo "mk_as: FAIL: make all-gas reported success but $WORK/gas/as-new is missing"; exit 1; }

echo "mk_as: compile the OVMX RMS I/O shim (vms-0b6b)"
$CC $CFLAGS -I"$WORK/gas" -c -o "$WORK/ovmx_rms_io.o" "$OVMX_DIR/ovmx_rms_io.c"

echo "mk_as: collect gas objects + the bfd/opcodes/libiberty static archives"
GAS_OBJS=$(find "$WORK/gas" -maxdepth 2 -name '*.o' ! -name 'as-new*' | sort)
NGASOBJ=$(echo "$GAS_OBJS" | grep -c . || true)
[ "$NGASOBJ" -gt 0 ] || { echo "mk_as: FAIL: no gas .o objects found under $WORK/gas"; exit 1; }
echo "  gas: $NGASOBJ objects"

find_archive() {
    # Prefer a libtool .libs/ copy if present (still a plain ar archive of
    # our just-compiled .o's; libtool's convenience-library shape), else the
    # top-level copy.
    name=$1
    f=$(find "$WORK" -name "$name" -path "*/.libs/*" 2>/dev/null | head -1)
    [ -n "$f" ] || f=$(find "$WORK" -maxdepth 2 -name "$name" 2>/dev/null | head -1)
    echo "$f"
}
LIBBFD=$(find_archive libbfd.a)
LIBOPCODES=$(find_archive libopcodes.a)
LIBIBERTY=$(find_archive libiberty.a)
for lib in "LIBBFD:$LIBBFD" "LIBOPCODES:$LIBOPCODES" "LIBIBERTY:$LIBIBERTY"; do
    val=${lib#*:}
    [ -n "$val" ] && [ -f "$val" ] || { echo "mk_as: FAIL: could not locate ${lib%%:*} under $WORK"; exit 1; }
done
echo "  bfd:       $LIBBFD"
echo "  opcodes:   $LIBOPCODES"
echo "  libiberty: $LIBIBERTY"

echo
echo "mk_as: LINK.EXE --executable --use {6 producers} -> $OUT"
# --allow-undefined: same posture as mk_tcc.sh — LINK.EXE's producer side
# ingests whole ar archives (link.c), so unlike mk_tcc.sh's fixed 12-object
# list this script does not know the exact deferred-import set in advance
# (gas/bfd/opcodes/libiberty is ~150 TUs' worth of external references, most
# already covered by DECC$SHR's whole-archived musl libc.a+libgcc.a, but
# this is the FIRST attempt at this link and the exact gap set is the
# vms-0b6b DELIVERABLE, not something to hide). This script does NOT
# hard-assert a specific deferred count the way mk_tcc.sh does (that assumed
# a known-good baseline from a prior successful link); it reports whatever
# LINK.EXE says and leaves pass/fail judgment to the caller (run_as_native.sh)
# and to the human-authored report — do not silently paper over a nonzero
# deferred count here.
# shellcheck disable=SC2086
LINK_ERR="$WORK/as-link.err"
set +e
"$LINK_EXE" --executable --allow-undefined \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" \
    -o "$OUT" $GAS_OBJS "$WORK/ovmx_rms_io.o" "$LIBBFD" "$LIBOPCODES" "$LIBIBERTY" 2>"$LINK_ERR"
LRC=$?
set -e
cat "$LINK_ERR" >&2
if [ "$LRC" -ne 0 ]; then
    echo "mk_as: FAIL: LINK.EXE --executable exited $LRC — see $LINK_ERR above (THE WALL, if this is where it stopped)"
    exit 1
fi

DEFEXT=$(grep -oE 'LINK-I-DEFEXT, [0-9]+' "$LINK_ERR" | grep -oE '[0-9]+')
echo "mk_as: LINK.EXE succeeded; deferred externals (LINK-I-DEFEXT) = ${DEFEXT:-0}"
echo "mk_as: created $OUT"
