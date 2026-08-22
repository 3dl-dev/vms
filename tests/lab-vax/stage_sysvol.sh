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
#       SYSUAF.DAT       account database (binary $UAFDEF indexed Files-11 file;
#                        arch-neutral by construction -- the LE codec serializes
#                        every on-disk field via le16/le32/le64 over uint8[] byte
#                        arrays, so the same bytes on ILP32-LE vax and LP64-LE host;
#                        reused verbatim. NOT text -- do NOT revert the seed to ASCII)
#       RIGHTSLIST.DAT   rights database (binary $RDBDEF indexed; arch-neutral, reused verbatim)
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
# TWO STAGING MODES (vms-4834 rung F, docs/design-vax-installer.md sec5.6):
#
#   DEFAULT (installed system volume) -- staged exactly as before. Byte-for-byte
#     unchanged: the same tree an already-installed OVMX/NetBSD-vax system disk
#     carries, with the plain Decision-A SYSTARTUP_VMS.COM and no OS kit. This
#     is what the nightly SIMH sysboot job masters and boots.
#
#   --distribution (installer media) -- the netbsd-vax mirror of the x86_64
#     install-media disk (distro/Dockerfile.bootable's "Install-media
#     distribution disk" stage, vms-dcf). SAME system tree, differing ONLY in
#     its payload ("the distribution disk differs from an installed system disk
#     ONLY IN ITS PAYLOAD", design-vms-faithful-install.md sec3.3):
#       (a) the VAX OS kit (OVMX-OS-VAX.KIT, product identity "OVMX VAXVMS VMS",
#           packed by tools/cross-vax/build-os-kit-vax.sh via tools/ovmx_kit_pack.c)
#           is laid down at SYS$UPDATE:OVMX-OS-VAX.KIT -- mirroring the x86_64
#           `cp /boot/ovmx-os.kit .../SYSUPD/OVMX-OS.KIT` step; and
#       (b) the DISTRIBUTION SYSTARTUP_VMS.COM
#           (distro/rootfs-distrib-only-vax/...) -- the Decision-A variant PLUS
#           the single `$ @SYS$MANAGER:OVMX$INSTALL.COM` menu block -- is staged
#           instead of the plain Decision-A one, so the mastered volume boots
#           into the install menu (OVMX$INSTALL.COM, reused byte-for-byte from
#           the arch-neutral SYSMGR tree) rather than a normal startup.
#     Mastered by vmsfs_master into ovmx-distrib-vax.img (the mastering is the
#     caller's step, e.g. tests/lab-vax/run-boot.sh / the two-disk lab harness,
#     rung G -- this script only produces the staged tree).
#
# Usage: stage_sysvol.sh [--distribution --kit <OVMX-OS-VAX.KIT>] \
#                        <images-dir> <repo-root> <stage-out-dir>
#   <images-dir> must contain the five ELF32-vax boot images by name:
#     DCL.EXE PROVISION.EXE LOGINOUT.EXE JOB_CONTROL.EXE STARTUP.EXE
#   --distribution           stage the installer-media shape (see above)
#   --kit <file>             path to the pre-built OVMX-OS-VAX.KIT to lay at
#                            SYS$UPDATE: (REQUIRED with --distribution; the kit
#                            is produced separately by build-os-kit-vax.sh --
#                            this script is staging glue, not a kit builder,
#                            mirroring the x86_64 split of kit-build vs. staging)
set -euo pipefail

DISTRIBUTION=0
KIT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --distribution) DISTRIBUTION=1; shift ;;
        --kit)          KIT="${2:?--kit requires a path to OVMX-OS-VAX.KIT}"; shift 2 ;;
        --kit=*)        KIT="${1#--kit=}"; shift ;;
        --)             shift; break ;;
        -*)             echo "[stage_sysvol] FATAL: unknown flag: $1" >&2; exit 1 ;;
        *)              break ;;
    esac
done

IMAGES_DIR="${1:?usage: $0 [--distribution --kit <kit>] <images-dir> <repo-root> <stage-out-dir>}"
REPO="${2:?usage: $0 [--distribution --kit <kit>] <images-dir> <repo-root> <stage-out-dir>}"
STAGE="${3:?usage: $0 [--distribution --kit <kit>] <images-dir> <repo-root> <stage-out-dir>}"

ROOTFS="$REPO/distro/rootfs/vms"
VAX_SYSTARTUP="$REPO/distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"
VAX_DISTRIB_SYSTARTUP="$REPO/distro/rootfs-distrib-only-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"
BOOT_IMAGES="DCL.EXE PROVISION.EXE LOGINOUT.EXE JOB_CONTROL.EXE STARTUP.EXE"
KIT_DEST_NAME="OVMX-OS-VAX.KIT"

die() { echo "[stage_sysvol] FATAL: $*" >&2; exit 1; }

[ -d "$ROOTFS" ]        || die "distro rootfs tree missing: $ROOTFS"
[ -f "$VAX_SYSTARTUP" ] || die "vax Decision-A SYSTARTUP_VMS.COM missing: $VAX_SYSTARTUP"

# In distribution mode, select the distribution SYSTARTUP (Decision-A + the
# install-menu block) and require the caller-built kit up front.
SYSTARTUP_SRC="$VAX_SYSTARTUP"
if [ "$DISTRIBUTION" -eq 1 ]; then
    [ -f "$VAX_DISTRIB_SYSTARTUP" ] || die "vax distribution SYSTARTUP_VMS.COM missing: $VAX_DISTRIB_SYSTARTUP"
    [ -n "$KIT" ]  || die "--distribution requires --kit <OVMX-OS-VAX.KIT> (build it with tools/cross-vax/build-os-kit-vax.sh)"
    [ -f "$KIT" ]  || die "kit file does not exist: $KIT"
    SYSTARTUP_SRC="$VAX_DISTRIB_SYSTARTUP"
fi

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
#    In --distribution mode this is the distribution variant (Decision-A + the
#    install-menu block); otherwise the plain installed-target Decision-A one.
cp "$SYSTARTUP_SRC" "$SYSMGR/SYSTARTUP_VMS.COM"

# The Decision-A discipline is load-bearing: assert the staged file carries no
# INSTALL ADD SYS$SHARE COMMAND line, so a future edit that reintroduces one (or
# an accidental copy of the Linux file) fails HERE, not on a red vax boot. The
# pattern anchors on a DCL command line ("$ " prefix) so the file's own
# explanatory "$!" comments naming the omitted block do not trip it. This holds
# for BOTH variants -- the distribution one is Decision-A too, it only adds the
# menu block.
if grep -qiE '^\$[[:space:]]+INSTALL[[:space:]]+ADD[[:space:]]+SYS\$SHARE' "$SYSMGR/SYSTARTUP_VMS.COM"; then
    die "staged SYSTARTUP_VMS.COM has an INSTALL ADD SYS\$SHARE command line -- Decision A forbids it on vax"
fi

# The distribution SYSTARTUP MUST invoke the install menu; the default one MUST
# NOT (the "differs only in its payload" contract cuts both ways -- an installed
# target that dropped into the menu would be the vms-dcf x86_64 CI regression).
# Anchor on the DCL command line so the distribution file's own "$!" comments
# naming the block do not satisfy the default-mode check.
if [ "$DISTRIBUTION" -eq 1 ]; then
    grep -qiE '^\$[[:space:]]+@SYS\$MANAGER:OVMX\$INSTALL\.COM' "$SYSMGR/SYSTARTUP_VMS.COM" \
        || die "distribution SYSTARTUP_VMS.COM does not invoke @SYS\$MANAGER:OVMX\$INSTALL.COM"
else
    if grep -qiE '^\$[[:space:]]+@SYS\$MANAGER:OVMX\$INSTALL\.COM' "$SYSMGR/SYSTARTUP_VMS.COM"; then
        die "default-mode SYSTARTUP_VMS.COM invokes the install menu -- an installed target must reach login with no menu"
    fi
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

# 4b. [USERS] and [SYSTMP] -- the two SYSTEM-writable persistent system-disk
#     directories (vms-329). distro/Dockerfile.bootable creates exactly these
#     two in /system-stage/vms before mastering ovmx-distrib.img and gates on
#     "]USERS.DIR;" being in the mastered listing; they are absent from
#     distro/rootfs/vms because git cannot carry an empty directory, so every
#     mastering step must create them itself. This one did not, and the vax
#     volume therefore shipped with no [USERS].
#
#     WHY IT ONLY SURFACED NOW. PROVISION's home-directory pass used to lchown()
#     a /vms passthrough path, and a missing parent came back ENOENT and was
#     silently swallowed -- it "provisioned" four home directories that did not
#     exist and said nothing. Post-cutover the ACP arm resolves the parent DID
#     for real and reports the truth:
#         %OVMX-W-OWNER, home directory SYS$SYSDEVICE:[USERS.DEFAULT] did not
#                        resolve over the ACP (parent missing?)
#     (x4: DEFAULT/GUEST/USER1/USER2). That is the fake-success being removed,
#     not a new defect -- the media was always short a directory. With [USERS]
#     present PROVISION CREATEs each home under it over IO$_CREATE, owned by the
#     account's UIC. vmsfs_master masters directories SYSTEM-owned [1,4] with
#     VMSFS_PROT_DEFAULT (tools/vmsfs_master.c), the ownership+protection
#     SYS$SCRATCH / SYS$LOGIN need.
mkdir -p "$STAGE/USERS" "$STAGE/SYSTMP"
[ -d "$STAGE/USERS" ]  || die "[USERS] not staged"
[ -d "$STAGE/SYSTMP" ] || die "[SYSTMP] not staged"

# 5. Distribution mode only: lay the VAX OS kit at SYS$UPDATE:OVMX-OS-VAX.KIT.
#    SYS$UPDATE is [SYS0.SYSCOMMON.SYSUPD] (DEFINEd in STARTUP.COM), the standard
#    VMS home for layered-product install kits -- the exact path OVMX$INSTALL.COM
#    reads the payload from. Mirrors distro/Dockerfile.bootable's
#    `cp /boot/ovmx-os.kit .../SYSUPD/OVMX-OS.KIT` step, landing the kit as an
#    ordinary SYSTEM-owned file on the already-mounted system disk (no separate
#    raw device needed).
if [ "$DISTRIBUTION" -eq 1 ]; then
    SYSUPD="$STAGE/SYS0/SYSCOMMON/SYSUPD"
    mkdir -p "$SYSUPD"
    cp "$KIT" "$SYSUPD/$KIT_DEST_NAME"
    [ -f "$SYSUPD/$KIT_DEST_NAME" ] || die "OS kit not staged at SYS\$UPDATE:$KIT_DEST_NAME"
    cmp -s "$KIT" "$SYSUPD/$KIT_DEST_NAME" || die "staged OS kit is not byte-exact with the source kit"
fi

echo "[stage_sysvol] staged OVMX/NetBSD-vax system tree at $STAGE"
if [ "$DISTRIBUTION" -eq 1 ]; then
    echo "[stage_sysvol]   MODE: --distribution (installer media)"
    echo "[stage_sysvol]   SYS\$UPDATE:$KIT_DEST_NAME staged (OS kit): $(ls -l "$STAGE/SYS0/SYSCOMMON/SYSUPD/$KIT_DEST_NAME" | awk '{print $5}') bytes"
    echo "[stage_sysvol]   SYSTARTUP_VMS.COM invokes @SYS\$MANAGER:OVMX\$INSTALL.COM (boots into the install menu)"
else
    echo "[stage_sysvol]   MODE: default (installed system volume)"
fi
echo "[stage_sysvol]   SYSEXE contents:"
ls -l "$SYSEXE" | sed 's/^/[stage_sysvol]     /'
