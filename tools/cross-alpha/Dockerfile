# Containerized Alpha (alpha-linux-gnu) cross-build + emulation env.
# Rule 9 / containerize-all-deps: BUILD/TEST tooling, never a runtime.
# Feasibility probe for OVMX-on-Alpha (rd vms-054).
FROM debian:trixie
RUN n=0; until [ "$n" -ge 3 ]; do \
    apt-get update -o Acquire::Retries=5 -o Acquire::http::Timeout=25 -o Acquire::https::Timeout=25 -qq && DEBIAN_FRONTEND=noninteractive apt-get install -o Acquire::Retries=5 -o Acquire::http::Timeout=25 -o Acquire::https::Timeout=25 -y --no-install-recommends \
      gcc-alpha-linux-gnu binutils-alpha-linux-gnu libc6-dev-alpha-cross \
      qemu-user qemu-system-misc qemu-utils \
      build-essential cmake make file ca-certificates \
      genisoimage \
      bc bison flex libssl-dev libelf-dev cpio xz-utils kmod \
      patch \
      wget curl \
    && break; \
    n=$((n + 1)); echo "apt-get update+install failed (attempt $n/3); retrying after backoff" >&2; sleep $((n * 5)); \
done; [ "$n" -lt 3 ] && \
    rm -rf /var/lib/apt/lists/*
# alpha-linux-gnu-gcc + alpha headers; qemu-alpha (user) + qemu-system-alpha (full ES40);
# kernel build deps for a minimal Linux/Alpha kernel.
WORKDIR /src
