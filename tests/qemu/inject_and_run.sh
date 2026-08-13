#!/bin/sh
#
# inject_and_run.sh - inject ONE facility defect into the already-built test
#                     image, rebuild only what the defect can reach, and run
#                     the QEMU harness (vms-e7d)
#
# Runs INSIDE the tests/qemu image, as the container command:
#
#   docker run --rm -e FACILITY_DEFECT=<name> ovmx-ktest /inject_and_run.sh
#
# WHY THIS IS A RUN-TIME STEP AND NOT A `docker build --build-arg`
#
# The first version of this was a final Dockerfile layer keyed on
# ARG FACILITY_DEFECT, which meant nine tagged images per run. Removing them
# afterwards (nine `rmi -f` calls against images whose lower layers are the
# shared build cache) left the engine's cache index referencing deleted
# layers, and a later build died with "getting top layer info: layer not
# known" -- INTERMITTENTLY. A flaky gate is a broken gate: it trains everyone
# to re-run and stop reading. Injecting in the container's own writable layer
# removes the whole class -- one image, built once, never mutated, never
# removed mid-run -- and is faster besides.
#
# WHAT IT REBUILDS, AND WHY EACH ONE
#   vms.ko                      the executive itself
#   every test_kmod_*.c that CALLS a vms_kif_ entry point
#                               the suites that link the REAL
#                               src/libvmssys/vms_kif.c client rather than a
#                               hand-rolled ioctl copy. DERIVED from the
#                               sources, never listed -- see the loop below.
#   qemu_syssvc_tests           the public sys$ suites, built through the
#                               CMake graph against src/libvms
#   DCL.EXE                     the user-visible command layer. A suite can
#                               drive a real DCL COMMAND against the real
#                               executive (test_syssvc_startup_service runs
#                               RUN/DETACHED and SHOW SYSTEM), so a defect
#                               under src/vmsdcl is reachable -- and before
#                               this was added, such a defect would have been
#                               injected into a source nobody recompiled and
#                               a STALE DCL.EXE would have been re-staged,
#                               reporting the gate as having caught nothing.
#
# All of them are rebuilt for EVERY defect, not just the ones whose sources
# changed. That is deliberate: a conditional rebuild is one more thing that
# can silently not happen, and it would leave a stale binary asserting
# "unaffected" for a facility it was never rebuilt against. The incremental
# cost is a few seconds.
#
# Exit codes -- the driver distinguishes these from a harness verdict:
#   3  BROKEN FIXTURE: the injection did not land (a sed anchor moved).
#      NOT a verdict about the executive.
#   4  the rebuild failed.
#   otherwise: run_tests.sh's own status (0 = all suites passed, 1 = failed).

set -u

DEFECT="${FACILITY_DEFECT:-}"

if [ -z "$DEFECT" ]; then
    echo "inject_and_run.sh: FACILITY_DEFECT is empty -- nothing to inject."
    echo "Run the image's default command for the pristine harness."
    exit 4
fi

echo "=== FACILITY_DEFECT=$DEFECT ==="

# The injection verifies itself: facility_defects.sh compares each target
# against a pristine copy and refuses to continue if the file did not change.
# A dead anchor must fail LOUDLY here, because the alternative is an
# unmutated image whose green run gets reported as "the gate caught it".
if ! sh /src/tests/qemu/facility_defects.sh apply "$DEFECT" /src /src/repo/src; then
    echo "inject_and_run.sh: injection failed -- see BROKEN FIXTURE above."
    exit 3
fi

KVER=$(cat /tmp/kver)
ARCH=$(uname -m)

echo "--- rebuilding vms.ko ---"
( cd /src/kernel && make KDIR="/lib/modules/${KVER}/build" clean >/dev/null 2>&1 \
                 && make KDIR="/lib/modules/${KVER}/build" ) || exit 4

# DERIVED, NEVER A LITERAL LIST. This was `for t in test_kmod_devtab
# test_kmod_procnam test_kmod_bind`, and vms-2b8 landed test_kmod_ident.c --
# a FOURTH libvmssys-linked suite -- which the list did not know about, so the
# defect rebuild would have left a stale test_kmod_ident in the initramfs
# asserting "unaffected" for a facility it was never rebuilt against. Exactly
# the hand-maintained-list failure the rest of this harness derives its way
# out of.
#
# The discriminator is a CALL to a vms_kif_ entry point, not a mention of one:
# test_kmod_lock_mproc.c and test_kmod_lock_sync.c both name vms_kif in a
# comment and must NOT be linked against it, or they would be built
# differently here than tests/qemu/Dockerfile built them.
echo "--- rebuilding the libvmssys-linked suites (derived from the sources) ---"
n_kif=0
for s in /src/tests/qemu/test_kmod_*.c; do
    grep -qE 'vms_kif_[A-Za-z0-9_]*\(' "$s" || continue
    t=$(basename "$s" .c)
    echo "    $t (calls vms_kif_*)"
    gcc -static -O2 -Wall -o "/src/tests/qemu/$t" "$s" \
        /src/libvmssys/vms_kif.c \
        /src/libvmssys/kif_transport_linux.c \
        /src/libvmssys/vms_string.c \
        "/src/libvmssys/arch/${ARCH}/syscall.S" \
        -I/src/kernel -I/src/libvmssys || exit 4
    n_kif=$((n_kif + 1))
done
# A rebuild that silently found nothing to rebuild would leave every
# client-side suite stale, and bind-client-no-register -- whose target IS
# src/libvmssys/vms_kif.c -- would then be injected into a binary nobody
# recompiled and report the gate as having caught nothing.
if [ "$n_kif" -lt 1 ]; then
    echo "FATAL: no tests/qemu/test_kmod_*.c calls a vms_kif_ entry point."
    echo "  Either every client suite was deleted or the discriminator no"
    echo "  longer matches the sources. Either way this run would inject into"
    echo "  binaries it never rebuilt."
    exit 4
fi

echo "--- rebuilding the public sys\$ suites ---"
( cd /src/repo && cmake --build build-static --target qemu_syssvc_tests \
                        --parallel "$(nproc)" ) || exit 4

# DCL.EXE is a SUBJECT the suites drive, not a suite, so nothing above builds
# it -- and a suite that runs a real DCL command against the real executive
# (test_syssvc_startup_service) is asserting about this binary. Rebuilt
# unconditionally, for the same reason everything else here is: a conditional
# rebuild is one more thing that can silently not happen.
echo "--- rebuilding DCL.EXE (the user-visible command layer) ---"
( cd /src/repo && cmake --build build-static --target vmsdcl \
                        --parallel "$(nproc)" ) || exit 4

# MMK.EXE is a SUBJECT the exec-drive suite drives, not a suite, so nothing
# above builds it -- and test_syssvc_mmk_drive (vms-b23, self-host spine #4)
# runs the shipped MMK.EXE, whose exec-drive companion (ovmx_mmk_sp.c, compiled
# into MMK.EXE) a defect targets (mmk-drive-command-not-sent). Rebuilt
# unconditionally, same reason as DCL.EXE: a conditional rebuild is one more
# thing that can silently not happen, leaving a stale MMK.EXE asserting the gate
# caught nothing.
echo "--- rebuilding MMK.EXE (the exec-drive subject test_syssvc_mmk_drive drives) ---"
( cd /src/repo && cmake --build build-static --target mmk_native \
                        --parallel "$(nproc)" ) || exit 4

echo "--- re-staging the initramfs ---"
cp /src/kernel/vms.ko /initramfs/lib/modules/ || exit 4
# Absence is FATAL, never skipped, exactly as in the image build: a missing
# subject would turn the SHOW SYSTEM and RUN/DETACHED assertions into no-ops
# with the job still green.
#
# TWO COPIES, NOT ONE (found this round, vms-d0b): tests/qemu/Dockerfile
# stages DCL.EXE at BOTH /initramfs/bin/DCL.EXE (test_syssvc_procnam.c,
# test_syssvc_showproc.c and test_syssvc_startup_service.c's DCL_IMAGE all
# execl() this path) AND /initramfs/tests/DCL.EXE (test_syssvc_showdev.c and
# test_syssvc_showterm.c's DCL_PATH_DEFAULT, overridable via OVMX_DCL). This
# script used to refresh only the first, so any defect targeting src/vmsdcl
# whose suite drives DCL through the SECOND path ran against the STALE
# image-build binary and could never go red -- a facility control with no
# teeth, discovered when showterm-width-page-fabricated (vmsdcl/dcl_cmd_show.c)
# rebuilt and linked correctly (confirmed with `strings` on the fresh
# build-static/bin/DCL.EXE) but produced zero effect on test_syssvc_showterm's
# output. Both copies must be the SAME freshly rebuilt binary.
cp /src/repo/build-static/bin/DCL.EXE /initramfs/bin/DCL.EXE || exit 4
cp /src/repo/build-static/bin/DCL.EXE /initramfs/tests/DCL.EXE || exit 4

# MMK.EXE + its spawned DCL.EXE at SYS$SYSTEM (vms-b23, spine #4) -- the SAME
# stale-binary trap the DCL.EXE two-copies note above documents. test_syssvc_
# mmk_drive execs MMK.EXE from /vms/SYS0/SYSCOMMON/SYSEXE, and MMK's lib$spawn
# launches SYS$SYSTEM:DCL.EXE from that SAME path (VMS_DCL_PATH), so a defect in
# the MMK exec-drive (ovmx_mmk_sp.c -> MMK.EXE) or in the DCL it drives must
# refresh THESE copies or the drive runs against the pristine image-build
# binaries and the mmk-drive-command-not-sent control could never go red.
# Absence is FATAL, exactly as in the image build (tests/qemu/Dockerfile).
mkdir -p /initramfs/vms/SYS0/SYSCOMMON/SYSEXE || exit 4
cp /src/repo/build-static/bin/MMK.EXE \
   /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE || exit 4
cp /src/repo/build-static/bin/DCL.EXE \
   /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE || exit 4
chmod +x /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE \
         /initramfs/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE || exit 4
for f in /src/tests/qemu/test_*; do
    [ -x "$f" ] && cp "$f" /initramfs/tests/
done
n_syssvc=0
for f in /src/repo/build-static/bin/test_syssvc_*; do
    [ -x "$f" ] || continue
    cp "$f" /initramfs/tests/ || exit 4
    n_syssvc=$((n_syssvc + 1))
done
# Same guard the image build carries: without a public-sys$ binary staged,
# the initramfs silently reverts to the kernel-only harness and the run would
# prove nothing about src/libvms.
if [ "$n_syssvc" -lt 1 ]; then
    echo "FATAL: no test_syssvc_* binary staged after the rebuild"
    exit 4
fi

# THE USERSPACE-ONLY imgact SUITES, RE-STAGED TOO (found this round).
# test_imgact_bind / test_imgact_publish are built by the qemu_syssvc_tests
# target above (qemu_syssvc_add_test() plumbing) into build-static/bin, but
# they are named test_imgact_*, NOT test_syssvc_*, so the syssvc loop above
# does not copy them -- and the /src/tests/qemu/test_* loop only copies the
# in-source gcc-built kmod binaries, never these cmake outputs. Before this
# block, a defect injected into libvms/syssvc/imgact_prodreg.c (consumer-
# import-not-bound-to-resident, publish-does-not-populate-registry) was
# recompiled into build-static/bin/test_imgact_* and then NEVER re-staged, so
# the initramfs kept the PRISTINE image-build binary: the mutated suite never
# ran, the control could not go red, and the per-facility gate reported the
# defect as caught while proving nothing (the exact stale-binary failure this
# script's DCL.EXE two-copies note documents for src/vmsdcl -- same class,
# different suite family). Same >=2 guard the image build carries (tests/qemu/
# Dockerfile), for the same reason.
n_imgact=0
for f in /src/repo/build-static/bin/test_imgact_*; do
    [ -x "$f" ] || continue
    cp "$f" /initramfs/tests/ || exit 4
    n_imgact=$((n_imgact + 1))
done
if [ "$n_imgact" -lt 2 ]; then
    echo "FATAL: expected at least 2 test_imgact_* binaries (test_imgact_bind,"
    echo "  test_imgact_publish) staged after the rebuild, found $n_imgact --"
    echo "  a userspace imgact defect would run against a stale pristine binary."
    exit 4
fi
( cd /initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > /initramfs.cpio.gz ) || exit 4
echo "Re-staged initramfs: $(ls -lh /initramfs.cpio.gz | awk '{print $5}')"

exec /run_tests.sh
