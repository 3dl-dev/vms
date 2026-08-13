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
