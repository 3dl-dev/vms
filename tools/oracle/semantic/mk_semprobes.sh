#!/bin/sh
# mk_semprobes.sh - build the semantic-oracle probes (rd vms-8d1) as VMS-native
# OVMX images: spgen.py turns each specs/<family>.py into C, cc compiles it, and
# LINK.EXE links SP_<FAMILY>.EXE against the shareable producer graph -- the same
# toolchain and flags as RC3.EXE/PARTS.EXE (distro/Dockerfile.bootable), so a
# probe activates through IMGACT.EXE and calls the services exactly as any
# OVMX program does.
# Usage: mk_semprobes.sh <LINK.EXE> <SYSLIB-dir> <out-dir>
# Env:   CC (default gcc), CFLAGS (x86_64 caller adds -mtls-dialect=gnu2),
#        PYTHON (default python3)
set -e
LINK_EXE=${1:?usage: mk_semprobes.sh <LINK.EXE> <SYSLIB-dir> <out-dir>}
SYSLIB=${2:?need the SYSLIB dir holding DECC\$SHR.EXE and the LIBVMS*\$SHR.EXE}
OUT=${3:?need an output dir}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
CC=${CC:-gcc}
PYTHON=${PYTHON:-python3}
CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
WORK=${WORK:-/tmp/mk-semprobes}
mkdir -p "$WORK" "$OUT"
USES=""
for s in 'DECC$SHR' 'LIBVMS$SHR' 'LIBVMSPROCESS$SHR' 'LIBVMSFS$SHR' 'LIBVMSLNM$SHR' \
         'LIBVMSRMS$SHR' 'LIBVMSSYS$SHR'; do
    [ -f "$SYSLIB/$s.EXE" ] || { echo "mk_semprobes: missing $SYSLIB/$s.EXE"; exit 1; }
    USES="$USES --use $SYSLIB/$s.EXE"
done
n=0
for spec in "$HERE"/specs/*.py; do
    fam=$(basename "$spec" .py)
    FAM=$(echo "$fam" | tr '[:lower:]' '[:upper:]')
    echo "mk_semprobes: $fam -> SP_$FAM.EXE"
    "$PYTHON" "$HERE/spgen.py" --c "$spec" > "$WORK/sp_$fam.c"
    # shellcheck disable=SC2086
    $CC $CFLAGS $DEFS -std=gnu99 -I"$REPO/src/libvms/include" -I"$REPO/src/vmsrms/include" -x c -c \
        -o "$WORK/sp_$fam.o" "$WORK/sp_$fam.c"
    # shellcheck disable=SC2086
    "$LINK_EXE" --executable $USES -o "$OUT/SP_$FAM.EXE" "$WORK/sp_$fam.o"
    n=$((n + 1))
done
# DCL families (specs-dcl/*.py, comgen.py): a command procedure, no compile/link
for spec in "$HERE"/specs-dcl/*.py; do
    [ -f "$spec" ] || continue
    fam=$(basename "$spec" .py)
    FAM=$(echo "$fam" | tr '[:lower:]' '[:upper:]')
    echo "mk_semprobes: $fam -> SP_$FAM.COM"
    "$PYTHON" "$HERE/comgen.py" "$spec" > "$OUT/SP_$FAM.COM"
done
[ "$n" -gt 0 ] || { echo "mk_semprobes: no specs found"; exit 1; }
echo "mk_semprobes: built $n probe image(s) in $OUT"
