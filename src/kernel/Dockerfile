# Dockerfile for building vms.ko kernel module
#
# Usage:
#   docker build -t vms-kmod -f src/kernel/Dockerfile .
#   docker run --rm vms-kmod cat /build/vms.ko > vms.ko
#
# The module is built against Ubuntu 24.04's stock kernel headers.
# To use it, boot a VM running the matching kernel version.

FROM ubuntu:24.04 AS kmod-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc kmod \
    linux-headers-generic \
    && rm -rf /var/lib/apt/lists/*

# Find the installed kernel headers directory
# (linux-headers-generic pulls in a version-specific package)
RUN KVER=$(ls /lib/modules/ | head -1) && \
    echo "Building against kernel: $KVER" && \
    echo "$KVER" > /tmp/kver

# src/kernel-core carries the substrate-agnostic executive core: the shim
# contracts (exec_kbackend.h, exec_list.h) AND the relocated facility sources
# (kernel-core/vms_eflag.c, rd vms-ec4). The Makefile references
# ../kernel-core/vms_eflag.o and adds -I$(src)/../kernel-core, so kernel-core
# must sit BESIDE /src/kernel as /src/kernel-core, and the build runs in
# /src/kernel. (This mirrors tests/qemu/Dockerfile's layout.)
COPY src/kernel-core/ /src/kernel-core/
COPY src/kernel/ /src/kernel/
WORKDIR /src/kernel

# Build the module
RUN KVER=$(cat /tmp/kver) && \
    make KDIR=/lib/modules/${KVER}/build 2>&1

# Verify
RUN KVER=$(cat /tmp/kver) && \
    ls -la /src/kernel/vms.ko && \
    modinfo /src/kernel/vms.ko

# Copy to a clean output location
RUN mkdir -p /build && cp /src/kernel/vms.ko /build/
