#!/bin/bash
# run_cluster_fork_hammer_60s.sh - drive the FC-P0.16 R3 same-CPU hammer
# (test_kmod_cluster_fork_hammer.c) for the FULL 60s design-doc figure (design
# docs/design-faithful-cluster-executive.md SS3.2.3 RULING, plan
# docs/plan-faithful-cluster-executive.md FC-P0.16's R3 done-condition: "a
# process-context poster loop pinned to the receiving CPU under a 0x6007 flood
# for 60 s -- no lockup, no panic, no lost wake").
#
# WHY THIS IS A SEPARATE SCRIPT, NOT PART OF run_tests.sh's DEFAULT BATTERY.
# run_tests.sh shares ONE ~600s wall across ~77 suites in a single QEMU boot
# (see that script's own header comment); a genuine 60s hammer wired into the
# default per-PR battery would cost 10% of that wall on every PR. The SAME
# mechanism -- same knob, same kthreads, same rxlock -- runs a real, short
# proof by default (test_kmod_cluster_fork_hammer.c with no cmdline override:
# the kernel-side default, 3000ms) inside that default battery; THIS script
# re-boots the identical already-built kernel+initramfs with
# OVMX_HAMMER_MS=60000 on the kernel command line (ovmx_hammer_ms=60000,
# test_kmod_cluster_fork_hammer.c's own /proc/cmdline read) so the full 60s
# figure runs from a deliberate, non-default invocation.
#
# USAGE (run INSIDE the container built from tests/qemu/Dockerfile, exactly
# where run_tests.sh itself runs -- this is a thin wrapper, not a second
# image):
#   tests/qemu/run_cluster_fork_hammer_60s.sh
#
# Raises the wall budget to comfortably cover the 60s hammer plus every other
# suite in the shared boot (KE_WALL_TIMEOUT), then delegates entirely to
# run_tests.sh with OVMX_HAMMER_MS set -- no duplicated QEMU invocation, no
# duplicated disk-fixture staging, so this can never drift from what
# run_tests.sh actually boots.

set -euo pipefail

export OVMX_HAMMER_MS="${OVMX_HAMMER_MS:-60000}"
export KE_WALL_TIMEOUT="${KE_WALL_TIMEOUT:-720}"

echo "=== FC-P0.16 R3 same-CPU hammer: full ${OVMX_HAMMER_MS}ms run ==="
echo "    (delegating to run_tests.sh with ovmx_hammer_ms=$OVMX_HAMMER_MS on the kernel cmdline;"
echo "     KE_WALL_TIMEOUT=$KE_WALL_TIMEOUT covers the hammer plus the rest of the shared boot)"

exec "$(dirname "$0")/run_tests.sh" "$@"
