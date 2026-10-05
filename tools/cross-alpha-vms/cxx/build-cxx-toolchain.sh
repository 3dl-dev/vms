#!/bin/bash
# build-cxx-toolchain.sh -- the stage-2 alpha-dec-vms C/C++ toolchain over the
# OVMX C RTL (vms-4d0). Runs INSIDE the ovmx-cross-alpha-vms toolchain image:
#
#   docker run -v <repo>:/src:ro -v <joint-out>:/joint:ro -v <out>:/out \
#       ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/cxx/build-cxx-toolchain.sh
#
# <joint-out> is a build-joint-image.sh OUT dir: LINK.EXE, DECC$SHR.EXE,
# LIBOTS_SHR.EXE, STARLET.a (+ LIBVMSRMS$SHR/LIBVMS$SHR on a veneer build).
#
# Stage 1 (the image) is the C-only, header-less cross compiler that builds the
# C RTL itself. Stage 2, built here into /out/cxx, is configured against an OVMX
# sysroot -- the alpha-dec-vms musl CRTL headers (build-musl.sh, every OVMX
# overlay/patch applied) plus the OVMX STARLET include surface
# (src/libvms/include) -- with C and C++. Because it has C RTL headers it also
# builds libgcc's EH unwinder (unwind-dw2 + the port's vms-unwind.h) and the
# port's own crt0/crtbegin/crtend; its `ld` is OVMX LINK.EXE (cxx/ovmx-ld), so
# libstdc++-v3's configure link tests are answered by the real OVMX surface.
# Output: /out/cxx (prefix: bin/alpha-dec-vms-{gcc,g++}, lib/gcc/...,
# alpha-dec-vms/lib/libstdc++.a + include/c++, ovmx/ link environment, sysroot/).
set -euo pipefail
GCC_VER=14.2.0
GCC_SHA256=a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
TARGET=alpha-dec-vms
STAGE1=/opt/cross-alpha-vms
X=/out/cxx
SYSROOT=$X/sysroot
JOBS=$(nproc)
export PATH="$STAGE1/bin:$PATH"
mkdir -p "$X" /tmp/cxx && cd /tmp/cxx

echo "== [1/6] OVMX sysroot: alpha-dec-vms CRTL headers + STARLET include surface =="
OVERLAY=/src/tools/cross-alpha-vms/musl-arch MUSL_HEADERS_ONLY="$SYSROOT" WORK=/tmp/cxx/musl \
    bash /src/tools/cross-alpha-vms/musl-arch/build-musl.sh > /tmp/cxx/musl.log 2>&1 \
    || { tail -40 /tmp/cxx/musl.log; exit 1; }
cp -r /src/src/libvms/include/. "$SYSROOT/usr/include/"

echo "== [2/6] GCC $GCC_VER source + the OVMX port patches =="
if [ -f "/src/tools/cross-alpha-vms/gcc-$GCC_VER.tar.xz" ]; then
    cp "/src/tools/cross-alpha-vms/gcc-$GCC_VER.tar.xz" .
else
    wget --tries=3 --timeout=30 -q "https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz" \
      || wget --tries=3 --timeout=30 -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
fi
echo "$GCC_SHA256  gcc-$GCC_VER.tar.xz" | sha256sum -c -
tar xf "gcc-$GCC_VER.tar.xz"
for p in /src/tools/cross-alpha-vms/patches/*.patch; do
    grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' "$p" && continue
    patch -p1 -d "gcc-$GCC_VER" < "$p"
done

echo "== [3/6] the OVMX link environment + ld (OVMX LINK.EXE) for the stage-2 target tools =="
mkdir -p "$X/ovmx" "$X/$TARGET/bin"
for f in LINK.EXE 'DECC$SHR.EXE' LIBOTS_SHR.EXE STARLET.a 'LIBVMSRMS$SHR.EXE' 'LIBVMS$SHR.EXE'; do
    [ -f "/joint/$f" ] && cp "/joint/$f" "$X/ovmx/"
done
for t in as ar nm ranlib objdump strip; do ln -sf "$STAGE1/bin/$TARGET-$t" "$X/$TARGET/bin/$t"; done
install -m 755 /src/tools/cross-alpha-vms/cxx/ovmx-ld "$X/$TARGET/bin/ld"

echo "== [4/6] stage-2 gcc (C, C++) configured against the OVMX sysroot =="
mkdir -p build && cd build
if [ -x "$X/bin/$TARGET-g++" ]; then
    echo "   (stage-2 gcc already installed in $X -- reusing it)"
else
"../gcc-$GCC_VER/configure" --target=$TARGET --prefix="$X" \
    --with-sysroot="$SYSROOT" --with-native-system-header-dir=/usr/include \
    --with-build-time-tools="$X/$TARGET/bin" \
    --enable-languages=c,c++ --disable-bootstrap --disable-multilib \
    --disable-libssp --disable-shared --disable-nls --disable-fixincludes \
    --disable-libstdcxx-pch --disable-libgomp --disable-libquadmath \
    --disable-libatomic --disable-libsanitizer --disable-libvtv \
    --with-gnu-as > /tmp/cxx/gcc-configure.log 2>&1 || { tail -40 /tmp/cxx/gcc-configure.log; exit 1; }
mkdir -p gcc/{c,cp,c-family,common,objc,d,rust,go,fortran,ada,lto,jit,m2,analyzer}
make all-gcc -j"$JOBS" > /tmp/cxx/gcc-make.log 2>&1 || { tail -40 /tmp/cxx/gcc-make.log; exit 1; }
make install-gcc > /dev/null
fi
# The OpenVMS-host configuration of GCC builds its own `ld` driver (vms-ld.c),
# which writes a VMS LINK options file and spawns the DCL LINK command; collect2
# finds it first in libexec. Here the linker IS OVMX LINK.EXE through ovmx-ld,
# so the libexec `ld` points at it.
# GCC's VMS configuration names the driver's version directory the VMS way
# (14_2_0) while the target libraries install under the dotted version: make the
# driver's directory the same one so it finds crt0.o/crtbegin.o/libgcc.a.
VERDIR=$(basename "$(dirname "$("$X/bin/$TARGET-gcc" -print-prog-name=cc1)")")
if [ "$VERDIR" != "$GCC_VER" ] && [ ! -e "$X/lib/gcc/$TARGET/$VERDIR" ]; then
    mkdir -p "$X/lib/gcc/$TARGET/$GCC_VER"
    ln -s "$GCC_VER" "$X/lib/gcc/$TARGET/$VERDIR"
fi
LIBEXEC_LD=$("$X/bin/$TARGET-gcc" -print-prog-name=ld)
case "$LIBEXEC_LD" in
    "$X"/libexec/*) ln -sf "$X/$TARGET/bin/ld" "$LIBEXEC_LD" ;;
esac

echo "== [5/6] libgcc: compiler runtime + the EH unwinder + the port's crt0/crtbegin/crtend =="
# System V `ar` containers (the vms-alpha LBR ar output is not what LINK.EXE
# reads), built WITHOUT function sections (vms-5f9: the alpha-dec-vms `as`
# mis-classifies per-function sections).
LGFLAGS="-g0 -O2 -mpointer-size=64 -fno-function-sections -fno-data-sections"
LGDIR=$(dirname "$("$X/bin/$TARGET-gcc" -print-libgcc-file-name)")   # (a VMS-style version dir, e.g. 14_2_0)
if [ ! -f "$LGDIR/libgcc.a" ]; then
make all-target-libgcc -j"$JOBS" CFLAGS_FOR_TARGET="$LGFLAGS" \
    AR_FOR_TARGET=ar AR_FLAGS=rcS RANLIB_FOR_TARGET=true > /tmp/cxx/libgcc.log 2>&1 \
    || { tail -40 /tmp/cxx/libgcc.log; exit 1; }
make install-target-libgcc RANLIB_FOR_TARGET=true > /dev/null
fi
for f in libgcc.a crt0.o crtbegin.o crtend.o vms-dwarf2eh.o; do
    [ -f "$LGDIR/$f" ] || { echo "FAIL: libgcc did not install $f"; exit 1; }
done
ar t "$LGDIR/libgcc.a" | grep -q '^unwind-dw2.o$' || { echo "FAIL: libgcc.a has no EH unwinder"; exit 1; }
cd /tmp/cxx

echo "== [6/6] libstdc++-v3 over the OVMX C RTL (configure link tests via OVMX LINK.EXE) =="
CXXF="-mpointer-size=64 -fno-function-sections -fno-data-sections"
mkdir -p lsc && cd lsc
CC="$X/bin/$TARGET-gcc $CXXF" CXX="$X/bin/$TARGET-g++ $CXXF" \
AR=ar AR_FLAGS=crS RANLIB=true \
"../gcc-$GCC_VER/libstdc++-v3/configure" --host=$TARGET --build=x86_64-pc-linux-gnu \
    --prefix="$X" --with-cross-host=x86_64-pc-linux-gnu \
    --disable-shared --disable-nls --disable-libstdcxx-pch --disable-multilib \
    --with-gxx-include-dir="$X/$TARGET/include/c++/$GCC_VER" \
    > /tmp/cxx/lsc-configure.log 2>&1 || { tail -40 /tmp/cxx/lsc-configure.log; exit 1; }
# libstdc++ adds -ffunction-sections/-fdata-sections itself (SECTION_FLAGS);
# strip them (vms-5f9, as above).
grep -rl -e '-ffunction-sections -fdata-sections' --include=Makefile . | xargs -r sed -i 's/-ffunction-sections -fdata-sections//g'
make -C include > /tmp/cxx/lsc-make.log 2>&1
# src/c++11/debug.cc casts a pointer to std::size_t for a hash bucket; under the
# DEC C data model size_t is 32-bit (vms-537), so that cast needs -fpermissive.
make -C src/c++11 debug.lo CXXFLAGS="-g -O2 -fpermissive" >> /tmp/cxx/lsc-make.log 2>&1
make -j"$JOBS" >> /tmp/cxx/lsc-make.log 2>&1 || { grep -E 'error' /tmp/cxx/lsc-make.log | head -20; exit 1; }
make install > /dev/null
[ -f "$X/$TARGET/lib/libstdc++.a" ] || { echo "FAIL: libstdc++.a not installed"; exit 1; }
echo "== C/C++ toolchain ready in $X =="
"$X/bin/$TARGET-g++" --version | head -1
