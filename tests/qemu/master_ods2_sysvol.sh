#!/bin/sh
# master_ods2_sysvol.sh -- master /ods2_sysvol.img (VDA300:, the guest's ODS-2
# system volume) from the build-static tree. ONE command line for both callers:
# the image build (tests/qemu/Dockerfile) and the per-facility negative control
# (tests/qemu/inject_and_run.sh), which re-masters it after rebuilding a subject
# that lives on the volume (SYS$SYSTEM:LINK.EXE, rd vms-3cb1) so an injected
# defect reaches the guest instead of the image-build copy.
set -e
R=${1:-/src/repo}
OUT=${2:-/ods2_sysvol.img}
set --
for f in "$R"/build-static/native/*.EXE; do
    [ -f "$f" ] && set -- "$@" "SYSEXE:$(basename "$f")=$f"
done
/src/tests/qemu/mkimage_ods2_sysvol "$OUT" 48 \
    "$R/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT" \
    "$R/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/RIGHTSLIST.DAT" \
    "SYSEXE:TCC.EXE=$R/build-static/bin/TCC.EXE" \
    "SYSEXE:LIBRARIAN.EXE=$R/build-static/bin/LIBRARIAN.EXE" \
    "SYSEXE:LINK.EXE=$R/build-static/bin/sysexe/LINK.EXE" \
    "SYSEXE:IMGACT.EXE=$R/src/imgact/IMGACT.EXE" \
    "SYSLIB:DECC\$SHR.EXE=$R/build-static/bin/DECC\$SHR.EXE" \
    "$@"
