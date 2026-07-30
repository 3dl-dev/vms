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

echo "--- re-staging the initramfs ---"
cp /src/kernel/vms.ko /initramfs/lib/modules/ || exit 4
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
( cd /initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > /initramfs.cpio.gz ) || exit 4
echo "Re-staged initramfs: $(ls -lh /initramfs.cpio.gz | awk '{print $5}')"

exec /run_tests.sh
