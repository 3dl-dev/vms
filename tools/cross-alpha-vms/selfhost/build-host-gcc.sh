#!/bin/bash
# build-host-gcc.sh -- GCC 14.2 for alpha-dec-vms BUILT FOR the alpha-dec-vms
# host (a Canadian cross: build=x86_64-linux, host=target=alpha-dec-vms), so its
# programs (xgcc.exe, cpp.exe, cc1.exe) are OpenVMS Alpha images that run on
# OVMX (vms-fd1 / vms-9a63). The same GCC source and port patches the stage-1
# and stage-2 toolchains use; GMP/MPFR/MPC are built for the host first. Runs
# INSIDE the ovmx-cross-alpha-vms toolchain image:
#
#   docker run -v <repo>:/src:ro -v <cxxtc>:/out:ro -v <joint>:/joint:ro \
#       -v <work>:/w ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/selfhost/build-host-gcc.sh
#
# <cxxtc> is a cxx/build-cxx-toolchain.sh output, mounted at /out as it was built
# (its specs and ld wrapper name /out/cxx; its cxx/ is the stage-2 C/C++
# compiler + the 32-bit libstdc++ in cxx/p32); <joint> the shareables the
# programs link against (DECC$SHR, LIBOTS_SHR, LIBVMS*$SHR, STARLET.a). The
# host GCC is built at the DEC C default pointer size (32-bit), as DEC C and
# the port's host configuration assume. Output: /w/host-gcc/{cc1,xgcc,cpp}.exe
# and /w/host-gcc/build.log.
set -euo pipefail
T=alpha-dec-vms; B=x86_64-pc-linux-gnu
X=/out/cxx
export PATH=$X/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
JOBS=${JOBS:-$(nproc)}
P32="-nostdinc++ -isystem $X/p32/include/c++/14_2_0 -isystem $X/p32/include/c++/14_2_0/$T -L$X/p32/lib"
O=/w/host-gcc; mkdir -p $O/src
exec > >(tee $O/build.log) 2>&1
cd $O/src
fetch() { [ -f "$1" ] || wget -q --tries=3 --timeout=60 "$2/$1"; }
fetch gmp-6.3.0.tar.xz https://ftp.gnu.org/gnu/gmp
fetch mpfr-4.2.1.tar.xz https://ftp.gnu.org/gnu/mpfr
fetch mpc-1.3.1.tar.gz https://ftp.gnu.org/gnu/mpc
echo "a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898  gmp-6.3.0.tar.xz" | sha256sum -c -
echo "277807353a6726978996945af13e52829e3abd7a9a5b7fb2793894e18f1fcbb2  mpfr-4.2.1.tar.xz" | sha256sum -c -
echo "ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8  mpc-1.3.1.tar.gz" | sha256sum -c -
for t in gmp-6.3.0.tar.xz mpfr-4.2.1.tar.xz mpc-1.3.1.tar.gz; do d=${t%.tar.*}; [ -d $d ] || tar xf $t; done
if [ ! -d gcc-14.2.0 ]; then
    tar xf /src/tools/cross-alpha-vms/gcc-14.2.0.tar.xz
    for p in /src/tools/cross-alpha-vms/patches/*.patch; do
        grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' "$p" && continue
        patch -s -p1 -d gcc-14.2.0 < "$p"
    done
fi
H=$O/host
lib() {   # lib <name> <srcdir> <configure args...>
    local n=$1 s=$2; shift 2
    [ -f "$H/lib/lib$n.a" ] && return 0
    echo "== $n (host $T)"
    mkdir -p $O/b-$n && cd $O/b-$n
    ../src/$s/configure --host=$T --build=$B --prefix=$H --disable-shared --enable-static \
        CC=$T-gcc AR=ar RANLIB=true "$@" > cfg.log 2>&1 || { tail -30 cfg.log; exit 1; }
    make -j"$JOBS" > make.log 2>&1 || { grep -E 'error|LINK-' make.log | head -30; exit 1; }
    make install > inst.log 2>&1
}
lib gmp gmp-6.3.0 --disable-assembly
lib mpfr mpfr-4.2.1 --with-gmp=$H
lib mpc mpc-1.3.1 --with-gmp=$H --with-mpfr=$H
echo "== gcc (build=$B host=$T target=$T, all-gcc)"
mkdir -p $O/b-gcc && cd $O/b-gcc
export ax_cv_cxx_compile_cxx11_FOR_BUILD=yes CONFIG_SITE=/src/tools/cross-alpha-vms/cxx/config.site
[ -f Makefile ] || ../src/gcc-14.2.0/configure --build=$B --host=$T --target=$T --prefix=/gnu \
    --enable-languages=c --disable-nls --disable-bootstrap --disable-shared --disable-lto \
    --disable-plugin --disable-fixincludes \
    --with-gmp=$H --with-mpfr=$H --with-mpc=$H --without-isl --without-zstd --with-gnu-as \
    CC="$T-gcc" CXX="$T-g++ $P32" AR=ar RANLIB=true CFLAGS="-O2" CXXFLAGS="-O2" \
    > cfg.log 2>&1 || { tail -30 cfg.log; exit 1; }
# configure-host serially: the sub-configures share config.cache
make -j1 configure-host > configure-host.log 2>&1 || { tail -30 configure-host.log; exit 1; }
make -j"$JOBS" all-gcc > make.log 2>&1 || { grep -nE ' error:|LINK-[FE]' make.log | sort -t: -k3 -u | head -40; exit 1; }
for e in cc1 xgcc cpp; do cp gcc/$e.exe $O/$e.exe; done
ls -la $O/*.exe
echo "HOST-GCC: OK"
