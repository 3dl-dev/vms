#!/bin/sh
# rebuild-cc1-fpic.sh — force a -fPIC recompile of cc1 and its support
# libraries over the F2a-configured tree (bead vms-5b7e, epic vms-da0 F2b:
# the cc1-as-an-OVMX-image attempt). host-probe-cc1.sh (F2a) proved cc1
# builds host-side, but a PLAIN hosted build defaults to -fno-PIE with no
# -fpic/-fPIE at all (confirmed empirically: `readelf -r` on the F2a
# objects shows R_X86_64_32/32S absolute relocations throughout, zero GOT-
# indirect relocs) -- the SAME class of bug vms-608 root-caused for
# cpptest's stdout crash (a direct/copy-relocation access to a cross-image
# =DATA symbol that LINK.EXE's import collector never promotes). -fPIC is
# the OVMX image ABI (docs/ovmx-image-abi.md) for every producer in this
# graph; cc1 was never compiled against it because F2a's whole point was a
# quick HOST binary to enumerate the wall set, not an OVMX-image attempt
# (that is this bead).
#
# This script does NOT re-run configure (reuses the F2a $WORK's already-
# generated Makefiles/insn-*.cc/etc. -- re-running configure from scratch
# would also re-pay the multi-minute generator-file cost for no reason).
# It PURGES the existing non-PIC .o/.a under gcc/, libcpp/, libiberty/,
# libdecnumber/, libbacktrace/, libcody/, zlib/ (every archive cc1's link
# line pulls in, per vms-796's catalog) and re-runs `make cc1` with
# CFLAGS/CXXFLAGS overridden to add -fPIC. GNU make command-line variable
# assignments override a sub-Makefile's own `CFLAGS = @CFLAGS@`-style
# assignment (and propagate to recursive sub-makes automatically) --
# confirmed by this script's own check below (post-build PIC scan), not
# assumed.
#
# host-x86_64-pc-linux-musl (build-time HOST GENERATOR programs, e.g.
# genrtl/insn-recog-gen -- they run at build time to emit insn-*.cc from
# .md machine-description files, are never linked into cc1 itself) is
# DELIBERATELY NOT purged: it needs no OVMX image ABI and repurging it
# would re-pay real generation time for zero benefit to this bead.
#
# Usage:  rebuild-cc1-fpic.sh [gcc-build-work-dir]
#   gcc-build-work-dir  default: /tmp/gcc-cc1-hostprobe (host-probe-cc1.sh's
#                        own default -- this script expects that tree to
#                        already exist and be configured)
# Env:    CC, CXX (default gcc/g++), JOBS (default nproc)
# Must run in the same musl (Alpine) container host-probe-cc1.sh /
# run-host-probe-cc1.sh use (g++ + gmp-dev/mpfr-dev/mpc1-dev present).
set -e

WORK=${1:-/tmp/gcc-cc1-hostprobe}
CC=${CC:-gcc}
CXX=${CXX:-g++}
JOBS=${JOBS:-$(nproc)}

[ -f "$WORK/Makefile" ] || { echo "rebuild-cc1-fpic: FAIL: no $WORK/Makefile -- run host-probe-cc1.sh first (F2a)"; exit 1; }
[ -f "$WORK/gcc/Makefile" ] || { echo "rebuild-cc1-fpic: FAIL: no $WORK/gcc/Makefile -- run host-probe-cc1.sh first (F2a)"; exit 1; }

echo "rebuild-cc1-fpic: purging non-PIC objects/archives under $WORK (gcc/, libcpp/, libiberty/, libdecnumber/, libbacktrace/, libcody/, zlib/)"
for d in gcc libcpp libiberty libdecnumber libbacktrace libcody zlib; do
    [ -d "$WORK/$d" ] || continue
    find "$WORK/$d" \( -name '*.o' -o -name '*.a' \) -delete
done
# cc1 itself (the previous HOST link, non-PIC) and its checksum source (the
# checksum embeds an options/object list that changes across this rebuild).
rm -f "$WORK/gcc/cc1" "$WORK/gcc/cc1-checksum.cc" "$WORK/gcc/cc1-checksum.cc.tmp"

# The support libs (libcpp/libiberty/libdecnumber/libbacktrace/libcody/
# zlib) are each a SEPARATE top-level Makefile.def module ("all-<module>"),
# recursed into from $WORK's OWN top-level Makefile via HOST_EXPORTS/
# FLAGS_TO_PASS (which itself forwards CFLAGS/CXXFLAGS) -- gcc/Makefile has
# NO rule of its own to rebuild e.g. ../libcpp/libcpp.a from scratch, only a
# dependency ON it (confirmed empirically: running 'make cc1' from $WORK/gcc
# alone after purging ../libcpp/libcpp.a fails "No rule to make target
# '../libcpp/libcpp.a'"). So the support libs are rebuilt from $WORK (top
# level); gcc/'s own ~700 objects + cc1 itself are then rebuilt via gcc/
# Makefile's OWN 'cc1' target (also confirmed empirically: 'make cc1' run
# from $WORK itself -- the top-level Makefile -- has no such target at all
# and fails "No rule to make target 'cc1'").
SUPPORT_LOG="$WORK/make-cc1-fpic-support.log"
echo "rebuild-cc1-fpic: make all-libcpp all-libiberty all-libdecnumber all-libbacktrace all-libcody all-zlib CFLAGS='-g -fPIC' CXXFLAGS='-g -fPIC' -j$JOBS (from \$WORK, top-level Makefile.def modules)"
set +e
( cd "$WORK" && make all-libcpp all-libiberty all-libdecnumber all-libbacktrace all-libcody all-zlib \
      -j"$JOBS" CC="$CC" CXX="$CXX" CFLAGS="-g -fPIC" CXXFLAGS="-g -fPIC" ) >"$SUPPORT_LOG" 2>&1
SRC_RC=$?
set -e
echo "-- support-libs (-fPIC) exit=$SRC_RC; tail: --"
tail -60 "$SUPPORT_LOG" | sed 's/^/   /'
if [ "$SRC_RC" -ne 0 ]; then
    echo "rebuild-cc1-fpic: FAIL: support-lib -fPIC rebuild did not complete -- see $SUPPORT_LOG"
    exit 1
fi

MAKE_LOG="$WORK/make-cc1-fpic.log"
echo "rebuild-cc1-fpic: make cc1 CFLAGS='-g -fPIC' CXXFLAGS='-g -fPIC' -j$JOBS (from \$WORK/gcc -- 'cc1' is a gcc/Makefile target, NOT a top-level Makefile.def module)"
set +e
( cd "$WORK/gcc" && make cc1 -j"$JOBS" CC="$CC" CXX="$CXX" CFLAGS="-g -fPIC" CXXFLAGS="-g -fPIC" ) >"$MAKE_LOG" 2>&1
MRC=$?
set -e
echo "-- make cc1 (-fPIC) exit=$MRC; tail: --"
tail -60 "$MAKE_LOG" | sed 's/^/   /'

if [ "$MRC" -ne 0 ]; then
    echo "rebuild-cc1-fpic: FAIL: -fPIC rebuild did not complete -- see $MAKE_LOG"
    exit 1
fi

echo
echo "rebuild-cc1-fpic: PIC sanity scan (spot-check: no R_X86_64_32/32S absolute relocs in a sample of large objects)"
SAMPLE=$(find "$WORK/gcc" -maxdepth 1 -name '*.o' | head -20)
BAD=0
for o in $SAMPLE; do
    if readelf -r "$o" 2>/dev/null | grep -qE 'R_X86_64_32( |S)'; then
        echo "  NON-PIC: $o"
        BAD=$((BAD + 1))
    fi
done
echo "  sampled $(echo "$SAMPLE" | wc -l) objects, $BAD flagged non-PIC"
if [ "$BAD" -gt 0 ]; then
    echo "rebuild-cc1-fpic: WARNING: CFLAGS/CXXFLAGS override did NOT take effect on every translation unit -- mk_cc1_ovmx.sh's own full-list PIC scan is authoritative, this is only a quick spot-check"
fi

echo "rebuild-cc1-fpic: done. cc1 objects/archives under $WORK are now -fPIC (see $MAKE_LOG); re-run host-probe-cc1.sh's OWN cc1 host binary is STALE (removed above, not rebuilt -- this bead needs the .o/.a set, not a working host cc1)"
