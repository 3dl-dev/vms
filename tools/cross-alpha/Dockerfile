# Containerized Alpha (alpha-linux-gnu) cross-build + emulation env.
# Rule 9 / containerize-all-deps: BUILD/TEST tooling, never a runtime.
# Feasibility probe for OVMX-on-Alpha (rd vms-054).
FROM debian:trixie
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-alpha-linux-gnu binutils-alpha-linux-gnu libc6-dev-alpha-cross \
      qemu-user qemu-system-misc qemu-utils \
      build-essential cmake make file ca-certificates \
      genisoimage \
      bc bison flex libssl-dev libelf-dev cpio xz-utils kmod \
      wget curl \
    && rm -rf /var/lib/apt/lists/*
# alpha-linux-gnu-gcc + alpha headers; qemu-alpha (user) + qemu-system-alpha (full ES40);
# kernel build deps for a minimal Linux/Alpha kernel.
WORKDIR /src
