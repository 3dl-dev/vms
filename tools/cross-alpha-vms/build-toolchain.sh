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

# ---- libgcc: the compiler runtime (soft-float / long-double / integer
#      division / __gcc_main_flags-adjacent builtins the compiled port code
#      references). `--without-headers` builds it in inhibit_libc mode — a static
#      libgcc.a with no C-library dependency. Needed by BOTH Alpha C-RTL-archive
#      options (it is the compiler runtime, orthogonal to the C library the
#      DECC$SHR whole-archives). See docs/design-alpha-crtl-archive.md. ----
# The alpha-dec-vms libgcc has ONE VMS-specific TU — vms-gcc_shell_handler.c,
# GCC's VMS condition-handling EXCEPTION-UNWIND shell handler — which needs VMS
# headers (vms/chfdef.h, vms/pdscdef.h, vms/ssdef.h) this headerless
# compiler-runtime build does not provide (OVMX has chfdef.h + ssdef.h but no
# pdscdef.h yet). That handler is NOT referenced by the arithmetic / soft-float /
# long-double / division builtins the DECC$SHR whole-archives — it is only GCC's
# VMS EH/unwind path. Exclude it here so the genuine compiler-runtime builtins
# build; wiring the VMS EH handler (with OVMX's VMS headers + a real pdscdef.h) is
# a labeled follow-on, needed only once the port uses VMS condition handling / C++
# EH. Honest scope, not a stub. (vms-da2c; see docs/design-alpha-crtl-archive.md)
sed -i '/vms-gcc_shell_handler\.c/d' "/src/gcc-${GCC_VER}/libgcc/config/alpha/t-vms"
# LIB2ADDEH= empties libgcc's exception-handling / DWARF-unwind sources
# (unwind-dw2 &c.), which pull in libc headers (stdlib.h via md-unwind-support.h)
# this headerless compiler-runtime build doesn't have. That is the documented
# GCC mechanism for an EH-less libgcc — the ARITHMETIC / soft-float / division
# builtins (what the DECC$SHR whole-archives) don't need EH. EH/unwind is the
# same labeled follow-on as the VMS shell handler (needs the C-RTL headers).
make all-target-libgcc LIB2ADDEH= LIB2ADDEHSTATIC= LIB2ADDEHSHARED= -j"${JOBS}"
make install-target-libgcc LIB2ADDEH= LIB2ADDEHSTATIC= LIB2ADDEHSHARED=

# ---- smoke test: the compiler emits genuine VMS/Alpha asm ----
echo 'int main(void){ return 0; }' > /tmp/t.c
"${PREFIX}/bin/${TARGET}-gcc" -S -mpointer-size=64 /tmp/t.c -o /tmp/t.s || \
  "${PREFIX}/libexec/gcc/${TARGET}/${GCC_VER}/cc1" /tmp/t.c -o /tmp/t.s -quiet -mpointer-size=64
grep -q "__gcc_main_flags = 3" /tmp/t.s \
  && grep -qE "\.ent|\.pdesc" /tmp/t.s \
  && echo "SMOKE OK: emits __gcc_main_flags + VMS procedure descriptors"

# ---- confirm libgcc.a was produced (the compiler runtime archive) ----
LIBGCC_A=$("${PREFIX}/bin/${TARGET}-gcc" -print-libgcc-file-name 2>/dev/null || true)
if [ -f "$LIBGCC_A" ]; then
    echo "LIBGCC OK: $LIBGCC_A ($(wc -c < "$LIBGCC_A") bytes, $("${PREFIX}/bin/${TARGET}-nm" "$LIBGCC_A" 2>/dev/null | grep -c ' T ') text syms)"
    "${PREFIX}/bin/${TARGET}-nm" "$LIBGCC_A" 2>/dev/null | grep -E ' T (__addtf3|__divdi3|__floatditf|__fixtfsi)' | head
else
    echo "LIBGCC MISSING (-print-libgcc-file-name -> '$LIBGCC_A')" >&2
    exit 1
fi

echo "=== alpha-dec-vms cross toolchain built under ${PREFIX} ==="
"${PREFIX}/bin/${TARGET}-gcc" -dumpmachine
