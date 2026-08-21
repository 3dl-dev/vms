#!/bin/sh
# host-probe-cc1.sh — HOST-SIDE build proof for cc1/cc1plus (bead vms-796,
# epic vms-da0 F2a). Companion to src/vmslink/mk_as.sh's "HOST-SIDE PROOF"
# step (see that script's header) but for the compiler proper rather than
# the assembler, and this bead does NOT go on to attempt a LINK.EXE
# OVMX-image link (that is F2b, out of scope here — see VENDOR-REV).
#
# GOAL: get `make all-gcc` (C+C++ front ends: cc1, cc1plus, plus their
# common driver/libcpp/libbacktrace/libdecnumber/libiberty support) to
# build to completion as ordinary HOST binaries, in a musl (Alpine)
# environment, so the genuine libstdc++/C++-ABI undefined-symbol set and
# the genuine deferred-external musl set can be enumerated against a FULL
# object set (vms-0d2's prior catalog was a partial 242-object build that
# stopped when gcc/cp/ was still pruned — see VENDOR-REV history).
#
# WALL E (this bead's name for it): the AS.EXE/TCC.EXE recipe pattern
# (mk_as.sh, mk_tcc.sh) compiles with -ffreestanding -fno-builtin, because
# those images are OVMX-native shareables/executables meant to run only
# against DECC$SHR's musl+libgcc floor with NO libstdc++. That CFLAGS
# profile #errors immediately on gmp.h's C++ compatibility shim (gmp.h,
# when compiled as C++, includes <iosfwd> for std::ostream/istream
# operator<< overloads) under a freestanding toolchain that refuses to see
# the C++ standard library headers at all. cc1/cc1plus are themselves
# ordinary HOST C++ programs at this stage (F2a) — they are not yet being
# built AS an OVMX image — so the correct recipe is the opposite of
# freestanding: a normal HOSTED build (this container's musl libc +
# libstdc++, both present via Alpine's g++ package), which is what THIS
# script does. -ffreestanding is dropped entirely; -fno-builtin is not
# used; the container's musl-libc + libstdc++.so/.a are both live. This
# produces a working host cc1/cc1plus binary and, in the process, whatever
# undefined-symbol wall a FUTURE freestanding OVMX-image attempt would hit
# is enumerated by nm on the resulting .o's (not by trying that attempt
# here — that is F2b).
#
# Usage:  host-probe-cc1.sh [gcc-src-dir] [work-dir]
#   gcc-src-dir  default: third-party/gcc/src (relative to repo root)
#   work-dir     default: /tmp/gcc-cc1-hostprobe (build to /tmp per CLAUDE.md)
# Env:    CC, CXX (default: the container's own gcc/g++ — musl-hosted)
#
# Must run in a container with a C++ host toolchain + GMP/MPFR/MPC dev
# packages (Alpine: g++ gmp-dev mpfr-dev mpc1-dev) — see
# third-party/gcc/run-host-probe-cc1.sh for the exact container invocation
# (never install these on the shared host, per CLAUDE.md).
set -e

HERE=$(cd "$(dirname "$0")" && pwd)                       # third-party/gcc
REPO=$(cd "$HERE/../.." && pwd)                            # repo root
GCC_SRC=${1:-$HERE/src}
WORK=${2:-/tmp/gcc-cc1-hostprobe}
CC=${CC:-gcc}
CXX=${CXX:-g++}

[ -d "$GCC_SRC/gcc/cp" ] || {
    echo "host-probe-cc1: $GCC_SRC/gcc/cp missing — re-vendor gcc/cp/ first (see VENDOR-REV)"
    exit 1
}

rm -rf "$WORK"
mkdir -p "$WORK"

# Known configure knob (vms-0d2 catalog): fixincludes/ is pruned (target-lib
# dependency only, see VENDOR-REV PRUNED), so the top-level configure must
# be told not to look for it.
CONFIG_LOG="$WORK/configure.log"
echo "host-probe-cc1: ./configure --enable-languages=c,c++ --disable-fixincludes (host CC=$CC CXX=$CXX)"
( cd "$WORK" && sh "$GCC_SRC/configure" \
      --enable-languages=c,c++ \
      --disable-fixincludes \
      --disable-bootstrap \
      --disable-multilib \
      --disable-nls \
      --disable-werror \
      CC="$CC" CXX="$CXX" ) >"$CONFIG_LOG" 2>&1
CRC=$?
echo "-- configure exit=$CRC --"
if [ "$CRC" -ne 0 ]; then
    echo "host-probe-cc1: FAIL: top-level configure — see $CONFIG_LOG"
    tail -80 "$CONFIG_LOG"
    exit 1
fi
[ -f "$WORK/Makefile" ] || { echo "host-probe-cc1: FAIL: no top-level Makefile after configure"; exit 1; }

# Known configure knob (vms-0d2 catalog): pre-create the gcc/d and gcc/rust
# build-output directories under the OBJECT dir before `make all-gcc`. D and
# Rust front ends are NOT vendored (see VENDOR-REV PRUNED) and this repo
# never configures them, but some of GCC 14's top-level Makefile generated
# rules (lang-specs / per-language stamp files driven off the full known-
# language list in the toplevel Makefile.def, not just --enable-languages)
# reference these object subdirectories unconditionally; an absent directory
# turns into a `mkdir` or stat failure partway through the build rather than
# a clean skip. Empty dirs are enough — nothing is ever written into them
# for languages this tree doesn't vendor.
mkdir -p "$WORK/gcc/d" "$WORK/gcc/rust"

MAKE_LOG="$WORK/make-all-gcc.log"
echo "host-probe-cc1: make all-gcc -j\$(nproc) (HOSTED — no -ffreestanding, musl+libstdc++ live)"
set +e
( cd "$WORK" && make all-gcc -j"$(nproc)" CC="$CC" CXX="$CXX" ) >"$MAKE_LOG" 2>&1
MRC=$?
set -e
echo "-- make all-gcc exit=$MRC; tail: --"
tail -60 "$MAKE_LOG" | sed 's/^/   /'

CC1=$(find "$WORK" -maxdepth 4 -name 'cc1' -type f 2>/dev/null | head -1)
CC1PLUS=$(find "$WORK" -maxdepth 4 -name 'cc1plus' -type f 2>/dev/null | head -1)

echo
echo "host-probe-cc1: RESULT"
echo "  make all-gcc exit code : $MRC"
echo "  cc1      binary        : ${CC1:-NOT PRODUCED}"
echo "  cc1plus  binary        : ${CC1PLUS:-NOT PRODUCED}"
if [ -n "$CC1" ] && [ -n "$CC1PLUS" ] && [ "$MRC" -eq 0 ]; then
    echo "  => cc1 AND cc1plus built FULLY host-side."
else
    echo "  => build did NOT complete fully — see $MAKE_LOG for where it stopped."
fi
echo
echo "  work dir (objects retained for wall enumeration): $WORK"
