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
#
# WHY linux-image-VIRTUAL AND NOT linux-image-generic (vms-2c6)
#
# These are the same kernel. `linux-image-virtual` resolves to the identical
# linux-image-6.8.0-NNN-generic + linux-modules-6.8.0-NNN-generic pair that
# `linux-image-generic` pulls, so the vmlinuz booted below, the module tree
# vms.ko/vmsfs.ko are built against, and loop.ko are all byte-for-byte what
# they were. NOTHING THIS HARNESS TESTS CHANGES.
#
# The one difference is that the `generic` meta-package hard-Depends on
# `linux-firmware` and the `virtual` one does not. linux-firmware is a
# 641 MB download of device blobs -- and run_tests.sh boots this guest with
# `-nic none -nodefaults -m 256M`, i.e. no NIC, no disk, no PCI device of any
# kind. Not one of those blobs is ever read.
#
# Measured with `apt-get install --print-uris` on ubuntu:24.04:
#     linux-image-generic + headers  ->  859 MB across 59 packages
#     linux-image-virtual + headers  ->  126 MB across 53 packages
#
# That 733 MB was downloaded from archive.ubuntu.com by THREE separate CI
# jobs on every run (kernel-executive, its negative control, and the
# per-facility controls), none of which share a layer cache -- a GitHub-hosted
# runner is a fresh VM, so there has never been a warm one to be cold. The
# cost was invisible while the mirror was fast (925 MB fetched in 16s at
# 56.7 MB/s on run 30913764276, whole build 104s) and fatal when it was not:
# on run 30917573860 the same fetch stalled 706s on the linux-firmware package
# alone and the job was cancelled at its 20m cap having never run its
# assertion step at all. A cancelled job yields NO verdict, so this was also
# silently degrading the authenticity gates to "not red" rather than "green".
#
# QEMU is installed in this SAME layer, and the arch-conditional picks only
# the package NAME, deliberately. It used to be a second RUN with its own
# `apt-get update` -- but the first one ends in `rm -rf /var/lib/apt/lists/*`,
# so the second had to re-download the whole 31.9 MB package index. That is
# not free at the mirror throughputs actually observed: on run 30927522942 the
# two index fetches cost 3min39s and 1min8s (146 kB/s and 472 kB/s) on top of
# the payload. One `apt-get update`, one index fetch. Same packages installed.
RUN apt-get update && \
    ARCH=$(dpkg --print-architecture) && \
    if [ "$ARCH" = "arm64" ]; then QEMU_PKG=qemu-system-arm; else QEMU_PKG=qemu-system-x86; fi && \
    apt-get install -y --no-install-recommends \
    make gcc libc6-dev kmod \
    linux-headers-generic \
    linux-image-virtual \
    busybox-static \
    cpio \
    zstd \
    cmake musl-tools linux-libc-dev \
    "$QEMU_PKG" \
    && rm -rf /var/lib/apt/lists/*

# Determine kernel version from installed headers
RUN KVER=$(ls /lib/modules/ | sort -V | tail -1) && \
    echo "$KVER" > /tmp/kver && \
    echo "Kernel version: $KVER"

# Copy kernel module source and build vms.ko. src/kernel-core carries the
# substrate-agnostic executive core + the kernel-backend shim contract
# (exec_kbackend.h, rd vms-adb); the Makefile adds -I$(src)/../kernel-core, so
# it must sit beside /src/kernel as /src/kernel-core.
COPY src/kernel-core/ /src/kernel-core/
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
#   test_syssvc_*.c,           -> NOT built here; built by the cmake/musl
#   test_imgact_*.c               stage below against the REAL src/libvms
#                                 public sys$ API (vms-1d9). test_imgact_*
#                                 (was test_syssvc_imgact_bind/publish --
#                                 main-red classification fix) needs the same
#                                 recipe: it #includes imgact_prodreg.h, a
#                                 libvms public header not on this early raw-
#                                 gcc stage's include path.
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
            */test_imgact_*.c) continue;; \
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
            /src/libvmssys/kif_transport_linux.c \
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
# BUILD_TOOLS=ON (vms-4c2): tools/CMakeLists.txt is only CONFIGURED here, not
# fully built -- the --target list below still names exactly the binaries
# this image needs, so flipping this on adds one requested target
# (vms_authorize) without pulling vms_login/vms_mail/vms_monitor/etc. into
# the build. OVMX_STATIC + BUILD_TOOLS=ON is not new ground: distro/
# Dockerfile.bootable already builds the WHOLE tools/ tree this way for the
# bootable distro image, so vms_authorize compiling static+musl here is
# precedented, not a first attempt.
RUN cd /src/repo && \
    cmake -B build-static \
        -DCMAKE_C_COMPILER=musl-gcc \
        -DCMAKE_BUILD_TYPE=Release \
        -DOVMX_STATIC=ON \
        -DBUILD_TESTS=ON \
        -DBUILD_TOOLS=ON \
    && cmake --build build-static --target qemu_syssvc_tests vmsdcl vms_authorize vms_initialize mmk_native --parallel $(nproc) 2>&1 && \
    echo "--- test_syssvc_* binaries (public sys\$ API, static musl) ---" && \
    ls -la build-static/bin/test_syssvc_* && \
    # MMK.EXE (vms-b23, self-host spine #4): the shipped MadGoat MMK built as an
    # OVMX image. test_syssvc_mmk_drive.c runs it against a real /dev/vms to
    # prove MMK drives a real build through its persistent mailbox-driven DCL
    # subprocess (spawn + mailbox + write-attention AST + $HIBER + IO$M_NOW) --
    # the exec-drive is a real executive facility (ovmx_mmk_sp.c), unprovable
    # without a real executive, so it must run where /dev/vms exists.
    echo "--- MMK.EXE (the shipped image test_syssvc_mmk_drive drives, vms-b23) ---" && \
    ls -la build-static/bin/MMK.EXE && \
    echo "--- test_imgact_* binaries (userspace-only, no /dev/vms needed, static musl) ---" && \
    ls -la build-static/bin/test_imgact_* && \
    # test_corpus_* + corpus_seed_lnm (vms-08c): the unmodified device-corpus
    # programs (tests/corpus/tier1-examples/lib_getdvi.c, sys_device_scan.c)
    # built against the real src/libvms public sys$ API, plus the LNM$SYSTEM
    # seeding fixture tests/qemu/init.sh runs ahead of them. See
    # tests/qemu/CMakeLists.txt's "DEVICE-CORPUS PROGRAMS" comment.
    echo "--- test_corpus_* binaries + corpus_seed_lnm fixture (vms-08c, static musl) ---" && \
    ls -la build-static/bin/test_corpus_lib_getdvi build-static/bin/test_corpus_sys_device_scan build-static/bin/corpus_seed_lnm && \
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
    ls -la build-static/bin/DCL.EXE && \
    # AUTHORIZE.EXE (vms-4c2): same reasoning as DCL.EXE above, for
    # tests/qemu/test_syssvc_authorize.c. check_privilege() in
    # tools/vms_authorize.c reads the executive's own privilege mask for
    # THIS process -- proving it discriminates SYSPRV holders from
    # everyone else needs the real binary run against a real /dev/vms,
    # not a unit test of the read helper in isolation.
    echo "--- AUTHORIZE.EXE (the shipped image test_syssvc_authorize drive) ---" && \
    ls -la build-static/bin/AUTHORIZE.EXE && \
    # TESTIMG.EXE (vms-68f iv): the in-process-eligible image
    # test_syssvc_imgact_inproc.c activates in DCL's own process. Built by the
    # qemu_syssvc_tests aggregate (which now DEPENDS on testimg_inproc), so it
    # arrives with the suites -- no separate --target name to drift.
    echo "--- TESTIMG.EXE (the in-process activation subject, test_syssvc_imgact_inproc) ---" && \
    ls -la build-static/bin/TESTIMG.EXE && \
    # TESTREAL.EXE (vms-db2): the REAL-image-class subject
    # test_syssvc_imgact_realimg.c activates in-process (the flip). Same aggregate
    # (qemu_syssvc_tests now DEPENDS on testreal_inproc), entered via the auxv ABI.
    echo "--- TESTREAL.EXE (the real-image in-process flip subject, test_syssvc_imgact_realimg) ---" && \
    ls -la build-static/bin/TESTREAL.EXE && \
    # TESTEXTERN.EXE + TESTEXTERN_FOREIGN.EXE (vms-db2): the EXTERNAL-image flip
    # subjects test_syssvc_imgact_extern.c activates (PT_INTERP naming the OVMX
    # loader runs in-process; a foreign interp forks). Same aggregate
    # (qemu_syssvc_tests now DEPENDS on both).
    echo "--- TESTEXTERN.EXE / TESTEXTERN_FOREIGN.EXE (external-image flip subjects, test_syssvc_imgact_extern) ---" && \
    ls -la build-static/bin/TESTEXTERN.EXE build-static/bin/TESTEXTERN_FOREIGN.EXE && \
    # TESTPROD.EXE + TESTNONRES.EXE (vms-db2): the NON-RESIDENT-producer flip
    # subjects test_syssvc_imgact_nonres.c activates. TESTNONRES's .vms$imp names
    # TESTPROD (a producer DCL does not hold resident); the activator maps
    # TESTPROD in-process to bind it. Same aggregate (qemu_syssvc_tests DEPENDS on
    # testprod_shr + testnonres_inproc).
    echo "--- TESTPROD.EXE / TESTNONRES.EXE (non-resident-producer flip subjects, test_syssvc_imgact_nonres) ---" && \
    ls -la build-static/bin/TESTPROD.EXE build-static/bin/TESTNONRES.EXE && \
    # TESTTLS.EXE (vms-db2): the OWN-PT_TLS flip subject test_syssvc_imgact_tls.c
    # activates -- TESTREAL plus its own PT_TLS reached through the biased .vms$tls
    # TLSDESC path. Same aggregate (qemu_syssvc_tests DEPENDS on testtls_inproc).
    echo "--- TESTTLS.EXE (the own-PT_TLS in-process flip subject, test_syssvc_imgact_tls) ---" && \
    ls -la build-static/bin/TESTTLS.EXE

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

# Find and decompress loop.ko for block-device tests, IF it is a module.
#
# THE "not found" BRANCH IS THE NORMAL ONE, AND IT IS NOT A SKIP (vms-2c6).
# Ubuntu builds the loop driver into the kernel (CONFIG_BLK_DEV_LOOP=y), so
# there is no loop.ko under /lib/modules at all and this step takes the else
# branch on every build -- including every build on main today. init.sh knows
# this ("loop may be built-in or a module"): it insmods /lib/modules/loop.ko
# only if the file exists, then runs losetup either way. Measured on run
# 30913764276: this step printed the "not found" message AND the guest still
# reported "OK: loop0 attached to vmsfs_test.img" and ran
# test_kmod_vmsfs_blkdev to completion.
#
# The old message claimed "block-device test will skip", which is FALSE and
# actively misleading -- it reads like a silently-dropped suite, which is the
# one thing this project treats as a failing test. Reworded to say what is
# actually true. Nothing about the image or the tests changes here; the whole
# edit is the text of an echo.
#
# This is also unaffected by the linux-image-virtual move above: `virtual`
# selects the SAME linux-image-6.8.0-NNN-generic binary, so the loop driver is
# built in there exactly as before.
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
        echo "loop.ko is not a module in this kernel (CONFIG_BLK_DEV_LOOP=y)."; \
        echo "This is the EXPECTED path on Ubuntu. The block-device test still"; \
        echo "runs -- init.sh calls losetup against the built-in driver; watch"; \
        echo "for 'OK: loop0 attached' and the test_kmod_vmsfs_blkdev suite in"; \
        echo "the guest output. Nothing is skipped."; \
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
    # Loop module, only if this kernel has it as a module. On Ubuntu it does
    # not (built in -- see the step above), so this is normally a no-op.
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
    # Userspace-only imgact suites (test_imgact_bind, test_imgact_publish --
    # main-red classification fix, were test_syssvc_imgact_bind/publish
    # through #225/#226). Staged the SAME way, by glob, so a future
    # test_imgact_*.c reaches QEMU with no Dockerfile edit -- but kept OUT of
    # the test_syssvc_* glob above on purpose: ci.yml's kernel-executive-
    # negative-control job polices every test_syssvc_* suite to return the
    # honest-skip code 77 with no /dev/vms, and these two suites never touch
    # /dev/vms at all, so they legitimately return 0 in that job and would
    # trip its "fabricated success" check for a real, non-executive pass.
    # Still staged here (not just built) so run_facility_negctl.sh's per-
    # facility proofs (consumer-import-not-bound-to-resident, publish-does-
    # not-populate-registry) keep seeing these suites run and go red under
    # their fault injection -- see that script's own EXPECTED derivation.
    n_imgact=0 && \
    for f in /src/repo/build-static/bin/test_imgact_*; do \
        [ -x "$f" ] || continue; \
        cp "$f" /initramfs/tests/; \
        n_imgact=$((n_imgact+1)); \
    done && \
    { [ "$n_imgact" -ge 2 ] || { echo "FATAL: expected at least 2 test_imgact_* binaries (test_imgact_bind, test_imgact_publish) staged into the initramfs, found $n_imgact"; exit 1; }; } && \
    # Device-corpus programs under a real /dev/vms (vms-08c). Staged by
    # glob, same convention as test_syssvc_*/test_imgact_* above, so a
    # future test_corpus_*.c reaches QEMU with no Dockerfile edit. Kept OUT
    # of the test_syssvc_* glob for the reason in tests/qemu/CMakeLists.txt's
    # "DEVICE-CORPUS PROGRAMS" comment: these are unmodified upstream
    # programs, not a suite this project can hold to the honest-skip-77
    # contract it wrote for its OWN test_syssvc_* suites.
    n_corpus=0 && \
    for f in /src/repo/build-static/bin/test_corpus_*; do \
        [ -x "$f" ] || continue; \
        cp "$f" /initramfs/tests/; \
        n_corpus=$((n_corpus+1)); \
    done && \
    { [ "$n_corpus" -ge 2 ] || { echo "FATAL: expected at least 2 test_corpus_* binaries (test_corpus_lib_getdvi, test_corpus_sys_device_scan) staged into the initramfs, found $n_corpus"; exit 1; }; } && \
    # corpus_seed_lnm (vms-08c): the LNM$SYSTEM seeding fixture, staged
    # separately (no test_ prefix -- it is not a suite, see its own header
    # comment). Absence is FATAL, not skipped: without it lib_getdvi's
    # SYS$SYSDEVICE lookup would silently miss and the corpus suite above
    # would go red for the wrong reason (fixture absent, not a real defect).
    { cp /src/repo/build-static/bin/corpus_seed_lnm /initramfs/tests/corpus_seed_lnm || \
      { echo "FATAL: corpus_seed_lnm was not built into the static tree -- lib_getdvi's SYS\$SYSDEVICE lookup would go unseeded"; exit 1; }; } && \
    chmod +x /initramfs/tests/corpus_seed_lnm && \
    # The shipped DCL image, staged at BOTH paths its two sets of consumers
    # expect: /bin/DCL.EXE (vms-8019's test_syssvc_procnam.c and
    # test_syssvc_showproc.c hard-code execl("/bin/DCL.EXE", ...)) and
    # /tests/DCL.EXE (vms-fb9's test_syssvc_showdev.c defaults DCL_PATH to
    # /tests/DCL.EXE, overridable via OVMX_DCL). init.sh execs everything
    # matching /tests/test_kmod_*, /tests/test_syssvc_* or /tests/test_imgact_*;
    # "DCL.EXE" matches none of those globs, so staging it under /tests/ does
    # not make init.sh try to run it as a suite. DCL is not a suite -- it is
    # a subject the suites drive. Absence is FATAL rather than skipped, for
    # the same reason the n_syssvc check above is: a missing subject would
    # silently turn the SHOW SYSTEM / SHOW DEVICE assertions into no-ops
    # with the job still green.
    { cp /src/repo/build-static/bin/DCL.EXE /initramfs/bin/DCL.EXE || \
      { echo "FATAL: DCL.EXE was not built into the static tree -- SHOW SYSTEM would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/bin/DCL.EXE && \
    cp /initramfs/bin/DCL.EXE /initramfs/tests/DCL.EXE && \
    # MMK.EXE + DCL.EXE at SYS$SYSTEM (vms-b23, self-host spine #4). MMK.EXE is
    # the subject test_syssvc_mmk_drive.c execs (OVMX_MMK default
    # /vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE); DCL.EXE is staged at that SAME
    # SYS$SYSTEM path because MMK's lib$spawn resolves SYS$SYSTEM:DCL.EXE
    # (VMS_DCL_PATH) to launch the persistent DCL it drives. Neither matches a
    # test_* glob, so init.sh never runs them as suites -- they are the drive
    # subject and the CLI it spawns. Absence is FATAL, not skipped: a missing
    # subject would silently turn the driven-build assertion into a no-op with
    # the job still green.
    mkdir -p /initramfs/vms/SYS0/SYSCOMMON/SYSEXE && \
    { cp /src/repo/build-static/bin/MMK.EXE /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE || \
      { echo "FATAL: MMK.EXE was not built into the static tree -- the MMK exec-drive would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE && \
    cp /initramfs/bin/DCL.EXE /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE && \
    chmod +x /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE && \
    # AUTHORIZE.EXE (vms-4c2), staged at /bin/AUTHORIZE.EXE --
    # tests/qemu/test_syssvc_authorize.c execs it by that path. Absence is
    # FATAL, not skipped, for the same reason as DCL.EXE above: a missing
    # subject would silently turn the SYSPRV admit/refuse assertions into
    # no-ops with the job still green.
    { cp /src/repo/build-static/bin/AUTHORIZE.EXE /initramfs/bin/AUTHORIZE.EXE || \
      { echo "FATAL: AUTHORIZE.EXE was not built into the static tree -- the SYSPRV positive control would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/bin/AUTHORIZE.EXE && \
    # INITIALIZE.EXE (vms-cf62), staged at /tests/INITIALIZE.EXE --
    # tests/qemu/test_syssvc_initialize.c execs it by that default path
    # (overridable via OVMX_INITIALIZE). It matches no test_* glob, so init.sh
    # never runs it as a suite -- it is the subject the suite drives. Absence is
    # FATAL, not skipped: a missing subject would turn the resolves-and-formats-
    # the-real-device / no-fake-success assertions into no-ops with the job
    # still green.
    { cp /src/repo/build-static/bin/INITIALIZE.EXE /initramfs/tests/INITIALIZE.EXE || \
      { echo "FATAL: INITIALIZE.EXE was not built into the static tree -- INITIALIZE's real-device resolution would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/tests/INITIALIZE.EXE && \
    # TESTIMG.EXE (vms-68f iv), staged at /tests/TESTIMG.EXE --
    # tests/qemu/test_syssvc_imgact_inproc.c activates it in-process by that
    # default path (overridable via OVMX_TESTIMG). It matches no test_* glob,
    # so init.sh never runs it as a suite -- it is the subject, not a suite.
    # Absence is FATAL, not skipped: a missing subject would turn the same-PID
    # and critical-P1 assertions into no-ops with the job still green.
    { cp /src/repo/build-static/bin/TESTIMG.EXE /initramfs/tests/TESTIMG.EXE || \
      { echo "FATAL: TESTIMG.EXE was not built into the static tree -- the in-process activation proof would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/tests/TESTIMG.EXE && \
    # TESTREAL.EXE (vms-db2), staged at /tests/TESTREAL.EXE --
    # tests/qemu/test_syssvc_imgact_realimg.c activates it in-process by that
    # default path (overridable via OVMX_TESTREAL). Matches no test_* glob, so
    # init.sh never runs it as a suite. Absence is FATAL, not skipped: a missing
    # subject would turn the real-image flip's same-PID/flows-back/SYS$EXIT
    # assertions into no-ops with the job still green.
    { cp /src/repo/build-static/bin/TESTREAL.EXE /initramfs/tests/TESTREAL.EXE || \
      { echo "FATAL: TESTREAL.EXE was not built into the static tree -- the real-image in-process flip would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/tests/TESTREAL.EXE && \
    # TESTEXTERN.EXE + TESTEXTERN_FOREIGN.EXE (vms-db2), staged at /tests/ --
    # tests/qemu/test_syssvc_imgact_extern.c activates them in-process by those
    # default paths (overridable via OVMX_TESTEXTERN / OVMX_TESTEXTERN_FOREIGN).
    # Neither matches a test_* glob. Absence is FATAL, not skipped: a missing
    # subject would turn the external-image flip's same-PID/flows-back/SYS$EXIT
    # and the foreign-interp-forks assertions into no-ops with the job green.
    { cp /src/repo/build-static/bin/TESTEXTERN.EXE /initramfs/tests/TESTEXTERN.EXE || \
      { echo "FATAL: TESTEXTERN.EXE was not built into the static tree -- the external-image in-process flip would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/tests/TESTEXTERN.EXE && \
    { cp /src/repo/build-static/bin/TESTEXTERN_FOREIGN.EXE /initramfs/tests/TESTEXTERN_FOREIGN.EXE || \
      { echo "FATAL: TESTEXTERN_FOREIGN.EXE was not built into the static tree -- the foreign-interp negative control would go unproven"; exit 1; }; } && \
    chmod +x /initramfs/tests/TESTEXTERN_FOREIGN.EXE && \
    # TESTNONRES.EXE + TESTPROD.EXE (vms-db2), the non-resident-producer flip --
    # tests/qemu/test_syssvc_imgact_nonres.c activates TESTNONRES at /tests/
    # (default, overridable via OVMX_TESTNONRES). TESTNONRES's .vms$imp names
    # TESTPROD, which the activator (imgact_map_producer) resolves via SYS$SHARE
    # = /vms/SYS0/SYSCOMMON/SYSLIB, so TESTPROD is staged THERE (not /tests) --
    # exactly the path the mapper searches. Neither matches a test_* glob.
    # Absence is FATAL, not skipped: a missing consumer turns the same-PID/
    # shared-instance assertions into no-ops with the job green; a missing
    # producer would make the flip fork (SS$_UNSUPPORTED) and the suite fail --
    # which is the honest outcome, but FATAL-here keeps the intent explicit.
    { cp /src/repo/build-static/bin/TESTNONRES.EXE /initramfs/tests/TESTNONRES.EXE || \
      { echo "FATAL: TESTNONRES.EXE was not built into the static tree -- the non-resident-producer in-process flip would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/tests/TESTNONRES.EXE && \
    mkdir -p /initramfs/vms/SYS0/SYSCOMMON/SYSLIB && \
    { cp /src/repo/build-static/bin/TESTPROD.EXE /initramfs/vms/SYS0/SYSCOMMON/SYSLIB/TESTPROD.EXE || \
      { echo "FATAL: TESTPROD.EXE was not built into the static tree -- the non-resident producer the activator maps would be missing and the flip would fork unproven"; exit 1; }; } && \
    chmod +x /initramfs/vms/SYS0/SYSCOMMON/SYSLIB/TESTPROD.EXE && \
    # TESTTLS.EXE (vms-db2), the OWN-PT_TLS flip -- tests/qemu/test_syssvc_imgact_tls.c
    # activates TESTTLS at /tests/ (default, overridable via OVMX_TESTTLS). It carries
    # its own PT_TLS reached through the .vms$tls TLSDESC path the activator biases
    # against DCL's thread pointer. Matches no test_* glob. Absence is FATAL: a missing
    # subject turns the own-TLS/no-clobber assertions into no-ops with the job green.
    { cp /src/repo/build-static/bin/TESTTLS.EXE /initramfs/tests/TESTTLS.EXE || \
      { echo "FATAL: TESTTLS.EXE was not built into the static tree -- the own-PT_TLS in-process flip would go unproven against a real executive"; exit 1; }; } && \
    chmod +x /initramfs/tests/TESTTLS.EXE && \
    # THE SYSTEM DATA FILES F$IDENTIFIER READS (vms-2f8).
    #
    # F$IDENTIFIER used to answer from a hardcoded pair of names inside
    # lex_identifier(), so the ident suite's SYSTEM/DEFAULT assertions passed
    # in an initramfs that had no system disk at all. It now READS the rights
    # database -- SYS$SYSTEM:RIGHTSLIST.DAT for general identifiers, SYSUAF
    # for UIC identifiers -- which means those assertions are only meaningful
    # if the guest has the files. Staged at the product's own layout, from
    # the product's own distro/rootfs, so the suite asserts against what
    # actually boots rather than a fixture invented for the harness.
    #
    # ovmx_init provisions these on the real boot path
    # (src/ovmx_init/ovmx_init.c:531); this harness runs busybox init, not
    # ovmx_init, so it stages them itself.
    #
    # ABSENCE IS FATAL, not skipped -- same reason as DCL.EXE and n_syssvc
    # above. A missing rights database does not fail loudly at run time: it
    # is indistinguishable from "no such identifier", which is a legal VMS
    # answer. The suite's positive assertions would simply stop being
    # reachable while the job stayed green.
    mkdir -p /initramfs/vms/SYS0/SYSCOMMON/SYSEXE && \
    for f in RIGHTSLIST.DAT SYSUAF.DAT; do \
        cp "/src/repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/$f" \
           /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/ || \
        { echo "FATAL: $f was not staged into the initramfs -- F\$IDENTIFIER reads the rights database now, and every identifier assertion would silently become a miss"; exit 1; }; \
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

# Copy test runner AND the helper it sources.
#
# run_tests.sh lands at /run_tests.sh, so its `. "$(dirname "$0")/lib/..."`
# resolves to //lib/harness_verdict.sh -- the layout below is what makes that
# path real inside the image. Copying run_tests.sh without lib/ produces
#
#     /run_tests.sh: line 17: //lib/harness_verdict.sh: No such file or directory
#
# and, because the shell exits 1 there, the CI step reports the harness itself
# as "exited nonzero" -- indistinguishable from a genuine executive failure
# until you read the log. Keep these two COPYs together.
# A single-FILE COPY, not `COPY tests/qemu/lib/ /lib/`, deliberately: on
# ubuntu:24.04 /lib is a symlink to usr/lib, and a directory COPY onto that
# path is not worth the risk of replacing the symlink with a real directory
# and cutting the image off from every shared library in it. One file is all
# lib/ holds, and the guard below is what notices if that stops being true.
COPY tests/qemu/run_tests.sh /run_tests.sh
COPY tests/qemu/lib/harness_verdict.sh /lib/harness_verdict.sh
RUN chmod +x /run_tests.sh

# The image must not be able to build without the helper run_tests.sh sources.
# Both halves are checked because they fail differently and both were live
# possibilities here: the file must EXIST (a dropped COPY), and it must be the
# only thing lib/ ships (a second helper added upstream that no COPY brings in,
# which would fail identically at boot and just as confusingly).
RUN test -r /lib/harness_verdict.sh \
    || { echo "FATAL: /lib/harness_verdict.sh missing -- run_tests.sh sources it at boot, so the harness would die before running one suite and CI would report it as 'harness exited nonzero'"; exit 1; }
COPY tests/qemu/lib/ /tmp/lib_check/
RUN n=$(ls -1 /tmp/lib_check | wc -l); \
    if [ "$n" -ne 1 ]; then \
        echo "FATAL: tests/qemu/lib/ now holds $n files but only harness_verdict.sh is COPYed into the image."; \
        ls -1 /tmp/lib_check; \
        echo "Add a COPY for each, then raise the count here. Do not delete this check."; \
        exit 1; \
    fi && rm -rf /tmp/lib_check

CMD ["/run_tests.sh"]
