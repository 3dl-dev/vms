#!/bin/bash
# run_vms_abi_headers.sh -- the VMS-layout STARLET headers (src/libvms/include/
# vms/, vms-022) compile with every offset/size assertion holding, under the
# real alpha-dec-vms cross compiler at the DEC C default (32-bit) and the
# 64-bit pointer size -- the #pragma __required_pointer_size __short regions
# keep every address field 32 bits either way. Control: with the pragma regions
# defeated (-D'__required_pointer_size=' is not possible, so the control is a
# 64-bit pointer field compiled without them) the assertions must fire.
#   IMG=ovmx-cross-alpha-vms tools/cross-alpha-vms/include-surface/run_vms_abi_headers.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}
docker run --rm -v "$REPO/src/libvms/include:/inc:ro" -v "$HERE:/t:ro" -v "$REPO/tools/cross-alpha-vms/binutils-2.43.tar.xz:/bu.tar.xz:ro" "$IMG" bash -c '
set -e
export PATH=/opt/cross-alpha-vms/bin:$PATH
rc=0
for ps in "" "-mpointer-size=64"; do
    if alpha-dec-vms-gcc $ps -std=gnu11 -Wall -Werror -fsyntax-only -I/inc /t/vms_abi_headers.c 2>/tmp/e; then
        echo "  PASS vms/ headers: every VMS-layout assertion holds (pointer size: ${ps:-default 32})"
    else
        echo "  FAIL vms/ headers (pointer size: ${ps:-default 32}):"; sed "s/^/      /" /tmp/e | head -20; rc=1
    fi
done
# vms-5303e: the DEC C shapes bfd/vms-misc.c uses compile against hostabi/ (the
# <rms.h>/<starlet.h> of a host-built program) ahead of the include root, and the
# SAME TU must NOT compile without it -- the native <rms.h> is the OVMX internal FAB
# (no fab-l-ctx, no VMS-ABI services), which is the wall the host gas build hit.
for ps in "" "-mpointer-size=64"; do
    if alpha-dec-vms-gcc $ps -std=gnu11 -Wall -Werror -fsyntax-only -isystem /t/libc-stub -I/inc/hostabi -I/inc /t/hostabi_bfd_shapes.c 2>/tmp/e; then
        echo "  PASS hostabi/ headers: bfd/vms-misc.c shapes compile (pointer size: ${ps:-default 32})"
    else
        echo "  FAIL hostabi/ headers (pointer size: ${ps:-default 32}):"; sed "s/^/      /" /tmp/e | head -20; rc=1
    fi
done
if alpha-dec-vms-gcc -std=gnu11 -fsyntax-only -isystem /t/libc-stub -I/inc /t/hostabi_bfd_shapes.c 2>/tmp/ec; then
    echo "  FAIL control: bfd/vms-misc.c shapes compiled WITHOUT hostabi/ (the native <rms.h> is not the VMS layout)"; rc=1
elif ! grep -q "rms/rms.h" /tmp/ec; then
    echo "  FAIL control: the shapes failed without hostabi/ but not at the native rms.h wall:"; head -5 /tmp/ec; rc=1
else
    echo "  PASS control: without hostabi/ the same shapes do not compile"
fi
# vms-5303e: bfd/bfdio.c (under VMS) needs the FOPEN_* modes WITH the RMS attributes
# (the binutils include/fopen-vms.h); a mode without commas makes _bfd_real_fopen fall off
# its end. The host build puts hostabi/ first for quoted includes (-iquote), so
# "fopen-same.h" is the VMS one; without it the binutils one is "w+b" -- the control.
mkdir -p /tmp/bui && tar xf /bu.tar.xz -C /tmp/bui binutils-2.43/include/fopen-same.h binutils-2.43/include/fopen-vms.h
cat > /tmp/fo.c <<C
#include "fopen-same.h"
typedef char wub_has_attributes[sizeof(FOPEN_WUB) == sizeof("w+b,rfm=udf,rat=none") ? 1 : -1];
C
if alpha-dec-vms-gcc -fsyntax-only -iquote /inc/hostabi -I/tmp/bui/binutils-2.43/include /tmp/fo.c 2>/tmp/e; then
    echo "  PASS hostabi/ fopen-same.h: FOPEN_WUB carries the RMS attributes"
else
    echo "  FAIL hostabi/ fopen-same.h:"; sed "s/^/      /" /tmp/e | head; rc=1
fi
if alpha-dec-vms-gcc -fsyntax-only -I/tmp/bui/binutils-2.43/include /tmp/fo.c 2>/dev/null; then
    echo "  FAIL control: the binutils fopen-same.h also carries the attributes (the stub proves nothing)"; rc=1
else
    echo "  PASS control: without hostabi/ FOPEN_WUB has no attributes"
fi
# Control: the same FAB shape with an ordinary (client-size) pointer field at
# 64 bits must break the layout assertion -- the pragma is what holds it.
cat > /tmp/ctl.c <<C
#include <vms/vms_abi.h>
struct ctl { unsigned char b[36]; void *xab; void *nam; };
__VMS_ABI_OFFSET(struct ctl, nam, 40);
C
if alpha-dec-vms-gcc -mpointer-size=64 -fsyntax-only -I/inc /tmp/ctl.c 2>/dev/null; then
    echo "  FAIL control: a 64-bit pointer field passed the 32-bit layout assertion"; rc=1
else
    echo "  PASS control: a 64-bit pointer field fails the layout assertion"
fi
exit $rc
'
