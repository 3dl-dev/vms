#!/bin/sh
# mk_evacwl_native.sh - build EVACWL.C as a VMS-native image (vms-06c), the
# same recipe tests/qemu/mk_corpus_native.sh uses for the Eight-Cubed corpus
# (rd vms-db7): compiled by OVMX's own tcc (TCC.EXE, which honours DEC C's
# #pragma extern_model), linked by LINK.EXE --use the OVMX shareables. The
# image's PT_INTERP is IMGACT.EXE, so it activates exactly as a customer's
# compiled program would -- this is NOT a gcc-built test binary linked against
# src/libvms's internals.
#
# Kept SEPARATE from mk_corpus_native.sh / corpus_runtime_native.txt on
# purpose: EVACWL is not an Eight-Cubed corpus program, and corpus_runtime_
# report.sh / docs/corpus-scoreboard.md's counts are specifically the
# curated corpus set (single-ledger discipline) -- this script must not
# make EVACWL show up there.
#
# WHERE THE PRODUCERS COME FROM, AND WHICH ACTIVATOR PATH GETS BAKED
# (both were wrong in the first cut, rd vms-06c):
#
#   * The five OVMX shareables EVACWL --uses (it calls sys$enqw / sys$getsyiw /
#     RMS, which DECC$SHR does not own) are NOT in build-static/bin in any
#     build: the qemu harness stages them into the initramfs SYS$LIBRARY, and
#     Dockerfile.bootable's link-native stage builds them under
#     link-native/SYSLIB. Hardcoding build-static/bin made the only caller fail
#     with "producer image not found". <producer-dir> is now a parameter, and
#     callers pass the directory holding the shareables the image will ACTUALLY
#     activate against -- .vms$imp binds by symbol-vector INDEX, so linking
#     against a different copy than the one staged is not a thing to leave to
#     luck.
#
#   * PT_INTERP is baked by LINK.EXE at BUILD time (link.c's IMGACT_INTERP,
#     overridable with -DIMGACT_INTERP_PATH=) and is opened BY THE HOST KERNEL,
#     before any OVMX code runs. The default "/vms/SYS0/SYSCOMMON/SYSEXE/
#     IMGACT.EXE" is right for the qemu harness (which stages IMGACT.EXE at
#     exactly that POSIX path) and WRONG for a booted node (the /vms
#     passthrough is retired; PID 1 stages the activator at
#     /run/ovmx-boot/IMGACT.EXE). Building its own linker with neither macro
#     therefore produced an image a booted node could not exec at all: execve
#     failed ENOENT and the program printed nothing, which is how this landed
#     in the lab. Pass LINK_EXE=<a linker that bakes the right path> or
#     IMGACT_INTERP_PATH=<path> for a non-default target.
#
# Usage: mk_evacwl_native.sh <repo-root> [producer-dir] [out-image]
#   <repo-root>     the OVMX source tree (headers, EVACWL.C, link.c)
#   <producer-dir>  directory holding DECC$SHR.EXE + the five LIBVMS*$SHR.EXE
#                   producers (default <repo>/build-static/bin)
#   <out-image>     where EVACWL.EXE lands (default
#                   <repo>/build-static/native/EVACWL.EXE, which the
#                   Dockerfile's build-static/native/*.EXE staging globs pick up)
# Env:
#   TCC                  TCC.EXE to compile with (default <repo>/build-static/bin/TCC.EXE)
#   LINK_EXE             a prebuilt LINK.EXE to link with; its baked PT_INTERP wins
#   IMGACT_INTERP_PATH   PT_INTERP to bake when this script builds its own LINK.EXE
#   CC                   compiler for building that LINK.EXE (default gcc)
#   WORK                 scratch directory (default /tmp/evacwl-native)
set -eu
REPO=${1:?usage: mk_evacwl_native.sh <repo-root> [producer-dir] [out-image]}
BIN=$REPO/build-static/bin
PRODUCERS=${2:-$BIN}
OUT=${3:-$REPO/build-static/native/EVACWL.EXE}
TCC=${TCC:-$BIN/TCC.EXE}
DECC="$PRODUCERS/DECC\$SHR.EXE"
VMS_SHR="$PRODUCERS/LIBVMS\$SHR.EXE"
RMS_SHR="$PRODUCERS/LIBVMSRMS\$SHR.EXE"
PROC_SHR="$PRODUCERS/LIBVMSPROCESS\$SHR.EXE"
FS_SHR="$PRODUCERS/LIBVMSFS\$SHR.EXE"
LNM_SHR="$PRODUCERS/LIBVMSLNM\$SHR.EXE"
SRC=$REPO/tests/lab/ci6/EVACWL.C
W=${WORK:-/tmp/evacwl-native}

[ -x "$TCC" ] || { echo "mk_evacwl_native: FATAL: no $TCC" >&2; exit 1; }
for f in "$DECC" "$VMS_SHR" "$RMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR"; do
    [ -f "$f" ] || { echo "mk_evacwl_native: FATAL: producer image not found: $f" >&2; exit 1; }
done
[ -f "$SRC" ] || { echo "mk_evacwl_native: FATAL: no $SRC" >&2; exit 1; }

rm -rf "$W"; mkdir -p "$W/vmsinc" "$(dirname "$OUT")"
# The two DEC C headers the corpus build also needs that musl does not have
# (mk_corpus_native.sh's own comment on this copy applies here unchanged).
cp "$REPO/src/libvms/include/unixlib.h" "$REPO/src/libvms/include/reentrancy.h" "$W/vmsinc/"
MUSL_INC=$(echo /usr/include/*-linux-musl)

if [ -n "${LINK_EXE:-}" ]; then
    [ -x "$LINK_EXE" ] || { echo "mk_evacwl_native: FATAL: no LINK_EXE at $LINK_EXE" >&2; exit 1; }
    LINK=$LINK_EXE
else
    LINK=$W/LINK.EXE
    ${CC:-gcc} -std=gnu11 -O2 -I"$REPO/src/vmslink/include" \
        ${IMGACT_INTERP_PATH:+-DIMGACT_INTERP_PATH=$IMGACT_INTERP_PATH} \
        -o "$LINK" "$REPO/src/vmslink/link.c"
fi

# -xc: EVACWL.C's VMS-canonical uppercase name is not the ".c" tcc's own
# extension sniffing recognises ("unrecognized file type" otherwise); -xc
# forces C mode regardless of case, the same way DCL's own TCC command would
# via its /LANGUAGE-equivalent default for a .C file.
# The six --use producers are the SAME set src/vmslink/mk_tcc.sh links TCC.EXE
# itself against: EVACWL.C calls sys$ services the DEC C RTL (DECC$SHR) does
# not own (sys$enqw/sys$getsyiw/RMS/...), which live in the five OVMX
# shareables instead -- mk_corpus_native.sh's own --use DECC$SHR alone is
# enough only because its one entry (ipc_pipe) never calls a sys$ service.
"$TCC" -nostdinc -I"$REPO/third-party/tcc/src/include" -I"$MUSL_INC" -I"$W/vmsinc" \
    -I"$REPO/src/libvms/include" -I"$REPO/src/vmsrms/include" \
    -I"$REPO/src/vmsprocess/include" -I"$REPO/src/vmsfs/include" \
    -xc -c -o "$W/EVACWL.o" "$SRC" \
    || { echo "mk_evacwl_native: FATAL: TCC.EXE did not compile EVACWL.C" >&2; exit 1; }

"$LINK" --executable \
    --use "$DECC" --use "$VMS_SHR" --use "$RMS_SHR" \
    --use "$PROC_SHR" --use "$FS_SHR" --use "$LNM_SHR" \
    -o "$OUT" "$W/EVACWL.o" \
    || { echo "mk_evacwl_native: FATAL: LINK.EXE did not link EVACWL" >&2; exit 1; }
chmod +x "$OUT"

# An image whose activator the host kernel cannot open exec-fails before any
# OVMX code runs, which is the whole failure this script's parameters exist to
# prevent -- so state the baked path, and refuse an image that carries none.
INTERP=$(readelf -lW "$OUT" | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p')
[ -n "$INTERP" ] || { echo "mk_evacwl_native: FATAL: $OUT has no PT_INTERP (not an IMGACT-activated image)" >&2; exit 1; }
echo "mk_evacwl_native: built $OUT (activator $INTERP, producers from $PRODUCERS)"
