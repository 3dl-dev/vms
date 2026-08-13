#!/bin/bash
# TEST: SET TERMINAL /WIDTH=132 is accepted, and does not make SHOW TERMINAL
#       report a terminal the executive never gave it
# EXPECT: contains:$STATUS = "%X00000001"
# EXPECT_NOT: contains:Width: 132
# EXPECT_NOT: contains:Terminal Characteristics:
#
# WHAT CHANGED AND WHY (vms-d0b). This file used to require SHOW TERMINAL to
# print "Width: 132" after SET TERMINAL /WIDTH=132, under ctest, with no
# /dev/vms. Both halves lived in struct dcl_context: SET TERMINAL wrote this
# process's own copy and SHOW TERMINAL read it back. The round trip was real
# and the terminal was not -- no other process could see either half.
#
# SHOW TERMINAL now reads the executive's device row ($GETJPI for which
# terminal, then $GETDVI for what it is), so with no executive it prints
# nothing at all and this assertion cannot be made here. The width round trip
# through a SHARED device is proven where it can be: tests/qemu/
# test_syssvc_showterm.c changes the console's width from a SECOND process
# through the executive and requires SHOW TERMINAL, in a third, to report it
# -- and to report the old value again when it is put back.
#
# WHAT IS STILL ASSERTED HERE, so this is not a deletion: SET TERMINAL /WIDTH
# is parsed and ACCEPTED ($STATUS = 1, DCL's own success), and it does not
# smuggle a terminal display back in. $STATUS is what stops the EXPECT_NOTs
# passing vacuously -- a DCL that rejected the qualifier, or never ran, would
# satisfy them too.
#
# KNOWN GAP, RECORDED RATHER THAN PAPERED OVER: SET TERMINAL is now a writer
# with no executive-visible effect. On VMS the writer is $QIO IO$_SETMODE on a
# channel to the device (src/kernel/vms_ioctl.h says so at the ioctl), and
# MEASURED under vms-d0b, sys$assign("OPA0:") returns SS$_NOSUCHDEV: the
# public $ASSIGN/$QIO path resolves a small hardcoded name list to Linux paths
# and does not reach the executive's device table at all. Converting SET
# TERMINAL by calling the ioctl wrapper directly would route the writer around
# the very interface VMS uses, so it waits on the $ASSIGN/$QIO work rather
# than being half-done here.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SET TERMINAL /WIDTH=132\nSHOW SYMBOL $STATUS\nSHOW TERMINAL\n' | $VMSDCL 2>&1
