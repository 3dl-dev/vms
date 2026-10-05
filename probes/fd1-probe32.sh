#!/bin/bash
exec > /w/fd1-p32.out 2>&1
# vms-fd1 probe, 32-bit host (DEC C default pointer size). Probe-only shim:
# /w/probe32-shim.h (st_fab_rfm) -- NOT a deliverable.
set -u
export PATH=/out/cxx/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
T=alpha-dec-vms; B=x86_64-pc-linux-gnu
P32="-nostdinc++ -isystem /out/cxx/p32/include/c++/14_2_0 -isystem /out/cxx/p32/include/c++/14_2_0/alpha-dec-vms -L/out/cxx/p32/lib"
O=/w/fd1; mkdir -p $O/src $O/host32; cd $O/src
fetch() { [ -f "$1" ] || wget -q --tries=3 "$2/$1" || { echo "FETCH FAIL $1"; exit 1; }; }
fetch gmp-6.3.0.tar.xz https://ftp.gnu.org/gnu/gmp
fetch mpfr-4.2.1.tar.xz https://ftp.gnu.org/gnu/mpfr
fetch mpc-1.3.1.tar.gz https://ftp.gnu.org/gnu/mpc
for t in gmp-6.3.0.tar.xz mpfr-4.2.1.tar.xz mpc-1.3.1.tar.gz; do d=${t%.tar.*}; [ -d $d ] || tar xf $t; done
[ -d gcc-14.2.0 ] || { tar xf /src/tools/cross-alpha-vms/gcc-14.2.0.tar.xz; for p in /src/tools/cross-alpha-vms/patches/*.patch; do grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' $p && continue; patch -p1 -d gcc-14.2.0 < $p > /dev/null; done; }
H=$O/host32
step() { echo "== $1"; }
if [ ! -f $H/lib/libgmp.a ]; then step gmp
  rm -rf $O/b32-gmp; mkdir -p $O/b32-gmp && cd $O/b32-gmp && ../src/gmp-6.3.0/configure --host=$T --build=$B --prefix=$H --disable-shared --enable-static --disable-assembly CC=$T-gcc AR=ar RANLIB=true > cfg.log 2>&1 && make -j8 > make.log 2>&1 && make install > inst.log 2>&1 || { echo GMP_FAIL; grep -E 'error|LINK-F' cfg.log make.log | head -20; exit 2; }
fi
if [ ! -f $H/lib/libmpfr.a ]; then step mpfr
  mkdir -p $O/b32-mpfr && cd $O/b32-mpfr && ../src/mpfr-4.2.1/configure --host=$T --build=$B --prefix=$H --disable-shared --with-gmp=$H CC=$T-gcc AR=ar RANLIB=true > cfg.log 2>&1 && make -j8 > make.log 2>&1 && make install > inst.log 2>&1 || { echo MPFR_FAIL; grep -E 'error|LINK-F' cfg.log make.log | head -20; exit 3; }
fi
if [ ! -f $H/lib/libmpc.a ]; then step mpc
  mkdir -p $O/b32-mpc && cd $O/b32-mpc && ../src/mpc-1.3.1/configure --host=$T --build=$B --prefix=$H --disable-shared --with-gmp=$H --with-mpfr=$H CC=$T-gcc AR=ar RANLIB=true > cfg.log 2>&1 && make -j8 > make.log 2>&1 && make install > inst.log 2>&1 || { echo MPC_FAIL; grep -E 'error|LINK-F' cfg.log make.log | head -20; exit 4; }
fi
step gcc-host32
mkdir -p $O/b32-gcc && cd $O/b32-gcc
export ax_cv_cxx_compile_cxx11_FOR_BUILD=yes
[ -f Makefile ] || ../src/gcc-14.2.0/configure --build=$B --host=$T --target=$T --prefix=/gnu \
   --enable-languages=c --disable-nls --disable-bootstrap --disable-shared --disable-lto --disable-plugin --disable-fixincludes \
   --with-gmp=$H --with-mpfr=$H --with-mpc=$H --without-isl --without-zstd --with-gnu-as \
   CC="$T-gcc" CXX="$T-g++ $P32" AR=ar RANLIB=true \
   CFLAGS="-O2 -include /w/probe32-shim.h" CXXFLAGS="-O2 -include /w/probe32-shim.h" > cfg.log 2>&1 || { echo GCCCFG_FAIL; tail -30 cfg.log; exit 5; }
make -k all-gcc -j8 > make.log 2>&1; echo "MAKE_RC=$?"
ls -la gcc/cc1.exe gcc/xgcc.exe 2>/dev/null
grep -nE ' error:|LINK-F|undefined symbol' make.log | sort -t: -k3 -u | head -60
