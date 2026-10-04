#!/bin/bash
# vms-4d0 probe: stage-2 alpha-dec-vms toolchain against an OVMX sysroot
# (musl-alpha CRTL headers with the DEC C size_t model + the OVMX vms/ include
# surface), so libgcc builds WITH libc headers (no inhibit_libc) and its EH
# unwinder (unwind-dw2 + vms-unwind.h) is produced.
set -x
export PATH=/opt/cross-alpha-vms/bin:$PATH
SYSROOT=/sysroot
if [ ! -f $SYSROOT/usr/include/stdlib.h ]; then
  mkdir -p /p && cd /p && tar xzf /repo/tools/cross-alpha-vms/musl-arch/musl-1.2.5.tar.gz && cd musl-1.2.5
  cp -r /repo/tools/cross-alpha-vms/musl-arch/arch/alpha-dec-vms arch/
  sed -i 's/^TYPEDEF unsigned _Addr size_t;$/TYPEDEF unsigned int size_t;/; s/^TYPEDEF _Addr ssize_t;$/TYPEDEF int ssize_t;/; s/^STRUCT iovec { void \*iov_base; size_t iov_len; };$/STRUCT iovec { void *iov_base; unsigned long long iov_len; };/' include/alltypes.h.in
  grep -q "ARCH=alpha-dec-vms" configure || sed -i 's#^unknown) fail#alpha*) ARCH=alpha-dec-vms ;;\nunknown) fail#' configure
  CC="alpha-dec-vms-gcc -mpointer-size=64" ./configure --target=alpha-dec-vms --prefix=$SYSROOT/usr --disable-shared >/dev/null 2>&1
  make install-headers >/dev/null 2>&1
fi
mkdir -p $SYSROOT/usr/include/vms && cp /repo/src/libvms/include/vms/*.h $SYSROOT/usr/include/vms/
mkdir -p /b/build2 && cd /b/build2
/b/gcc-14.2.0/configure --target=alpha-dec-vms --prefix=/opt/cross-alpha-vms \
  --with-sysroot=$SYSROOT --with-native-system-header-dir=/usr/include \
  --enable-languages=c,c++ --disable-bootstrap --disable-multilib \
  --disable-libssp --disable-shared --disable-nls --disable-fixincludes \
  --disable-libstdcxx-pch --disable-libgomp --disable-libquadmath --disable-libatomic \
  --with-gnu-as --with-gnu-ld > /w/s2-configure.log 2>&1; echo CONF_RC=$?
mkdir -p gcc/{c,cp,c-family,common,objc,d,rust,go,fortran,ada,lto,jit,m2,analyzer}
make all-gcc -j"$(nproc)" > /w/s2-gcc.log 2>&1; echo GCC_RC=$?
make -k all-target-libgcc -j"$(nproc)" \
  CFLAGS_FOR_TARGET="-g0 -O2 -mpointer-size=64 -fno-function-sections -fno-data-sections" > /w/s2-libgcc.log 2>&1; echo LIBGCC_RC=$?
cd alpha-dec-vms/libgcc 2>/dev/null && ls -la unwind-dw2.o unwind-dw2-fde.o vms-gcc_shell_handler.o vms-ucrt0.o libgcc.a libgcc_eh.a 2>&1
grep -E 'error:' /w/s2-libgcc.log | sort | uniq -c | sort -rn | head -20
echo S2_DONE
