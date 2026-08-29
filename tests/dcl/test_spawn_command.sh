#!/bin/bash
# TEST: SPAWN requires the executive -- plain-host ($CREPRC) fails HONESTLY (vms-e9a B0)
#
# SPAWN creates its subprocess through the executive service $CREPRC
# (src/libvms/syssvc/sys_process.c), exactly as real OpenVMS does -- SPAWN ->
# lib$spawn -> $CREPRC, one registered path (the B0 dedup). $CREPRC genuinely
# cannot create a process without the executive, so on this PLAIN-HOST ctest
# (no /dev/vms) SPAWN reports %DCL-F-CREPRC and creates nothing -- it does NOT
# fabricate a subprocess and does NOT print the command's output.
#
# This is the Rule-9-honest behavior (conductor ruling on the record:
# SPAWN-requires-executive). The pre-dedup plain-host "SPAWN_OK" came from an
# UNREGISTERED fork/exec fallback (old cmd_spawn: "no executive: run
# unregistered", dcl_cmd_process.c) -- the INV-6 userspace fake B0 excised.
#
# The POSITIVE proof that SPAWN/lib$spawn actually RUNS a DCL command and
# produces its output lives where it can run against a real executive:
# tests/qemu/test_syssvc_libspawn_reg.c (registration BY NAME + command output).
# EXPECT: contains:%DCL-F-CREPRC
# EXPECT_NOT: contains:SPAWN_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SPAWN WRITE SYS$OUTPUT "SPAWN_OK"\n' | $VMSDCL 2>&1
