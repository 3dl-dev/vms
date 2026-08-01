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

# Install: build tools, kernel packages, QEMU, busybox for initramfs
RUN apt-get update && apt-get install -y --no-install-recommends \
    make gcc libc6-dev kmod \
    linux-headers-generic \
    linux-image-generic \
    busybox-static \
    cpio \
    zstd \
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

# Copy test source and build statically-linked test programs
COPY tests/qemu/test_*.c /src/tests/qemu/
RUN for f in /src/tests/qemu/test_*.c; do \
        case "$f" in */test_kmod_devtab.c|*/test_kmod_procnam.c|*/test_kmod_bind.c) continue;; esac; \
        out="${f%.c}"; \
        gcc -static -O2 -Wall -o "$out" "$f" -I/src/kernel && \
        echo "Built: $(basename $out)"; \
    done

# The device-table test (vms-d0b) and the process-table test (vms-8019) drive
# the real userspace kernel-interface
# client rather than a hand-rolled ioctl copy, so the client libvms will call is
# the client under test. That needs libvmssys's freestanding syscall trampoline
# and string helpers linked in -- it does NOT pull in libvms, which still cannot
# be built into this initramfs (tracked separately).
COPY src/libvmssys/ /src/libvmssys/
RUN ARCH=$(uname -m) && \
    for t in test_kmod_devtab test_kmod_procnam test_kmod_bind; do \
        gcc -static -O2 -Wall -o /src/tests/qemu/$t \
            /src/tests/qemu/$t.c \
            /src/libvmssys/vms_kif.c \
            /src/libvmssys/vms_string.c \
            /src/libvmssys/arch/${ARCH}/syscall.S \
            -I/src/kernel -I/src/libvmssys && \
        echo "Built: $t (linked against src/libvmssys/vms_kif.c)" || exit 1; \
    done

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
