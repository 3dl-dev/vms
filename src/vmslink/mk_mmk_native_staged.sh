#!/bin/sh
# mk_mmk_native_staged.sh — build MMK.EXE as an OVMX-native, IMGACT-ACTIVATED
# image (mk_mmk.sh) together with the full seven-producer shareable graph it
# --uses, and stage them where IMGACT resolves them (SYS$SYSTEM / SYS$LIBRARY),
# for the QEMU per-facility rail (test_syssvc_mmk_build / test_syssvc_mmk_drive,
# vms-c09f). This CENTRALISES the recipe so the Dockerfile base build and the
# inject_and_run.sh per-defect rebuild cannot drift.
#
# WHY: vms-16d/vms-ec70 built MMK.EXE VMS-native (mk_mmk.sh: 19 objects, LINK.EXE
# --executable --use {DECC$SHR + the six OVMX shareables}, PT_INTERP=IMGACT.EXE).
# vms-c09f swaps the rail's MMK subject from the STATIC mmk_native CMake target to
# THIS activated image so the drive tests exercise the real spawn+mailbox+
# WRTATTN-AST exec-drive of an activated image over a real /dev/vms — the
# convergence "OVMX builds OVMX with the real activated image, the VMS way".
#
# It REUSES the proven graph builder src/imgact/test/lib_build_graph.sh
# (build_producer_graph), which is arch-adaptive (aarch64: -mno-outline-atomics;
# x86_64: -mtls-dialect=gnu2, vms-cb5f) — NOT a forked copy. The aarch64-only SKIP
# in run_mmk_native.sh is that HOST harness's own gate, not a toolchain limit.
#
# link.c / imgact.c are the complete toolchain and are OUT of file-domain — this
# script only builds + stages (vms-c09f boundary).
#
# Usage:  mk_mmk_native_staged.sh <repo-root> <SYSEXE-dir> <SYSLIB-dir> [MODE] [LINK.EXE]
#   <SYSEXE-dir>  where MMK.EXE (+ IMGACT.EXE) land, e.g. .../SYS0/SYSCOMMON/SYSEXE
#   <SYSLIB-dir>  where the 7 shareables land, e.g. .../SYS0/SYSCOMMON/SYSLIB
#   MODE          "full" (default) — build the whole 7-producer graph + MMK.EXE
#                 (Dockerfile base build, once); or "mmk-only" — rebuild ONLY
#                 MMK.EXE via mk_mmk.sh, REUSING the 7 shareables already in
#                 <SYSLIB-dir> and the given <LINK.EXE>. The per-defect rebuild
#                 (inject_and_run.sh) uses mmk-only: the negctl shard runs ~7
#                 defects under a 50m budget, so a full graph rebuild per defect
#                 would risk the timeout, and only ovmx_mmk_sp.c (in MMK) is ever
#                 mutated — the shareables never change per-defect.
#   LINK.EXE      required in mmk-only mode (mk_mmk.sh needs the linker); ignored
#                 in full mode (build_producer_graph builds its own).
# Env:  CC (default musl-gcc), ARCH (default from uname -m), WORK (default
#       /tmp/mmk-staged), LIBC / LIBGCC (auto-detected for the musl toolchain).
set -e

REPO=$1; SYSEXE=$2; SYSLIB=$3; MODE=${4:-full}; LINK_EXE_ARG=$5
[ -n "$REPO" ] && [ -n "$SYSEXE" ] && [ -n "$SYSLIB" ] || {
    echo "usage: mk_mmk_native_staged.sh <repo-root> <SYSEXE-dir> <SYSLIB-dir> [full|mmk-only] [LINK.EXE]" >&2; exit 2; }

CC=${CC:-musl-gcc}
case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCH=aarch64; ARCHFLAG="-mno-outline-atomics" ;;
    x86_64|amd64)  ARCH=x86_64;  ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "mk_mmk_native_staged.sh: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
# mk_mmk.sh's DEFAULT CFLAGS carry the aarch64-only -mno-outline-atomics (invalid
# on x86_64). Pass the arch-correct flag explicitly, matching lib_build_graph.sh's
# producer CFLAGS (-mtls-dialect=gnu2 on x86_64, vms-cb5f), so the 19 MMK objects
# compile on x86_64 too -- the aarch64 default broke the first x86_64 build (vms-c09f).
MMK_CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $ARCHFLAG"
export CC ARCH

# The dir layout lib_build_graph.sh's build_producer_graph() reads (same names
# run_mmk_native.sh exports). IMGACT_DIR/LINK_DIR anchor the toolchain; the *_DIR
# vars point each mk_*_shr.sh producer at its source; *_INC at its headers.
SRC="$REPO/src"
IMGACT_DIR="$SRC/imgact"
LINK_DIR="$SRC/vmslink"
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
export SRC IMGACT_DIR LINK_DIR LIBVMSSYS_DIR VMSPROC_DIR VMSLNM_DIR VMSFS_DIR \
       LIBVMS_DIR VMSRMS_DIR LIBVMS_INC LNM_INC VMSFS_INC

WORK=${WORK:-/tmp/mmk-staged}
rm -rf "$WORK"; mkdir -p "$WORK" "$SYSEXE" "$SYSLIB"
export WORK

# musl static libc.a + host libgcc.a for DECC$SHR's whole-archive (the ubuntu+
# musl-gcc rail image keeps libc.a under /usr/lib/<triple>-linux-musl; the alpine
# dev container keeps it at /usr/lib/libc.a — accept either).
if [ -z "${LIBC:-}" ]; then
    LIBC=$(ls /usr/lib/*-linux-musl/libc.a 2>/dev/null | head -1)
    [ -n "$LIBC" ] || LIBC=/usr/lib/libc.a
fi
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "FATAL: no musl libc.a at $LIBC" >&2; exit 1; }
[ -f "$LIBGCC" ] || { echo "FATAL: no libgcc.a at $LIBGCC" >&2; exit 1; }
export LIBC LIBGCC

if [ "$MODE" = "full" ]; then
    echo "--- mk_mmk_native_staged[full]: building the 7-producer graph + activated MMK.EXE (ARCH=$ARCH, CC=$CC) ---"
    # build_producer_graph builds IMGACT.EXE + LINK.EXE + DECC$SHR + the six OVMX
    # shareables into $SYSEXE/$SYSLIB (identical recipe to the Dockerfile's own
    # IMGACT/DECC$SHR, so re-emitting them is byte-stable, not a divergence).
    . "$IMGACT_DIR/test/lib_build_graph.sh"
    build_producer_graph
    MK_LINK="$WORK/LINK.EXE"
elif [ "$MODE" = "mmk-only" ]; then
    [ -n "$LINK_EXE_ARG" ] && [ -f "$LINK_EXE_ARG" ] || {
        echo "FATAL: mmk-only mode needs an existing <LINK.EXE> (got '$LINK_EXE_ARG')" >&2; exit 2; }
    for s in DECC LIBVMS LIBVMSPROCESS LIBVMSFS LIBVMSLNM LIBVMSRMS LIBVMSSYS; do
        [ -f "$SYSLIB/$s\$SHR.EXE" ] || {
            echo "FATAL: mmk-only mode expects the 7 shareables already in $SYSLIB (missing $s\$SHR.EXE) -- run a full build first" >&2; exit 2; }
    done
    echo "--- mk_mmk_native_staged[mmk-only]: relinking MMK.EXE only, reusing the staged 7 shareables (ARCH=$ARCH) ---"
    MK_LINK="$LINK_EXE_ARG"
else
    echo "FATAL: unknown MODE '$MODE' (expected full|mmk-only)" >&2; exit 2
fi

echo "--- mk_mmk_native_staged: linking MMK.EXE (mk_mmk.sh: 19 objects, LINK.EXE --executable --use {7 shareables}) ---"
CC="$CC" CFLAGS="$MMK_CFLAGS" WORK="$WORK/mk-mmk" sh "$LINK_DIR/mk_mmk.sh" \
    "$MK_LINK" "$SYSEXE/MMK.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SRC"

# PT_INTERP proves it is an activated image, not a bare static binary.
readelf -lW "$SYSEXE/MMK.EXE" | grep -q 'INTERP' || {
    echo "FATAL: MMK.EXE has no PT_INTERP (not an IMGACT-activated image)" >&2; exit 1; }
chmod +x "$SYSEXE/MMK.EXE"
echo "--- mk_mmk_native_staged: MMK.EXE (activated, PT_INTERP=IMGACT.EXE) + 7 shareables staged under $SYSEXE / $SYSLIB ---"
