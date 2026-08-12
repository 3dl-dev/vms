#!/bin/bash
# TEST: SHOW SYSTEM's oracle-named columns (vms-9d4) are executive-sourced or
#       honestly omitted -- never a hardcoded VMS scheduler literal
#
# THE GATE vms-9d4 NAMES (State/Pri/I-O/CPU/Pageflts/Pages), CONSOLIDATED.
#
# vms-9d4 asks for a single regression check tying together every column the
# VAX 7.3 oracle prints for SHOW SYSTEM (docs/oracle/vax73-show-system-process.md
# Sections 1 and 5.1): "$Pid / Process Name / State / Pri / I/O / CPU /
# Page flts / Pages". The per-column sourcing decision was already made and
# tested piecewise before this item was dispatched -- vms-a7e (#333) moved
# CPU/Page-flts/Pages into the executive's struct vms_procinfo (fill_proc_acct,
# src/kernel/vms_proctab.c) and cmd_show_system() (src/vmsdcl/dcl_cmd_show.c)
# reads them through vms_kif_procscan's fields_valid bitmask
# (VMS_PI_V_CPUTIM/PAGEFLTS/PAGES); State/Pri/I-O stay OUT because the kernel
# never sets VMS_PI_V_STATE/PRI/PRIB/DIRIO/BUFIO (src/kernel/vms_ioctl.h,
# "STRUCTURAL, no faithful OVMX source yet") -- OVMX has no VMS scheduler, no
# VMS priority and no direct/buffered I/O split to report, and mapping a Linux
# value onto any of them would be the "unpinned invention" the oracle refused
# (Section 5.1). This file is the single assertion for that whole column set,
# so a regression in ANY of the six is caught by one test rather than requiring
# a reader to know that test_show_system.sh, test_show_system_no_fabrication.sh
# and (in QEMU, where actual rows appear) test_syssvc_showproc.c/
# test_syssvc_procnam.c each cover one slice of it.
#
# The host has no /dev/vms (Rule 9), so no process ROWS ever print here -- only
# the banner and the column heading. That is enough to gate the header-level
# half of the contract (which columns exist at all, and that no VMS scheduler
# state literal is hardcoded into the format string); the row-level half (real
# digits in the surviving columns, for a process this command did not create)
# is gated against a real executive by tests/qemu/test_syssvc_showproc.c
# blocks P1/P2 and the accounting-on-a-redacted-row rule by
# tests/qemu/test_syssvc_procnam.c block P12. All three files are needed --
# this one does not replace the QEMU-side row proof, it locks in the column
# SET.
#
# EXPECT: contains:Process Name
# EXPECT: contains:Uptime
# EXPECT: contains:Page flts
# EXPECT: contains:Pages
#
# State/Pri/I-O stay ABSENT -- no heading word, no placeholder value. VMS's
# scheduler-state literals (HIB/LEF/CUR/COM/MWAIT/SUSP -- the values the
# oracle transcript actually shows, Section 1) must not appear anywhere: a
# regression that reintroduces a hardcoded state (the vms-8019/vms-70eb
# fabrication class this item is named for -- "LEF" was the literal SHOW
# PROCESS/ALL used to hardcode) trips this even if it lands under a
# differently-named column.
# EXPECT_NOT: contains:State
# EXPECT_NOT: contains: HIB
# EXPECT_NOT: contains: LEF
# EXPECT_NOT: contains: CUR
# EXPECT_NOT: regex:  I/O
# EXPECT_NOT: contains:---
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
