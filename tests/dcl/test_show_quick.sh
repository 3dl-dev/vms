#!/bin/bash
# TEST: SHOW quick-win subcommands produce correct VMS-format output
# EXPECT: contains:OVMX
# EXPECT: contains:OVMX-TCPIP
# EXPECT: contains:%SYSTEM-I-NOTMEMBER
# EXPECT: regex:OVMX TCP/IP Services V[0-9]+\.[0-9]+
# EXPECT: contains:Node: OVMX
# EXPECT: contains:Device Error Count Summary
# EXPECT: contains:No errors logged.
# vms-050 / INV-6: SHOW WORKING_SET no longer prints an unconditional
# "Working Set" line. It used to, only because the line was FABRICATED -- a
# hardcoded 8192 quota and a quota*2 extent read from the DCL ctx, not the
# executive. It is now a $GETJPI reader (cmd_show_working_set): on a host with
# no /dev/vms (as here under ctest -- Rule 9) $GETJPI fails and the command
# honestly prints NOTHING rather than a fabricated quota. So "Working Set" is
# no longer asserted here. The POSITIVE proof (real "/Limit=" from a live
# executive) is in tests/qemu/lib/dcl_acceptance_battery.sh, and the
# no-fabrication guard is tests/dcl/test_show_working_set_no_fabrication.sh.
# EXPECT: contains:Accounting is currently disabled.
# EXPECT: contains:SYS$MANAGER:ACCOUNTNG.DAT
# EXPECT: contains:security auditing is currently disabled.
# EXPECT: contains:SYS$MANAGER:AUDIT.LOG
# EXPECT: contains:%SYSTEM-F-NODISKQUOTA
# EXPECT: contains:System root is SYS$SYSDEVICE:[SYS0.SYSCOMMON.]
#
# vms-17d (INV-DCL): SHOW ACCOUNTING now reads a REAL, system-wide,
# PERSISTED flag (ovmx_accounting_is_enabled(), src/libvms/rtl/
# ovmx_accounting.c) instead of a per-process bool that was always freshly
# zero at the start of every vmsdcl invocation. That means this script can
# no longer rely on "disabled" being the incidental default of an
# unwritten per-process struct -- another script in this same suite run
# (sharing the one real /vms mount, see tests/dcl/run_dcl_tests.sh's own
# non-hermetic-ordering note) may have left the flag ENABLED. SET
# ACCOUNTING/DISABLE first to pin the state this script actually asserts,
# the same way test_set_quick.sh pins SET AUDIT/DISABLE before checking it.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'SET ACCOUNTING/DISABLE\nSHOW LICENSE\nSHOW CLUSTER\nSHOW NETWORK\nSHOW ERROR\nSHOW WORKING_SET\nSHOW ACCOUNTING\nSHOW AUDIT\nSHOW QUOTA\nSHOW ROOT\n' | $VMSDCL 2>&1
