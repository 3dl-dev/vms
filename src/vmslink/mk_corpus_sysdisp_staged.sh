#!/bin/sh
# mk_corpus_sysdisp_staged.sh — build the vms-sys corpus ladder RUNG-1 subject
# (tests/qemu/corpus_sysdisp_assign.c) as an OVMX-native, IMGACT-ACTIVATED image
# and stage it where IMGACT resolves a main image off OVMX_SYSDEVICE (SYS$SYSTEM),
# for the QEMU per-facility rail (test_syssvc_sysdispatch, vms-a2b).
#
# WHY: vms-sys's join gate needs an IMGACT-ACTIVATED image (LINK.EXE --executable
# --use {shareables}, PT_INTERP=IMGACT.EXE, NO ld) whose SYS$ call dispatches into
# the executive over /dev/vms. This compiles the single-TU subject and links it the
# activated way against the SAME 7-producer shareable graph the MMK build stages
# (DECC$SHR + the six OVMX shareables), reusing the proven LINK.EXE --use recipe
# (mk_mmk.sh). The subject imports sys$assign/$dassgn (LIBVMS$SHR/LIBVMSRMS$SHR),
# vms_kif_acp_mount (LIBVMSSYS$SHR), vms_pcb_init (LIBVMSPROCESS$SHR) and crt0/exit
# (DECC$SHR); --use'ing all seven is harmless (only imported symbols bind) and
# matches MMK so the same staged graph serves both.
#
# link.c / imgact.c are OUT of file-domain — this script only compiles + links +
# stages (vms-a2b / vms-c09f boundary).
#
# Usage:  mk_corpus_sysdisp_staged.sh <repo-root> <out-EXE> <LINK.EXE> <SYSLIB-dir>
#   <out-EXE>     where the activated subject lands, e.g.
#                 /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/CORPUS_SYSDISP_ASSIGN.EXE
#   <LINK.EXE>    the OVMX linker (already built + staged by the MMK full build)
#   <SYSLIB-dir>  where the 7 shareables already live (SYS$LIBRARY), e.g.
#                 /initramfs/vms/SYS0/SYSCOMMON/SYSLIB
# Env:  CC (default musl-gcc), ARCH (default from uname -m), WORK (default
#       /tmp/corpus-sysdisp).
set -e

REPO=$1; OUT=$2; LINK_EXE=$3; SYSLIB=$4
[ -n "$REPO" ] && [ -n "$OUT" ] && [ -n "$LINK_EXE" ] && [ -n "$SYSLIB" ] || {
    echo "usage: mk_corpus_sysdisp_staged.sh <repo-root> <out-EXE> <LINK.EXE> <SYSLIB-dir>" >&2; exit 2; }
[ -f "$LINK_EXE" ] || { echo "FATAL: no LINK.EXE at $LINK_EXE (run the MMK full build first)" >&2; exit 2; }
for s in DECC LIBVMS LIBVMSPROCESS LIBVMSFS LIBVMSLNM LIBVMSRMS LIBVMSSYS; do
    [ -f "$SYSLIB/$s\$SHR.EXE" ] || {
        echo "FATAL: expected the 7 shareables in $SYSLIB (missing $s\$SHR.EXE) -- run the MMK full build first" >&2; exit 2; }
done

CC=${CC:-musl-gcc}
case "${ARCH:-$(uname -m)}" in
    aarch64|arm64) ARCHFLAG="-mno-outline-atomics" ;;
    x86_64|amd64)  ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "mk_corpus_sysdisp_staged.sh: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
# Match lib_build_graph.sh's producer CFLAGS (arch-correct, freestanding) so the
# subject compiles on x86_64 too (the aarch64 default broke the first x86_64 MMK
# build, vms-c09f).
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $ARCHFLAG"

SRC="$REPO/src"
SUBJECT="$REPO/tests/qemu/corpus_sysdisp_assign.c"
[ -f "$SUBJECT" ] || { echo "FATAL: subject $SUBJECT missing" >&2; exit 2; }
INCS="-I$SRC/libvms/include -I$SRC/libvmssys -I$SRC/vmsprocess/include -I$SRC/vmsrms/include -I$SRC/vmsfs/include -I$SRC/vmslnm/include"

WORK=${WORK:-/tmp/corpus-sysdisp}
rm -rf "$WORK"; mkdir -p "$WORK" "$(dirname "$OUT")"

echo "--- mk_corpus_sysdisp: compile the subject (ARCH=${ARCH:-$(uname -m)}, CC=$CC) ---"
# shellcheck disable=SC2086
$CC $CFLAGS $INCS -c -o "$WORK/corpus_sysdisp_assign.o" "$SUBJECT"

echo "--- mk_corpus_sysdisp: LINK.EXE --executable --use {7 shareables} -> $OUT ---"
"$LINK_EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" --use "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    -o "$OUT" "$WORK/corpus_sysdisp_assign.o"

# PT_INTERP proves it is an activated image, not a bare static binary.
readelf -lW "$OUT" | grep -q 'INTERP' || {
    echo "FATAL: $OUT has no PT_INTERP (not an IMGACT-activated image)" >&2; exit 1; }
chmod +x "$OUT"
echo "--- mk_corpus_sysdisp: staged activated subject (PT_INTERP=IMGACT.EXE) at $OUT ---"
