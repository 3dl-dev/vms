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
# In addition to SIMH (the genuine VAX cluster), this image now also carries an
# x86_64 system emulator (qemu-system-x86). The booted-cluster-join harness
# (tests/lab/tools/labjoin_pod_boot.sh, rd vms-bd3/vms-fa1a) boots ONE booted-OVMX
# node -- the shipped distro/Dockerfile.bootable runtime -- under QEMU/TCG INSIDE
# this pod, its virtio NIC bridged to the pod's br0 tap, so it reaches the VAX
# cluster's L2 (the pod's own netns). Without qemu-system-x86_64 in the pod that
# harness FATALs honestly ("qemu-system-x86_64 not found in the pod"). The boot
# artifacts themselves (vmlinuz / initramfs / ovmx-distrib.img) are NOT baked in;
# labjoin_booted.sh stages them at runtime via `kubectl cp` into /lab/run-<tag>/
# and md5-verifies them in-pod. This image only supplies the emulator.
#
# Build + push to the k3s registry (from a machine with docker). BUMP THE TAG on
# every rebuild and update tests/lab/k8s/20-vaxlab.yaml to match (IfNotPresent):
#   docker build -t 192.168.2.43:30500/ovmx-vaxlab:4 tests/lab
#   docker push  192.168.2.43:30500/ovmx-vaxlab:4

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
# SIMH has no --version switch; record the pinned commit as the provenance
# string instead (entrypoint.sh prints it at lab start).
RUN make vax -j"$(nproc)" \
 && install -D BIN/vax /out/vax \
 && echo "open-simh ${SIMH_REF} ($(uname -m))" > /out/simh-version.txt

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 iproute2 bridge-utils iputils-ping tcpdump libpcap0.8 \
        libpcre3 libedit2 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# x86_64 system emulator for the booted-OVMX cluster-join node (vms-bd3). The
# `qemu-system-x86` package (not the arch-fat `qemu-system`) keeps this lean: it
# provides /usr/bin/qemu-system-x86_64 plus SeaBIOS firmware -- all the harness
# needs. Its invocation contract (labjoin_pod_boot.sh, mirroring
# distro/boot/run-qemu.sh OVMX_NET_MODE=tap): default `pc`/i440fx machine,
# `-accel tcg` (no /dev/kvm in the pod), `-serial stdio`, a virtio-blk disk
# (if=virtio) and a tap-backed virtio-net-pci NIC
# (-netdev tap,...,script=no,downscript=no -device virtio-net-pci,...,romfile=).
# All of that is stock in qemu-system-x86; tap needs only the already-present
# iproute2 + the pod's /dev/net/tun mount + CAP_NET_ADMIN. Version is not
# apt-pinned (a hard apt version pin breaks the build when the Ubuntu archive
# rotates point releases, and nothing else in this image apt-pins either -- the
# base ubuntu:24.04 digest is the reproducibility anchor); the exact installed
# version is recorded to /etc/qemu-version.txt for provenance, mirroring
# /etc/simh-version.txt.
RUN apt-get update && apt-get install -y --no-install-recommends \
        qemu-system-x86 \
    && rm -rf /var/lib/apt/lists/* \
    && qemu-system-x86_64 --version | head -1 > /etc/qemu-version.txt \
    && cat /etc/qemu-version.txt
COPY --from=builder /out/vax /usr/local/bin/vax
COPY --from=builder /out/simh-version.txt /etc/simh-version.txt
COPY nodedrv.py /usr/local/bin/nodedrv.py
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/nodedrv.py /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
