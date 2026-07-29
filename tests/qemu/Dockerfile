# Dockerfile for QEMU-based kernel module testing
#
# Builds vms.ko against Ubuntu's stock kernel, assembles an initramfs
# with statically-linked test programs, and boots QEMU to run tests
# on the serial console. Fully autonomous -- no manual VM setup.
#
# Usage:
#   docker build -f tests/qemu/Dockerfile -t ovmx-ktest .
#   docker run --rm ovmx-ktest
#
# Exit code 0 = all tests pass, 1 = failures

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# NEGATIVE_CONTROL=1 simulates the executive being absent: vms.ko is built
# normally (so the module and vmsfs.ko are proven buildable) but init.sh's
# insmod of vms.ko is skipped, so /dev/vms never appears and every test that
# depends on it fails honestly with "cannot open /dev/vms" (the real
# SS$_NOSUCHDEV path, not a fake). This build arg exists ONLY to prove the CI
# gate can actually go red — it is never set for the real barrier job.
ARG NEGATIVE_CONTROL=0

# Install: build tools, kernel packages, QEMU, busybox for initramfs.
# cmake + musl-tools + linux-libc-dev are for the userspace system-service
# layer (src/libvms, statically linked with musl -- see the OVMX_STATIC
# build below, vms-1d9): the raw-ioctl test_kmod_* programs only need
# gcc/libc6-dev, but exercising the PUBLIC sys$ API needs the real
# src/libvms build graph, which this job did not build at all before.
RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc libc6-dev kmod \
    linux-headers-generic \
    linux-image-generic \
    busybox-static \
    cpio \
    zstd \
    cmake musl-tools linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

# Install QEMU for the correct architecture
RUN apt-get update && \
    ARCH=$(dpkg --print-architecture) && \
    if [ "$ARCH" = "arm64" ]; then \
        apt-get install -y --no-install-recommends qemu-system-arm; \
    else \
        apt-get install -y --no-install-recommends qemu-system-x86; \
    fi && \
    rm -rf /var/lib/apt/lists/*

# Determine kernel version from installed headers
RUN KVER=$(ls /lib/modules/ | sort -V | tail -1) && \
    echo "$KVER" > /tmp/kver && \
    echo "Kernel version: $KVER"

# Copy kernel module source and build vms.ko
COPY src/kernel/ /src/kernel/
RUN KVER=$(cat /tmp/kver) && \
    cd /src/kernel && \
    make KDIR=/lib/modules/${KVER}/build 2>&1 && \
    echo "--- vms.ko info ---" && \
    ls -la /src/kernel/vms.ko && \
    modinfo /src/kernel/vms.ko

# Build vmsfs.ko (VMS filesystem kernel module)
RUN KVER=$(cat /tmp/kver) && \
    cd /src/kernel/vmsfs && \
    make KDIR=/lib/modules/${KVER}/build 2>&1 && \
    echo "--- vmsfs.ko info ---" && \
    ls -la /src/kernel/vmsfs/vmsfs.ko && \
    modinfo /src/kernel/vmsfs/vmsfs.ko

# Copy test source and build statically-linked test programs. Only
# test_kmod_* here -- those are raw-ioctl programs needing just
# src/kernel/vms_ioctl.h. test_syssvc_* (vms-1d9) needs the real libvms
# build graph instead and is built separately below, via cmake/musl, from
# the full source tree copied in that stage.
COPY tests/qemu/test_kmod_*.c /src/tests/qemu/
RUN for f in /src/tests/qemu/test_kmod_*.c; do \
        out="${f%.c}"; \
        gcc -static -O2 -Wall -o "$out" "$f" -I/src/kernel && \
        echo "Built: $(basename $out)"; \
    done

# --- Userspace system-service layer (vms-1d9) ---------------------------
# Everything above proves the KERNEL lock manager (raw ioctls). This stage
# builds the REAL src/libvms public sys$ API statically (musl, same
# OVMX_STATIC mode distro/Dockerfile.bootable already uses to produce the
# bootable distro) so tests/qemu/test_syssvc_lock.c can call sys$enq /
# sys$enqw / sys$deq -- not raw ioctls -- against a real /dev/vms. A static
# musl binary needs no separate C runtime staged into the initramfs: it
# carries its own libc, identically to how STARTUP.EXE/DCL.EXE already
# boot with no dynamic linker present. This is the initramfs's ONLY
# consumer of the OVMX build graph; the raw-ioctl test_kmod_* programs
# above stay plain "gcc -static" against vms_ioctl.h, unchanged.
#
# musl doesn't ship linux/ or asm/ UAPI headers -- symlink from
# linux-libc-dev (same trick as distro/Dockerfile.bootable).
RUN MUSL_INC=$(echo /usr/include/*-linux-musl) && \
    ARCH_INC=$(echo /usr/include/*-linux-gnu) && \
    ln -s /usr/include/linux "$MUSL_INC/linux" && \
    ln -s /usr/include/asm-generic "$MUSL_INC/asm-generic" && \
    ln -s "$ARCH_INC/asm" "$MUSL_INC/asm"

# Full source tree needed: the OVMX CMake graph resolves libvms's
# dependency chain (vmsprocess, vmsfs, vmslnm, vmssys) itself -- see
# CMakeLists.txt's "Subdirectories (order matters for dependencies)".
COPY . /src/repo/
RUN cd /src/repo && \
    cmake -B build-static \
        -DCMAKE_C_COMPILER=musl-gcc \
        -DCMAKE_BUILD_TYPE=Release \
        -DOVMX_STATIC=ON \
        -DBUILD_TESTS=ON \
        -DBUILD_TOOLS=OFF \
    && cmake --build build-static --target test_syssvc_lock --parallel $(nproc) 2>&1 && \
    echo "--- test_syssvc_lock (public sys\$ API, static musl) ---" && \
    ls -la build-static/bin/test_syssvc_lock

# Build mkimage_vmsfs (host tool) and create test image
COPY tests/qemu/mkimage_vmsfs.c /src/tests/qemu/
RUN gcc -O2 -Wall -o /src/tests/qemu/mkimage_vmsfs \
        /src/tests/qemu/mkimage_vmsfs.c \
        -I/src/kernel/vmsfs && \
    /src/tests/qemu/mkimage_vmsfs /vmsfs_test.img

# Locate the kernel image (may already exist as symlink)
RUN KVER=$(cat /tmp/kver) && \
    if [ ! -e /boot/vmlinuz ]; then \
        ln -s vmlinuz-${KVER} /boot/vmlinuz; \
    fi && \
    ls -la /boot/vmlinuz

# Find and decompress loop.ko for block-device tests
RUN KVER=$(cat /tmp/kver) && \
    LOOP_MOD=$(find /lib/modules/$KVER -name 'loop.ko*' | head -1) && \
    if [ -n "$LOOP_MOD" ]; then \
        echo "Found loop module: $LOOP_MOD"; \
        case "$LOOP_MOD" in \
            *.zst) zstd -d "$LOOP_MOD" -o /tmp/loop.ko ;; \
            *.xz)  xz -d -c "$LOOP_MOD" > /tmp/loop.ko ;; \
            *.gz)  gzip -d -c "$LOOP_MOD" > /tmp/loop.ko ;; \
            *)     cp "$LOOP_MOD" /tmp/loop.ko ;; \
        esac; \
        echo "Decompressed: $(ls -la /tmp/loop.ko)"; \
    else \
        echo "WARNING: loop.ko not found, block-device test will skip"; \
    fi

# Assemble initramfs
COPY tests/qemu/init.sh /src/tests/qemu/init.sh
RUN mkdir -p /initramfs/bin /initramfs/lib/modules /initramfs/dev \
             /initramfs/proc /initramfs/sys /initramfs/tests \
             /initramfs/tmp /initramfs/mnt/vmsfs \
             /initramfs/mnt/vmsfs_blkdev /initramfs/test_data && \
    # Busybox provides sh, mount, insmod, reboot, grep, losetup, etc.
    cp /bin/busybox /initramfs/bin/busybox && \
    ln -s busybox /initramfs/bin/sh && \
    # Kernel modules
    cp /src/kernel/vms.ko /initramfs/lib/modules/ && \
    cp /src/kernel/vmsfs/vmsfs.ko /initramfs/lib/modules/ && \
    # Loop module (for block-device test)
    [ -f /tmp/loop.ko ] && cp /tmp/loop.ko /initramfs/lib/modules/ || true && \
    # Block-device test image
    [ -f /vmsfs_test.img ] && cp /vmsfs_test.img /initramfs/test_data/ || true && \
    # Test programs (statically linked, no libc needed in VM)
    for f in /src/tests/qemu/test_kmod_*; do \
        [ -x "$f" ] && cp "$f" /initramfs/tests/ || true; \
    done && \
    # Public sys$ API test(s), statically linked against real libvms
    # (musl) -- see the OVMX_STATIC build stage above (vms-1d9).
    cp /src/repo/build-static/bin/test_syssvc_lock /initramfs/tests/ && \
    # Init script (PID 1)
    cp /src/tests/qemu/init.sh /initramfs/init && \
    if [ "$NEGATIVE_CONTROL" = "1" ]; then \
        sed -i 's/^insmod \/lib\/modules\/vms\.ko$/echo "NEGATIVE CONTROL: simulating vms.ko load failure (executive absent)"/' /initramfs/init && \
        echo "NEGATIVE_CONTROL=1: patched init.sh to skip insmod vms.ko"; \
    fi && \
    chmod +x /initramfs/init && \
    # Pack initramfs
    cd /initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > /initramfs.cpio.gz && \
    echo "Initramfs: $(ls -lh /initramfs.cpio.gz | awk '{print $5}')"

# Copy test runner
COPY tests/qemu/run_tests.sh /run_tests.sh
RUN chmod +x /run_tests.sh

CMD ["/run_tests.sh"]
