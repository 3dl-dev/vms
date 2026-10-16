#!/bin/bash
# build-host-as.sh -- GNU as (binutils 2.43 gas) for alpha-dec-vms BUILT FOR the
# alpha-dec-vms host (a Canadian cross: build=x86_64-linux, host=target=
# alpha-dec-vms), so as.exe is an OpenVMS Alpha image that runs on OVMX
# (vms-5303e). It is the assembler the GCC driver runs after cc1: the same
# binutils source and port patches the cross toolchain's `as` is built from,
# unmodified for the host. Sibling of build-host-gcc.sh; runs INSIDE the
# ovmx-cross-alpha-vms toolchain image:
#
#   docker run --network none -v <repo>:/src:ro -v <cxxtc>:/out:ro -v <joint>:/joint:ro \
#       -v <work>:/w ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/selfhost/build-host-as.sh
#
# <cxxtc> is a cxx/build-cxx-toolchain.sh output (the stage-2 C compiler over
# the build's C RTL); <joint> the shareables the program links against. Like
# the host GCC, gas is built at the DEC C default pointer size (32-bit).
# Output: /w/host-as/as.exe; progress and any failure go to stdout.
set -euo pipefail
T=alpha-dec-vms; B=x86_64-pc-linux-gnu
X=/out/cxx
export PATH=$X/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
JOBS=${JOBS:-$(nproc)}
BV=2.43
O=/w/host-as; mkdir -p $O
trap 'echo "build-host-as: FAIL at line $LINENO: $BASH_COMMAND"' ERR
cd $O
if [ ! -d binutils-$BV ]; then
    echo "${BINUTILS_SHA256:-b53606f443ac8f01d1d5fc9c39497f2af322d99e14cea5c0b4b124d630379365}  /src/tools/cross-alpha-vms/binutils-$BV.tar.xz" | sha256sum -c -
    tar xf /src/tools/cross-alpha-vms/binutils-$BV.tar.xz
    # the alpha-dec-vms port patches that touch binutils, exactly as the cross build applies them
    for p in /src/tools/cross-alpha-vms/patches/*.patch; do
        grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' "$p" || continue
        echo "== applying binutils port patch: $(basename "$p") =="
        patch -s -p1 -d binutils-$BV < "$p"
    done
fi
echo "== gas (build=$B host=$T target=$T)"
mkdir -p $O/b && cd $O/b
export CONFIG_SITE=/src/tools/cross-alpha-vms/cxx/config.site
[ -f Makefile ] || ../binutils-$BV/configure --build=$B --host=$T --target=$T --prefix=/gnu \
    --disable-nls --disable-werror --disable-shared --enable-static --disable-gprofng \
    --disable-plugins --disable-gdb --disable-ld --disable-binutils --disable-gold \
    --disable-sim --disable-readline --without-zstd --with-system-zlib=no \
    CC="$T-gcc" AR=ar RANLIB=true CFLAGS="-O2" \
    > cfg.log 2>&1 || { tail -40 cfg.log; exit 1; }
make -j"$JOBS" all-gas > make.log 2>&1 || { grep -nE ' error:|LINK-[FE]|Error ' make.log | head -40; tail -20 make.log; exit 1; }
mkdir -p $O/out
cp gas/as-new.exe $O/out/as.exe 2>/dev/null || cp gas/as-new $O/out/as.exe
ls -la $O/out/as.exe
echo "HOST-AS: OK"
