#!/bin/bash
exec > /w/fd1-probe.out 2>&1
# vms-fd1 probe: canadian cross -- GCC host=alpha-dec-vms target=alpha-dec-vms,
# built by the stage-2 toolchain (/out/cxx) over the OVMX C RTL (/joint).
# Records where it stops (the vms-3e4 gap list). Output under /w/fd1.
set -u
export PATH=/out/cxx/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
T=alpha-dec-vms; B=x86_64-pc-linux-gnu
O=/w/fd1; mkdir -p $O/src $O/host; cd $O/src
fetch() { [ -f "$1" ] || wget -q --tries=3 "$2/$1" || { echo "FETCH FAIL $1"; exit 1; }; sha256sum "$1"; }
fetch gmp-6.3.0.tar.xz https://ftp.gnu.org/gnu/gmp
fetch mpfr-4.2.1.tar.xz https://ftp.gnu.org/gnu/mpfr
fetch mpc-1.3.1.tar.gz https://ftp.gnu.org/gnu/mpc
for t in gmp-6.3.0.tar.xz mpfr-4.2.1.tar.xz mpc-1.3.1.tar.gz; do d=${t%.tar.*}; [ -d $d ] || tar xf $t; done
H=$O/host
step() { echo "== $1"; }
if [ ! -f $H/lib/libgmp.a ]; then step gmp
  mkdir -p $O/b-gmp && cd $O/b-gmp && ../src/gmp-6.3.0/configure --host=$T --build=$B --prefix=$H --disable-shared --enable-static --disable-assembly CC="$T-gcc -mpointer-size=64" AR=ar RANLIB=true > cfg.log 2>&1 && make -j$(nproc) > make.log 2>&1 && make install > inst.log 2>&1 || { echo GMP_FAIL; tail -30 cfg.log make.log 2>/dev/null | tail -40; exit 2; }
fi
if [ ! -f $H/lib/libmpfr.a ]; then step mpfr
  mkdir -p $O/b-mpfr && cd $O/b-mpfr && ../src/mpfr-4.2.1/configure --host=$T --build=$B --prefix=$H --disable-shared --with-gmp=$H CC="$T-gcc -mpointer-size=64" AR=ar RANLIB=true > cfg.log 2>&1 && make -j$(nproc) > make.log 2>&1 && make install > inst.log 2>&1 || { echo MPFR_FAIL; tail -40 cfg.log make.log 2>/dev/null | tail -40; exit 3; }
fi
if [ ! -f $H/lib/libmpc.a ]; then step mpc
  mkdir -p $O/b-mpc && cd $O/b-mpc && ../src/mpc-1.3.1/configure --host=$T --build=$B --prefix=$H --disable-shared --with-gmp=$H --with-mpfr=$H CC="$T-gcc -mpointer-size=64" AR=ar RANLIB=true > cfg.log 2>&1 && make -j$(nproc) > make.log 2>&1 && make install > inst.log 2>&1 || { echo MPC_FAIL; tail -40 cfg.log make.log 2>/dev/null | tail -40; exit 4; }
fi
step gcc-host
cd $O/src; [ -d gcc-14.2.0 ] || { tar xf /src/tools/cross-alpha-vms/gcc-14.2.0.tar.xz; for p in /src/tools/cross-alpha-vms/patches/*.patch; do patch -p1 -d gcc-14.2.0 < $p > /dev/null; done; }
mkdir -p $O/b-gcc && cd $O/b-gcc
[ -f Makefile ] || ../src/gcc-14.2.0/configure --build=$B --host=$T --target=$T --prefix=/gnu \
   --enable-languages=c --disable-nls --disable-bootstrap --disable-shared --disable-lto --disable-plugin \
   --with-gmp=$H --with-mpfr=$H --with-mpc=$H --without-isl --without-zstd --with-gnu-as \
   ax_cv_cxx_compile_cxx11_FOR_BUILD=yes CC="$T-gcc -mpointer-size=64" AR=ar RANLIB=true CXX="$T-g++ -mpointer-size=64" > cfg.log 2>&1 || { echo GCCCFG_FAIL; tail -40 cfg.log; exit 5; }
make all-gcc -j$(nproc) > make.log 2>&1; echo "MAKE_RC=$?"
ls -la gcc/cc1.exe gcc/cc1 gcc/xgcc.exe 2>/dev/null
grep -nE 'error|LINK-F|Error' make.log | head -40
