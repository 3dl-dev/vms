#!/bin/bash
# TEST: SET DEFAULT to the volume root [000000] — header + device fidelity (vms-272)
#
# Regression for vms-272. `SET DEFAULT sys$disk:[000000]` followed by
# `DIRECTORY` printed the header "Directory SYS$DISK:[VMS]" — the device-root
# mount name reverse-derived from the host path — instead of the VMS spec the
# process default actually names, and it dropped the real device. `[000000]`
# is the Master File Directory (the volume root); the DIRECTORY header must
# reflect it verbatim and the listing must show the top-level files.
#
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY — the
# listing is headed "Directory dev:[dir]"; [000000] names the MFD/volume root.
#
# A DEFINEd logical (TD) stands in for the disk so the test is host-hermetic
# (no dependence on the /vms system-disk mount existing) AND it proves the
# DEVICE is preserved (TD:, not collapsed to SYS$DISK:) — the second half of
# the same defect. The header must never carry the host mount name or lose the
# device, so EXPECT_NOT locks out both "[VMS]" and a "SYS$DISK:[" header.
#
# EXPECT: regex:Directory TD:\[000000\]
# EXPECT: contains:ALPHA.DAT;1
# EXPECT: contains:BETA.DAT;1
# EXPECT: contains:SHOW_DEFAULT_AGREES_OK
# EXPECT: contains:NODEVICE_ROOT_OK
# EXPECT: contains:ODS2_HOMEBLOCK_MFD_OK
# EXPECT_NOT: regex:Directory .*\[VMS\]
# EXPECT_NOT: regex:Directory SYS\$DISK:\[
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/alpha.dat;1" "$TDIR/beta.dat;1"

# --- SET DEFAULT <disk>:[000000] then a bare DIRECTORY ---
# Header must read "Directory TD:[000000]" (not [VMS], not the host path) and
# the two top-level files must be listed.
OUT=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nSHOW DEFAULT\nDIRECTORY\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$OUT"

# SHOW DEFAULT must report the SAME spec the DIRECTORY header shows — the DCL
# default and what the user sees must not diverge.
if printf '%s\n' "$OUT" | grep -Eq '^[[:space:]]*TD:\[000000\]$'; then
    echo "SHOW_DEFAULT_AGREES_OK"
else
    echo "SHOW_DEFAULT_AGREES_BAD"
fi

# --- SET DEFAULT [000000] with NO device must land at the root and keep TD: ---
# (Regression for the "silent no-op" half: a device-less [000000] must resolve
# against the current default's device, not vanish.)
OUT2=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nSET DEFAULT [000000]\nSHOW DEFAULT\nDIRECTORY\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$OUT2"
if printf '%s\n' "$OUT2" | grep -Eq 'Directory TD:\[000000\]'; then
    echo "NODEVICE_ROOT_OK"
else
    echo "NODEVICE_ROOT_BAD"
fi

rm -rf "$TDIR"

# --- SET DEFAULT SYS$DISK:[000000] resolves through the REAL ODS-2 home block --
#
# The ODS-2 runtime flip (vms-496, rung R3): when the live SYS$DISK is a genuine
# ODS-2 volume, SET DEFAULT [000000] and its DIRECTORY reflect the REAL Master
# File Directory sourced from the volume's home block -- NOT a getcwd/opendir
# alias (the substantive half of the vms-272 defect). Proof: the listing carries
# INDEXF.SYS;1, an ODS-2 reserved system file that exists ONLY in a genuine MFD
# and can never come from a POSIX opendir of a host directory. The raw listing
# is captured (not echoed) so its legitimate "Directory SYS$DISK:[000000]"
# header does not trip this file's EXPECT_NOT header guards above.
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
    echo "ODS2_HOMEBLOCK_MFD_BAD (vmsfs_master not found -- cannot build ODS-2 fixture)"
else
    OWORK="$(mktemp -d "${TMPDIR:-/tmp}/setdef_ods2.XXXXXX")"
    OTREE="$OWORK/tree"; OIMG="$OWORK/ods2.img"
    mkdir -p "$OTREE/SYS0"
    printf 'root record\n' > "$OTREE/README.TXT"
    if "$MASTER" --ods2 master "$OIMG" OVMXSYS "$OTREE" 4 >/dev/null 2>&1; then
        ODS2_OUT=$(printf 'SET DEFAULT SYS$DISK:[000000]\nSHOW DEFAULT\nDIRECTORY\n' \
                   | OVMX_SYSDISK_DEV="$OIMG" $VMSDCL 2>&1)
        # SHOW DEFAULT must land at [000000]; the listing must carry the genuine
        # MFD's INDEXF.SYS (proving the real home block, not a host-dir alias).
        if printf '%s\n' "$ODS2_OUT" | grep -Eq '^[[:space:]]*SYS\$DISK:\[000000\]$' \
           && printf '%s\n' "$ODS2_OUT" | grep -q 'INDEXF.SYS;1'; then
            echo "ODS2_HOMEBLOCK_MFD_OK"
        else
            echo "ODS2_HOMEBLOCK_MFD_BAD"
        fi
    else
        echo "ODS2_HOMEBLOCK_MFD_BAD (master --ods2 failed)"
    fi
    rm -rf "$OWORK"
fi
