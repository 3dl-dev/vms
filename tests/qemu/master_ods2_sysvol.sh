#!/bin/sh
# master_ods2_sysvol.sh -- master /ods2_sysvol.img (VDA300:, the guest's ODS-2
# system volume) from the build-static tree. ONE command line for both callers:
# the image build (tests/qemu/Dockerfile) and the per-facility negative control
# (tests/qemu/inject_and_run.sh), which re-masters it after rebuilding a subject
# that lives on the volume (SYS$SYSTEM:LINK.EXE, rd vms-3cb1) so an injected
# defect reaches the guest instead of the image-build copy.
#
# Optional third argument: a SYS$LIBRARY directory whose OVMX shareables
# (*$SHR.EXE other than DECC$SHR, which always comes from build-static/bin) are
# mastered into [SYS0.SYSCOMMON.SYSLIB] too. The RMS-backed DECC$SHR (rd vms-569)
# imports LIBVMSRMS$SHR and LIBVMS$SHR, so a volume that carries it must carry
# them and theirs -- the corpus guest activates its native images straight off
# this volume, with no suite to stage them first.
set -e
R=${1:-/src/repo}
OUT=${2:-/ods2_sysvol.img}
SYSLIB_DIR=${3:-}
set --
if [ -n "$SYSLIB_DIR" ]; then
    for f in "$SYSLIB_DIR"/*'$SHR.EXE'; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        [ "$b" = 'DECC$SHR.EXE' ] && continue
        set -- "$@" "SYSLIB:$b=$f"
    done
fi
for f in "$R"/build-static/native/*.EXE; do
    [ -f "$f" ] && set -- "$@" "SYSEXE:$(basename "$f")=$f"
done
/src/tests/qemu/mkimage_ods2_sysvol "$OUT" 64 \
    "$R/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT" \
    "$R/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/RIGHTSLIST.DAT" \
    "SYSEXE:TCC.EXE=$R/build-static/bin/TCC.EXE" \
    "SYSEXE:LIBRARIAN.EXE=$R/build-static/bin/LIBRARIAN.EXE" \
    "SYSEXE:LINK.EXE=$R/build-static/bin/sysexe/LINK.EXE" \
    "SYSEXE:IMGACT.EXE=$R/src/imgact/IMGACT.EXE" \
    "SYSLIB:DECC\$SHR.EXE=$R/build-static/bin/DECC\$SHR.EXE" \
    "$@"
