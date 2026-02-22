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

EXPOSE 22

ENTRYPOINT ["/vms/SYS0/SYSCOMMON/SYSEXE/STARTUP.EXE"]
