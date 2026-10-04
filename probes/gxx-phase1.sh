#!/bin/bash
# vms-4d0 phase 1 (exploratory, k3s rail): add the C++ front end (cc1plus +
# alpha-dec-vms-g++) to the cached alpha-dec-vms cross toolchain image, same
# GCC 14.2.0 + OVMX gcc patches, same configure as build-toolchain.sh except
# --enable-languages=c,c++. Runs INSIDE the ovmx-cross-alpha-vms container with
# the repo mounted at /repo.
set -euxo pipefail
GCC_VER=14.2.0
GCC_SHA256=a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
TARGET=alpha-dec-vms
PREFIX=/opt/cross-alpha-vms
export PATH="$PREFIX/bin:$PATH"
mkdir -p /b && cd /b
[ -f gcc-$GCC_VER.tar.xz ] || wget --tries=3 --timeout=30 -q "https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" \
  || wget --tries=3 --timeout=30 -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
echo "$GCC_SHA256  gcc-$GCC_VER.tar.xz" | sha256sum -c -
tar xf gcc-$GCC_VER.tar.xz
for p in /repo/tools/cross-alpha-vms/patches/*.patch; do
  grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' "$p" && continue
  patch -p1 -d /b/gcc-$GCC_VER < "$p"
done
mkdir -p build && cd build
/b/gcc-$GCC_VER/configure --target=$TARGET --prefix=$PREFIX \
  --enable-languages=c,c++ --disable-bootstrap --disable-multilib \
  --disable-libssp --disable-shared --disable-nls --disable-fixincludes \
  --without-headers --with-gnu-as --with-gnu-ld
mkdir -p gcc/{c,cp,c-family,common,objc,d,rust,go,fortran,ada,lto,jit,m2,analyzer}
make all-gcc -j"$(nproc)"
make install-gcc
ls -la $PREFIX/bin | grep -E 'g\+\+|c\+\+'
echo GXX_PHASE1_OK
