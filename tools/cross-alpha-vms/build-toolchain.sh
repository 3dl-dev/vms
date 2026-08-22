#!/bin/bash
# build-toolchain.sh — build the alpha-dec-vms (OpenVMS/Alpha, VMS ABI) cross
# toolchain: GNU binutils (as/ld/objdump for EVAX objects) + GCC cc1 (the
# compiler proper). Runs INSIDE the tools/cross-alpha-vms Dockerfile container.
#
# WHY THIS EXISTS (build/oracle tooling, Rule-9-clean): this compiler emits
# genuine alpha-dec-vms EVAX objects, so OVMX's LINK.EXE can be gap-probed
# against REAL GCC-port output instead of reasoning to (sometimes false) gaps.
# It NEVER runs inside the OVMX guest — it is the oracle-side cross compiler on
# the build host. Note the OVMX Alpha *runtime* lane is OVMX/Linux-Alpha (the
# Linux ABI on Alpha), a DIFFERENT target; this is the VMS ABI the GCC port
# compiles for, which no Linux distro packages — hence a from-source build.
#
# cc1-only: `make all-gcc` builds the compiler proper (emits .s); no target
# libc/headers are needed to produce assembly, so this stays small and fast.
set -euxo pipefail

BINUTILS_VER=${BINUTILS_VER:-2.43}
GCC_VER=${GCC_VER:-14.2.0}
TARGET=${TARGET:-alpha-dec-vms}
PREFIX=${PREFIX:-/opt/cross-alpha-vms}
JOBS=$(nproc)

cd /src

# ---- binutils: target assembler/linker/objdump for EVAX ----
wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
tar xf "binutils-${BINUTILS_VER}.tar.xz"
mkdir -p build-binutils && cd build-binutils
"/src/binutils-${BINUTILS_VER}/configure" \
    --target="${TARGET}" --prefix="${PREFIX}" --disable-nls --disable-werror
make -j"${JOBS}"
make install
export PATH="${PREFIX}/bin:${PATH}"
cd /src

# ---- gcc cc1 (compiler proper) ----
wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
tar xf "gcc-${GCC_VER}.tar.xz"
mkdir -p build-gcc && cd build-gcc
"/src/gcc-${GCC_VER}/configure" \
    --target="${TARGET}" --prefix="${PREFIX}" \
    --enable-languages=c --disable-bootstrap --disable-multilib \
    --disable-libssp --disable-shared --disable-nls \
    --disable-fixincludes \
    --without-headers --with-gnu-as --with-gnu-ld
# WRINKLE 1 (--disable-fixincludes above): a cross `make all-gcc` otherwise tries
# to build stmp-fixinc, which needs the BUILD-side fixincludes/fixinc.sh that a
# cross build never produces -> "No rule to make target .../fixinc.sh".
#
# WRINKLE 2 (mkdir below): all-gcc generates per-frontend target-hooks headers
# for EVERY frontend (d/, rust/, ...) even with --enable-languages=c, but only
# the enabled languages' build subdirs exist, so the genhooks `mv` fails
# ("cannot move tmp-d-target-hooks-def.h to d/"). Pre-create the subdirs.
mkdir -p gcc/{c,cp,c-family,common,objc,d,rust,go,fortran,ada,lto,jit,m2,analyzer}
make all-gcc -j"${JOBS}"
make install-gcc

# ---- smoke test: the compiler emits genuine VMS/Alpha asm ----
echo 'int main(void){ return 0; }' > /tmp/t.c
"${PREFIX}/bin/${TARGET}-gcc" -S -mpointer-size=64 /tmp/t.c -o /tmp/t.s || \
  "${PREFIX}/libexec/gcc/${TARGET}/${GCC_VER}/cc1" /tmp/t.c -o /tmp/t.s -quiet -mpointer-size=64
grep -q "__gcc_main_flags = 3" /tmp/t.s \
  && grep -qE "\.ent|\.pdesc" /tmp/t.s \
  && echo "SMOKE OK: emits __gcc_main_flags + VMS procedure descriptors"

echo "=== alpha-dec-vms cross toolchain built under ${PREFIX} ==="
"${PREFIX}/bin/${TARGET}-gcc" -dumpmachine
