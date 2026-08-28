#!/bin/bash
# TEST: SHOW CPU reports the primary CPU and the active + configured sets from real system data
#
# LAYOUT IS DCL-DICTIONARY-PINNED (vms-SHOWFID). The public OpenVMS DCL
# Dictionary (SHOW CPU) prints the summary display at COLUMN ZERO -- no indent:
#
#   SOWHAT, A VAX 8800
#   Multiprocessing is ENABLED. Full checking synchronization image loaded.
#   Minimum multiprocessing revision levels: CPU = 0 uCODE = 0 UWCS = 0.
#
#   PRIMARY CPU = 01
#   Active CPUs:      00 01
#   Configured CPUs:  00 01
#
# The heading, Multiprocessing line, PRIMARY CPU and the two CPU-set lines are
# therefore anchored at start-of-line below: a regression that reintroduces the
# old 8-space indent (which VMS never uses) trips the EXPECT_NOT guard. OVMX
# omits the two lines it cannot source -- "Full checking synchronization image
# loaded" and the VAX-microcode "Minimum multiprocessing revision levels" --
# rather than fabricate them (INV-6), so those are not asserted.
# EXPECT: regex:^Multiprocessing is (ENABLED|DISABLED)\.$
# EXPECT: regex:^PRIMARY CPU = [0-9][0-9]$
# EXPECT: regex:^Active CPUs: +[0-9]
# EXPECT: regex:^Configured CPUs: +[0-9]
# EXPECT_NOT: regex:^ +PRIMARY CPU
# EXPECT_NOT: regex:^ +Active CPUs
# EXPECT_NOT: contains:IVKEYW
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW CPU" | $VMSDCL 2>&1
