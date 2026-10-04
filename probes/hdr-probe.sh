#!/bin/bash
# vms-4d0 probe: do OVMX's alpha-dec-vms musl CRTL headers agree with the port
# compiler's own size_t (__SIZE_TYPE__) in C and C++?
set -x
export PATH=/opt/cross-alpha-vms/bin:$PATH
mkdir -p /p && cd /p
tar xzf /repo/tools/cross-alpha-vms/musl-arch/musl-1.2.5.tar.gz
cd musl-1.2.5
O=/repo/tools/cross-alpha-vms/musl-arch
cp -r $O/arch/alpha-dec-vms arch/
grep -q "ARCH=alpha-dec-vms" configure || sed -i 's#^unknown) fail#alpha*) ARCH=alpha-dec-vms ;;\nunknown) fail#' configure
CC=alpha-dec-vms-gcc ./configure --target=alpha-dec-vms --prefix=/sysroot/usr --disable-shared >/dev/null 2>&1 || { echo CONFIGURE_FAIL; tail -20 config.log; }
make install-headers >/dev/null 2>&1 || echo INSTALL_HEADERS_FAIL
ls /sysroot/usr/include | head -5
grep -n 'size_t\b' /sysroot/usr/include/bits/alltypes.h | head -5
cat > /p/t.c <<'EOF'
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
size_t f(const char *s) { return strlen(s) + sizeof(int); }
EOF
for L in c c++; do
  for M in "" -mpointer-size=64; do
    echo "== lang=$L flags=$M"
    alpha-dec-vms-gcc -x $L $M -isystem /sysroot/usr/include -fsyntax-only /p/t.c 2>&1 | head -8
    echo "rc=$?"
  done
done
echo PROBE_DONE
