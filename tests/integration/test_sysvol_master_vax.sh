#!/usr/bin/env bash
#
# test_sysvol_master_vax.sh - prove the OVMX/NetBSD-vax SYSTEM volume masters
# correctly (rd vms-d9c, epic vms-8e8, parent vms-d59).
#
# This is the HOST, per-PR half of vms-d9c: it exercises the mastering
# MECHANISM (tests/lab-vax/stage_sysvol.sh + tools/vmsfs_master.c) that lays
# down a bootable OVMX system ODS-2 volume for netbsd-vax, WITHOUT the SIMH boot
# (that is the nightly netbsd-vax-sysboot job). vmsfs_master writes little-endian
# vmsfs, which is the on-disk format both the Linux vmsfs.ko and the netbsd-vax
# vmsfs.kmod read, so a host-built vmsfs_master masters a vax-bootable disk
# directly (docs: same tool the Linux Dockerfile.bootable uses).
#
# It proves the two things the mastering step must get right for the boot to
# proceed PAST ovmx_init's installed-system gate and reach the PROVISION.EXE
# exec:
#
#   1. ROOTED LAYOUT ROUND-TRIP. The boot images and data files must land at the
#      rooted+concealed [SYS0.SYSCOMMON.SYSEXE] path (require_installed_system()
#      stats /vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE; run_startup() execs
#      .../PROVISION.EXE). A flat [SYSEXE] layout halts the boot %OVMX-F-SYSINIT
#      (vms-649). Multi-block images and nested directories must round-trip
#      byte-exact: master -> extract -> compare.
#
#   2. DECISION-A DISCIPLINE. The staged SYS$MANAGER:SYSTARTUP_VMS.COM must be
#      the vax variant with NO `INSTALL ADD SYS$SHARE:*$SHR.EXE' block -- those
#      shareables do not exist under vax static linking and INSTALL.EXE is not in
#      the boot cross-build set, so a copy of the Linux file would red the boot
#      (see distro/rootfs-vax/.../SYSTARTUP_VMS.COM's header).
#
# The .EXE images here are STAND-INS (deterministic patterns, image-sized to
# force multi-block retrieval) -- this test does NOT need the vax cross
# toolchain; the REAL cross-built images are mastered + booted by the nightly
# SIMH job. What is under test here is the staging + mastering, not the images.
#
# Usage: test_sysvol_master_vax.sh <vmsfs_master-binary> <repo-root>
set -euo pipefail

MASTER="${1:?usage: $0 <vmsfs_master-binary> <repo-root>}"
REPO="${2:?usage: $0 <vmsfs_master-binary> <repo-root>}"

[ -x "$MASTER" ] || { echo "FAIL: mastering tool not executable: $MASTER" >&2; exit 1; }
[ -d "$REPO/distro/rootfs/vms" ] || { echo "FAIL: repo root has no distro/rootfs/vms: $REPO" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE_SCRIPT="$HERE/../lab-vax/stage_sysvol.sh"
[ -x "$STAGE_SCRIPT" ] || fail "stage_sysvol.sh not executable: $STAGE_SCRIPT"

# ---------------------------------------------------------------------------
# 1. Synthesize a stand-in images dir. Each image gets a DISTINCT, deterministic
#    body several blocks long (so the multi-block retrieval path is exercised),
#    plus an ELF32-vax-looking magic prefix so a reader can tell them apart.
# ---------------------------------------------------------------------------
IMAGES="$WORK/images"
mkdir -p "$IMAGES"
make_image() {  # <name> <fill-byte> <size-bytes>
    local name="$1" fill="$2" size="$3" f="$IMAGES/$1"
    # ELF32 LSB magic prefix (\x7fELF\x01\x01) then a name-tagged fill body.
    printf '\x7f\x45\x4c\x46\x01\x01' > "$f"
    printf 'OVMX-VAX-STANDIN:%s\n' "$name" >> "$f"
    head -c "$size" /dev/zero | tr '\0' "$fill" >> "$f"
}
# Sizes chosen to span several 512-byte blocks each (multi-block retrieval).
make_image DCL.EXE          D 20000
make_image PROVISION.EXE    P 15000
make_image LOGINOUT.EXE     L 12000
make_image JOB_CONTROL.EXE  J 11000
make_image STARTUP.EXE      S  9000

# ---------------------------------------------------------------------------
# 2. Stage the system tree, then master a 32 MB volume from it.
# ---------------------------------------------------------------------------
STAGE="$WORK/stage"
"$STAGE_SCRIPT" "$IMAGES" "$REPO" "$STAGE" >/dev/null || fail "stage_sysvol.sh exited non-zero"

IMG="$WORK/ovmx-sysvol-vax.img"
OUT="$WORK/extract"
"$MASTER" master "$IMG" OVMXSYS "$STAGE" 32 >/dev/null || fail "vmsfs_master master exited non-zero"
"$MASTER" list "$IMG" >"$WORK/list.txt"                || fail "vmsfs_master list exited non-zero"
"$MASTER" extract "$IMG" "$OUT" >/dev/null             || fail "vmsfs_master extract exited non-zero"

# ---------------------------------------------------------------------------
# 3. The whole staged tree must round-trip byte-exact.
# ---------------------------------------------------------------------------
if ! diff -r "$STAGE" "$OUT" >"$WORK/diff.txt" 2>&1; then
    echo "----- system-volume round-trip diff -----" >&2
    cat "$WORK/diff.txt" >&2
    fail "mastered system volume does not round-trip byte-exact"
fi
echo "PASS: staged system tree round-trips master -> extract byte-exact"

# ---------------------------------------------------------------------------
# 4. The boot-critical files must be present at the ROOTED path and byte-exact.
#    (require_installed_system() gates on DCL.EXE; run_startup() execs
#    PROVISION.EXE; both at /vms/SYS0/SYSCOMMON/SYSEXE.)
# ---------------------------------------------------------------------------
ROOTED="SYS0/SYSCOMMON/SYSEXE"
for f in DCL.EXE PROVISION.EXE LOGINOUT.EXE JOB_CONTROL.EXE STARTUP.EXE \
         SYSUAF.DAT RIGHTSLIST.DAT OVMXVMSSYS.PAR; do
    [ -f "$OUT/$ROOTED/$f" ] || fail "boot file absent from rooted SYSEXE after round-trip: $ROOTED/$f"
done
cmp -s "$IMAGES/DCL.EXE"       "$OUT/$ROOTED/DCL.EXE"       || fail "DCL.EXE not byte-exact at rooted path"
cmp -s "$IMAGES/PROVISION.EXE" "$OUT/$ROOTED/PROVISION.EXE" || fail "PROVISION.EXE not byte-exact at rooted path"
cmp -s "$REPO/distro/rootfs/vms/$ROOTED/SYSUAF.DAT" "$OUT/$ROOTED/SYSUAF.DAT" \
    || fail "SYSUAF.DAT not reused byte-exact"
echo "PASS: DCL.EXE + PROVISION.EXE + SYSUAF.DAT present + byte-exact at rooted [SYS0.SYSCOMMON.SYSEXE]"

# The mastered layout must be ROOTED, never flat: a flat [SYSEXE]DCL.EXE (i.e.
# SYSEXE directly under the MFD) is the exact shape that halts %OVMX-F-SYSINIT.
[ ! -e "$OUT/SYSEXE" ] || fail "mastered volume has a FLAT top-level SYSEXE dir -- must be rooted [SYS0.SYSCOMMON.SYSEXE]"
echo "PASS: mastered layout is rooted (no flat top-level SYSEXE)"

# ---------------------------------------------------------------------------
# 5. Decision A: the staged/mastered SYSTARTUP_VMS.COM must carry NO
#    INSTALL ADD SYS$SHARE line, and must be the vax variant.
# ---------------------------------------------------------------------------
SYSTARTUP="$OUT/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"
[ -f "$SYSTARTUP" ] || fail "SYSTARTUP_VMS.COM absent after round-trip"
if grep -qiE '^\$[[:space:]]+INSTALL[[:space:]]+ADD[[:space:]]+SYS\$SHARE' "$SYSTARTUP"; then
    fail "mastered SYSTARTUP_VMS.COM contains an INSTALL ADD SYS\$SHARE line (Decision A violated)"
fi
grep -qiF 'netbsd-vax variant' "$SYSTARTUP" \
    || fail "mastered SYSTARTUP_VMS.COM is not the vax Decision-A variant"
cmp -s "$REPO/distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM" "$SYSTARTUP" \
    || fail "mastered SYSTARTUP_VMS.COM is not byte-exact with the vax Decision-A source"
echo "PASS: Decision-A SYSTARTUP_VMS.COM on the volume (no INSTALL ADD SYS\$SHARE block)"

# Teeth: prove step 5 would CATCH a Linux SYSTARTUP_VMS.COM (which HAS the block)
# -- so this assertion is not vacuously green.
if ! grep -qiE '^\$[[:space:]]+INSTALL[[:space:]]+ADD[[:space:]]+SYS\$SHARE' \
        "$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"; then
    fail "control failed: the Linux SYSTARTUP_VMS.COM no longer has an INSTALL ADD SYS\$SHARE block -- \
this test's Decision-A check can no longer distinguish the two files; re-derive the discipline"
fi
echo "PASS: control -- the Linux SYSTARTUP_VMS.COM DOES carry the block the vax variant drops (check has teeth)"

# ---------------------------------------------------------------------------
# 6. INSTALLER MEDIA (--distribution) -- vms-d0e5.
#
#    Installer media is a BOOTABLE SYSTEM DISK that must additionally be able to
#    RUN THE INSTALL off itself: OVMX$INSTALL.COM does `PRODUCT INSTALL VMS
#    /SOURCE=SYS$UPDATE:<kit>' and then RUNs SYS$SYSTEM:AUTHORIZE.EXE and
#    SYS$SYSTEM:SYSGEN.EXE. The vax media used to carry ONLY the five boot
#    images, so PRODUCT.EXE was not on the volume at all -- PID 1's utility
#    staging (best-effort by design) skipped it, dcl_exec_utility()'s execvp
#    fell through, and the two-disk SIMH install died %PCSI-F-NOIMG with the
#    blank target byte-for-byte untouched. And the media staged its kit under
#    the vax BUILD artifact name (OVMX-OS-VAX.KIT) while the arch-neutral
#    procedure reads OVMX-OS.KIT.
#
#    Both are staging contracts, so both are provable here on the host, per-PR,
#    with no vax toolchain and no SIMH.
# ---------------------------------------------------------------------------
UTILS="PRODUCT.EXE AUTHORIZE.EXE INITIALIZE.EXE SYSGEN.EXE"
make_image PRODUCT.EXE     R 13000
make_image AUTHORIZE.EXE   A 14000
make_image INITIALIZE.EXE  I  8000
make_image SYSGEN.EXE      G 10000

KIT="$WORK/OVMX-OS-VAX.KIT"
head -c 40000 /dev/urandom > "$KIT"

DSTAGE="$WORK/dstage"
"$STAGE_SCRIPT" --distribution --kit "$KIT" "$IMAGES" "$REPO" "$DSTAGE" >/dev/null \
    || fail "stage_sysvol.sh --distribution exited non-zero"

DIMG="$WORK/ovmx-distrib-vax.img"
DOUT="$WORK/dextract"
"$MASTER" master "$DIMG" OVMXSYS "$DSTAGE" 32 >/dev/null || fail "vmsfs_master master (distribution) exited non-zero"
"$MASTER" extract "$DIMG" "$DOUT" >/dev/null             || fail "vmsfs_master extract (distribution) exited non-zero"

for f in $UTILS; do
    [ -f "$DOUT/$ROOTED/$f" ] \
        || fail "installer media is MISSING SYS\$SYSTEM:$f at rooted $ROOTED -- OVMX\$INSTALL.COM cannot run it (%PCSI-F-NOIMG class)"
    cmp -s "$IMAGES/$f" "$DOUT/$ROOTED/$f" || fail "$f not byte-exact at the rooted media path"
done
echo "PASS: installer media carries PRODUCT/AUTHORIZE/INITIALIZE/SYSGEN byte-exact at rooted [SYS0.SYSCOMMON.SYSEXE]"

# The media's kit filename must be EXACTLY the one OVMX$INSTALL.COM reads. Do
# not hard-code it twice: derive it from the procedure itself, so a rename on
# either side fails HERE instead of three minutes into a SIMH install.
INSTALL_COM="$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/OVMX\$INSTALL.COM"
[ -f "$INSTALL_COM" ] || fail "OVMX\$INSTALL.COM not found: $INSTALL_COM"
KIT_SPEC="$(sed -n 's/.*\/SOURCE=SYS\$UPDATE:\([A-Za-z0-9._-]*\).*/\1/p' "$INSTALL_COM" | head -1)"
[ -n "$KIT_SPEC" ] \
    || fail "could not read the /SOURCE=SYS\$UPDATE:<kit> filename out of OVMX\$INSTALL.COM"
[ -f "$DOUT/SYS0/SYSCOMMON/SYSUPD/$KIT_SPEC" ] \
    || fail "installer media has no SYS\$UPDATE:$KIT_SPEC -- OVMX\$INSTALL.COM's PRODUCT INSTALL /SOURCE names a file the media does not carry"
cmp -s "$KIT" "$DOUT/SYS0/SYSCOMMON/SYSUPD/$KIT_SPEC" \
    || fail "the staged kit is not byte-exact with the source kit"
echo "PASS: installer media carries the OS kit byte-exact at SYS\$UPDATE:$KIT_SPEC (the name OVMX\$INSTALL.COM reads)"

# The distribution SYSTARTUP must invoke the menu (already asserted inside
# stage_sysvol.sh) -- assert it on the MASTERED volume too, so a mastering
# regression cannot drop it silently.
grep -qiE '^\$[[:space:]]+@SYS\$MANAGER:OVMX\$INSTALL\.COM' \
     "$DOUT/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM" \
    || fail "mastered installer media does not boot into @SYS\$MANAGER:OVMX\$INSTALL.COM"
echo "PASS: mastered installer media boots into OVMX\$INSTALL.COM"

# TEETH: media that cannot run PRODUCT.EXE must be REFUSED at staging, not
# mastered and shipped short. Without this the section above could be satisfied
# by a stage script that merely copies whatever it happens to find.
rm -f "$IMAGES/PRODUCT.EXE"
if "$STAGE_SCRIPT" --distribution --kit "$KIT" "$IMAGES" "$REPO" "$WORK/dstage-neg" >/dev/null 2>&1; then
    fail "control failed: stage_sysvol.sh --distribution accepted an images dir with NO PRODUCT.EXE -- installer media would ship unable to install"
fi
echo "PASS: control -- --distribution REFUSES media that cannot run PRODUCT.EXE (check has teeth)"

# ...and the DEFAULT (installed-system) mode must be unaffected by the utility
# images: it stages them only when present, so the boot-images-only dir that
# section 2 used still stages and masters exactly as before.
make_image PRODUCT.EXE R 13000     # restore for any later section
DEF2="$WORK/stage-default-2"
for f in $UTILS; do rm -f "$IMAGES/$f"; done
"$STAGE_SCRIPT" "$IMAGES" "$REPO" "$DEF2" >/dev/null \
    || fail "stage_sysvol.sh (default mode, boot images only) exited non-zero"
diff -r "$STAGE" "$DEF2" >/dev/null 2>&1 \
    || fail "default-mode staging changed -- the installer-media utility staging must be present-only, not a new default-mode dependency"
echo "PASS: default (installed-system) staging is byte-for-byte unchanged by the media utility set"

echo "ALL PASS: OVMX/NetBSD-vax system volume masters to a rooted, Decision-A-clean bootable layout, and the installer media carries the utilities + kit the install procedure runs"
