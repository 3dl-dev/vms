#!/bin/bash
# TEST: MOUNT/DISMOUNT are real mount(2)/umount(2) plumbing through the
#       executive (vms-651) -- honest-failure path, exercised here where
#       ctest runs and no /dev/vms exists.
# EXPECT: contains:DCL-ALIVE
# EXPECT: contains:$STATUS = "%X000002A4"
# EXPECT_NOT: contains:%MOUNT-I-MOUNTED
# EXPECT_NOT: contains:%DISMOUNT-I-DISMOUNTED
# EXPECT_NOT: contains:DUA0:
# EXPECT_NOT: contains:TESTDISK
#
# WHAT USED TO BE HERE, and why it is gone (docs/design-vms-faithful-install.md
# sec 3.1/3.3). cmd_mount never called mount(2): it wrote a per-process
# userspace device table (struct vms_device / vms_device_table[],
# src/vmsdcl/dcl_builtin.c -- deleted with this rewrite), used getcwd() as
# the "mount path", and printed %MOUNT-I-MOUNTED unconditionally. This test
# used to assert THAT unconditional success. It cannot any more: MOUNT now
# resolves the unit through the executive (vms_kif_disk_resolve, vms-3e8),
# checks PRV$M_MOUNT through the executive (vms_kif_chkpriv, not getuid()),
# and only then mount(2)s -- none of which this environment has, so the
# ONLY thing checkable here (CLAUDE.md Rule 9) is that MOUNT fails HONESTLY
# rather than fabricating success. The real mount/dismount/persistence path
# needs a real /dev/vms and a real disk and is proven in QEMU instead --
# tests/qemu/test_mount_e2e.sh.
#
# THE POSITIVE ANCHOR IS $STATUS, NOT A PRINTED MESSAGE, same reasoning as
# tests/dcl/test_show_device.sh. With no /dev/vms, vms_kif_open() fails and
# every ioctl this process issues fails too (EBADF), which
# src/libvmssys/vms_kif.c's vms_kif_kerr_to_ss() maps to SS$_BUGCHECK (676)
# by default. cmd_mount's FIRST executive call is vms_kif_chkpriv(PRV$M_MOUNT)
# -- before it ever tries to resolve the unit or touch a mount point -- so
# that is where the failure happens, and it is silent (Rule 10: an
# ioctl-level failure is not "the executive did not answer", the same
# unreachable-in-product condition cmd_show_device's default: case
# documents; nothing is rendered, $STATUS carries it).
#
# `WRITE SYS$OUTPUT "DCL-ALIVE"` is the liveness anchor: it never touches the
# executive, so it always leaves $STATUS = 1. A later $STATUS = 676 can only
# be MOUNT's.
#
# THE NEGATIVE ANCHORS keep the FACADE'S OWN VOCABULARY out for good: DUA0:
# and TESTDISK never appear anywhere in this transcript now that MOUNT does
# not fabricate a row for them, and %MOUNT-I-MOUNTED / %DISMOUNT-I-DISMOUNTED
# are the exact facade success lines this item deleted.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WRITE SYS$OUTPUT "DCL-ALIVE"\nMOUNT DKA100: WORK\nSHOW SYMBOL $STATUS\nDISMOUNT DKA100:\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
