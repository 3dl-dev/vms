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
# THE WALL (vms-0b6b, this bead — see the report for full detail): AS.EXE
# BUILDS and LINK.EXE's --executable step SUCCEEDS (162 objects, 245 deferred
# externals via --allow-undefined). IMGACT.EXE activates it — DECC$SHR,
# LIBVMS$SHR and LIBVMSPROCESS$SHR all load and relocate — but AS.EXE then
# SIGSEGVs (si_addr=0x8, a near-null dereference) before reaching main()'s
# own output. Root cause (confirmed via readelf on bfd's libbfd.o + the
# LINK.EXE build log): bfd/bfd.c carries a REAL GNU-C `.init_array` static
# constructor (one entry, -> .text.startup, registering bfd's internal
# page-size cache) — the first non-empty .init_array this toolchain's build
# recipes have ever produced (TCC.EXE/DECC$SHR are pure musl+libgcc with NO
# .init_array, per mk_decc_shr.sh's header comment). LINK.EXE prints
# "%LINK-W-RELSKIP ... .rela.init_array: 1 relocation NOT applied — target
# section .init_array is allocatable but LINK.EXE does not place it flat" —
# it SILENTLY DROPS the one constructor reference instead of running it at
# activation, so bfd's ctor-initialized state stays zeroed and the first
# real access into it (early in bfd_init()-adjacent startup, right around a
# getrusage() call per strace) dereferences near-NULL. This is the "static
# constructors via LIB$INITIALIZE#" forcing target the design doc predicted
# for F2 (GCC) — it surfaced one stage earlier, at F1 (as), because bfd
# itself already needs one. NOT an RMS/LIB$SPAWN/condition-handling/logical-
# name gap — it is LINK.EXE/IMGACT's activation path never running
# `.init_array` constructors at all.
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
[ -f "$WORK/Makefile" ] || { echo "mk_as: top-level configure did not produce a Makefile"; exit 1; }
# NOTE: gas/Makefile does NOT exist yet here — the top-level ./configure only
# emits the TOP-level Makefile; gas's own ./configure (and bfd's/opcodes's/
# libiberty's) runs LATER, as the `configure-gas` prerequisite of `make
# all-gas` below (standard binutils/gcc recursive-configure convention). An
# earlier version of this script checked for gas/Makefile right here and
# always failed — a harness bug (mistook this for a wall), not a build wall;
# fixed during this bead's build-proof iteration.

# -DOVMX_RMS_IO (vms-0b6b): activates the seams in gas/input-file.c and
# gas/output-file.c that route the primary .s read + the output .o's final
# delivery through RMS instead of raw fopen()/fread()/fwrite(). Applied
# uniformly across bfd/opcodes/libiberty/gas via the CFLAGS override below —
# harmless no-op for every TU that doesn't reference it (same convention as
# mk_tcc.sh's -DOVMX_RMS_IO).
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector $ARCHFLAG -U_FORTIFY_SOURCE"
CFLAGS="$CFLAGS -DOVMX_RMS_IO -I$OVMX_DIR -I$VMSRMS_INC -I$LIBVMS_INC"

echo "mk_as: compile the OVMX RMS I/O shim (vms-0b6b) first — gas's OWN internal"
echo "       'as-new' link (a host artifact make all-gas produces along the way,"
echo "       distinct from AS.EXE) needs it too, since input-file.o/output-file.o"
echo "       now reference ovmx_rms_*() unconditionally under -DOVMX_RMS_IO."
mkdir -p "$WORK/gas"
$CC $CFLAGS -c -o "$WORK/ovmx_rms_io.o" "$OVMX_DIR/ovmx_rms_io.c"

echo "mk_as: make configure-{bfd,opcodes,gas,libiberty,zlib,libsframe} (PLAIN CC/CFLAGS)"
# Two-phase build, discovered necessary during this bead's build-proof
# iteration: sub-configuring libiberty/zlib WITH the freestanding CFLAGS
# already applied makes autoconf's own compiler-capability probe conclude
# "GCC_NO_EXECUTABLES" (a -ffreestanding compile is, by autoconf's classic
# heuristic, assumed unable to produce a RUNNABLE test program — cross-
# compilation-shaped, even though we are building natively). Once that
# heuristic latches, every later AC_RUN_IFELSE probe in that subdir's
# configure hard-fails ("Link tests are not allowed after
# GCC_NO_EXECUTABLES") — libiberty's "checking for CET support" and zlib's
# own runtime probes both hit this. bfd/opcodes/gas's own configure.ac
# scripts happen not to need a run-time probe, so they built fine even with
# CFLAGS pre-polluted — this was invisible until libiberty/zlib's turn came.
# FIX: configure every subdir with the PLAIN host CC/CFLAGS first (so
# autoconf's capability probes see an ordinary, runnable-executable
# compiler), THEN recompile with the freestanding + OVMX_RMS_IO profile in
# a second `make all-gas` pass — since each subdir's Makefile already
# exists at that point, make treats configure as satisfied and goes
# straight to the compile rules with the new CFLAGS override.
CONFIGURE_LOG="$WORK/configure-subdirs.log"
( cd "$WORK" && make configure-bfd configure-opcodes configure-gas configure-libiberty configure-zlib configure-libsframe -j"$(nproc)" CC="$CC" ) >"$CONFIGURE_LOG" 2>&1
CRC=$?
echo "-- make configure-{bfd,opcodes,gas,libiberty,zlib,libsframe} exit=$CRC --"
[ "$CRC" -eq 0 ] || { echo "mk_as: FAIL: sub-configure (plain CC) failed — see $CONFIGURE_LOG"; tail -60 "$CONFIGURE_LOG"; exit 1; }

# Get our shim object onto as-new's OWN (host, intermediate — distinct from
# AS.EXE) link line. gas/Makefile.in's as-new rule is
# `$(LINK) $(as_new_OBJECTS) $(as_new_LDADD) $(LIBS)`, so appending to the
# now-generated gas/Makefile's `LIBS = @LIBS@` line does it. Tried first as a
# `make ... LIBS=...` command-line override (this bead's build-proof
# iteration) — that did NOT propagate through binutils' recursive-make
# FLAGS_TO_PASS (LIBS isn't one of the top-level-relayed variables, unlike
# CC/CFLAGS), so as-new's link kept missing the ovmx_rms_*() symbols even
# with the override present at the outer `make` invocation. Patching the
# generated (not vendored) Makefile directly is a build-time edit to an
# artifact this script itself just created, not a change to the vendored
# tree — no automake/Makefile.am edit needed.
[ -f "$WORK/gas/Makefile" ] || { echo "mk_as: FAIL: gas/Makefile missing after configure-gas"; exit 1; }
sed -i "s|^LIBS = .*|& $WORK/ovmx_rms_io.o|" "$WORK/gas/Makefile"
grep -q "ovmx_rms_io.o" "$WORK/gas/Makefile" || { echo "mk_as: FAIL: could not patch gas/Makefile's LIBS line"; exit 1; }

echo "mk_as: make all-gas CC=$CC (freestanding musl CFLAGS + OVMX_RMS_IO, pulls bfd/opcodes/libiberty as deps)"
MAKE_LOG="$WORK/make.log"
set +e
( cd "$WORK" && make all-gas -j"$(nproc)" CC="$CC" CFLAGS="$CFLAGS" ) >"$MAKE_LOG" 2>&1
MRC=$?
set -e
echo "-- make all-gas exit=$MRC; tail: --"
tail -40 "$MAKE_LOG" | sed 's/^/   /'
if [ "$MRC" -ne 0 ]; then
    # EXPECTED failure mode, once ovmx_rms_io.o is on as-new's link line: gas's
    # OWN internal "as-new" is a plain HOST ld link (glibc/musl ABI, not an
    # OVMX image) — it can never resolve sys$open/sys$get/sys$create/sys$put/
    # sys$connect/sys$close, because those are OVMX executive universals that
    # only bind through LINK.EXE's --use symbol-vector graph (RMS's shareable
    # image), never through a normal ELF dynamic/static host link. This is not
    # a build defect: it is the exact seam where "a host tool" stops and "an
    # OVMX image" starts. We don't need as-new (a host artifact we discard);
    # we need the .o's underneath it, all of which ARE built by this point
    # (make -j only fails the FINAL link step). Tolerate ONLY this signature;
    # anything else is a real compile/link regression and stays fatal.
    # Positive evidence only (no ": error:" exclusion — collect2 itself always
    # prints "collect2: error: ld returned 1 exit status" as PART of this
    # exact expected failure, which made an earlier version of this check
    # wrongly self-reject): compilation reached the final link step at all
    # (CCLD as-new — everything upstream of it compiled clean) AND the link
    # failure is specifically unresolved sys$* references, nothing else.
    if grep -Fq "CCLD     as-new" "$MAKE_LOG" \
        && grep -Fq "as-new] Error" "$MAKE_LOG" \
        && grep -Fq "undefined reference to \`sys\$" "$MAKE_LOG"; then
        echo "mk_as: make all-gas failed ONLY at as-new's host-ld link, on sys\$* — the"
        echo "  expected seam between 'host tool' and 'OVMX image'. Continuing to LINK.EXE."
    else
        echo "mk_as: FAIL: make all-gas (freestanding OVMX_RMS_IO build) exited $MRC — see $MAKE_LOG"
        exit 1
    fi
fi
GAS_OBJ_SAMPLE="$WORK/gas/input-file.o"
[ -f "$GAS_OBJ_SAMPLE" ] || { echo "mk_as: FAIL: $GAS_OBJ_SAMPLE missing — gas did not actually compile"; exit 1; }

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
