#!/bin/bash
# stage_sysvol.sh - assemble the staging tree for an OVMX/NetBSD-vax system
# ODS-2 volume (rd vms-d9c, epic vms-8e8, parent vms-d59).
#
# Produces a Linux directory tree in the rooted+concealed [SYS0.SYSCOMMON]
# layout (ovmx_layout.h) that `tools/vmsfs_master.c master' turns into a
# bootable OVMX system volume. This is the netbsd-vax mirror of the Linux
# distro/Dockerfile.bootable staging step (which stages /system-stage/vms then
# runs `vmsfs_master master /boot/ovmx-distrib.img OVMXSYS /system-stage/vms').
#
# WHAT THE SYSTEM VOLUME CARRIES (vms-d9c deliverable):
#   SYS0/SYSCOMMON/SYSEXE/   the netbsd-vax boot images + the boot data files:
#       DCL.EXE          the marker require_installed_system() gates on, and the
#                        CLI PROVISION.EXE execs on STARTUP.COM
#       PROVISION.EXE    the startup process PID 1 (ovmx_init) execs
#       LOGINOUT.EXE     the image JOB_CONTROL forks per console session
#       JOB_CONTROL.EXE  the END-phase console-login component
#       STARTUP.EXE      the PID-1 image (also installed as /sbin/init off-disk;
#                        present here so the SYSEXE tree matches an installed
#                        volume)
#       SYSUAF.DAT       account database (TEXT, reused verbatim -- arch-neutral)
#       RIGHTSLIST.DAT   rights database (TEXT, reused verbatim)
#       OVMXVMSSYS.PAR   SYSGEN parameter file (LE, no long/pointer fields in
#                        struct sysgen_param -> identical layout on ILP32-LE vax
#                        and LP64-LE host; reused verbatim -- and read_boot_
#                        parameters() falls back to the default node name if it
#                        is ever unreadable, so this is never a boot-halt input)
#   SYS0/SYSCOMMON/SYSMGR/   STARTUP.COM (reused) + the Decision-A
#       SYSTARTUP_VMS.COM (distro/rootfs-vax variant WITHOUT the INSTALL ADD
#       SYS$SHARE:*$SHR.EXE block -- no OVMX shareables exist under vax static
#       linking, and INSTALL.EXE is not in the boot cross-build set; see that
#       file's header) + the other site files (reused)
#   SYS0/SYSCOMMON/SYS$STARTUP/  the STDRV phase + component data files (reused)
#   SYS0/SYSCOMMON/SYSHLP/       HELPLIB.HLP (reused)
#   SYS0/SYSCOMMON/SYSUPD/       PARTS_SETUP.COM (reused)
#
# Everything except the five .EXE images and SYSTARTUP_VMS.COM is copied
# VERBATIM from distro/rootfs/vms -- those files are architecture-independent
# data/DCL (INV-DRIFT: one source of truth, no vax fork of the data).
#
# Usage: stage_sysvol.sh <images-dir> <repo-root> <stage-out-dir>
#   <images-dir> must contain the five ELF32-vax boot images by name:
#     DCL.EXE PROVISION.EXE LOGINOUT.EXE JOB_CONTROL.EXE STARTUP.EXE
set -euo pipefail

IMAGES_DIR="${1:?usage: $0 <images-dir> <repo-root> <stage-out-dir>}"
REPO="${2:?usage: $0 <images-dir> <repo-root> <stage-out-dir>}"
STAGE="${3:?usage: $0 <images-dir> <repo-root> <stage-out-dir>}"

ROOTFS="$REPO/distro/rootfs/vms"
VAX_SYSTARTUP="$REPO/distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"
BOOT_IMAGES="DCL.EXE PROVISION.EXE LOGINOUT.EXE JOB_CONTROL.EXE STARTUP.EXE"

die() { echo "[stage_sysvol] FATAL: $*" >&2; exit 1; }

[ -d "$ROOTFS" ]        || die "distro rootfs tree missing: $ROOTFS"
[ -f "$VAX_SYSTARTUP" ] || die "vax Decision-A SYSTARTUP_VMS.COM missing: $VAX_SYSTARTUP"

rm -rf "$STAGE"
mkdir -p "$STAGE"

# 1. Copy the arch-independent data + DCL tree verbatim (one source of truth).
cp -a "$ROOTFS/." "$STAGE/"

SYSEXE="$STAGE/SYS0/SYSCOMMON/SYSEXE"
SYSMGR="$STAGE/SYS0/SYSCOMMON/SYSMGR"
[ -d "$SYSEXE" ] || die "staged tree has no SYSEXE dir (rootfs layout changed?)"
[ -d "$SYSMGR" ] || die "staged tree has no SYSMGR dir (rootfs layout changed?)"

# 2. Overlay the Decision-A SYSTARTUP_VMS.COM (replaces the Linux one that
#    INSTALL ADDs OVMX shareables which do not exist under vax static linking).
cp "$VAX_SYSTARTUP" "$SYSMGR/SYSTARTUP_VMS.COM"

# The Decision-A discipline is load-bearing: assert the staged file carries no
# INSTALL ADD SYS$SHARE COMMAND line, so a future edit that reintroduces one (or
# an accidental copy of the Linux file) fails HERE, not on a red vax boot. The
# pattern anchors on a DCL command line ("$ " prefix) so the file's own
# explanatory "$!" comments naming the omitted block do not trip it.
if grep -qiE '^\$[[:space:]]+INSTALL[[:space:]]+ADD[[:space:]]+SYS\$SHARE' "$SYSMGR/SYSTARTUP_VMS.COM"; then
    die "staged SYSTARTUP_VMS.COM has an INSTALL ADD SYS\$SHARE command line -- Decision A forbids it on vax"
fi

# 3. Drop the five cross-built ELF32-vax boot images into SYSEXE.
for img in $BOOT_IMAGES; do
    src="$IMAGES_DIR/$img"
    [ -f "$src" ] || die "boot image missing from images dir: $src"
    cp "$src" "$SYSEXE/$img"
done

# 4. Sanity: the boot gate marker (DCL.EXE) and the startup image PID 1 execs
#    (PROVISION.EXE) must be present at the rooted path require_installed_system()
#    / run_startup() will stat -- /vms/SYS0/SYSCOMMON/SYSEXE/{DCL,PROVISION}.EXE.
[ -f "$SYSEXE/DCL.EXE" ]       || die "DCL.EXE not staged at the rooted SYSEXE path"
[ -f "$SYSEXE/PROVISION.EXE" ] || die "PROVISION.EXE not staged at the rooted SYSEXE path"

echo "[stage_sysvol] staged OVMX/NetBSD-vax system tree at $STAGE"
echo "[stage_sysvol]   SYSEXE contents:"
ls -l "$SYSEXE" | sed 's/^/[stage_sysvol]     /'
