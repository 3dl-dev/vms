# ============================================================================
# DEAD LEGACY — NOT AN OVMX RUNTIME TARGET.
#
# Operator ruling 2026-07-28 (CLAUDE.md Project-Specific Rule 9): the Docker
# RUNTIME layer is dead. OVMX has exactly one runtime: the real-kernel / QEMU
# path, where vms.ko provides the VMS executive via /dev/vms.
#
# This image is glibc, has NO /dev/vms, and bypasses IMGACT.EXE + LINK.EXE. It
# is retained ONLY because CI still depends on it, and is pending removal once
# those jobs migrate to the musl/QEMU path.
#
# DO NOT: add features here, document this as a way to "run OVMX", add new CI
# jobs on it, or design executive behavior around its lack of /dev/vms (never
# add a silent per-process fallback -- fail honestly with SS$_NOSUCHDEV).
#
# Build/test tooling in Docker is fine and expected: distro/Dockerfile.bootable,
# src/kernel/Dockerfile, tests/qemu/Dockerfile all produce/test the REAL runtime.
# ============================================================================
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake gcc make libc6-dev libreadline-dev libssh-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/ \
    -DBUILD_TOOLS=ON \
    && cmake --build build --parallel $(nproc) 2>&1 \
    && DESTDIR=/install cmake --install build 2>&1

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libreadline8t64 libssh-4 \
    && rm -rf /var/lib/apt/lists/*

# Install OVMX: executables → SYSEXE, libraries → SYSLIB
COPY --from=builder /install/ /
COPY --from=builder /src/distro/rootfs/ /

# Provide short-name symlinks so CI can use --entrypoint vmsdcl
RUN ln -s /vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE /usr/local/bin/vmsdcl

EXPOSE 22

ENTRYPOINT ["/vms/SYS0/SYSCOMMON/SYSEXE/STARTUP.EXE"]
