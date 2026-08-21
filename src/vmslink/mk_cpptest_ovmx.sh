#!/bin/sh
# mk_cpptest_ovmx.sh — build recipe for CPPTEST.EXE, a MINIMAL C++ program
# linked as a VMS-native OVMX executable image (bead vms-5562, epic vms-da0
# "GCC-as-VMS-oracle" — F2b: the C++-runtime wall probe, done with a tiny TU
# instead of the 300MB cc1/cc1plus this epic's F2a already host-side-proved).
#
# SHAPE: mirrors mk_as.sh's "compile with the stock upstream build, then
# LINK.EXE --executable --use {DECC$SHR + the five OVMX shareables} -o OUT"
# pattern, but the compile step here is the OPPOSITE of mk_as.sh's/mk_tcc.sh's
# freestanding-musl profile: cpptest.cpp is compiled HOSTED (no -ffreestanding,
# no -fno-builtin), same reasoning as third-party/gcc/host-probe-cc1.sh (F2a) —
# the C++ standard library headers (<string>, <stdexcept>) #error under a
# freestanding toolchain (gmp.h's C++ shim hits the identical #error for the
# same reason, per host-probe-cc1.sh's "WALL E" comment), and a normal C++
# program that will run against libstdc++ is not itself freestanding code.
# What makes CPPTEST.EXE an OVMX image is how it is LINKED (LINK.EXE, not
# `g++ -o` / ld) and ACTIVATED (IMGACT.EXE, not the ELF interpreter/ld.so
# g++'s own driver would normally wire up) -- not the compile flags.
#
# C++ RUNTIME (operator's maintainability guard: WHOLE-ARCHIVE UPSTREAM,
# NEVER hand-implement a libstdc++ slice): the container's own Alpine `g++`
# package ships a real, upstream-built libstdc++.a/libgcc.a/libgcc_eh.a
# (musl-hosted) -- exactly the toolchain host-probe-cc1.sh already uses to
# host-side-build cc1/cc1plus itself (F2a). This script locates those
# archives via `$CXX -print-file-name=...` and passes them to LINK.EXE
# alongside cpptest.o. LINK.EXE's producer side ingests a whole `ar` archive
# per positional argument (link.c's header comment: "N relocatable ELF
# objects (and/or whole `ar` archives ... no ld -r)") -- there is no
# ld-style --whole-archive/--gc-sections selective pull to configure; every
# member of every archive named on the command line is ingested. This is a
# probe of what LINK.EXE does with that whole set, not a curated subset.
#
# crtbeginT.o/crtendT.o (OPTIONAL, best-effort): a normal static g++ link
# also includes these two GCC-internal objects, whose frame_dummy()
# .init_array entry calls __register_frame_info() to register the TU's
# .eh_frame data with libgcc's unwinder -- the mechanism a FULLY STATIC C++
# binary uses instead of the PT_GNU_EH_FRAME-segment / dl_iterate_phdr path
# a normal ld-linked ET_DYN/PIE consumer would use (LINK.EXE emits neither a
# PT_GNU_EH_FRAME phdr nor a dynamic section per se -- see the vms-5562
# report). This script includes them on the LINK.EXE line if found and
# reports whether they were available; whether the RESULT is enough for
# _Unwind_Find_FDE to locate cpptest's .eh_frame is exactly the empirical
# question this probe exists to answer (report it, don't assume it).
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain -- do NOT edit them here. If LINK.EXE/IMGACT.EXE need
# a change to carry this further, REPORT it (a backfill item), don't patch
# it in this recipe.
#
# Usage:  mk_cpptest_ovmx.sh <LINK.EXE> <out-CPPTEST.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             [cpptest.cpp path]
# Env:    CXX (default g++), WORK (default /tmp/mk-cpptest)
# Must run in the musl container with a host C++ toolchain (Alpine's `g++`
# package -- same container shape run-host-probe-cc1.sh uses).
set -e

LINK_EXE=${1:?usage: mk_cpptest_ovmx.sh <LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> [cpptest.cpp]}
OUT=${2:?need output CPPTEST.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)                                  # src/vmslink
CPPTEST_SRC=${9:-$(cd "$HERE/../../third-party/gcc" && pwd)/cpptest.cpp}
CXX=${CXX:-g++}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR"; do
    [ -f "$f" ] || { echo "mk_cpptest_ovmx: producer image not found: $f"; exit 1; }
done
[ -f "$CPPTEST_SRC" ] || { echo "mk_cpptest_ovmx: cpptest.cpp not found: $CPPTEST_SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-cpptest}
rm -rf "$WORK"
mkdir -p "$WORK"

echo "mk_cpptest_ovmx: $CXX -fPIC -c cpptest.cpp (HOSTED -- no -ffreestanding, musl+libstdc++ live, mirrors host-probe-cc1.sh)"
# -fPIC (NOT gcc's -fPIE default): vms-608 root-caused the earlier SIGSEGV
# (stdout resolving to a garbage FILE*) to -fPIE's direct/copy-relocation
# data-access model (R_X86_64_PC32 straight at a cross-image =DATA symbol) --
# LINK.EXE's import collection only promotes is_call (PLT32/CALL26) or
# is_gotr (GOT) relocations to cross-image imports, so a PC32-to-cross-image-
# DATA site is silently left unresolved (see src/vmslink/test/
# debug_stdout_data_reloc.sh and docs/ovmx-image-abi.md). -fPIC makes gcc
# emit R_X86_64_REX_GOTPCRELX (GOT-indirect) for that same access, which
# LINK.EXE already binds correctly -- and matches EVERY other producer in the
# OVMX graph (mk_decc_shr.sh's whole-archived musl aside, every mk_*.sh that
# compiles its own C/C++ sources already hardcodes -fPIC in CFLAGS; cpptest
# was the sole holdout). This is the OVMX image ABI (docs/ovmx-image-abi.md),
# not a per-test workaround -- do not remove it.
COMPILE_LOG="$WORK/compile.log"
if ! "$CXX" -std=c++17 -O2 -Wall -fPIC -c -o "$WORK/cpptest.o" "$CPPTEST_SRC" >"$COMPILE_LOG" 2>&1; then
    echo "mk_cpptest_ovmx: FAIL: $CXX could not compile cpptest.cpp -- see $COMPILE_LOG"
    tail -40 "$COMPILE_LOG"
    exit 1
fi
[ -f "$WORK/cpptest.o" ] || { echo "mk_cpptest_ovmx: FAIL: cpptest.o not produced"; exit 1; }
echo "  compiled: $WORK/cpptest.o"

echo "mk_cpptest_ovmx: locate the upstream C++ runtime archives ($CXX -print-file-name=...)"
LIBSTDCXX=$("$CXX" -print-file-name=libstdc++.a)
LIBGCC=$("$CXX" -print-file-name=libgcc.a)
LIBGCC_EH=$("$CXX" -print-file-name=libgcc_eh.a)
LIBSUPCXX=$("$CXX" -print-file-name=libsupc++.a)
CRTBEGIN=$("$CXX" -print-file-name=crtbeginT.o)
CRTEND=$("$CXX" -print-file-name=crtendT.o)

[ -f "$LIBSTDCXX" ] || { echo "mk_cpptest_ovmx: FAIL: libstdc++.a not found via $CXX -print-file-name (got '$LIBSTDCXX') -- need the g++ package"; exit 1; }
[ -f "$LIBGCC" ]    || { echo "mk_cpptest_ovmx: FAIL: libgcc.a not found (got '$LIBGCC')"; exit 1; }
echo "  libstdc++.a : $LIBSTDCXX"
echo "  libgcc.a    : $LIBGCC"
if [ -f "$LIBGCC_EH" ]; then echo "  libgcc_eh.a : $LIBGCC_EH (found, unwinder support routines)"; else echo "  libgcc_eh.a : NOT FOUND (got '$LIBGCC_EH') -- unwind-dw2.c support may already be folded into libgcc.a; proceeding without it"; LIBGCC_EH=""; fi
if [ -f "$LIBSUPCXX" ]; then echo "  libsupc++.a : $LIBSUPCXX (found, separate from libstdc++.a)"; else echo "  libsupc++.a : NOT FOUND (got '$LIBSUPCXX') -- likely already folded into libstdc++.a on this toolchain; proceeding without it"; LIBSUPCXX=""; fi
# crtbeginT.o/crtendT.o are DELIBERATELY NOT passed to LINK.EXE (vms-70d).
# Their purpose in a normal static link is EH-frame registration:
# crtbeginT provides frame_dummy() (an .init_array ctor calling
# __register_frame_info(__EH_FRAME_BEGIN__)) and crtendT provides the
# terminating 0-length FDE (__FRAME_END__). LINK.EXE now does BOTH jobs
# natively and more robustly: it lays every .eh_frame input section out
# contiguously, appends its OWN 0-terminator after the LAST one, and records
# the block start + __register_frame in .vms$ehf so IMGACT registers the frames
# before .init_array runs. Passing crtendT.o here would insert its __FRAME_END__
# ZERO in the MIDDLE of that contiguous block (crtend is linked before the
# libstdc++/libgcc archives), prematurely terminating the registry's FDE walk
# so libstdc++/libgcc frames are never seen -> the unwinder misses them. So we
# rely solely on LINK.EXE's synthesized begin/terminator/registration.
CRT_OBJS=""
if [ -f "$CRTBEGIN" ] && [ -f "$CRTEND" ]; then
    echo "  crtbeginT.o/crtendT.o : found but INTENTIONALLY NOT linked (LINK.EXE synthesizes .eh_frame begin/terminator + registration natively -- vms-70d)"
else
    echo "  crtbeginT.o/crtendT.o : not found -- not needed (LINK.EXE handles EH-frame registration natively, vms-70d)"
fi

echo
echo "mk_cpptest_ovmx: LINK.EXE --executable --use {6 producers} -> $OUT"
# --allow-undefined: same posture as mk_as.sh -- this is the FIRST attempt at
# this link and the exact deferred/unresolved set (if any) is part of the
# vms-5562 deliverable, not something to hide or hard-assert in advance.
LINK_ERR="$WORK/cpptest-link.err"
set +e
# shellcheck disable=SC2086
"$LINK_EXE" --executable --allow-undefined \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" \
    -o "$OUT" "$WORK/cpptest.o" $CRT_OBJS \
    "$LIBSTDCXX" $LIBSUPCXX "$LIBGCC" $LIBGCC_EH 2>"$LINK_ERR"
LRC=$?
set -e
cat "$LINK_ERR" >&2
if [ "$LRC" -ne 0 ]; then
    echo "mk_cpptest_ovmx: FAIL: LINK.EXE --executable exited $LRC -- see $LINK_ERR above (THE WALL, if this is where it stopped)"
    exit 1
fi

DEFEXT=$(grep -oE 'LINK-I-DEFEXT, [0-9]+' "$LINK_ERR" | grep -oE '[0-9]+')
echo "mk_cpptest_ovmx: LINK.EXE succeeded; deferred externals (LINK-I-DEFEXT) = ${DEFEXT:-0}"
echo "mk_cpptest_ovmx: created $OUT"
