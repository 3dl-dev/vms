#!/bin/bash
# TEST: DIRECTORY [SUB] resolves against the current default's DEVICE (vms-eb8)
#
# Regression for vms-eb8. After SET DEFAULT onto a non-root directory of some
# device, a device-less bracketed argument — "DIRECTORY [SUB]" — must list SUB
# on the CURRENT DEFAULT's device, not the system-disk volume root. The bug:
# dcl_resolve_path() passed a device-less "[SUB]" straight to vmsfs, whose
# device-less path resolves the directory against the SYSDISK_MOUNT root
# (/vms) — so the listing came from the wrong volume entirely. vms-272 fixed
# the DIRECTORY *header* device defaulting but NOT the *listing* resolution.
#
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY — when
# the device is omitted the listing is taken relative to the current default;
# VSI OpenVMS User's Manual, "File Specifications" — an omitted device field
# defaults to the current default device.
#
# A DEFINEd logical (TD) stands in for the disk so the test is host-hermetic
# (no dependence on the /vms system-disk mount existing) AND proves the DEVICE
# is inherited: [SUB] must resolve to TD:[SUB] (= $TDIR/SUB), which contains
# INSUB.DAT, and NOT to the /vms root's SUB. The listing CONTENT is the
# discriminator — the header already defaults its device since vms-272, so a
# buggy build still prints "Directory TD:[SUB]" but lists nothing from there.
#
# EXPECT: regex:Directory TD:\[SUB\]
# EXPECT: contains:INSUB.DAT;1
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
mkdir -p "$TDIR/SUB"
touch "$TDIR/SUB/insub.dat;1"

# SET DEFAULT onto the device root, then list a device-less bracketed subdir.
# The listing must come from TD:[SUB] ($TDIR/SUB), inheriting device TD from
# the current default — NOT from the /vms volume root.
OUT=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY [SUB]\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$OUT"

rm -rf "$TDIR"
