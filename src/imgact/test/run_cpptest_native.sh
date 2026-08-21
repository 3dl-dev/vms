#!/bin/sh
# run_cpptest_native.sh — F2b C++-runtime wall probe (bead vms-5562, epic
# vms-da0 "GCC-as-VMS-oracle"): build a MINIMAL C++ program
# (third-party/gcc/cpptest.cpp) as a VMS-native EXECUTABLE image
# (CPPTEST.EXE, mk_cpptest_ovmx.sh), activate it through IMGACT.EXE (NO ld /
# NO ld.so), and report where it stops. Mirrors run_as_native.sh's shape
# (build-to-/tmp, hard FAIL on any assertion miss unless the *_EXPECT_LINK
# knob softens it) but probes the C++ runtime (exceptions/.eh_frame, RTTI,
# static constructors, libstdc++) with a TINY TU instead of attempting the
# full 300MB cc1/cc1plus link (that stays a LATER rung, once these walls are
# known — see the design doc's F2a/F2b split and host-probe-cc1.sh).
#
# THIS HARNESS DOES NOT ASSUME SUCCESS. Per the epic's method ("pick the GCC
# base -> build/run it as an OVMX image -> hit a missing/faked OS facility ->
# backfill it GENUINELY -> repeat"), bead vms-5562's deliverable is the FIRST
# C++-runtime WALL, not a green CPPTEST.EXE. CPPTEST_EXPECT_LINK (default 1)
# softens the LINK.EXE step and everything downstream to SKIP (exit 2)
# instead of hard FAIL when set to 0 -- use that while iterating. Separately,
# CPPTEST_EXPECT_RUN (default 1) softens ONLY the activation/execution
# assertions (not the link) to SKIP when set to 0, so a caller can pin
# "LINK.EXE succeeds" as the checked baseline while leaving activation an
# open question during initial wall-hunting.
#
# DONE conditions (bead vms-5562), each checked for REAL below:
#   1. CPPTEST.EXE (mk_cpptest_ovmx.sh's LINK.EXE --executable output) links.
#   2. CPPTEST.EXE has PT_INTERP=IMGACT.EXE and IMGACT.EXE activates it
#      (`CPPTEST.EXE` runs, VMS-native, no ld/ld.so).
#   3. The activated CPPTEST.EXE actually RUNS to completion (prints its
#      three probe lines, exits 0) -- i.e. the global ctor's std::string
#      construction, the std::string concat in main(), and the throw/catch
#      round-trip all genuinely executed, not just "the image loaded".
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain -- do NOT edit them here.
#
# Runs on x86_64 (native, primary per CLAUDE.md Rule 5) or aarch64. musl
# Alpine container only (CLAUDE.md test loop) with BOTH a musl C toolchain
# (gcc, for the producer graph) and a musl C++ toolchain (g++, for
# cpptest.cpp + upstream libstdc++/libgcc archives) -- see
# run-cpptest-native.sh (if present) for the exact container invocation, or
# the as-native CI job in .github/workflows/ci.yml for the base package set
# (add g++ to it). Needs root to create /vms.
set -e
CC=${CC:-gcc}
CXX=${CXX:-g++}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
CPPTEST_SRC="$REPO/third-party/gcc/cpptest.cpp"
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
RMS_INC="$VMSRMS_DIR/include"
WORK=${WORK:-/tmp/cpptest-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64)  ARCH=x86_64 ;;
    *) echo "SKIP-FAIL: run_cpptest_native.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
esac
export ARCH

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need a $ARCH musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }
command -v "$CXX" >/dev/null 2>&1 || { echo "SKIP-FAIL: no $CXX in PATH (need Alpine's g++ package)"; exit 1; }
[ -f "$CPPTEST_SRC" ] || { echo "FAIL: $CPPTEST_SRC not found"; exit 1; }

# Shared producer-graph build (IMGACT.EXE + bootstrap LINK.EXE + DECC$SHR +
# the five OVMX shareables) -- the same builder run_dcl_native.sh /
# run_as_native.sh use. Built with the plain C toolchain ($CC); independent
# of $CXX (used only for cpptest.cpp + locating the upstream C++ runtime
# archives below).
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build CPPTEST.EXE VMS-native (mk_cpptest_ovmx.sh: cpptest.cpp hosted-g++, whole-archived upstream libstdc++/libgcc, LINK.EXE --executable) =="
set +e
CXX="$CXX" WORK="$WORK/mk-cpptest" sh "$LINK_DIR/mk_cpptest_ovmx.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/CPPTEST.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$CPPTEST_SRC" 2>"$WORK/cpptest-link.err"
CRC=$?
set -e
echo "-- mk_cpptest_ovmx.sh exit=$CRC; message (last 30 lines): --"
tail -30 "$WORK/cpptest-link.err" | sed 's/^/   /'

if [ "$CRC" -ne 0 ]; then
    if [ "${CPPTEST_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: CPPTEST.EXE build failed. See mk-cpptest/{compile,cpptest-link}.log/.err in \$WORK."
        echo "  THIS IS THE C++ WALL (vms-5562) if it stopped at LINK.EXE -- set"
        echo "  CPPTEST_EXPECT_LINK=0 to soften this to SKIP while iterating on a backfill."
        exit 1
    fi
    echo "SKIP (CPPTEST_EXPECT_LINK=0): CPPTEST.EXE build failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/CPPTEST.EXE" | grep -q 'INTERP' || { echo "FAIL: CPPTEST.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/CPPTEST.EXE"
echo "-- CPPTEST.EXE linked; PT_INTERP present --"

echo
echo "== CPPTEST.EXE linked -- activate through IMGACT.EXE -- DONE condition 2 =="
set +e
"$SYSEXE/CPPTEST.EXE" > "$WORK/cpptest-run.out" 2>&1
RRC=$?
set -e
echo "-- CPPTEST.EXE run output: --"; sed 's/^/   /' "$WORK/cpptest-run.out"
echo "exit code = $RRC"

if [ "$RRC" -ne 0 ] || ! grep -q 'cpptest: OK' "$WORK/cpptest-run.out"; then
    if [ "${CPPTEST_EXPECT_RUN:-1}" = "1" ]; then
        echo "FAIL: CPPTEST.EXE linked but did not run to completion (THE C++ WALL -- see"
        echo "  output above for which facility failed: global ctor / std::string / throw-catch)."
        exit 1
    fi
    echo "SKIP (CPPTEST_EXPECT_RUN=0): activation/run failed but the assertion is disabled."
    exit 2
fi

grep -q 'Greeter::Greeter' "$WORK/cpptest-run.out"    || { echo "FAIL: global ctor (.init_array -> libstdc++) did not run"; exit 1; }
grep -q 'OVMX C++ probe' "$WORK/cpptest-run.out"      || { echo "FAIL: std::string operation in main() did not run"; exit 1; }
grep -q 'caught: cpptest exception' "$WORK/cpptest-run.out" || { echo "FAIL: throw/catch round-trip did not complete (unwinder/.eh_frame/personality-routine wall)"; exit 1; }
echo "-- confirmed: global ctor ran, std::string worked, throw/catch round-tripped --"

echo
echo "================================================================================"
echo "MILESTONE (vms-5562, F2b of epic vms-da0): a minimal C++ program (global"
echo "ctor + std::string + try/catch) runs AS an OVMX image -- CPPTEST.EXE, built"
echo "hosted-g++ + whole-archived upstream libstdc++/libgcc, linked VMS-native via"
echo "LINK.EXE --executable --use {DECC\$SHR + the five OVMX shareables} --"
echo "activates through IMGACT.EXE with NO ld / NO ld.so, and the C++ runtime"
echo "(exceptions, RTTI-adjacent machinery, static constructors) genuinely"
echo "executed. The C++-runtime walls for the GCC-as-VMS-oracle lane's F2 are"
echo "now known before attempting the full cc1/cc1plus link."
echo "================================================================================"
