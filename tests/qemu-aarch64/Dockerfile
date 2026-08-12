# Dockerfile - OVMX aarch64 system-emulation boot proof (rd vms-f66).
#
# Builds a self-contained image that boots a REAL arm64 Linux kernel under
# system-mode qemu-system-aarch64 and runs a genuine OVMX freestanding
# EM_AARCH64 artifact as PID 1. All dependencies (cross toolchain, arm64
# kernel, qemu-system-aarch64) are contained -- no host tools, no host install.
#
# This is BUILD/TEST tooling, not an OVMX runtime (CLAUDE.md Rule 9): booting a
# kernel in QEMU to *test* the freestanding layer is exactly what tests/qemu/
# (Linux vms.ko) and tests/netbsd/ (NetBSD/amd64) already do. The Rule-9 gate
# (tests/integration/test_runtime_target.sh) inspects only src/** C/H, so
# nothing here trips it.
#
#   docker build -f tests/qemu-aarch64/Dockerfile -t ovmx-aarch64-boot .
#   docker run --rm ovmx-aarch64-boot
#
# The build host is amd64; the arm64 kernel is EXTRACTED (not installed) from a
# Debian arm64 package and the payload is CROSS-compiled with
# aarch64-linux-gnu-gcc. The x86_64 negative-control /init is native-compiled
# with the host gcc. QEMU runs the arm64 guest under TCG (no KVM needed).
FROM --platform=linux/amd64 debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
        qemu-system-arm \
        gcc gcc-aarch64-linux-gnu \
        binutils binutils-aarch64-linux-gnu \
        cpio gzip xz-utils file dpkg-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Real arm64 Linux kernel Image (extracted from the Debian arm64 package).
RUN sh tests/qemu-aarch64/fetch_kernel.sh /boot/vmlinuz-arm64

# Positive payload: EM_AARCH64 OVMX freestanding /init (cross toolchain).
RUN sh tests/qemu-aarch64/build_init.sh aarch64 /initramfs-aarch64.cpio.gz

# Negative control: EM_X86_64 build of the SAME /init (native host gcc). The
# arm64 kernel must refuse to exec it -- proving the positive boot is genuinely
# aarch64, not a silently-x86_64 emulation.
RUN sh tests/qemu-aarch64/build_init.sh x86_64 /initramfs-x86-negctl.cpio.gz

ENTRYPOINT ["bash", "/src/tests/qemu-aarch64/run_tests.sh"]
