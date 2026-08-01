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
# depends on it fails honestly with "cannot open /dev/vms" instead of
# silently passing. (This comment used to call that "the real SS$_NOSUCHDEV
# path"; vms-0ff DELETED that path -- OVMX has no executive-absent state,
# PID 1 refuses to boot without one. This image is a test RIG deliberately
# booted without an executive, which is why it can still reach a state the
# product cannot.) This build arg exists ONLY to prove the CI gate can
# actually go red — it is never set for the real barrier job.
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

# Copy test source and build statically-linked test programs.
#
# GLOB, NEVER A LITERAL NAME (vms-1d9). Every tests/qemu/test_*.c is copied
# by pattern and dispatched to a build recipe by pattern, so a suite added by
# a future item reaches the image with no Dockerfile edit. Three recipes:
#
#   test_syssvc_*.c            -> NOT built here; built by the cmake/musl
#                                 stage below against the REAL src/libvms
#                                 public sys$ API (vms-1d9).
#   test_kmod_*.c that include -> NOT built here; built below against
#   "vms_kif.h"                   src/libvmssys (the real kernel-interface
#                                 client), see that stage's comment. This
#                                 currently covers devtab/procnam (vms-d0b/
#                                 vms-8019), ident/bind (vms-2b8/vms-fb9), and
#                                 ast/access/lock/lock_sync/lock_mproc
#                                 (vms-290) -- vms-290's five also still drive
#                                 the raw ioctl layer directly for calls out
#                                 of its scope (REGISTER, ENQ/DEQ/CONVERT,
#                                 GETMODE, CHKPRIV/SETPRV, and one deliberate
#                                 raw NULL-arg DELIVERAST -- see the comments
#                                 in those sources), so they still need
#                                 vms_ioctl.h on the include path too. None of
#                                 this is a list to remember: see the
#                                 grep-driven selector below.
#   everything else            -> plain gcc -static against vms_ioctl.h.
#
# THE SECOND RECIPE IS SELECTED BY WHAT THE SOURCE INCLUDES, not by a list of
# file names. It used to be the literal set
# devtab/procnam/ident/bind -- a list a human had to remember to extend, which
# is the drift shape this harness has been bitten by before. The include line
# is the thing that actually decides which recipe a suite needs, so it is what
# is asked. A new kernel-interface suite reaches the right recipe by being
# written, not by being remembered.
#
# `|| exit 1` is load-bearing: without it a compile failure only breaks the
# `&&` for that one iteration, the loop continues, and RUN exits with the
# status of the LAST iteration -- so a suite that stops compiling would be
# silently absent from the initramfs while the image build stayed green.
COPY tests/qemu/test_*.c /src/tests/qemu/
RUN for f in /src/tests/qemu/test_*.c; do \
        case "$f" in \
            */test_syssvc_*.c) continue;; \
        esac; \
        if grep -q '#include "vms_kif.h"' "$f"; then continue; fi; \
        out="${f%.c}"; \
        gcc -static -O2 -Wall -o "$out" "$f" -I/src/kernel || exit 1; \
        echo "Built: $(basename $out)"; \
    done

# The device-table test (vms-d0b), the process-table test (vms-8019), the
# identity/privilege test (vms-2b8) and the AST/access/lock-manager tests
# (vms-290) drive the real userspace kernel-interface client rather than a
# hand-rolled ioctl copy for at least part of their coverage, so the client
# libvms will call is the client under test. That needs libvmssys's
# freestanding syscall trampoline and string helpers linked in -- it does
# not pull in libvms; the libvms build graph arrives in the NEXT stage
# (vms-1d9), which is a heavier, separate build.
COPY src/libvmssys/ /src/libvmssys/
RUN ARCH=$(uname -m) && n_kif=0 && \
    for f in /src/tests/qemu/test_kmod_*.c; do \
        if ! grep -q '#include "vms_kif.h"' "$f"; then continue; fi; \
        t=$(basename "$f" .c); \
        gcc -static -O2 -Wall -pthread -o /src/tests/qemu/$t \
            "$f" \
            /src/libvmssys/vms_kif.c \
            /src/libvmssys/vms_string.c \
            /src/libvmssys/arch/${ARCH}/syscall.S \
            -I/src/kernel -I/src/libvmssys || exit 1; \
        n_kif=$((n_kif + 1)); \
        echo "Built: $t (linked against src/libvmssys/vms_kif.c)"; \
    done && \
    # FLOOR, for the same reason the staging step below carries one: if the
    # include-line probe above ever stops matching, this loop builds NOTHING
    # and the previous stage has already skipped the same files -- so every
    # kernel-interface suite would be absent from the initramfs with the image
    # build still green. A zero here is a broken selector, not an empty tree.
    { [ "$n_kif" -ge 1 ] || { echo "FATAL: no test_kmod_* suite was matched as a kernel-interface client -- the '#include \"vms_kif.h\"' selector no longer matches anything"; exit 1; }; }

# --- Userspace system-service layer (vms-1d9) ---------------------------
# Everything above proves the KERNEL side: raw ioctls (test_kmod_*) and the
# freestanding kernel-interface client (devtab/procnam). NONE of it links
# src/libvms, so the PUBLIC sys$ entry points -- the half of the system every
# Phase 1/3 item actually delivers -- were invisible to this gate: an
# adversary reverted src/libvms/syssvc/sys_event.c wholesale to its stub and
# the harness produced byte-identical output.
#
# This stage builds the REAL src/libvms public sys$ API statically (musl,
# the same OVMX_STATIC mode distro/Dockerfile.bootable uses to produce the
# bootable distro) so tests/qemu/test_syssvc_*.c can call sys$enq / sys$enqw
# / sys$deq -- not raw ioctls -- against a real /dev/vms. A static musl
# binary needs no separate C runtime staged into the initramfs: it carries
# its own libc, identically to how STARTUP.EXE/DCL.EXE already boot with no
# dynamic linker present.
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
#
# `--target qemu_syssvc_tests` is an AGGREGATE target defined in
# tests/qemu/CMakeLists.txt over a file(GLOB) of test_syssvc_*.c -- NOT a
# binary named here. Naming one binary was a real, adversary-proven defect
# in an earlier round of this item: the next item to add
# tests/qemu/test_syssvc_event.c would have built and ctest-SKIPped fine
# locally while never entering the initramfs and never running in QEMU,
# with the job staying green.
COPY . /src/repo/
#
# `vmsdcl` is built alongside them (vms-fb9). tests/qemu/test_syssvc_showdev.c
# does not call a sys$ entry point to make its assertions -- it EXECUTES the
# shipped DCL.EXE and reads what a user would see, because the thing under
# test is whether the user-visible SHOW DEVICE is a reader of the executive's
# device table or is answering from somewhere else. Naming the target here is
# correct and is NOT the "one binary by literal name" defect the comment
# above warns about: that rule is about which TEST suites reach the image
# (still a glob), not about which product image a suite needs.
RUN cd /src/repo && \
    cmake -B build-static \
        -DCMAKE_C_COMPILER=musl-gcc \
        -DCMAKE_BUILD_TYPE=Release \
        -DOVMX_STATIC=ON \
        -DBUILD_TESTS=ON \
        -DBUILD_TOOLS=OFF \
    && cmake --build build-static --target qemu_syssvc_tests vmsdcl --parallel $(nproc) 2>&1 && \
    echo "--- test_syssvc_* binaries (public sys\$ API, static musl) ---" && \
    ls -la build-static/bin/test_syssvc_* && \
    # DCL.EXE (already built above in the qemu_syssvc_tests/vmsdcl target
    # list), so a suite can run a real user-visible DCL COMMAND against a
    # real executive. SHOW SYSTEM is a READER of the executive's process
    # table (vms-8019) and SHOW DEVICE is a READER of the executive's device
    # table (vms-fb9); the only way to prove either lists more than the
    # calling process's own view is to run the command itself where the
    # table exists -- ctest never has a /dev/vms (CLAUDE.md Rule 9), and
    # asserting on the reader helper alone would leave dcl_cmd_show.c
    # ungated in the one environment where it can actually work.
    echo "--- DCL.EXE (the shipped image test_syssvc_showdev/procnam/showproc drive) ---" && \
    ls -la build-static/bin/DCL.EXE

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
    # Test programs (statically linked, no libc needed in VM). GLOB over
    # every test_* binary the gcc stages produced -- the .c sources sitting
    # in the same directory are not executable, so [ -x ] filters them out.
    for f in /src/tests/qemu/test_*; do \
        [ -x "$f" ] && cp "$f" /initramfs/tests/ || true; \
    done && \
    # Public sys$ API test(s), statically linked against real libvms
    # (musl) -- see the OVMX_STATIC build stage above (vms-1d9). Copies
    # EVERY test_syssvc_* binary the qemu_syssvc_tests target produced, by
    # glob -- not one binary by literal name -- so a future test_syssvc_*.c
    # (e.g. test_syssvc_event.c) needs no Dockerfile edit to reach QEMU.
    n_syssvc=0 && \
    for f in /src/repo/build-static/bin/test_syssvc_*; do \
        [ -x "$f" ] || continue; \
        cp "$f" /initramfs/tests/; \
        n_syssvc=$((n_syssvc+1)); \
    done && \
    # Fail the IMAGE BUILD, loudly, if the userspace sys$ layer produced
    # nothing to stage. Without this the initramfs silently reverts to the
    # kernel-only harness this item exists to replace, and the QEMU job
    # stays green while proving nothing about src/libvms.
    { [ "$n_syssvc" -ge 1 ] || { echo "FATAL: no test_syssvc_* binary staged into the initramfs -- the userspace public-sys\$ layer would be untested"; exit 1; }; } && \
    # The shipped DCL image, staged at BOTH paths its two sets of consumers
    # expect: /bin/DCL.EXE (vms-8019's test_syssvc_procnam.c and
    # test_syssvc_showproc.c hard-code execl("/bin/DCL.EXE", ...)) and
    # /tests/DCL.EXE (vms-fb9's test_syssvc_showdev.c defaults DCL_PATH to
    # /tests/DCL.EXE, overridable via OVMX_DCL). init.sh execs everything
    # matching /tests/test_kmod_* or /tests/test_syssvc_*; "DCL.EXE" matches
    # neither glob, so staging it under /tests/ does not make init.sh try to
    # run it as a suite. DCL is not a suite -- it is a subject the suites
    # drive. Absence is FATAL rather than skipped, for the same reason the
    # n_syssvc check above is: a missing subject would silently turn the
    # SHOW SYSTEM / SHOW DEVICE assertions into no-ops with the job still
    # green.
    { cp /src/repo/build-static/bin/DCL.EXE /initramfs/bin/DCL.EXE || \
      { echo "FATAL: DCL.EXE was not built into the static tree -- SHOW SYSTEM would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/bin/DCL.EXE && \
    cp /initramfs/bin/DCL.EXE /initramfs/tests/DCL.EXE && \
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

# --- Per-facility negative controls (vms-e7d) ----------------------------
#
# NEGATIVE_CONTROL=1 above removes the WHOLE executive. That proves the gate
# can go red, but not that it can say WHICH facility broke -- "something
# failed" is not a gate, and a facility whose assertions had quietly stopped
# meaning anything would still be covered by it.
#
# The per-facility controls inject ONE minimal defect into ONE facility (the
# vms-e4d compat[EX][CR] flip, mechanised, plus one of the same shape for
# every other facility) and check that the RIGHT suite goes red naming the
# RIGHT assertion, while that facility's sibling assertions and every other
# suite stay green.
#
# THEY ARE NOT A BUILD MODE. Nothing here is keyed on a build ARG: the
# injection happens at RUN time, in the container's own writable layer,
#
#     docker run --rm -e FACILITY_DEFECT=<name> ovmx-ktest /inject_and_run.sh
#
# so this image is built ONCE and never mutated. The first version did use a
# final ARG-keyed layer, which meant one tagged image per defect; removing
# them afterwards left the engine's build cache referencing deleted layers and
# a later build died with "getting top layer info: layer not known",
# intermittently. A flaky gate is a broken gate, so the mechanism that made it
# possible is gone rather than retried around.
#
# tests/qemu/run_facility_negctl.sh drives the whole set; the ci.yml job
# "Kernel Executive - Per-Facility Negative Controls" runs that.
COPY tests/qemu/facility_defects.sh /src/tests/qemu/facility_defects.sh
COPY tests/qemu/inject_and_run.sh /inject_and_run.sh
RUN chmod +x /inject_and_run.sh /src/tests/qemu/facility_defects.sh

# Copy test runner
COPY tests/qemu/run_tests.sh /run_tests.sh
RUN chmod +x /run_tests.sh

CMD ["/run_tests.sh"]
