#!/bin/bash
# run_mmk_alpha_compile.sh (vms-59a) -- MMK, the VMS build tool the self-host
# spine drives (tests/corpus/tier3-mmk, MadGoat MMK, unchanged), compiles for
# the alpha-dec-vms host: every TU src/vmslink/mk_mmk.sh builds, plus the
# grammar and the tagged OVMX companions, compiled by the alpha-dec-vms port
# compiler at its default (32-bit) pointer size against the same C RTL header
# set the stage-2 toolchain installs as its sysroot (build-cxx-toolchain.sh
# step 1: the musl alpha-dec-vms headers + src/libvms/include). This is the
# first rung toward MMK.EXE on OVMX/Alpha, the build driver GCC stage 2 needs
# (rd vms-9e6).
#
# The compiler defines __ALPHA, so MMK takes its Alpha branches: its queue
# macros call DEC C's __PAL_INSQUEL/__PAL_REMQUEL from <builtins.h>. Control:
# with the companion <builtins.h> that declares nothing (as before this rung),
# the queue-using TUs must fail to compile, or the test proves nothing.
#
# Flags: -Wno-incompatible-pointer-types and -Wno-int-conversion match DEC C,
# which reports MMK's mismatches as warnings (a descriptor passed as a char *;
# the address of a terminator mask passed as $QIOW's __int64 P2; a zone id
# passed as LIB$INSERT_TREE's user-data pointer); GCC 14 makes them errors.
# Implicit declarations stay errors.
#
# Runs inside the ovmx-cross-alpha-vms image with the repo mounted read-only:
#   docker run --rm --network none -v <repo>:/src:ro ovmx-cross-alpha-vms:latest \
#       bash /src/tools/cross-alpha-vms/test/run_mmk_alpha_compile.sh /src
set -euo pipefail
REPO=${1:-/src}
export PATH=/opt/cross-alpha-vms/bin:$PATH
CC=alpha-dec-vms-gcc
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CORPUS=$REPO/tests/corpus/tier3-mmk
OVMX=$CORPUS/ovmx

# The C RTL header sysroot, built as build-cxx-toolchain.sh builds it.
OVERLAY=$REPO/tools/cross-alpha-vms/musl-arch MUSL_HEADERS_ONLY=$W/sysroot WORK=$W/musl \
    bash "$REPO/tools/cross-alpha-vms/musl-arch/build-musl.sh" > "$W/musl.log" 2>&1 \
    || { tail -30 "$W/musl.log"; echo "FAIL: musl alpha-dec-vms headers"; exit 1; }
cp -r "$REPO/src/libvms/include/." "$W/sysroot/usr/include/"

# The TU list is mk_mmk.sh's own, so the two recipes cannot drift apart.
TUS=$(awk '/^CORPUS_TUS="/{f=1} f{print} f&&/"[[:space:]]*$/&&NR>1{exit}' "$REPO/src/vmslink/mk_mmk.sh" \
      | tr -d '"\\' | sed 's/^CORPUS_TUS=//' | tr '\n' ' ')
COMPANIONS=$(sed -n 's/^COMPANIONS="\(.*\)"/\1/p' "$REPO/src/vmslink/mk_mmk.sh")
[ -n "$TUS" ] && [ -n "$COMPANIONS" ] || { echo "FAIL: could not read the TU lists from mk_mmk.sh"; exit 1; }

mkdir -p "$W/gen"
{
    echo "static const char mmk_cld_source[] ="
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n"/' "$CORPUS/mmk_cld.cld"
    echo ";"
} > "$W/gen/mmk_cld_src.h"

compile_all() {  # $1 = directory holding the companion <builtins.h>; $2 = object dir
    local shim=$1 out=$2 t src rc=0
    mkdir -p "$out"
    local incs="-I$shim -I$W/gen -I$OVMX -I$CORPUS -I$REPO/tests/libvms \
        -isystem $W/sysroot/usr/include -I$REPO/src/vmsrms/include -I$REPO/src/vmsfs/include \
        -I$REPO/src/vmslnm/include -I$REPO/src/vmsprocess/include"
    local flags="-std=gnu11 -O2 -g0 -Werror=implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion \
        -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_MMK -include $OVMX/ovmx_mmk_compat.h"
    for t in $TUS mmk_parse_tables $COMPANIONS ovmx_mmk_compat; do
        case $t in
            mmk_parse_tables) src=$REPO/tests/libvms/mmk_parse_tables.c; extra=-DOVMX_MMK_PRODUCTION ;;
            ovmx_*)           src=$OVMX/$t.c; extra= ;;
            *)                src=$CORPUS/$t.c; extra= ;;
        esac
        if $CC $flags $extra $incs -c -o "$out/$t.obj" "$src" 2>"$out/$t.err"; then
            echo "  ok   $t"
        else
            echo "  FAIL $t: $(grep -m1 'error' "$out/$t.err")"; rc=1
        fi
    done
    return $rc
}

rc=0
echo "== MMK for alpha-dec-vms (unchanged MadGoat sources + OVMX companions) =="
if compile_all "$OVMX" "$W/obj"; then
    n=$(ls "$W"/obj/*.obj | wc -l)
    bad=0
    for o in "$W"/obj/*.obj; do
        alpha-dec-vms-objdump -f "$o" | grep -q 'file format vms-alpha' || { echo "  FAIL $o is not an EVAX object"; bad=1; }
    done
    [ $bad -eq 0 ] && echo "  PASS all $n MMK objects compiled as EVAX (vms-alpha) objects" || rc=1
else
    echo "  FAIL MMK does not compile for alpha-dec-vms"; rc=1
fi

echo "== control: a <builtins.h> without the Alpha queue built-ins =="
mkdir -p "$W/emptyshim"
printf '#ifndef OVMX_SHIM_BUILTINS_H\n#define OVMX_SHIM_BUILTINS_H\n#endif\n' > "$W/emptyshim/builtins.h"
if compile_all "$W/emptyshim" "$W/ctl" >/dev/null 2>&1; then
    echo "  FAIL control: MMK compiled without __PAL_INSQUEL/__PAL_REMQUEL declared, so this test proves nothing"; rc=1
else
    grep -lq '__PAL_INSQUEL\|__PAL_REMQUEL' "$W"/ctl/*.err \
        && echo "  PASS control: without them the queue-using TUs are rejected (implicit __PAL_INSQUEL/__PAL_REMQUEL)" \
        || { echo "  FAIL control: rejected for another reason"; rc=1; }
fi
[ $rc -eq 0 ] && echo "ALL MMK ALPHA-COMPILE (vms-59a) CHECKS PASSED"
exit $rc
