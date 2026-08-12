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
# The deterministic console driver (rd vms-2d9), imported by ALL three harnesses
# (drive_netbsd.py, drive_netbsd_p2b.py, drive_netbsd_p2c.py). It sits in /netbsd
# next to the drivers, so `import netbsd_console' resolves via the script dir.
COPY tests/netbsd/netbsd_console.py  /netbsd/netbsd_console.py

# P2b (rd vms-bfe) + P2c (rd vms-4b4): the OVMX/NetBSD `vms' pseudo-device, the
# transport seam leaf, and the userspace probes -- staged for the in-guest build.
# The drivers bundle /netbsd/guest-src into an ISO, attach it to the boot, and
# build inside the NetBSD guest. Selected at run time by overriding the
# entrypoint (/netbsd/run_p2b.sh or /netbsd/run_p2c.sh) on the same image.
#
# THE MODULE NOW COMPILES THE SHARED EXECUTIVE CORE. Since P2c, the `vms' module
# builds src/kernel-core/vms_eflag.c (the SAME event-flag facility source the
# Linux vms.ko builds) with the NetBSD backend. So the guest source tree carries
# THREE dirs: kmod/ (the NetBSD backend glue + headers), kernel-core/ (the shared
# facility + shim contracts), and probe/ (the userspace tools + transport). The
# Makefile in kmod/ reaches ../kernel-core for the shared source. P2b builds the
# same module (it just exercises only PING); both drivers stage kernel-core.
#
# --- kmod/: the NetBSD backend glue + its headers ---
COPY src/kernel-netbsd/vms_netbsd.c          /netbsd/guest-src/kmod/vms_netbsd.c
COPY src/kernel-netbsd/Makefile              /netbsd/guest-src/kmod/Makefile
COPY src/kernel-netbsd/vms_ping.h            /netbsd/guest-src/kmod/vms_ping.h
COPY src/kernel-netbsd/vms_eflag_nb.h        /netbsd/guest-src/kmod/vms_eflag_nb.h
COPY src/kernel-netbsd/vms_internal.h        /netbsd/guest-src/kmod/vms_internal.h
COPY src/kernel-netbsd/exec_kbackend_netbsd.h /netbsd/guest-src/kmod/exec_kbackend_netbsd.h
COPY src/kernel-netbsd/exec_list_netbsd.h    /netbsd/guest-src/kmod/exec_list_netbsd.h
COPY src/kernel-netbsd/exec_list_netbsd.c    /netbsd/guest-src/kmod/exec_list_netbsd.c
# --- kernel-core/: the shared, substrate-agnostic facility + shim contracts ---
COPY src/kernel-core/vms_eflag.c             /netbsd/guest-src/kernel-core/vms_eflag.c
COPY src/kernel-core/exec_kbackend.h         /netbsd/guest-src/kernel-core/exec_kbackend.h
COPY src/kernel-core/exec_list.h             /netbsd/guest-src/kernel-core/exec_list.h
# --- probe/: the userspace tools + the NetBSD transport seam ---
COPY tests/netbsd/guest/vmsprobe.c           /netbsd/guest-src/probe/vmsprobe.c
COPY tests/netbsd/guest/vmseflag.c           /netbsd/guest-src/probe/vmseflag.c
COPY src/libvmssys/kif_transport_netbsd.c    /netbsd/guest-src/probe/kif_transport_netbsd.c
COPY src/libvmssys/kif_transport.h           /netbsd/guest-src/probe/kif_transport.h
COPY src/kernel-netbsd/vms_ping.h            /netbsd/guest-src/probe/vms_ping.h
COPY src/kernel-netbsd/vms_eflag_nb.h        /netbsd/guest-src/probe/vms_eflag_nb.h
COPY tests/netbsd/drive_netbsd_p2b.py        /netbsd/drive_netbsd_p2b.py
COPY tests/netbsd/run_p2b.sh                 /netbsd/run_p2b.sh
COPY tests/netbsd/drive_netbsd_p2c.py        /netbsd/drive_netbsd_p2c.py
COPY tests/netbsd/run_p2c.sh                 /netbsd/run_p2c.sh

RUN chmod +x /netbsd/run_tests.sh /netbsd/drive_netbsd.py \
             /netbsd/run_p2b.sh /netbsd/drive_netbsd_p2b.py \
             /netbsd/run_p2c.sh /netbsd/drive_netbsd_p2c.py

# The installed NetBSD image + download mirror live under /cache so they persist
# across runs when a cache volume is mounted there (the cold-install cost is
# paid once). Nothing here bakes the multi-hundred-MB image into a layer.
VOLUME ["/cache"]

ENTRYPOINT ["/netbsd/run_tests.sh"]
