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
# DECNETD.EXE (rd vms-c1f): stage_sysvol.sh's BOOT_IMAGES now includes it --
# the DCL acceptance battery's DECnet CTERM section hard-gates on it being on
# the VAX sysvol, so a stand-in is required here too or stage_sysvol.sh dies
# "boot image missing from images dir".
make_image DECNETD.EXE      N  8000
# FAL.EXE (rd vms-d85): the FAL network server process, staged beside it.
make_image FAL.EXE          F  7000
# MAIL_SERVER.EXE (rd vms-47fd): the MAIL-11 network server process, likewise.
make_image MAIL_SERVER.EXE  M  6500
# MAIL.EXE (rd vms-47fd): the MAIL utility that reads delivered mail back.
make_image MAIL.EXE         R  6200
# NCP.EXE (rd vms-5bb5): the DECnet network control program, staged beside it.
make_image NCP.EXE          C  6000

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
         DECNETD.EXE SYSUAF.DAT RIGHTSLIST.DAT OVMXVMSSYS.PAR; do
    [ -f "$OUT/$ROOTED/$f" ] || fail "boot file absent from rooted SYSEXE after round-trip: $ROOTED/$f"
done
cmp -s "$IMAGES/DCL.EXE"       "$OUT/$ROOTED/DCL.EXE"       || fail "DCL.EXE not byte-exact at rooted path"
cmp -s "$IMAGES/PROVISION.EXE" "$OUT/$ROOTED/PROVISION.EXE" || fail "PROVISION.EXE not byte-exact at rooted path"
cmp -s "$IMAGES/DECNETD.EXE"   "$OUT/$ROOTED/DECNETD.EXE"   || fail "DECNETD.EXE not byte-exact at rooted path"
cmp -s "$REPO/distro/rootfs/vms/$ROOTED/SYSUAF.DAT" "$OUT/$ROOTED/SYSUAF.DAT" \
    || fail "SYSUAF.DAT not reused byte-exact"
echo "PASS: DCL.EXE + PROVISION.EXE + DECNETD.EXE + SYSUAF.DAT present + byte-exact at rooted [SYS0.SYSCOMMON.SYSEXE]"

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
# 6. --status-proof (rd vms-b869): the $STATUS proof volume run-boot.sh
#    status-gate boots. The proof SYSTARTUP replaces the Decision-A one, the
#    proof images land in SYS$SYSTEM, and every image the proof RUNs must be
#    staged -- a missing one is refused at staging time, not in a SIMH boot.
# ---------------------------------------------------------------------------
PROOF_IMAGES="$WORK/proof-images"
mkdir -p "$PROOF_IMAGES"
printf '\x7f\x45\x4c\x46\x01\x01STSNORM\n' > "$PROOF_IMAGES/STSNORM.EXE"
printf '\x7f\x45\x4c\x46\x01\x01STSCOND\n' > "$PROOF_IMAGES/STSCOND.EXE"
# Every other image the proof SYSTARTUP RUNs: the native VAX images (rd
# vms-b869) from tests/native-images/vax/, NOTIMG.EXE (a text file, as
# run-boot.sh stages it) and NATIVEACT.EXE (stand-in bytes; the activator).
for name in $(sed -n 's/^\$[[:space:]]*RUN[[:space:]]\{1,\}SYS\$SYSTEM:\([A-Za-z0-9_$]*\).*/\1/p' \
                  "$REPO/tests/lab-vax/SYSTARTUP_VMS_STATUS_PROOF.COM"); do
    [ -f "$PROOF_IMAGES/$name.EXE" ] && continue
    if [ -f "$REPO/tests/native-images/vax/$name.EXE" ]; then
        cp "$REPO/tests/native-images/vax/$name.EXE" "$PROOF_IMAGES/"
    else
        printf 'NOT AN IMAGE %s\n' "$name" > "$PROOF_IMAGES/$name.EXE"
    fi
done
printf '\x7f\x45\x4c\x46\x01\x01NATIVEACT\n' > "$PROOF_IMAGES/NATIVEACT.EXE"
PSTAGE="$WORK/proof-stage"
"$STAGE_SCRIPT" --status-proof "$PROOF_IMAGES" "$IMAGES" "$REPO" "$PSTAGE" >/dev/null \
    || fail "stage_sysvol.sh --status-proof exited non-zero"
cmp -s "$REPO/tests/lab-vax/SYSTARTUP_VMS_STATUS_PROOF.COM" "$PSTAGE/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM" \
    || fail "--status-proof did not stage the proof SYSTARTUP_VMS.COM"
for f in STSNORM.EXE STSCOND.EXE; do
    cmp -s "$PROOF_IMAGES/$f" "$PSTAGE/$ROOTED/$f" || fail "--status-proof did not stage $f byte-exact into SYS\$SYSTEM"
done
[ -f "$PSTAGE/$ROOTED/DCL.EXE" ] || fail "--status-proof dropped the boot images"
PIMG="$WORK/proof.img"
"$MASTER" master "$PIMG" OVMXSYS "$PSTAGE" 32 >/dev/null || fail "vmsfs_master could not master the --status-proof tree"
"$MASTER" list "$PIMG" | grep -qiF 'STSCOND.EXE' || fail "mastered --status-proof volume lacks STSCOND.EXE"
echo "PASS: --status-proof stages the proof SYSTARTUP + proof images and masters"

# Teeth: a proof image the SYSTARTUP RUNs but that is not supplied is refused.
rm -f "$PROOF_IMAGES/STSCOND.EXE"
if "$STAGE_SCRIPT" --status-proof "$PROOF_IMAGES" "$IMAGES" "$REPO" "$WORK/proof-stage2" >/dev/null 2>&1; then
    fail "--status-proof staged a volume whose SYSTARTUP RUNs STSCOND with no STSCOND.EXE"
fi
# Teeth: a proof image may not replace a boot image.
cp "$IMAGES/DCL.EXE" "$PROOF_IMAGES/DCL.EXE"
printf '\x7f\x45\x4c\x46\x01\x01STSCOND\n' > "$PROOF_IMAGES/STSCOND.EXE"
if "$STAGE_SCRIPT" --status-proof "$PROOF_IMAGES" "$IMAGES" "$REPO" "$WORK/proof-stage3" >/dev/null 2>&1; then
    fail "--status-proof let a proof image replace SYS\$SYSTEM:DCL.EXE"
fi
echo "PASS: --status-proof refuses a missing proof image and a proof image that collides with a boot image"

echo "ALL PASS: OVMX/NetBSD-vax system volume masters to a rooted, Decision-A-clean bootable layout"
