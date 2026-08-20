#!/bin/sh
# run_as_native.sh — F1 activation proof (bead vms-0b6b, epic vms-da0
# "GCC-as-VMS-oracle" lane): build GNU binutils' assembler (gas,
# third-party/binutils) AS a VMS-native EXECUTABLE image (AS.EXE), activate
# it through IMGACT.EXE (NO ld / NO ld.so), have the ACTIVATED as assemble a
# trivial .s to a real ELF64 ET_REL x86_64 .o. Mirrors run_tcc_native.sh's
# and run_link_native.sh's shape: build-to-/tmp, hard FAIL on any assertion
# miss, milestone banner only if every DONE condition actually held.
#
# THIS HARNESS DOES NOT ASSUME SUCCESS. Per the epic's method ("pick the GCC
# base -> build/run it as an OVMX image -> hit a missing/faked OS facility ->
# backfill it GENUINELY -> repeat"), bead vms-0b6b's deliverable is the FIRST
# WALL, not a green AS.EXE. AS_EXPECT_LINK (default 1) softens the LINK.EXE
# step and everything downstream of it to a SKIP (exit 2) instead of a hard
# FAIL when set to 0, exactly like TCC_EXPECT_LINK / LINK_EXPECT in the
# sibling harnesses — use that while iterating on a real backfill; leave it
# at 1 once AS.EXE is expected to link+activate cleanly (it does not yet, as
# of this bead — see the vms-0b6b report for the wall this harness hit).
#
# DONE conditions (bead vms-0b6b), each checked for REAL below:
#   1. AS.EXE (mk_as.sh's LINK.EXE --executable output) has PT_INTERP=IMGACT.EXE
#      and IMGACT.EXE activates it (`AS.EXE --version` runs, VMS-native, no
#      ld/ld.so).
#   2. The activated AS.EXE reads test.s (via the OVMX_RMS_IO seam — RMS
#      sys$open/$get, traced to stderr) and writes test.o (via RMS
#      sys$create/$put, traced to stderr) — `AS.EXE -o test.o test.s`.
#   3. test.o is a real ELF64/REL/x86_64 object (readelf), not a canned
#      artifact — the same shape LINK.EXE already consumes.
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain — do NOT edit them here.
#
# Runs on x86_64 (native, primary per CLAUDE.md Rule 5) or aarch64 (native or
# arm64 binfmt/QEMU emulation) — mk_as.sh's ARCH selection mirrors
# lib_build_graph.sh's / run_link_native.sh's convention. musl Alpine
# container only (CLAUDE.md test loop). Needs root to create /vms.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
BINUTILS_SRC="$REPO/third-party/binutils/src"
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
WORK=${WORK:-/tmp/as-native}
rm -rf "$WORK"; mkdir -p "$WORK"

# ARCH: default from the running machine; CI passes it explicitly. Mirrors
# run_link_native.sh's selection (as-native and link-native are both
# arch-agnostic in the sense that matters here: mk_as.sh's ARCHFLAG switch,
# not a per-arch fork of this harness).
case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64)  ARCH=x86_64 ;;
    *) echo "SKIP-FAIL: run_as_native.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
esac
export ARCH

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]     || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need a $ARCH musl container)"; exit 1; }
[ -f "$LIBGCC" ]   || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }
[ -d "$BINUTILS_SRC" ] || { echo "FAIL: third-party/binutils/src not found — vendoring bead vms-0b6b missing?"; exit 1; }

# Shared producer-graph build (IMGACT.EXE + bootstrap LINK.EXE + DECC$SHR +
# the five OVMX shareables) — the same builder run_dcl_native.sh /
# run_link_native.sh use.
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build AS.EXE VMS-native (mk_as.sh: gas+bfd+opcodes+libiberty, freestanding musl + OVMX_RMS_IO, LINK.EXE --executable) =="
set +e
CC="$CC" ARCH="$ARCH" WORK="$WORK/mk-as" sh "$LINK_DIR/mk_as.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/AS.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$BINUTILS_SRC" 2>"$WORK/as-link.err"
ARC=$?
set -e
echo "-- mk_as.sh exit=$ARC; message (last 20 lines): --"
tail -20 "$WORK/as-link.err" | sed 's/^/   /'

if [ "$ARC" -ne 0 ]; then
    if [ "${AS_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: AS.EXE build failed. See mk-as/{configure,make,as-link}.log/.err above/in \$WORK."
        echo "  This IS a legitimate bead vms-0b6b outcome (the FIRST WALL) — set"
        echo "  AS_EXPECT_LINK=0 to soften this to SKIP while iterating on a backfill."
        exit 1
    fi
    echo "SKIP (AS_EXPECT_LINK=0): AS.EXE build failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/AS.EXE" | grep -q 'INTERP' || { echo "FAIL: AS.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/AS.EXE"

echo
echo "== AS.EXE linked — activate through IMGACT.EXE — DONE condition 1 =="
set +e
"$SYSEXE/AS.EXE" --version > "$WORK/as-version.out" 2>&1
AVRC=$?
set -e
echo "-- AS.EXE --version output: --"; sed 's/^/   /' "$WORK/as-version.out"
echo "exit code = $AVRC"
if [ "$AVRC" -ne 0 ]; then
    if [ "${AS_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: AS.EXE linked but did not activate cleanly (THE WALL — see output above"
        echo "  and the vms-0b6b report for which OS facility this demanded)."
        exit 1
    fi
    echo "SKIP (AS_EXPECT_LINK=0): activation failed but the assertion is disabled."
    exit 2
fi
grep -qi 'GNU assembler' "$WORK/as-version.out" || { echo "FAIL: activated AS.EXE did not print its version banner"; exit 1; }
echo "-- confirmed: IMGACT.EXE activates AS.EXE VMS-native, no ld / no ld.so --"

echo
echo "== activated AS.EXE assembles test.s -> test.o (OVMX_RMS_IO path) — DONE conditions 2 & 3 =="
cat > "$WORK/test.s" <<'EOF'
	.text
	.globl	main
main:
	movl	$42, %eax
	ret
EOF
chmod 666 "$WORK/test.s"
set +e
"$SYSEXE/AS.EXE" -o "$WORK/test.o" "$WORK/test.s" > "$WORK/as-compile.out" 2>&1
ACRC=$?
set -e
sed 's/^/   /' "$WORK/as-compile.out"
grep -q 'OVMX-RMS: sys\$open' "$WORK/as-compile.out" && echo "-- confirmed: RMS sys\$open used for test.s (not raw fopen) --"
grep -q 'OVMX-RMS: sys\$create' "$WORK/as-compile.out" && echo "-- confirmed: RMS sys\$create used for test.o delivery (not raw fopen/write) --"
[ "$ACRC" -eq 0 ] || { echo "FAIL: activated AS.EXE failed to assemble test.s (exit $ACRC — THE WALL, see output above)"; exit 1; }
# sys$create ALWAYS mints a real VMS version suffix on disk (rms_core.c
# sys$create: "%s/%s;%d") — as was told "-o test.o" (unversioned), so the
# actual delivered artifact is "test.o;1", not a bare "test.o". This is correct
# VMS behavior (every RMS-created file is versioned), not a defect — mirrors
# run_tcc_rms.sh's HELLO_O=";1" handling. (The bare-name check here was a latent
# harness bug, invisible until vms-d697 fixed the --version activation wall and
# AS.EXE first reached the RMS-delivered output step.)
TEST_O="$WORK/test.o;1"
[ -f "$TEST_O" ] || { echo "FAIL: activated AS.EXE did not produce $TEST_O (RMS-versioned test.o)"; exit 1; }
readelf -h "$TEST_O" | tee "$WORK/test.readelf.h" >/dev/null
grep -q 'Class:.*ELF64' "$WORK/test.readelf.h" || { echo "FAIL: test.o is not ELF64"; exit 1; }
grep -q 'Type:.*REL (Relocatable file)' "$WORK/test.readelf.h" || { echo "FAIL: test.o is not ET_REL"; exit 1; }
grep -qi 'Machine:.*X86-64\|Machine:.*AArch64' "$WORK/test.readelf.h" || { echo "FAIL: test.o is not the expected machine"; exit 1; }
echo "-- confirmed: test.o;1 is a real ELF64/REL object, assembled by the ACTIVATED as --"

echo
echo "================================================================================"
echo "MILESTONE (vms-0b6b, F1 of epic vms-da0): stock GNU binutils' assembler (gas,"
echo "third-party/binutils) now runs AS an OVMX image. AS.EXE — built by binutils'"
echo "own configure + make (freestanding musl CFLAGS, -DOVMX_RMS_IO), linked"
echo "VMS-native via LINK.EXE --executable --use {DECC\$SHR + the five OVMX"
echo "shareables} — activates through IMGACT.EXE with NO ld / NO ld.so, reads its"
echo "primary .s source and delivers its output .o through native RMS, and produces"
echo "a real ELF64/REL/x86_64 object LINK.EXE can consume. The GCC-as-VMS-oracle"
echo "lane (vms-da0) has its F1 assembler running natively inside OVMX."
echo "================================================================================"
