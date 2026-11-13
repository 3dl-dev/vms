#!/bin/bash
# build-host-mmk.sh -- MMK.EXE (MadGoat MMK, tests/corpus/tier3-mmk, unchanged)
# built FOR the alpha-dec-vms host (vms-4ac2 rung 2): the stage-2 alpha-dec-vms
# compiler compiles every TU src/vmslink/mk_mmk.sh builds (the 13 MMK sources,
# the grammar and the OVMX companions) as 32-bit DEC C, and links
# them with the VMS-native LINK.EXE (STRICT: an undefined symbol is
# %LINK-F-UNDEF) against the build's shareables. The result is an
# OpenVMS Alpha image that runs on OVMX/Alpha. Sibling of build-host-link.sh;
# runs INSIDE the ovmx-cross-alpha-vms image:
#
#   docker run --network none -v <repo>:/src:ro -v <cxxtc>:/out:ro -v <joint>:/joint:ro \
#       -v <work>:/w ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/selfhost/build-host-mmk.sh
#
# Output: /w/host-mmk/mmk.exe; progress and any failure go to stdout.
# Flags as run_mmk_alpha_compile.sh (rung 1): -Wno-incompatible-pointer-types
# and -Wno-int-conversion match DEC C, which reports MMK's mismatches as
# warnings; implicit declarations stay errors.
set -euo pipefail
X=/out/cxx
export PATH=$X/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
REPO=/src
CORPUS=$REPO/tests/corpus/tier3-mmk
OVMX=$CORPUS/ovmx
O=/w/host-mmk; rm -rf $O; mkdir -p $O/gen $O/obj
trap 'echo "build-host-mmk: FAIL at line $LINENO: $BASH_COMMAND"' ERR
cd $O

# The TU list is mk_mmk.sh's own, so the recipes cannot drift apart.
TUS=$(awk '/^CORPUS_TUS="/{f=1} f{print} f&&/"[[:space:]]*$/&&NR>1{exit}' "$REPO/src/vmslink/mk_mmk.sh" \
      | tr -d '"\\' | sed 's/^CORPUS_TUS=//' | tr '\n' ' ')
COMPANIONS=$(sed -n 's/^COMPANIONS="\(.*\)"/\1/p' "$REPO/src/vmslink/mk_mmk.sh")
[ -n "$TUS" ] && [ -n "$COMPANIONS" ] || { echo "build-host-mmk: could not read the TU lists from mk_mmk.sh"; exit 1; }

{
    echo "static const char mmk_cld_source[] ="
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n"/' "$CORPUS/mmk_cld.cld"
    echo ";"
} > gen/mmk_cld_src.h

INCS="-I$OVMX -I$O/gen -I$CORPUS -I$REPO/tests/libvms \
    -I$REPO/src/vmsrms/include -I$REPO/src/vmsfs/include -I$REPO/src/vmslnm/include -I$REPO/src/vmsprocess/include"
FLAGS="-std=gnu11 -O1 -g0 -Werror=implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_MMK -include $OVMX/ovmx_mmk_compat.h"
echo "== MMK (host alpha-dec-vms, DEC C default 32-bit pointers)"
OBJS=""
for t in $TUS mmk_parse_tables $COMPANIONS ovmx_mmk_compat; do
    case $t in
        mmk_parse_tables) src=$REPO/tests/libvms/mmk_parse_tables.c; extra=-DOVMX_MMK_PRODUCTION ;;
        ovmx_*)           src=$OVMX/$t.c; extra= ;;
        *)                src=$CORPUS/$t.c; extra= ;;
    esac
    alpha-dec-vms-gcc $FLAGS $extra $INCS -c -o obj/$t.obj "$src" > obj/$t.err 2>&1 \
        || { echo "  cc $t: FAIL"; grep -m5 'error' obj/$t.err; exit 1; }
    OBJS="$OBJS obj/$t.obj"
done
N=$(echo $OBJS | wc -w)
echo "   $N objects compiled (13 MMK + grammar + 5 companions = 19 expected)"
[ "$N" -eq 19 ] || { echo "build-host-mmk: expected 19 objects, got $N"; exit 1; }

# The LINK the driver's ld (ovmx-ld) runs -- same start files, libgcc, STARLET
# last, STRICT -- with every OVMX system shareable MMK calls into named, as an
# OpenVMS link resolves against IMAGELIB: MMK's companions reach the executive
# services in LIBVMSPROCESS$SHR / LIBVMSFS$SHR / LIBVMSLNM$SHR / LIBVMSSYS$SHR
# (the same seven producers src/vmslink/mk_mmk.sh links the x86 MMK.EXE against).
echo "== LINK (VMS-native LINK.EXE, STRICT: an undefined symbol is %LINK-F-UNDEF)"
GL=$X/lib/gcc/alpha-dec-vms/14_2_0
S=$OVMX_ALPHA_SYSROOT
USES=""
for p in 'DECC$SHR' LIBOTS_SHR 'LIBVMSRMS$SHR' 'LIBVMS$SHR' 'LIBVMSPROCESS$SHR' 'LIBVMSFS$SHR' 'LIBVMSLNM$SHR' 'LIBVMSSYS$SHR'; do
    [ -f "$S/$p.EXE" ] || { echo "build-host-mmk: producer $S/$p.EXE missing"; exit 1; }
    USES="$USES --use $S/$p.EXE"
done
"$S/LINK.EXE" --transfer __main $USES -o $O/mmk.exe \
    $GL/vms-dwarf2eh.o $GL/crt0.o $GL/crtbegin.o $OBJS --library $GL/libgcc.a $GL/crtend.o \
    --library "$S/STARLET.a" > link.log 2>&1 || { grep -E 'LINK-|UNDEF' link.log | sort -u | head -60; exit 1; }
if grep -qE 'LINK-[FE]|UNDEF' link.log; then grep -E 'LINK-|UNDEF' link.log | head -40; exit 1; fi
ls -la $O/mmk.exe
echo "HOST-MMK: OK"
