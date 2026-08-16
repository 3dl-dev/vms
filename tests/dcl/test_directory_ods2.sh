#!/bin/bash
# TEST: DIRECTORY / SET DEFAULT resolve through a GENUINE ODS-2 SYS$DISK (vms-496, R3)
#
# The ODS-2 runtime flip (epic vms-5eb, rung R3, docs/design-ods2-runtime-flip.md)
# reroutes DCL DIRECTORY / SET DEFAULT off the POSIX /vms passthrough onto the
# genuine-ODS-2 volume handle (the ods2_sysdisk read adapter). This test proves
# the reroute reads a REAL Files-11 volume -- the Master File Directory (4,4,0),
# INDEXF.SYS and the other reserved system files, and per-file File IDs from the
# real FID chains -- NOT a POSIX opendir() of a host directory.
#
# GROUND TRUTH is the genuine ODS-2 reader itself: `vmsfs_master --ods2 list`
# walks the same image back over the raw block device (ods2_bdev_*), and this
# test asserts the DIRECTORY listing's File IDs are byte-for-byte the reader's.
#
# The three proofs that this is the ODS-2 path and NOT POSIX opendir:
#   1. DIRECTORY [000000] lists INDEXF.SYS;1 / BITMAP.SYS;1 -- the reserved
#      ODS-2 system files, which exist ONLY in a genuine MFD and can never come
#      from an opendir() of the /vms tree.
#   2. DIRECTORY/FULL prints a genuine "File ID: (n,n,n)" that EQUALS the ODS-2
#      reader's File ID for that file -- the POSIX passthrough has none and omits
#      the line entirely (the vms-272 defect this rung structurally closes).
#   3. With NO ODS-2 volume registered the same command fails HONESTLY with
#      %SYSTEM-E-DEVNOTMOUNT (Rule 9 / INV-6) -- never a silent POSIX fallback.
#
# And the reroute is scoped: a NON-SYS$DISK directory (a DEFINEd logical to a
# host temp dir) still lists through POSIX and carries NO File ID.
#
# EXPECT: contains:ODS2_MFD_RESERVED_FILES_OK
# EXPECT: contains:FID_MATCHES_ODS2_READER_OK
# EXPECT: contains:SET_DEFAULT_MFD_OK
# EXPECT: contains:DEVNOTMOUNT_HONEST_OK
# EXPECT: contains:POSIX_PASSTHROUGH_OK
# EXPECT: regex:File ID:  \([0-9]+,[0-9]+,[0-9]+\)
# EXPECT_NOT: contains:ODS2_TEST_INFRA_MISSING

set -u

VMSDCL="${VMSDCL:-vmsdcl}"

# vmsfs_master (the genuine ODS-2 master + reader tool) is on PATH via the test
# harness's build/bin entry. Locate it robustly; a hard, LOUD failure if absent
# (Rule 7 -- an absent tool is a failing test, never a silent skip).
MASTER=""
if command -v vmsfs_master >/dev/null 2>&1; then
    MASTER="$(command -v vmsfs_master)"
else
    for c in ./build/bin/vmsfs_master ../build/bin/vmsfs_master \
             "$(dirname "${VMSDCL}")/vmsfs_master"; do
        [ -x "$c" ] && { MASTER="$c"; break; }
    done
fi
if [ -z "$MASTER" ]; then
    echo "ODS2_TEST_INFRA_MISSING: vmsfs_master not found (needed to build the ODS-2 fixture)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/dir_ods2.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM
TREE="$WORK/tree"
IMG="$WORK/ods2.img"

# --- Build a genuine ODS-2 volume from a small system-disk-shaped tree. ------
mkdir -p "$TREE/SYS0/SYSCOMMON/SYSEXE"
printf '$ SET NOON\n$ WRITE SYS$OUTPUT "hi"\n' > "$TREE/SYS0/SYSCOMMON/SYSEXE/LOGIN.COM"
head -c 2048 /dev/urandom                      > "$TREE/SYS0/SYSCOMMON/SYSEXE/DCL.EXE"
if ! "$MASTER" --ods2 master "$IMG" OVMXSYS "$TREE" 4 > "$WORK/master.out" 2>&1; then
    echo "ODS2_TEST_INFRA_MISSING: master --ods2 failed"; cat "$WORK/master.out"; exit 1
fi

# --- Ground truth from the genuine ODS-2 reader. ----------------------------
"$MASTER" --ods2 list "$IMG" > "$WORK/reader.out" 2>&1 || {
    echo "ODS2_TEST_INFRA_MISSING: list --ods2 failed"; cat "$WORK/reader.out"; exit 1; }
echo "=== genuine ODS-2 reader (vmsfs_master --ods2 list) ==="
cat "$WORK/reader.out"

SYSEXE='SYS$DISK:[SYS0.SYSCOMMON.SYSEXE]'

# --- 1. DIRECTORY [000000] lists the genuine MFD reserved system files. -----
echo "=== DIRECTORY SYS\$DISK:[000000] (through ODS-2) ==="
MFD_OUT="$(printf 'DIRECTORY SYS$DISK:[000000]\n' | OVMX_SYSDISK_DEV="$IMG" "$VMSDCL" 2>&1)"
printf '%s\n' "$MFD_OUT"
if printf '%s\n' "$MFD_OUT" | grep -q 'INDEXF.SYS;1' \
   && printf '%s\n' "$MFD_OUT" | grep -q 'BITMAP.SYS;1'; then
    echo "ODS2_MFD_RESERVED_FILES_OK"
else
    echo "ODS2_MFD_RESERVED_FILES_BAD (no INDEXF.SYS/BITMAP.SYS -- not a genuine MFD)"
fi

# --- 2. DIRECTORY/FULL File ID EQUALS the ODS-2 reader's File ID. ------------
echo "=== DIRECTORY/FULL ${SYSEXE} (genuine File IDs) ==="
FULL_OUT="$(printf 'DIRECTORY/FULL %s\n' "$SYSEXE" | OVMX_SYSDISK_DEV="$IMG" "$VMSDCL" 2>&1)"
printf '%s\n' "$FULL_OUT"

# DCL /FULL line: "DCL.EXE;1                File ID:  (14,1,0)"
DCL_FID="$(printf '%s\n' "$FULL_OUT" | sed -n 's/.*DCL\.EXE;1.*File ID:  (\([0-9,]*\)).*/\1/p' | head -1)"
# Reader line:    "[SYS0.SYSCOMMON.SYSEXE]DCL.EXE;1  (14,1,0)"
RDR_FID="$(sed -n 's/.*DCL\.EXE;1  (\([0-9,]*\)).*/\1/p' "$WORK/reader.out" | head -1)"
echo "DCL /FULL File ID for DCL.EXE;1 = (${DCL_FID}) ; ODS-2 reader = (${RDR_FID})"
if [ -n "$DCL_FID" ] && [ "$DCL_FID" = "$RDR_FID" ]; then
    echo "FID_MATCHES_ODS2_READER_OK"
else
    echo "FID_MATCHES_ODS2_READER_BAD"
fi

# --- 3. SET DEFAULT to the real MFD reflects it (sourced from the home block). --
echo "=== SET DEFAULT SYS\$DISK:[000000] then SHOW DEFAULT ==="
SD_OUT="$(printf 'SET DEFAULT SYS$DISK:[000000]\nSHOW DEFAULT\n' | OVMX_SYSDISK_DEV="$IMG" "$VMSDCL" 2>&1)"
printf '%s\n' "$SD_OUT"
# It must land at [000000] (not error out), AND a bogus ODS-2 directory must be
# rejected -- both decided by the genuine volume, not a POSIX stat.
BAD_OUT="$(printf 'SET DEFAULT SYS$DISK:[NOSUCHDIR]\n' | OVMX_SYSDISK_DEV="$IMG" "$VMSDCL" 2>&1)"
if printf '%s\n' "$SD_OUT" | grep -Eq '^[[:space:]]*SYS\$DISK:\[000000\]$' \
   && printf '%s\n' "$BAD_OUT" | grep -q 'invalid directory'; then
    echo "SET_DEFAULT_MFD_OK"
else
    echo "SET_DEFAULT_MFD_BAD"
    printf 'bogus-dir result: %s\n' "$BAD_OUT"
fi

# --- 4. FAIL-HONEST: no ODS-2 volume registered -> %SYSTEM-E-DEVNOTMOUNT. ----
echo "=== DIRECTORY SYS\$DISK:[000000] with NO volume registered (fail-honest) ==="
NM_OUT="$(env -u OVMX_SYSDISK_DEV bash -c "printf 'DIRECTORY SYS\$DISK:[000000]\n' | '$VMSDCL'" 2>&1)"
printf '%s\n' "$NM_OUT"
if printf '%s\n' "$NM_OUT" | grep -q 'DEVNOTMOUNT'; then
    echo "DEVNOTMOUNT_HONEST_OK"
else
    echo "DEVNOTMOUNT_HONEST_BAD (silent POSIX fallback -- forbidden by Rule 9/INV-6)"
fi

# --- 5. Scope: a NON-SYS$DISK directory still lists through POSIX, no File ID. --
echo "=== DIRECTORY of a DEFINEd logical to a host dir (POSIX passthrough) ==="
TT="$(mktemp -d "${TMPDIR:-/tmp}/dir_posix.XXXXXX")"
touch "$TT/alpha.dat;1" "$TT/beta.dat;1"
TT_OUT="$(printf 'DEFINE TT "%s"\nDIRECTORY/FULL TT:[000000]\n' "$TT" | OVMX_SYSDISK_DEV="$IMG" "$VMSDCL" 2>&1)"
printf '%s\n' "$TT_OUT"
rm -rf "$TT"
# Lists the host files, and (INV-6) carries NO fabricated File ID for them.
if printf '%s\n' "$TT_OUT" | grep -q 'ALPHA.DAT;1' \
   && ! printf '%s\n' "$TT_OUT" | grep -q 'File ID:'; then
    echo "POSIX_PASSTHROUGH_OK"
else
    echo "POSIX_PASSTHROUGH_BAD"
fi
