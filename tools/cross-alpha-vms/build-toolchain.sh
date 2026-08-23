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

# ---- apply checked-in alpha-dec-vms port patches (vms-f97) ----
# Minimal codegen-consistency fixes to the FETCHED GCC source (we patch, never
# vendor the whole tree). Each patch is a plain `patch -p1` unified diff under
# tools/cross-alpha-vms/patches/, COPYed into /src/patches by the Dockerfile.
if [ -d /src/patches ]; then
  for p in /src/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "== applying port patch: $(basename "$p") =="
    patch -p1 -d "/src/gcc-${GCC_VER}" < "$p"
  done
fi

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

# ---- libgcc.a: the compiler runtime (vms-7b96, RUNG-1) --------------------
# The alpha-dec-vms C-RTL shareable (DECC$SHR) whole-archives libgcc.a alongside
# musl's libc.a (mk_decc_shr.sh) — the compiler support routines musl references
# internally. Build it here so the image carries it.
#
# -g0 (no .vmsdebug): UNLIKE libc.a, libgcc carries no genuine debug value we
# need to preserve, and building it -g0 means no DST -> GNU `ar`/`ranlib` archive
# it normally (the vms-7b96 DST reader gap only bites objects that carry DST).
# -mpointer-size=64 matches musl's LP64/P64 objects so the two archives share one
# ABI. --without-headers configured the tree, so libgcc builds in inhibit_libc
# mode (the soft-float / integer / __clear_cache helpers, no libc-dependent bits).
make all-target-libgcc -j"${JOBS}" \
    CFLAGS_FOR_TARGET='-g0 -O2 -mpointer-size=64' \
    || { echo "== all-target-libgcc failed; retry single-threaded for a clean error =="; \
         make all-target-libgcc CFLAGS_FOR_TARGET='-g0 -O2 -mpointer-size=64'; }
make install-target-libgcc
# Surface libgcc.a at a stable, easy-to-extract path.
LIBGCC_A="$("${PREFIX}/bin/${TARGET}-gcc" -mpointer-size=64 -print-libgcc-file-name 2>/dev/null || true)"
if [ -f "$LIBGCC_A" ]; then
  cp -v "$LIBGCC_A" "${PREFIX}/lib/libgcc.a"
  echo "== libgcc.a at ${PREFIX}/lib/libgcc.a (from $LIBGCC_A) =="
else
  echo "== WARNING: could not locate libgcc.a via -print-libgcc-file-name =="
  find "${PREFIX}" -name 'libgcc.a' -exec cp -v {} "${PREFIX}/lib/libgcc.a" \; || true
fi

# ---- smoke test: the compiler emits genuine VMS/Alpha asm ----
echo 'int main(void){ return 0; }' > /tmp/t.c
"${PREFIX}/bin/${TARGET}-gcc" -S -mpointer-size=64 /tmp/t.c -o /tmp/t.s || \
  "${PREFIX}/libexec/gcc/${TARGET}/${GCC_VER}/cc1" /tmp/t.c -o /tmp/t.s -quiet -mpointer-size=64
grep -q "__gcc_main_flags = 3" /tmp/t.s \
  && grep -qE "\.ent|\.pdesc" /tmp/t.s \
  && echo "SMOKE OK: emits __gcc_main_flags + VMS procedure descriptors"

echo "=== alpha-dec-vms cross toolchain built under ${PREFIX} ==="
"${PREFIX}/bin/${TARGET}-gcc" -dumpmachine
