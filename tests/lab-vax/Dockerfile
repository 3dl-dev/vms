# lab-vax -- containerized NetBSD/vax-on-SIMH proving ground (rd vms-0041, epic
# vms-8e8 "OVMX/NetBSD SYSKRNL").
#
# This is the SIMH sibling of tests/qemu/Dockerfile. The Linux executive tests
# boot a Linux kernel under qemu-system-* and exercise /dev/vms; there is no
# qemu-system-vax, so the NetBSD/vax path runs under SIMH -- the same emulator
# the OpenVMS VAX labs use (tests/lab/, tests/lab-alpha/). See
# docs/design-ovmx-netbsd-syskrnl.md sec 6.
#
# BUILD/TEST TOOLING, NOT A RUNTIME. Per the generalized Rule 9
# (docs/runtime-target.md): OVMX has one runtime MODEL -- a real host kernel
# exposing /dev/vms. This image builds and drives an emulator to PROVE that
# model on VAX; it is never itself a runtime. Nothing is installed on the host
# (Rule: ALL deps containerized) -- SIMH is open-source and built here; anita
# and pexpect are pip-installed into a venv here; NetBSD is downloaded here.
#
#   REUSE, not reinvention:
#     * SIMH is built from open-simh at the SAME pinned commit tests/lab uses
#       (SIMH_REF below), so the NetBSD/vax lab and the OpenVMS/vax lab run the
#       identical emulator source. Only the VAX target is built.
#     * NO DEC ROM IS PROVISIONED. The SIMH MicroVAX 3900 (KA655) model carries
#       its boot ROM compiled into the simulator -- tests/lab/entrypoint.sh's
#       vax.ini loads no external ka655x.bin, and neither does anita's. We reuse
#       that verbatim: there is no DEC-copyrighted ROM binary to handle here.
#     * sysinst-over-serial (the fragile part flagged in the design doc sec 6) is
#       driven by NetBSD's own anita ("Automated NetBSD Installation and Test
#       Application", ISC-licensed, gson1703/anita), which the NetBSD project
#       uses for exactly this on the vax/simh combination. Pinned by tag+commit.
#
# Build (from a machine with docker):
#   docker build -f tests/lab-vax/Dockerfile -t ovmx-vax-lab tests/lab-vax
# Run: see tests/lab-vax/run-local.sh and entrypoint.sh.

ARG SIMH_REF=2e0d51e9e97b2c3a28c55261108550d1f533916b

# --- SIMH VAX builder ---------------------------------------------------------
FROM ubuntu:24.04 AS simh-builder
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
# Only the VAX target -- the machine is a MicroVAX 3900 (KA655), same model as
# the OpenVMS labs. SIMH has no --version switch; record the pinned commit as
# the provenance string (entrypoint.sh prints it at lab start).
RUN make vax -j"$(nproc)" \
 && install -D BIN/vax /out/vax \
 && echo "open-simh ${SIMH_REF} ($(uname -m))" > /out/simh-version.txt

# --- runtime image ------------------------------------------------------------
FROM ubuntu:24.04

# anita pin. Tag v2_18 == commit 79c0a3f1789... on gson1703/anita. Pinning BOTH
# (tag for readability, commit for immutability) is the same discipline SIMH_REF
# gets: a lab result must name exactly which install driver produced it.
ARG ANITA_REF=79c0a3f178918b0a12216685b35734e038207198

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-venv python3-pip git ca-certificates curl \
        coreutils genisoimage \
        libpcap0.8 libpcre3 libedit2 zlib1g \
    && rm -rf /var/lib/apt/lists/*

# anita + pexpect in an isolated venv (PEP 668: don't touch the system python).
# anita's setup.py declares no dependencies, so pexpect (which anita.py imports
# unconditionally) must be installed explicitly. Pinned for reproducibility.
ARG PEXPECT_VERSION=4.9.0
RUN python3 -m venv /opt/anita \
 && /opt/anita/bin/pip install --no-cache-dir \
        "pexpect==${PEXPECT_VERSION}" \
        "anita @ git+https://github.com/gson1703/anita@${ANITA_REF}" \
 && /opt/anita/bin/anita --help >/dev/null \
 && echo "anita ${ANITA_REF} (pexpect ${PEXPECT_VERSION})" > /etc/anita-version.txt
ENV PATH="/opt/anita/bin:${PATH}"

COPY --from=simh-builder /out/vax /usr/local/bin/vax
COPY --from=simh-builder /out/simh-version.txt /etc/simh-version.txt
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/vax

# anita invokes the emulator as bare `vax` (its non-NetBSD-host codepath), found
# on PATH. Confirm the wiring at build time so a broken image fails here, loud.
RUN command -v vax && vax --version >/dev/null 2>&1 || true

# The disk cache + downloaded ISO live under /cache, meant to be a mounted
# volume so the slow install is paid once. See entrypoint.sh.
ENV CACHE=/cache
VOLUME ["/cache"]

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["smoke"]
