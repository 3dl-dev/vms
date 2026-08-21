#!/bin/sh
# mk_cc1_ovmx.sh — build recipe attempting CC1.EXE, GCC's C compiler proper
# (cc1), linked as a VMS-native OVMX executable image (bead vms-5b7e, epic
# vms-da0 "GCC-as-VMS-oracle" — F2b/F2c: the REAL PRIZE of the lane, a big
# C++ program using the same runtime C++ first-light (vms-5562/CPPTEST.EXE)
# already proved). Mirrors mk_cpptest_ovmx.sh's shape (compile with the
# stock upstream build, whole-archive the C++ runtime, LINK.EXE --executable
# --use {6 producers} -o OUT) scaled up to cc1's ~956-object, ~324MB build.
#
# CATALOG-ONLY (operator instruction): this script's job is to reach and
# REPORT the first genuine wall (link / static-lib / activation), not to
# force a working cc1. link.c/imgact.c are the complete toolchain and are
# OUT of this bead's file-domain — do NOT edit them here; a LINK.EXE/
# IMGACT.EXE gap found while running this script is a finding to report, a
# scoped backfill item, not a patch to slip in here.
#
# PREREQUISITE (NOT done by this script): a HOSTED -fPIC build of cc1 and
# its support libraries (libcpp/libiberty/libdecnumber/libbacktrace/libcody/
# zlib), i.e. the OVMX image ABI (docs/ovmx-image-abi.md, vms-608) applied
# to cc1's own build — cc1's normal host build (host-probe-cc1.sh, F2a) uses
# -fno-PIE / R_X86_64_32 absolute relocations throughout (confirmed via
# readelf -r on the F2a objects: zero GOT-indirect relocs), the SAME class
# of bug vms-608 root-caused for cpptest's stdout crash, at 956-object
# scale. See third-party/gcc/rebuild-cc1-fpic.sh for that rebuild step (runs
# `make cc1 CFLAGS=-fPIC CXXFLAGS=-fPIC` over the F2a-configured tree after
# purging its non-PIC objects/archives) — run that FIRST, then this script.
#
# OBJECT/ARCHIVE LIST — NOT hardcoded (maintainability): cc1's own build
# emits its final "g++ ... -o cc1 <objects+archives> -lmpc -lmpfr -lgmp
# -rdynamic -L./../zlib -lz" link line into $WORK/make-cc1.log every time it
# builds (see rebuild-cc1-fpic.sh). This script PARSES that line for the
# object/archive list, so it self-updates across a GCC version bump or a
# frontend/backend file being added/removed upstream, rather than drifting
# out of sync with a copy-pasted list (the exact class of staleness
# CLAUDE.md's mk_libvms_shr.sh LIST comment warns about).
#
# Usage:  mk_cc1_ovmx.sh <LINK.EXE> <out-CC1.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             [gcc-build-work-dir]
# Env:    CXX (default g++, used only to -print-file-name the upstream C++/
#         math-runtime archives — NOT to recompile cc1, that already
#         happened in gcc-build-work-dir), WORK (scratch, default
#         /tmp/mk-cc1)
# Must run in the SAME musl (Alpine) container rebuild-cc1-fpic.sh used —
# same reasoning as mk_cpptest_ovmx.sh (a host C++ toolchain + the musl
# libstdc++/libgcc archives must be live).
set -e

LINK_EXE=${1:?usage: mk_cc1_ovmx.sh <LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> [gcc-build-work-dir]}
OUT=${2:?need output CC1.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
GCC_WORK=${9:-/tmp/gcc-cc1-hostprobe}
CXX=${CXX:-g++}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR"; do
    [ -f "$f" ] || { echo "mk_cc1_ovmx: producer image not found: $f"; exit 1; }
done
MAKE_LOG="$GCC_WORK/make-cc1.log"
[ -f "$MAKE_LOG" ] || { echo "mk_cc1_ovmx: FAIL: no $MAKE_LOG — run rebuild-cc1-fpic.sh first (need a -fPIC cc1 build)"; exit 1; }
[ -f "$GCC_WORK/gcc/cc1" ] || echo "mk_cc1_ovmx: WARNING: $GCC_WORK/gcc/cc1 (the HOST link) not present or stale — proceeding anyway, this script only needs the .o/.a objects, not that binary"

WORK=${WORK:-/tmp/mk-cc1}
rm -rf "$WORK"
mkdir -p "$WORK"

echo "mk_cc1_ovmx: parse cc1's own final link line out of $MAKE_LOG (self-updating object/archive list)"
# The recipe line looks like:
#   g++ -no-pie ... -o cc1 <objs+archives...> -lmpc -lmpfr -lgmp -rdynamic -L./../zlib -lz
# Take the LAST such line (in case of a partial/incremental log), strip the
# leading "g++ ... -o cc1 " prefix and the trailing system-lib tail.
LINKLINE=$(grep -E '^g\+\+ .* -o cc1 ' "$MAKE_LOG" | tail -1)
[ -n "$LINKLINE" ] || { echo "mk_cc1_ovmx: FAIL: no 'g++ ... -o cc1 ...' link line found in $MAKE_LOG — cc1 build did not reach its final link step"; exit 1; }

# Strip everything up to and including "-o cc1 " (the objects/archives start
# right after); then strip the trailing " -lmpc -lmpfr -lgmp ... -lz" system
# math/zlib tail (we supply STATIC equivalents ourselves, per vms-5b7e).
OBJLIST_RAW=$(echo "$LINKLINE" | sed -E 's/^.* -o cc1 //; s/ -lmpc.*$//')
[ -n "$OBJLIST_RAW" ] || { echo "mk_cc1_ovmx: FAIL: could not isolate the object/archive list from the link line"; exit 1; }

echo "mk_cc1_ovmx: resolve each parsed path against \$GCC_WORK/gcc, verify -fPIC (no absolute code relocs)"
NEWLIST=""
MISSING=0
NONPIC=0
for tok in $OBJLIST_RAW; do
    case "$tok" in
        -*) NEWLIST="$NEWLIST $tok"; continue ;;   # stray flag, e.g. duplicated -rdynamic — pass through
    esac
    # tok is relative to $GCC_WORK/gcc (that's where make-cc1.log's recipe ran)
    p="$GCC_WORK/gcc/$tok"
    p=$(cd "$(dirname "$p")" 2>/dev/null && pwd)/$(basename "$p") || p=""
    if [ -z "$p" ] || [ ! -f "$p" ]; then
        echo "  MISSING: $tok"
        MISSING=$((MISSING + 1))
        continue
    fi
    # PIC sanity check on .o members / plain .o (skip .a — mixed members,
    # spot-checked separately below): a NON-PIC x86_64 object compiled
    # without -fPIC/-fPIE typically carries R_X86_64_32/32S absolute-address
    # relocs against .rodata/.data; a real -fPIC object should not (GOT-
    # indirect / PC32 / PLT32 instead) — same class of check vms-608 used.
    case "$p" in
        *.o)
            if readelf -r "$p" 2>/dev/null | grep -qE 'R_X86_64_32( |S)'; then
                NONPIC=$((NONPIC + 1))
            fi
            ;;
    esac
    NEWLIST="$NEWLIST $p"
done
echo "  objects/archives resolved: $(echo $OBJLIST_RAW | wc -w) parsed, $MISSING missing, $NONPIC flagged non-PIC (R_X86_64_32/32S present)"
if [ "$MISSING" -gt 0 ]; then
    echo "mk_cc1_ovmx: FAIL: $MISSING object(s)/archive(s) from cc1's link line not found under $GCC_WORK/gcc — rebuild-cc1-fpic.sh did not complete cleanly"
    exit 1
fi
if [ "$NONPIC" -gt 0 ]; then
    echo "mk_cc1_ovmx: FAIL: $NONPIC object(s) are NOT -fPIC (absolute R_X86_64_32/32S relocs present) — rebuild-cc1-fpic.sh's CFLAGS/CXXFLAGS override did not take effect on every translation unit. THIS IS THE OVMX IMAGE ABI (vms-608) — do not attempt the LINK.EXE step against non-PIC objects (same silent =DATA-import failure class cpptest hit)."
    exit 1
fi

echo
echo "mk_cc1_ovmx: locate the upstream C++/math-runtime archives (STATIC — vms-5b7e: GMP/MPFR/MPC static-linked like libgcc, NOT DECC\$SHR exports)"
LIBSTDCXX=$("$CXX" -print-file-name=libstdc++.a)
LIBGCC=$("$CXX" -print-file-name=libgcc.a)
LIBGCC_EH=$("$CXX" -print-file-name=libgcc_eh.a)
LIBSUPCXX=$("$CXX" -print-file-name=libsupc++.a)
LIBGMP=$("$CXX" -print-file-name=libgmp.a)
LIBMPFR=$("$CXX" -print-file-name=libmpfr.a)
LIBMPC=$("$CXX" -print-file-name=libmpc.a)
LIBZ="$GCC_WORK/zlib/libz.a"   # cc1's OWN in-tree zlib (its link line uses -L./../zlib -lz, not system libz)

[ -f "$LIBSTDCXX" ] || { echo "mk_cc1_ovmx: FAIL: libstdc++.a not found via $CXX -print-file-name (got '$LIBSTDCXX')"; exit 1; }
[ -f "$LIBGCC" ]    || { echo "mk_cc1_ovmx: FAIL: libgcc.a not found (got '$LIBGCC')"; exit 1; }
echo "  libstdc++.a : $LIBSTDCXX"
echo "  libgcc.a    : $LIBGCC"
if [ -f "$LIBGCC_EH" ]; then echo "  libgcc_eh.a : $LIBGCC_EH"; else echo "  libgcc_eh.a : NOT FOUND — proceeding without it (per mk_cpptest_ovmx.sh precedent)"; LIBGCC_EH=""; fi
if [ -f "$LIBSUPCXX" ]; then echo "  libsupc++.a : $LIBSUPCXX"; else echo "  libsupc++.a : NOT FOUND — likely folded into libstdc++.a"; LIBSUPCXX=""; fi

GMPWALL=0
if [ -f "$LIBGMP" ]; then echo "  libgmp.a    : $LIBGMP"; else echo "  libgmp.a    : NOT FOUND (got '$LIBGMP') -- Alpine's gmp-dev package may ship .so only"; GMPWALL=1; fi
if [ -f "$LIBMPFR" ]; then echo "  libmpfr.a   : $LIBMPFR"; else echo "  libmpfr.a   : NOT FOUND (got '$LIBMPFR') -- Alpine's mpfr-dev package may ship .so only"; GMPWALL=1; fi
if [ -f "$LIBMPC" ]; then echo "  libmpc.a    : $LIBMPC"; else echo "  libmpc.a    : NOT FOUND (got '$LIBMPC') -- Alpine's mpc1-dev package may ship .so only"; GMPWALL=1; fi
if [ -f "$LIBZ" ]; then echo "  libz.a (in-tree): $LIBZ"; else echo "  libz.a (in-tree): NOT FOUND at $LIBZ"; GMPWALL=1; fi

if [ "$GMPWALL" -ne 0 ]; then
    echo "mk_cc1_ovmx: FAIL: GMP/MPFR/MPC/z static-link wall (vms-5b7e headline question) -- one or more static archives are missing."
    echo "  THIS IS THE WALL if it stopped here: vendor GMP/MPFR/MPC source (same clean-room basis as third-party/gcc) and build"
    echo "  them statically -fPIC, OR confirm Alpine ships static .a for these -dev packages and locate them by an explicit path"
    echo "  rather than -print-file-name (that flag only searches the COMPILER's own default library dirs, not every -dev package path)."
    exit 1
fi

echo
echo "mk_cc1_ovmx: LINK.EXE --executable --use {6 producers} -> $OUT"
echo "  ($(echo $OBJLIST_RAW | wc -w) cc1 objects/archives + libstdc++/libsupc++/libgcc + static gmp/mpfr/mpc/z)"
LINK_ERR="$WORK/cc1-link.err"
set +e
# shellcheck disable=SC2086
"$LINK_EXE" --executable --allow-undefined \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" \
    -o "$OUT" $NEWLIST \
    "$LIBSTDCXX" $LIBSUPCXX "$LIBGCC" $LIBGCC_EH \
    "$LIBGMP" "$LIBMPFR" "$LIBMPC" "$LIBZ" 2>"$LINK_ERR"
LRC=$?
set -e
cat "$LINK_ERR" >&2
if [ "$LRC" -ne 0 ]; then
    echo "mk_cc1_ovmx: FAIL: LINK.EXE --executable exited $LRC -- see $LINK_ERR above (THE WALL, if this is where it stopped)"
    exit 1
fi

DEFEXT=$(grep -oE 'LINK-I-DEFEXT, [0-9]+' "$LINK_ERR" | grep -oE '[0-9]+')
echo "mk_cc1_ovmx: LINK.EXE succeeded; deferred externals (LINK-I-DEFEXT) = ${DEFEXT:-0}"
ls -la "$OUT"
echo "mk_cc1_ovmx: created $OUT"
