FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake gcc make libc6-dev libreadline-dev libssh-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TOOLS=ON \
    && cmake --build build --parallel $(nproc) 2>&1

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libreadline8t64 libssh-4 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bin/ /usr/local/bin/
COPY --from=builder /src/build/lib/ /usr/local/lib/
COPY --from=builder /src/distro/rootfs/ /

RUN ldconfig && mkdir -p /vms/sys\$system /vms/sys\$library /vms/sys\$manager \
    /vms/sys\$login /vms/sys\$help /tmp/ovmx/locks \
    /home/DEFAULT /home/GUEST /home/USER1 /home/USER2 \
    /etc/ovmx/lastlogin

# Populate SYS$SYSTEM with symlinks to installed binaries
RUN ln -sf /usr/local/bin/vms_login      /vms/sys\$system/LOGINOUT.EXE \
 && ln -sf /usr/local/bin/vmsdcl         /vms/sys\$system/DCL.EXE \
 && ln -sf /usr/local/bin/vms_help       /vms/sys\$system/HELP.EXE \
 ; ln -sf /usr/local/bin/vms_monitor     /vms/sys\$system/MONITOR.EXE  2>/dev/null \
 ; ln -sf /usr/local/bin/vms_mail        /vms/sys\$system/MAIL.EXE     2>/dev/null \
 ; ln -sf /usr/local/bin/vms_authorize   /vms/sys\$system/AUTHORIZE.EXE 2>/dev/null \
 ; ln -sf /usr/local/bin/vmssshd        /vms/sys\$system/VMSSSHD.EXE  2>/dev/null \
 ; true

# Populate SYS$LIBRARY with VMS header files
RUN cp /usr/local/include/starlet.h  /vms/sys\$library/ 2>/dev/null || true \
 && cp /usr/local/include/ssdef.h    /vms/sys\$library/ 2>/dev/null || true \
 && cp /usr/local/include/descrip.h  /vms/sys\$library/ 2>/dev/null || true \
 && cp /usr/local/include/rms.h      /vms/sys\$library/ 2>/dev/null || true \
 && cp /usr/local/include/libdef.h   /vms/sys\$library/ 2>/dev/null || true \
 && cp /usr/local/include/str\$routines.h /vms/sys\$library/ 2>/dev/null || true

# Create home directories for SYSUAF users
RUN grep -v '^#' /etc/ovmx/sysuaf.dat | grep -v '^$' | while IFS=: read -r uname rest; do \
        lower=$(echo "$uname" | tr 'A-Z' 'a-z'); \
        mkdir -p "/home/$lower"; \
    done

EXPOSE 22

ENTRYPOINT ["ovmx_init"]
