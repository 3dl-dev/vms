#!/bin/bash
# build-host-binutils.sh -- GNU as for alpha-dec-vms (binutils 2.43, the
# assembler the alpha-dec-vms GCC port drives) BUILT FOR the alpha-dec-vms
# host, so as.exe is an OpenVMS Alpha image that runs on OVMX (vms-8e1). The
# same binutils source and port patches the cross toolchain uses. Runs INSIDE
# the ovmx-cross-alpha-vms toolchain image:
#
#   docker run -v <repo>:/src:ro -v <cxxtc>:/out:ro -v <joint>:/joint:ro \
#       -v <work>:/w ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/selfhost/build-host-binutils.sh
#
# <cxxtc>/<joint> as for build-host-gcc.sh. Output: /w/host-binutils/as.exe;
# progress and any failure go to stdout.
set -euo pipefail
T=alpha-dec-vms; B=x86_64-pc-linux-gnu
V=2.43
SHA=b53606f443ac8f01d1d5fc9c39497f2af322d99e14cea5c0b4b124d630379365
X=/out/cxx
export PATH=$X/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
JOBS=${JOBS:-$(nproc)}
O=/w/host-binutils; mkdir -p $O
trap 'echo "build-host-binutils: FAIL at line $LINENO: $BASH_COMMAND"' ERR
cd $O
echo "$SHA  /src/tools/cross-alpha-vms/binutils-$V.tar.xz" | sha256sum -c -
if [ ! -d binutils-$V ]; then
    tar xf /src/tools/cross-alpha-vms/binutils-$V.tar.xz
    for p in /src/tools/cross-alpha-vms/patches/*.patch; do
        grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' "$p" || continue
        patch -s -p1 -d binutils-$V < "$p"
    done
fi
echo "== binutils $V (build=$B host=$T target=$T, all-gas)"
mkdir -p b && cd b
export CONFIG_SITE=/src/tools/cross-alpha-vms/cxx/config.site
[ -f Makefile ] || ../binutils-$V/configure --build=$B --host=$T --target=$T --prefix=/gnu \
    --disable-nls --disable-werror --disable-shared --disable-plugins --disable-gdb \
    --disable-gprofng --disable-sim --without-zstd --without-system-zlib \
    CC="$T-gcc" AR=ar RANLIB=true CFLAGS="-O2" > cfg.log 2>&1 || { tail -30 cfg.log; exit 1; }
make -j"$JOBS" all-gas > make.log 2>&1 || { grep -nE ' error:|LINK-[FE]' make.log | sort -t: -k3 -u | head -40; tail -20 make.log; exit 1; }
cp gas/as-new.exe $O/as.exe 2>/dev/null || cp gas/as-new $O/as.exe
ls -la $O/as.exe
echo "HOST-BINUTILS: OK"
