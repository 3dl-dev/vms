# Containerized vax--netbsdelf GCC cross toolchain for the OVMX/NetBSD SYSKRNL.
#
# Rule 9 / containerize-all-deps: this is BUILD/TEST TOOLING, never a runtime.
# It produces a `vax--netbsdelf` cross `gcc`/`ld` plus a NetBSD/vax sysroot
# (headers + libc + crt objects), so `libvmssys` can be cross-compiled for
# netbsd-vax entirely inside a container -- nothing installed on the host.
#
# Design: docs/design-ovmx-netbsd-syskrnl.md §4.1/§4.3 -- the VAX libvmssys
# backend LINKS NetBSD libc (it does NOT hand-write a VAX freestanding syscall
# ABI), so the toolchain must carry a real NetBSD/vax libc + headers, not just a
# bare compiler. VAX is 32-bit little-endian (ILP32): a NEW width class relative
# to every existing 64-bit OVMX target -- this toolchain is what the P3 ILP32 +
# endianness width audit (rd vms-9dc) is validated against.
#
# There is deliberately NO emulation here: QEMU has no VAX system target and this
# is a build-only gate. Booting NetBSD/vax under SIMH is a later phase (P4), not
# this image.
#
# Usage:
#   docker build -f tools/cross-vax/Dockerfile -t ovmx-cross-vax tools/cross-vax
#   docker run --rm -v "$PWD:/src" -w /src ovmx-cross-vax \
#       tools/cross-vax/build-ovmx-images-vax-cmake.sh
#
# Everything is pinned. Bump versions deliberately, together, with fresh hashes.

FROM debian:trixie-slim

ARG BINUTILS_VERSION=2.42
ARG GCC_VERSION=13.3.0
ARG NETBSD_VERSION=10.1
ARG TARGET=vax--netbsdelf

# The NetBSD/vax sets (headers + libc + crt objects) come off the NetBSD CDN and
# are the security-relevant download here, so they are SHA512-pinned from the
# CDN's own published manifest:
#   https://cdn.netbsd.org/pub/NetBSD/NetBSD-10.1/vax/binary/sets/SHA512
ARG NETBSD_BASE_SHA512=ebb03d22d947691609b7127562210dd746112eac8aa188685b353b494bb9017c9979bd9f46ce70b6f2406ad17e63af073572e41ec47d710214606db68a0e6789
ARG NETBSD_COMP_SHA512=34105e9eb2f644ee6ff26205d1069841b3996942537d8881b4cb0be94bbcc3b31e694da4c49f909efc0874d576c4b0a23aae34cd74698b64d0234022e958ae89

ENV PREFIX=/opt/cross
ENV SYSROOT=/opt/cross/sysroot
ENV PATH=/opt/cross/bin:$PATH
ENV DEBIAN_FRONTEND=noninteractive

# Build deps for binutils+gcc, plus cmake/make/file so libvmssys can be built in
# this same image. libgmp/mpfr/mpc-dev satisfy gcc's math libs (no in-tree build).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        wget ca-certificates \
        xz-utils \
        libgmp-dev libmpfr-dev libmpc-dev \
        texinfo bison flex \
        cmake make ninja-build file \
    && rm -rf /var/lib/apt/lists/*

# ------------------------------------------------------------------
# NetBSD/vax sysroot: headers + libc + crt objects (base + comp sets).
# Only the paths a cross toolchain needs are extracted, to keep the image lean.
# ------------------------------------------------------------------
RUN set -eux; \
    mkdir -p "$SYSROOT"; cd /tmp; \
    CDN="https://cdn.netbsd.org/pub/NetBSD/NetBSD-${NETBSD_VERSION}/vax/binary/sets"; \
    wget -q "$CDN/base.tgz" "$CDN/comp.tgz"; \
    echo "${NETBSD_BASE_SHA512}  base.tgz" | sha512sum -c -; \
    echo "${NETBSD_COMP_SHA512}  comp.tgz" | sha512sum -c -; \
    tar -C "$SYSROOT" -xzf base.tgz ./usr/include ./usr/lib ./lib || \
        tar -C "$SYSROOT" -xzf base.tgz ./usr/lib ./lib; \
    tar -C "$SYSROOT" -xzf comp.tgz ./usr/include ./usr/lib; \
    rm -f base.tgz comp.tgz; \
    test -f "$SYSROOT/usr/include/stdio.h"; \
    test -f "$SYSROOT/usr/lib/libc.a" || test -f "$SYSROOT/usr/lib/libc.so"

# ------------------------------------------------------------------
# binutils for vax--netbsdelf. Build tree removed in-layer so only the install
# (a few MB) persists.
# ------------------------------------------------------------------
RUN set -eux; cd /tmp; \
    wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"; \
    tar xf "binutils-${BINUTILS_VERSION}.tar.xz"; \
    mkdir build-binutils; cd build-binutils; \
    ../binutils-${BINUTILS_VERSION}/configure \
        --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
        --disable-nls --disable-werror --disable-gdb --disable-multilib; \
    make -j"$(nproc)"; make install; \
    cd /tmp; rm -rf build-binutils "binutils-${BINUTILS_VERSION}" "binutils-${BINUTILS_VERSION}.tar.xz"

# ------------------------------------------------------------------
# gcc (C only) + libgcc + libatomic for vax--netbsdelf, against the NetBSD
# sysroot. The heavy build tree is created and removed inside this one RUN so the
# committed layer is just the installed compiler; peak disk lives only during
# the RUN.
#
# libatomic is REQUIRED (was --disable-libatomic, vms-271): VAX is 32-bit ILP32
# with no lock-free 8-byte atomic instruction, so any `__atomic_*_n` on a
# 64-bit object -- e.g. the executive LNM arena's `uint64_t generation` seqlock
# read in libvmssys vms_kif.c (lnm_gen_load) -- lowers to a libatomic call
# (__atomic_load_8). Without libatomic those are undefined references at link
# time, which blocked the link proofs of every userspace library above
# libvmssys that touches the arena (vmslnm/vmsfs/vmsrms, and next vmsdcl). The
# arena `generation` is a kernel<->userspace ABI field and stays 64-bit;
# libatomic's lock-based 8-byte ops give a correct atomic read on VAX.
# ------------------------------------------------------------------
RUN set -eux; cd /tmp; \
    wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"; \
    tar xf "gcc-${GCC_VERSION}.tar.xz"; \
    mkdir build-gcc; cd build-gcc; \
    ../gcc-${GCC_VERSION}/configure \
        --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
        --enable-languages=c \
        --disable-nls --disable-werror --disable-multilib \
        --disable-libssp --disable-libquadmath --disable-libgomp \
        --enable-libatomic --disable-libvtv --disable-shared \
        --with-gnu-as --with-gnu-ld; \
    make -j"$(nproc)" all-gcc; \
    make -j"$(nproc)" all-target-libgcc; \
    make -j"$(nproc)" all-target-libatomic; \
    make install-gcc install-target-libgcc install-target-libatomic; \
    cd /tmp; rm -rf build-gcc "gcc-${GCC_VERSION}" "gcc-${GCC_VERSION}.tar.xz"; \
    "$PREFIX/bin/${TARGET}-gcc" --version

WORKDIR /src

# The four scripts formerly baked in here (build-libvmssys-vax.sh,
# build-vmsprocess-vax.sh, build-libvms-vax.sh, build-activation-vax.sh) were
# deleted by rd vms-08cb (epic vms-509 "unified cross-platform build" Rung E,
# docs/design-unified-cross-build.md §5/§6/§8): their per-job netbsd-vax CI
# gates were redundant with the ovmx-images aggregate (vax-cmake-images job)
# and every teeth/assertion they carried was migrated there. Every other
# tools/cross-vax/*.sh script (kept or retired) is invoked from the mounted
# source tree by its full relative path (`docker run ... ovmx-cross-vax
# tools/cross-vax/<script>.sh`), never baked into the image -- these four
# were the only exceptions, and with them gone nothing needs to be COPY'd
# into /usr/local/bin anymore.

CMD ["bash"]
