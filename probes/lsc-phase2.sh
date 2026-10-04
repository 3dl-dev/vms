#!/bin/bash
# vms-4d0 phase 2 probe: configure + build libstdc++-v3 (hosted) for
# alpha-dec-vms against OVMX's musl-alpha CRTL headers, with the phase-1 g++.
# Runs in the vms4d0-gxx image (has /b/gcc-14.2.0 source) with repo at /repo.
set -x
export PATH=/opt/cross-alpha-vms/bin:$PATH
# 1. CRTL headers (OVMX alpha-dec-vms musl) into a sysroot
mkdir -p /p && cd /p
tar xzf /repo/tools/cross-alpha-vms/musl-arch/musl-1.2.5.tar.gz
cd musl-1.2.5
cp -r /repo/tools/cross-alpha-vms/musl-arch/arch/alpha-dec-vms arch/
sed -i 's/^TYPEDEF unsigned _Addr size_t;$/TYPEDEF unsigned int size_t;/; s/^TYPEDEF _Addr ssize_t;$/TYPEDEF int ssize_t;/; s/^STRUCT iovec { void \*iov_base; size_t iov_len; };$/STRUCT iovec { void *iov_base; unsigned long long iov_len; };/' include/alltypes.h.in
grep -c 'unsigned int size_t' include/alltypes.h.in
grep -q "ARCH=alpha-dec-vms" configure || sed -i 's#^unknown) fail#alpha*) ARCH=alpha-dec-vms ;;\nunknown) fail#' configure
CC="alpha-dec-vms-gcc -mpointer-size=64" ./configure --target=alpha-dec-vms --prefix=/sysroot/usr --disable-shared >/dev/null 2>&1 || echo CONFIGURE_FAIL
make install-headers >/dev/null 2>&1 || echo INSTALL_HEADERS_FAIL
cp /b/gcc-14.2.0/libgcc/unwind-generic.h /opt/cross-alpha-vms/lib/gcc/alpha-dec-vms/14.2.0/include/unwind.h
# 2. libstdc++-v3 standalone configure as a target library
rm -rf /b/lsc; mkdir -p /b/lsc && cd /b/lsc
export CC="alpha-dec-vms-gcc -mpointer-size=64 -fno-function-sections -fno-data-sections -isystem /sysroot/usr/include -B/w/sysroot/"
export CXX="alpha-dec-vms-g++ -mpointer-size=64 -fno-function-sections -fno-data-sections -isystem /sysroot/usr/include -B/w/sysroot/"
export AR=ar AR_FLAGS=crS RANLIB=true
/b/gcc-14.2.0/libstdc++-v3/configure --host=alpha-dec-vms --build=x86_64-pc-linux-gnu \
  --prefix=/opt/cross-alpha-vms --with-cross-host=x86_64-pc-linux-gnu \
  --disable-shared --disable-nls --disable-libstdcxx-pch --disable-multilib \
  --with-gxx-include-dir=/opt/cross-alpha-vms/alpha-dec-vms/include/c++/14.2.0 \
  > /b/lsc-configure.log 2>&1
echo CONFIGURE_RC=$?
tail -30 /b/lsc-configure.log; cp /b/lsc-configure.log /b/lsc/config.log /w/ 2>/dev/null
grep -E 'os_include_dir|enable_hosted|gcc_no_link|GCC_NO_EXECUTABLES' config.log | head
make -C include > /b/lsc-make.log 2>&1 || true
make -C src/c++11 debug.lo CXXFLAGS="-g -O2 -fpermissive" >> /b/lsc-make.log 2>&1 || true
make -k -j"$(nproc)" >> /b/lsc-make.log 2>&1
echo MAKE_RC=$?
grep -E 'error:|Error [0-9]' /b/lsc-make.log | sort | uniq -c | sort -rn | head -40
ls -la src/.libs/*.a libsupc++/.libs/*.a 2>/dev/null
cp /b/lsc-make.log /w/; echo LSC_PHASE2_DONE
