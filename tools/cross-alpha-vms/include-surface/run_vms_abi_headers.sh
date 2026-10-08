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
docker run --rm -v "$REPO/src/libvms/include:/inc:ro" -v "$HERE:/t:ro" "$IMG" bash -c '
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
