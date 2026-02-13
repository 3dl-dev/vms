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

WORKDIR /src
COPY src/kernel/ /src/

# Build the module
RUN KVER=$(cat /tmp/kver) && \
    make KDIR=/lib/modules/${KVER}/build 2>&1

# Verify
RUN KVER=$(cat /tmp/kver) && \
    ls -la /src/vms.ko && \
    modinfo /src/vms.ko

# Copy to a clean output location
RUN mkdir -p /build && cp /src/vms.ko /build/
