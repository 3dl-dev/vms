#!/bin/bash
# TEST: F$GETJPI self-form completes honestly (no DCL error, no fabrication)
# EXPECT: contains:X =
# EXPECT_NOT: contains:%DCL-
#
# vms-9e2 changed the self form ("", the DCL Dictionary's "current process"
# form) to read the caller's OWN executive row via vms_kif_getjpi_self() --
# the SAME live source SHOW PROCESS reads -- instead of ctx->username. On a
# host with no /dev/vms (where this ctest suite runs, CLAUDE.md Rule 9) that
# read fails honestly (INV-6) and F$GETJPI returns an EMPTY value rather than
# fabricating a name from ctx/getpid. So this host smoke test asserts only
# that the self form runs cleanly and raises no DCL error; the
# EXECUTIVE-sourced value of F$GETJPI("","USERNAME") is proven against a real
# /dev/vms under QEMU (tests/qemu/test_syssvc_ident.c).
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$GETJPI("","USERNAME")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
