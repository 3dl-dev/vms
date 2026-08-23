#!/bin/sh
# run-purdy.sh - the cross-width Purdy golden-vector VAX gate (rd vms-b86).
#
# Runs the SEVEN real OpenVMS oracle vectors (docs/oracle/purdy-hash-vectors.md;
# the SAME table tests/libvms/test_purdy.c asserts on the host, LP64) on a real
# NetBSD/vax guest under SIMH -- ILP32 -- and requires every one byte-exact. The
# two together are the cross-width regression gate: they fail if purdy.c's -O0
# workaround for the gcc-vax 13.3.0 -O2 DImode miscompile (rd vms-b86) is removed
# or any width-value divergence returns -- the exact compile-clean/value-diverge
# hole the 3-way build gate cannot see, and the reason VAX login could not
# authenticate until the workaround landed.
#
# Thin wrapper over run-devvms.sh's `purdy' mode: it reuses the identical
# NetBSD/vax cross-build (build-devvms-vax.sh now also builds the static
# elf32-vax vmspurdy), cached-disk, and single-user SIMH boot plumbing, then
# runs vmspurdy off the OVMX CD and asserts PURDY-VAX-PASS (decision on the real
# exit code, never an echoed-console substring). Nothing installed on the host.
#
#   tests/lab-vax/run-purdy.sh
#
HERE="$(cd "$(dirname "$0")" && pwd)"
exec "${HERE}/run-devvms.sh" purdy "$@"
