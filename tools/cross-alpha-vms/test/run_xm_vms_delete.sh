#!/bin/bash
# run_xm_vms_delete.sh -- GCC's VMS host header (gcc/config/vms/xm-vms.h)
# DELETE_IF_ORDINARY compiles against the GCC 14 driver's reporting call
# (vms-fd1, patches/0008). Unpatched it calls perror_with_name, which gcc.cc no
# longer defines. Usage (inside or outside the toolchain image):
#   bash run_xm_vms_delete.sh <tools/cross-alpha-vms dir>
set -euo pipefail
TC=${1:-$(cd "$(dirname "$0")/.." && pwd)}
CC=${CC:-cc}
W=$(mktemp -d)
tar -C "$W" -xf "$TC/gcc-14.2.0.tar.xz" gcc-14.2.0/gcc/config/vms/xm-vms.h
cp "$W/gcc-14.2.0/gcc/config/vms/xm-vms.h" "$W/xm-vms.unpatched.h"
for p in "$TC"/patches/*.patch; do
    grep -q '^+++ b/gcc/config/vms/xm-vms.h' "$p" || continue
    patch -s -p1 -d "$W/gcc-14.2.0" < "$p"
done
cat > "$W/t.c" <<'C'
#include <sys/stat.h>
#include <unistd.h>
extern void error (const char *, ...);   /* gcc/diagnostic-core.h */
#include XM
void delete_if_ordinary (const char *name, int verbose_flag)
{
    struct stat st;
    DELETE_IF_ORDINARY (name, st, verbose_flag);
}
C
rc=0
if $CC -Werror=implicit-function-declaration -DXM="\"$W/gcc-14.2.0/gcc/config/vms/xm-vms.h\"" -fsyntax-only "$W/t.c" 2>"$W/p.err"; then
    echo "  PASS patched xm-vms.h: DELETE_IF_ORDINARY compiles against the GCC 14 driver"
else
    echo "  FAIL patched xm-vms.h:"; sed 's/^/      /' "$W/p.err"; rc=1
fi
if $CC -Werror=implicit-function-declaration -DXM="\"$W/xm-vms.unpatched.h\"" -fsyntax-only "$W/t.c" 2>/dev/null; then
    echo "  FAIL control: the unpatched header compiled, so this test proves nothing"; rc=1
else
    echo "  PASS control: the unpatched header fails (perror_with_name)"
fi
rm -rf "$W"
[ "$rc" -eq 0 ] && echo "ALL XM-VMS DELETE_IF_ORDINARY (vms-fd1) CHECKS PASSED"
exit $rc
