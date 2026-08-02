# SIMH VAX reference-lab node image (vms-a5c).
#
# Builds the open-simh VAX emulator for the pod's own architecture. This is NOT
# the binary lab-1 uses: lab-1's ./vax is an aarch64 build carried over from the
# old Surface dev seat, and workshop/k3s-worker are x86_64, so a rebuild is
# unavoidable. SIMH_REF is pinned to the exact commit lab-1's source tree sits
# on (/data/training/vax/build/simh @ 2e0d51e, 2025-04-05) so the two labs are
# the same emulator source built for two architectures.
#
#   FIDELITY CAVEAT: same source, different target arch and toolchain. Before
#   trusting a lab-2 result that CONTRADICTS a lab-1 result, reproduce it on
#   lab-1. Agreeing results need no such check.
#
# Build + push to the k3s registry (from a machine with docker):
#   docker build -t 192.168.2.43:30500/ovmx-vaxlab:1 tests/lab
#   docker push  192.168.2.43:30500/ovmx-vaxlab:1

ARG SIMH_REF=2e0d51e9e97b2c3a28c55261108550d1f533916b

FROM ubuntu:24.04 AS builder
ARG SIMH_REF
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git ca-certificates \
        libpcap-dev libpcre3-dev libedit-dev zlib1g-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git init . \
 && git remote add origin https://github.com/open-simh/simh \
 && git fetch --depth 1 origin "${SIMH_REF}" \
 && git checkout FETCH_HEAD
# Only the VAX target — the lab is a MicroVAX 3900 (KA655) cluster.
RUN make vax -j"$(nproc)" \
 && install -D BIN/vax /out/vax \
 && /out/vax --version > /out/simh-version.txt 2>&1 || true

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 iproute2 bridge-utils iputils-ping tcpdump libpcap0.8 \
        libpcre3 libedit2 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /out/vax /usr/local/bin/vax
COPY --from=builder /out/simh-version.txt /etc/simh-version.txt
COPY nodedrv.py /usr/local/bin/nodedrv.py
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/nodedrv.py /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
