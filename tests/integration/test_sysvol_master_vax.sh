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

echo "ALL PASS: OVMX/NetBSD-vax system volume masters to a rooted, Decision-A-clean bootable layout"
