# AXPbox Alpha reference-lab node image (rd vms-e2c).
#
# The Alpha counterpart of tests/lab/Dockerfile. Same shape as lab-2 -- one pod
# is one complete, isolated lab -- but a much smaller image, because unlike SIMH
# the AXPbox project publishes tagged Linux amd64 release binaries built by its
# own CI, and k3s-worker is amd64. So there is no cross-arch rebuild to do.
#
#   PROVENANCE: the binaries record their own build. `axpbox` with no arguments
#   prints "AXPBox Alpha Emulator (commit <sha>)", and entrypoint.sh prints that
#   line at lab start, so every run states exactly which build produced it --
#   the same guarantee lab-2 gets from pinning SIMH_REF.
#
# TWO VERSIONS ARE SHIPPED ON PURPOSE. v1.2.0 backported large CPU changes from
# the ES40-Emu project (and dropped the cpu `icache` option the OpenVMS install
# guide tells you to set). OpenVMS 8.4 bugchecks INVEXCEPTN under it before it
# reaches the installer. v1.1.2 predates those backports. Which one actually
# boots VMS is an open question this image exists to answer, so both are here
# and AXPBOX_VERSION picks at runtime -- no rebuild to A/B them.
#
# Build + push to the k3s registry (from a machine with docker):
#   docker build -t 192.168.2.43:30500/ovmx-alphalab:1 tests/lab-alpha
#   docker push  192.168.2.43:30500/ovmx-alphalab:1

FROM ubuntu:24.04

# libsdl1.2debian is for v1.1.2 and earlier, which link against SDL 1.2.
# Installing it HERE is correct and is not a violation of "never install on the
# host" -- that rule is about workshop, and containerising the dependency is
# exactly what it asks for. v1.2.0 does not need it.
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 ca-certificates curl iproute2 tcpdump \
        libpcap0.8 libx11-6 libsdl1.2debian \
    && rm -rf /var/lib/apt/lists/*

ARG AXPBOX_V120_URL=https://github.com/lenticularis39/axpbox/releases/download/v1.2.0/AXPbox-linux-x86-gcc
ARG AXPBOX_V112_URL=https://github.com/lenticularis39/axpbox/releases/download/v1.1.2/axpbox-v1.1.2-linux-x86-gcc

RUN curl -fsSL -o /usr/local/bin/axpbox-1.2.0 "${AXPBOX_V120_URL}" \
 && curl -fsSL -o /usr/local/bin/axpbox-1.1.2 "${AXPBOX_V112_URL}" \
 && chmod +x /usr/local/bin/axpbox-1.2.0 /usr/local/bin/axpbox-1.1.2 \
 && /usr/local/bin/axpbox-1.2.0 2>&1 | head -1 >  /etc/axpbox-version.txt \
 && /usr/local/bin/axpbox-1.1.2 2>&1 | head -1 >> /etc/axpbox-version.txt \
 && cat /etc/axpbox-version.txt

COPY tools/srmdrv.py /usr/local/bin/srmdrv.py
COPY entrypoint.sh   /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/srmdrv.py /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
