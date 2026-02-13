FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake gcc make libc6-dev libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TOOLS=ON \
    && cmake --build build --parallel $(nproc) 2>&1

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libreadline8t64 openssh-server \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bin/ /usr/local/bin/
COPY --from=builder /src/build/lib/ /usr/local/lib/
COPY --from=builder /src/distro/rootfs/ /

RUN ldconfig && mkdir -p /vms/sys\$system /vms/sys\$library /vms/sys\$manager \
    /vms/sys\$login /vms/sys\$help /tmp/ovmx/locks \
    /home/DEFAULT /home/GUEST /home/USER1 /home/USER2

# Create Linux users from sysuaf.dat for SSH access (PAM authenticates against sysuaf.dat)
RUN grep -v '^#' /etc/ovmx/sysuaf.dat | grep -v '^$' | while IFS=: read -r uname rest; do \
        lower=$(echo "$uname" | tr 'A-Z' 'a-z'); \
        id "$lower" >/dev/null 2>&1 || useradd -m -s /bin/sh "$lower"; \
    done \
    && ssh-keygen -A \
    && mkdir -p /run/sshd \
    && cp /etc/ssh/sshd_config.ovmx /etc/ssh/sshd_config

EXPOSE 22

ENTRYPOINT ["ovmx_init"]
