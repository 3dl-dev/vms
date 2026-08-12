# Dockerfile for the OVMX/NetBSD amd64 harness (rd vms-7f7, epic vms-8e8).
#
# Carries qemu-system-x86_64 + anita + the NetBSD serial-console tooling, so the
# whole "boot NetBSD/amd64 and assert uname" proof runs inside one container --
# nothing is installed on the host. This is the NetBSD-substrate sibling of
# tests/qemu/Dockerfile (which does the same for the Linux vms.ko executive).
#
# This is BUILD/TEST TOOLING, not a runtime (CLAUDE.md Rule 9 /
# docs/runtime-target.md): it boots a real NetBSD guest in QEMU to TEST it,
# exactly as tests/qemu/ boots a real Linux guest. NetBSD-as-a-real-runtime is
# the product goal of epic vms-8e8; a QEMU harness that boots it for testing is
# tooling.
#
# Usage:
#   docker build -f tests/netbsd/Dockerfile -t ovmx-netbsd-ktest .
#   docker run --rm -v "$PWD/.netbsd-cache:/cache" \
#              --device /dev/kvm ovmx-netbsd-ktest      # --device optional
#
# Exit 0 = NetBSD booted and `uname -srm` asserted; nonzero = failure/timeout.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# qemu-system-x86  : the amd64 emulator anita drives
# qemu-utils       : qemu-img (disk image handling)
# python3/pip      : anita is a Python program
# python3-pexpect  : anita "screen scrapes" the serial console via pexpect
# genisoimage      : anita builds the install-sets ISO with mkisofs/genisoimage
# curl/ca-certs    : anita downloads the pinned release from cdn.netbsd.org
# gzip, cpio       : anita unpacks release media
RUN apt-get update && apt-get install -y --no-install-recommends \
        qemu-system-x86 \
        qemu-utils \
        python3 \
        python3-pip \
        python3-pexpect \
        genisoimage \
        curl \
        ca-certificates \
        gzip \
        cpio \
    && rm -rf /var/lib/apt/lists/*

# anita is pinned (see tests/netbsd/netbsd_version.env). It is NOT the unrelated
# PyPI package named "anita"; the real tool ships as a tarball from gson.org, so
# we download that, verify its SHA256, and pip-install it. Ubuntu 24.04 marks the
# system Python environment externally-managed; this is a throwaway test image
# with no other Python software to disturb, so installing into it is fine.
ARG ANITA_VERSION=2.18
ARG ANITA_SHA256=fa5ac3a8b3e30a3a35df9254bbee213b28210a396544592c3e604a1e53a294ce
RUN curl -sSL -o /tmp/anita.tar.gz \
        "https://www.gson.org/netbsd/anita/download/anita-${ANITA_VERSION}.tar.gz" \
    && echo "${ANITA_SHA256}  /tmp/anita.tar.gz" | sha256sum -c - \
    && pip3 install --no-cache-dir --break-system-packages /tmp/anita.tar.gz \
    && rm -f /tmp/anita.tar.gz \
    && anita --version

COPY tests/netbsd/netbsd_version.env /netbsd/netbsd_version.env
COPY tests/netbsd/drive_netbsd.py    /netbsd/drive_netbsd.py
COPY tests/netbsd/run_tests.sh       /netbsd/run_tests.sh
RUN chmod +x /netbsd/run_tests.sh /netbsd/drive_netbsd.py

# The installed NetBSD image + download mirror live under /cache so they persist
# across runs when a cache volume is mounted there (the cold-install cost is
# paid once). Nothing here bakes the multi-hundred-MB image into a layer.
VOLUME ["/cache"]

ENTRYPOINT ["/netbsd/run_tests.sh"]
