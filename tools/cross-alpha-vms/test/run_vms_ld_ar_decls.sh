#!/bin/bash
# run_vms_ld_ar_decls.sh -- GCC's VMS-host linker/librarian wrappers
# (gcc/config/vms/vms-ld.c, vms-ar.c) compile with every function they call
# declared (vms-fd1, patches/0009). GCC 14 rejects implicit declarations; the
# unpatched files call SYS$TRNLNM, SYS$CRELNM, decc$to_vms and mkstemps with
# none in scope. Compiled here as the VMS host compiles them (VMS defined),
# against a <unixlib.h> that declares decc$to_vms as the DEC C RTL's does.
# Usage: bash run_vms_ld_ar_decls.sh <tools/cross-alpha-vms dir>
set -euo pipefail
TC=${1:-$(cd "$(dirname "$0")/.." && pwd)}
CC=${CC:-cc}
W=$(mktemp -d)
tar -C "$W" -xf "$TC/gcc-14.2.0.tar.xz" gcc-14.2.0/gcc/config/vms/vms-ld.c \
    gcc-14.2.0/gcc/config/vms/vms-ar.c gcc-14.2.0/include
mkdir -p "$W/unpatched" "$W/inc"
cp "$W/gcc-14.2.0/gcc/config/vms/vms-ld.c" "$W/gcc-14.2.0/gcc/config/vms/vms-ar.c" "$W/unpatched/"
for p in "$TC"/patches/*.patch; do
    grep -qE '^\+\+\+ b/gcc/config/vms/vms-(ld|ar)\.c' "$p" || continue
    patch -s -p1 -d "$W/gcc-14.2.0" < "$p"
done
# The DEC C RTL forms these files are written against, over the host's own
# headers: <unixlib.h> declares decc$to_vms (OVMX: src/libvms/include/
# unixlib.h) and getcwd takes DEC C's optional third (format) argument (OVMX:
# the __VMS client <unistd.h>).
cat > "$W/inc/unixlib.h" <<'H'
int decc$to_vms(const char *, int (*)(char *, int), int, int);
H
cat > "$W/inc/unistd.h" <<'H'
#include_next <unistd.h>
#define getcwd(buf, size, ...) getcwd(buf, size)
H
FLAGS="-std=gnu11 -Werror=implicit-function-declaration -DVMS -fsyntax-only -I$W/inc -I$W/gcc-14.2.0/include"
rc=0
for f in vms-ld vms-ar; do
    if $CC $FLAGS "$W/gcc-14.2.0/gcc/config/vms/$f.c" 2>"$W/$f.err"; then
        echo "  PASS patched $f.c compiles with every call declared (VMS host)"
    else
        echo "  FAIL patched $f.c:"; sed 's/^/      /' "$W/$f.err" | head -20; rc=1
    fi
    if $CC $FLAGS "$W/unpatched/$f.c" 2>/dev/null; then
        echo "  FAIL control: unpatched $f.c compiled, so this test proves nothing"; rc=1
    else
        echo "  PASS control: unpatched $f.c is rejected (implicit declaration)"
    fi
done
rm -rf "$W"
[ "$rc" -eq 0 ] && echo "ALL VMS-LD/AR DECLARATION (vms-fd1) CHECKS PASSED"
exit $rc
