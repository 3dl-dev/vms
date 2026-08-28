# Containerized alpha-dec-vms (OpenVMS/Alpha, VMS ABI) cross toolchain:
# GNU binutils + GCC cc1. BUILD / ORACLE TOOLING ONLY.
#
# Rule-9-clean: this image is NOT an OVMX runtime target. The compiler runs on
# the build host and emits genuine alpha-dec-vms EVAX objects so OVMX's LINK.EXE
# can be gap-probed against REAL GCC-port output. It never runs in the OVMX
# guest and has no /dev/vms dependency. (The OVMX Alpha *runtime* lane is
# OVMX/Linux-Alpha — the Linux ABI on Alpha — which is a different target.)
#
# Build:   docker build -t ovmx-cross-alpha-vms tools/cross-alpha-vms
# Extract: tools/cross-alpha-vms/build.sh   (runs this + copies cc1/as/... out)
FROM debian:trixie

ARG BINUTILS_VER=2.43
ARG GCC_VER=14.2.0
ARG TARGET=alpha-dec-vms
ARG PREFIX=/opt/cross-alpha-vms
ENV BINUTILS_VER=${BINUTILS_VER} GCC_VER=${GCC_VER} TARGET=${TARGET} PREFIX=${PREFIX}

RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential libgmp-dev libmpfr-dev libmpc-dev libisl-dev \
      flex bison texinfo gawk zlib1g-dev file wget xz-utils ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /src
# vms-f97: checked-in alpha-dec-vms port patches, applied to the fetched GCC
# source by build-toolchain.sh (we patch, never vendor the whole tree).
COPY patches/ /src/patches/
# vms-8a06: VENDORED binutils tarball (SHA256-pinned in build-toolchain.sh), COPY'd
# to /src (the script's CWD) so the offline/hermetic path takes it instead of one
# ftp.gnu.org hiccup reddening the gate. gcc is NOT vendored (90MB) — hardened fetch.
COPY binutils-2.43.tar.xz /src/binutils-2.43.tar.xz
COPY build-toolchain.sh /src/build-toolchain.sh
RUN bash /src/build-toolchain.sh && rm -rf /src/binutils-* /src/gcc-* /src/build-*

# The toolchain lives under ${PREFIX}; `${TARGET}-gcc -S` emits VMS/Alpha asm.
