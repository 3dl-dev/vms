#!/bin/sh
# run_mmk_link_selfhost_build.sh -- the OVMX-native LINK.EXE reproduces itself
# byte-for-byte across successive self-links (the self-host S4 FIXPOINT, gen2 ==
# gen3), executing the EXACT build LINKSH.MMS describes (bead vms-89d, the
# vms-678 Build-native 1.0 gate).
#
# This is the host, real-toolchain half of porting the multi-TU LINK.EXE
# self-host fixpoint from BUILD.COM to MMK -- the LINK.EXE analog of
# tests/toolchain/run_mmk_component_build.sh (which proves the real
# LIBRARIAN.EXE/LINK.EXE build the freestanding-runtime component byte-
# identically).  It proves, on the real OVMX linker on the actual LINK.EXE
# sources, the SAME property src/imgact/test/run_link_selfhost_native.sh proves
# under BUILD.COM: an OVMX-built linker linking the OVMX linker's own two
# translation units reproduces itself.
#
# THE FIXPOINT (mirrors run_link_selfhost_native.sh gen1/gen2/gen3):
#   boot   a host-tool bootstrap LINK.EXE (build_producer_graph: plain host cc
#          of link.c) -- allowed, a build step is not an activation proof
#          (CLAUDE.md Rule 9);
#   gen1 = boot compiles+links link.c + ovmx_link_rms_io.c into an OVMX LINK.EXE
#          image (PT_INTERP=IMGACT.EXE);
#   gen2 = the OVMX-built gen1, activated through IMGACT, links the SAME two
#          objects into a LINK.EXE -- the first linker built by an OVMX linker;
#   gen3 = the OVMX-built gen2 links the same two objects again;
#   ASSERT gen2 == gen3, byte-for-byte (sha256 + cmp).  A linker that reproduces
#          itself under its own successor is a true self-hosting fixpoint.
#
# Each gen is produced by src/vmslink/mk_link.sh, which is the byte-for-byte
# executor of the LINKSH.MMS recipe: it compiles link.c + ovmx_link_rms_io.c
# with the freestanding CFLAGS + -DOVMX_RMS_IO (the CFLAGS/INCS macros) and
# `--executable --use {DECC$SHR + the five OVMX shareables}` links them (the
# RTLIBS macro) -- exactly the two-TCC-compiles-then-one-LINK plan MMK resolves
# from LINKSH.MMS (proven by run_mmk_link_selfhost_plan.sh).  The MMK-DRIVEN
# execution of that plan through MMK's persistent mailbox DCL rides QEMU
# (tests/qemu/test_syssvc_mmk_link_selfhost.c) -- MMK's real drive needs a live
# executive (ovmx_mmk_sp.c $CREMBX/lib$spawn), which a host container lacks.
#
# COMPILE STEP (honest, Rule 9 / same concession as run_mmk_component_build.sh):
# this proof uses the host CC for the object step -- TCC.EXE's compile
# determinism (byte-identical objects across runs) is the S2 fixpoint already
# proven by src/imgact/test/run_tcc_selfhost.sh (gen2==gen3), so the objects are
# a constant here and what this proof isolates is the LINK output's self-host
# fixpoint.  aarch64 (native or arm64 emulation) OR x86_64 native -- unlike
# run_link_selfhost_native.sh this proof is NOT aarch64-only, because it does not
# run TCC.EXE (tinycc's configure keys off uname -m); it reuses the same
# lib_build_graph.sh producer graph that already supports both arches (vms-cb5f).
#
# LINK_SELFHOST_EXPECT (default 1, BLOCKING): a failure of the producer graph, of
# any gen build, or of the gen2==gen3 byte-identity check is a real regression.
# Set LINK_SELFHOST_EXPECT=0 to soften the native build steps to a SKIP (exit 2)
# while iterating; the fixpoint assertion stays a hard FAIL.  Needs root to
# create /vms.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # tests/toolchain
REPO=$(cd "$HERE/../.." && pwd)              # repo root
SRC="$REPO/src"
IMGACT_DIR="$SRC/imgact"
LINK_DIR="$SRC/vmslink"
GRAPH="$IMGACT_DIR/test/lib_build_graph.sh"
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
WORK=${WORK:-/tmp/mmk-link-selfhost-build}
rm -rf "$WORK"; mkdir -p "$WORK"

# ARCH: default from the running machine (CI may pass it explicitly). No TCC here,
# so both arches run natively -- the aarch64-only gate the BUILD.COM fixpoint
# carries does not apply.
case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64 ;;
    x86_64|amd64)  ARCH=x86_64 ;;
    *) echo "SKIP-FAIL: run_mmk_link_selfhost_build.sh needs aarch64 or x86_64 (got $(uname -m))"; exit 1 ;;
esac
export ARCH

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need an $ARCH musl toolchain)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

# ---- Producer graph: IMGACT.EXE + host-tool bootstrap LINK.EXE ($WORK/LINK.EXE)
# + DECC$SHR + the five OVMX shareables at SYS$LIBRARY.  Same builder the BUILD.COM
# fixpoint (run_link_selfhost_native.sh) and run_link_native.sh use.
. "$GRAPH"
build_producer_graph

# resolve_rms_out <intended-path> -- echo the real on-disk image an OVMX-native
# linker wrote.  gen1's bootstrap is a host tool (plain open() -> exact path), but
# gen1/gen2 are OVMX images whose -DOVMX_RMS_IO image write goes through OVMX RMS,
# which lowercases the unqualified name and mints a ;version suffix (+ a
# "<name>;N.rms_meta" sidecar).  Mirrors run_link_selfhost_native.sh find_output:
# pick the highest pure-digit ;version, else the exact path if it exists.
resolve_rms_out() {
    _d=$(dirname "$1"); _b=$(basename "$1"); _lb=$(echo "$_b" | tr 'A-Z' 'a-z')
    _v=$(ls "$_d/$_lb;"* "$_d/$_b;"* 2>/dev/null | grep -vE '\.rms_meta$' \
            | sort -t';' -k2 -n | tail -1)
    if [ -n "$_v" ]; then echo "$_v"; return; fi
    if [ -f "$_d/$_lb" ]; then echo "$_d/$_lb"; return; fi
    [ -f "$1" ] && echo "$1"
}

# build_one <input-linker> <tag> -- one LINKSH.MMS-equivalent build in a fresh
# output dir: mk_link.sh compiles link.c + ovmx_link_rms_io.c (host CC,
# -DOVMX_RMS_IO) and has <input-linker> `--executable --use {6 shareables}` link
# them into LINKSH.EXE.  <input-linker> is activated as-is (a host tool for boot,
# an OVMX image via IMGACT for gen1/gen2).  Echoes the resolved image path via the
# global RESOLVED (RMS may version/lowercase the name).
build_one() {
    _in="$1"; _tag="$2"
    _od="$WORK/out-$_tag"; rm -rf "$_od"; mkdir -p "$_od"
    _intended="$_od/LINKSH.EXE"
    set +e
    CC="$CC" ARCH="$ARCH" WORK="$WORK/mk-$_tag" sh "$LINK_DIR/mk_link.sh" \
        "$_in" "$_intended" \
        "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
        "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
        "$SRC" 2>"$WORK/$_tag.err"
    _rc=$?
    set -e
    echo "-- mk_link.sh ($_tag) exit=$_rc; message: --"; tail -4 "$WORK/$_tag.err" | sed 's/^/   /'
    if [ "$_rc" -ne 0 ]; then
        [ "${LINK_SELFHOST_EXPECT:-1}" = "1" ] && { echo "FAIL: $_tag LINK.EXE build failed (regression)"; exit 1; }
        echo "SKIP (LINK_SELFHOST_EXPECT=0): $_tag LINK.EXE build failed."; exit 2
    fi
    RESOLVED=$(resolve_rms_out "$_intended")
    if [ -z "$RESOLVED" ] || [ ! -f "$RESOLVED" ]; then
        echo "FAIL: $_tag produced no image; output dir:"; ls -la "$_od" | sed 's/^/   /'; exit 1
    fi
    readelf -lW "$RESOLVED" | grep -q 'INTERP' || { echo "FAIL: $_tag LINK.EXE ($RESOLVED) has no PT_INTERP (IMGACT)"; exit 1; }
    chmod +x "$RESOLVED"
    echo "-- $_tag image: $RESOLVED ($(wc -c < "$RESOLVED") bytes) --"
}

echo
echo "== gen1: the host-tool bootstrap LINK.EXE builds the OVMX LINK.EXE (link.c + ovmx_link_rms_io.c) =="
build_one "$WORK/LINK.EXE" gen1
GEN1="$RESOLVED"; cp "$GEN1" "$WORK/LINK.gen1"

echo
echo "== gen2: the OVMX-built gen1 LINK.EXE (activated via IMGACT) links LINK.EXE again =="
build_one "$GEN1" gen2
GEN2="$RESOLVED"; cp "$GEN2" "$WORK/LINK.gen2"

echo
echo "== gen3: the OVMX-built gen2 LINK.EXE links LINK.EXE again -- the linker linking itself =="
build_one "$GEN2" gen3
GEN3="$RESOLVED"; cp "$GEN3" "$WORK/LINK.gen3"

echo
echo "== FIXPOINT: gen2 == gen3 (byte-stable self-link) =="
S2=$(sha256sum "$WORK/LINK.gen2" | cut -d' ' -f1)
S3=$(sha256sum "$WORK/LINK.gen3" | cut -d' ' -f1)
echo "   gen2 sha256 = $S2 ($(wc -c < "$WORK/LINK.gen2") bytes)"
echo "   gen3 sha256 = $S3 ($(wc -c < "$WORK/LINK.gen3") bytes)"
if ! cmp -s "$WORK/LINK.gen2" "$WORK/LINK.gen3"; then
    echo "FAIL: gen2 != gen3 -- the OVMX-built LINK.EXE does NOT reproduce itself byte-for-byte."
    echo "      first differing bytes:"; cmp "$WORK/LINK.gen2" "$WORK/LINK.gen3" | sed 's/^/      /' || true
    exit 1
fi

echo
echo "================================================================================"
echo "MILESTONE (vms-89d, self-host S4 -- LINK.EXE self-host fixpoint, MMK path): the"
echo "OVMX-native LINK.EXE, executing the two-TU compile + six-shareable link that"
echo "LINKSH.MMS describes (and MMK resolves -- run_mmk_link_selfhost_plan.sh), linked"
echo "the OVMX linker (link.c + ovmx_link_rms_io.c) into gen2, then the OVMX-BUILT gen2"
echo "LINK.EXE linked LINK.EXE again into gen3.  gen2 and gen3 are BYTE-IDENTICAL"
echo "($S2) -- a true self-hosting fixpoint, the same"
echo "property BUILD.COM proves (run_link_selfhost_native.sh), now for the MMK driver."
echo "MMK's mailbox-driven execution of this plan rides QEMU"
echo "(tests/qemu/test_syssvc_mmk_link_selfhost.c)."
echo "================================================================================"
