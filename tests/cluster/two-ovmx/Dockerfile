# Two-OVMX-node SCS harness (rd vms-f3e).
#
# Stage 1 builds SCSD.EXE (+ its LIBVMS*$SHR.EXE shared libs) from the repo at
# the current tip. Stage 2 is a thin runtime carrying the daemon, the harness
# scripts, and just enough net tooling (iproute2 + tcpdump + python3) to stand
# up br0 + veth and capture the wire. Build context is the REPO ROOT so stage 1
# can see the whole source tree:
#
#   docker build -f tests/cluster/two-ovmx/Dockerfile -t ovmx-two-node .
#
# This is BUILD/TEST tooling (CLAUDE.md Rule 9): it produces + observes the real
# SCSD userspace daemon. It is not an OVMX runtime.

# ---- stage 1: build SCSD.EXE from source -----------------------------------
FROM --platform=linux/amd64 ubuntu:24.04 AS builder
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    cmake gcc g++ make ca-certificates && \
    rm -rf /var/lib/apt/lists/*
COPY . /src
RUN cmake -S /src -B /build -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
        -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /build --target scsd_exe -j"$(nproc)" && \
    test -x /build/bin/SCSD.EXE

# ---- stage 2: runtime harness ----------------------------------------------
FROM --platform=linux/amd64 ubuntu:24.04 AS harness
RUN apt-get update -qq && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    iproute2 tcpdump python3 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=builder /build/bin/SCSD.EXE       /harness/bin/SCSD.EXE
COPY --from=builder /build/lib/LIBVMS*.EXE    /harness/lib/
COPY tests/cluster/two-ovmx/mk_sysgen_scratch.py /harness/mk_sysgen_scratch.py
COPY tests/cluster/two-ovmx/entrypoint.sh     /harness/entrypoint.sh
COPY tests/cluster/two-ovmx/verdict.sh        /harness/verdict.sh
RUN chmod +x /harness/entrypoint.sh /harness/verdict.sh
ENTRYPOINT ["/harness/entrypoint.sh"]
