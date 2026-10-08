#!/bin/sh
# mk_evacwl_native.sh - build EVACWL.C as a VMS-native image (vms-06c), the
# same recipe tests/qemu/mk_corpus_native.sh uses for the Eight-Cubed corpus
# (rd vms-db7): compiled by OVMX's own tcc (TCC.EXE, which honours DEC C's
# #pragma extern_model), linked by LINK.EXE --use DECC$SHR. The image's
# PT_INTERP is IMGACT.EXE at its SYS$SYSTEM path, so it activates exactly as
# a customer's compiled program would -- this is NOT a gcc-built test binary
# linked against src/libvms's internals.
#
# Kept SEPARATE from mk_corpus_native.sh / corpus_runtime_native.txt on
# purpose: EVACWL is not an Eight-Cubed corpus program, and corpus_runtime_
# report.sh / docs/corpus-scoreboard.md's counts are specifically the
# curated corpus set (single-ledger discipline) -- this script must not
# make EVACWL show up there.
#
# Output: <repo>/build-static/native/EVACWL.EXE. Nothing else needs to know
# this script ran: the Dockerfile's two existing `build-static/native/*.EXE`
# globs (the ODS-2 system-disk image at mkimage_ods2_sysvol.c's invocation,
# and the booted-guest /vms/SYS0/SYSCOMMON/SYSEXE staging step) already pick
# up every image in that directory, so EVACWL.EXE reaches both the corpus
# system disk and the booted-OVMX initramfs with no further Dockerfile edit
# beyond the one RUN line that invokes this script.
#
# Usage: mk_evacwl_native.sh <repo-root>
set -eu
REPO=${1:?usage: mk_evacwl_native.sh <repo-root>}
BIN=$REPO/build-static/bin
TCC=$BIN/TCC.EXE
DECC="$BIN/DECC\$SHR.EXE"
VMS_SHR="$BIN/LIBVMS\$SHR.EXE"
RMS_SHR="$BIN/LIBVMSRMS\$SHR.EXE"
PROC_SHR="$BIN/LIBVMSPROCESS\$SHR.EXE"
FS_SHR="$BIN/LIBVMSFS\$SHR.EXE"
LNM_SHR="$BIN/LIBVMSLNM\$SHR.EXE"
NATIVE=$REPO/build-static/native
SRC=$REPO/tests/lab/ci6/EVACWL.C
W=${WORK:-/tmp/evacwl-native}

[ -x "$TCC" ] || { echo "mk_evacwl_native: FATAL: no $TCC" >&2; exit 1; }
for f in "$DECC" "$VMS_SHR" "$RMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR"; do
    [ -f "$f" ] || { echo "mk_evacwl_native: FATAL: producer image not found: $f" >&2; exit 1; }
done
[ -f "$SRC" ] || { echo "mk_evacwl_native: FATAL: no $SRC" >&2; exit 1; }

rm -rf "$W"; mkdir -p "$W/vmsinc" "$NATIVE"
# The two DEC C headers the corpus build also needs that musl does not have
# (mk_corpus_native.sh's own comment on this copy applies here unchanged).
cp "$REPO/src/libvms/include/unixlib.h" "$REPO/src/libvms/include/reentrancy.h" "$W/vmsinc/"
MUSL_INC=$(echo /usr/include/*-linux-musl)

# LINK.EXE with its default PT_INTERP (the SYS$SYSTEM IMGACT.EXE path --
# mk_corpus_native.sh's own comment on this invocation applies here too).
${CC:-gcc} -std=gnu11 -O2 -I"$REPO/src/vmslink/include" -o "$W/LINK.EXE" "$REPO/src/vmslink/link.c"

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

"$W/LINK.EXE" --executable \
    --use "$DECC" --use "$VMS_SHR" --use "$RMS_SHR" \
    --use "$PROC_SHR" --use "$FS_SHR" --use "$LNM_SHR" \
    -o "$NATIVE/EVACWL.EXE" "$W/EVACWL.o" \
    || { echo "mk_evacwl_native: FATAL: LINK.EXE did not link EVACWL" >&2; exit 1; }
chmod +x "$NATIVE/EVACWL.EXE"

echo "mk_evacwl_native: built $NATIVE/EVACWL.EXE"
