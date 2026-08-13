#!/bin/bash
# TEST: DIRECTORY output-format + file-version fidelity (vms-1c6)
#
# This gates the surface a VMS user reads every minute: the DIRECTORY header,
# real ;N file versions listed highest-version-first, and the exact "Total of
# ..." trailer form.
#
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY command
# examples (OpenVMS DCL Dictionary, DIRECTORY —
# www0.mi.infn.it/~calcolo/OpenVMS/ssb71/9996/9996p013.htm):
#   - the listing is headed         "Directory dev:[dir]"
#   - a bare DIRECTORY ends         "Total of N files."   (NO block count)
#   - /SIZE ends                    "Total of N files, M blocks."
# The block count appears ONLY when file sizes are displayed. The prior OVMX
# code printed ", M blocks." even for a bare DIRECTORY — the vms-1c6 fidelity
# bug this test locks down.
#
# VERSIONS ARE REAL, NOT FABRICATED (INV-6): the three files are placed on disk
# with the exact "name.type;N" convention sys$create() (src/vmsrms/rms_core.c)
# uses, so DIRECTORY reads the stored version numbers. If cmd_directory()
# regressed to synthesizing ;1 on every file, the ;2/;3 assertions and the
# ordering check below go red.
#
# NOTE: this script runs BOTH a bare and a /SIZE listing, so their outputs share
# one capture — the "no blocks in the bare trailer" claim can't be an EXPECT_NOT
# (the /SIZE trailer would trip it). It is asserted in-script via the
# DEFAULT_NOBLOCKS_OK marker below; the bare-trailer-has-no-blocks case is also
# gated end-to-end by test_directory_version.sh (bare DIRECTORY only).
#
# --- default DIRECTORY: header, three real versions, no-blocks trailer ---
# EXPECT: regex:Directory .*\[
# EXPECT: contains:VERFMT.DAT;3
# EXPECT: contains:VERFMT.DAT;2
# EXPECT: contains:VERFMT.DAT;1
# EXPECT: regex:Total of 3 files\.
# EXPECT: contains:DEFAULT_NOBLOCKS_OK
#
# --- /SIZE DIRECTORY: highest-version-first ordering + blocks in the trailer ---
# EXPECT: contains:VERSION_ORDER_OK
# EXPECT: regex:Total of 3 files, [0-9]+ blocks\.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/verfmt.dat;1" "$TDIR/verfmt.dat;2" "$TDIR/verfmt.dat;3"

# Bare DIRECTORY: header + all three real versions + "Total of 3 files."
DEF=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY VERFMT.DAT\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$DEF"
# The bare-listing trailer must NOT carry a block count.
if printf '%s\n' "$DEF" | grep -Eq 'Total of [0-9]+ files?, [0-9]+ blocks\.'; then
    echo "DEFAULT_HAS_BLOCKS_BAD"
else
    echo "DEFAULT_NOBLOCKS_OK"
fi

# /SIZE: one file per line, so relative line order = listing order. Assert the
# highest version (;3) is listed before the lowest (;1), and blocks appear.
SZ=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY/SIZE VERFMT.DAT\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$SZ"
l3=$(printf '%s\n' "$SZ" | grep -n 'VERFMT.DAT;3' | head -1 | cut -d: -f1)
l1=$(printf '%s\n' "$SZ" | grep -n 'VERFMT.DAT;1' | head -1 | cut -d: -f1)
if [ -n "$l3" ] && [ -n "$l1" ] && [ "$l3" -lt "$l1" ]; then
    echo "VERSION_ORDER_OK"
else
    echo "VERSION_ORDER_BAD (;3 at line '$l3', ;1 at line '$l1')"
fi

rm -rf "$TDIR"
