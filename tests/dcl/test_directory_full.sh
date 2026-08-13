#!/bin/bash
# TEST: DIRECTORY/FULL + /SIZE=option fidelity (vms-5e2)
#
# Locks the VMS-faithful DIRECTORY/FULL per-file block and the /SIZE=option
# block-count modes. Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary,
# DIRECTORY command — the /FULL multi-line layout (Size used/allocated, Owner
# UIC, Created/Revised/Expired/Backup dates, File protection) and
# /SIZE[=USED|ALLOCATION|ALL].
#
# EVERY asserted field is sourced from REAL file metadata (INV-6):
#   - Created:  genuine birth time (statx STATX_BTIME).
#   - Revised:  real st_mtime — and BOTH carry the real ".cc" centisecond
#               fraction, not the hardcoded ".00" the old single-line /FULL
#               emitted (the vms-5e2 fidelity bug this test gates). The old
#               /FULL had NO "Created:" line at all, so its presence alone is a
#               regression tripwire.
#   - Size:     used/allocated from st_size / st_blocks (real on-disk figures).
#   - Owner:    UIC [group,member] from st_gid/st_uid.
#   - File protection: long form decoded from the real st_mode.
#   - Expired/Backup: a passthrough file genuinely has none — VMS prints exactly
#               "<None specified>" / "<No backup recorded>" for such a file, so
#               these are faithful, not invented.
#
# HONEST GAP (INV-6, deliberately NOT fabricated): File ID, File organization,
# Record format/attributes and longest-record length live in an ODS-2 file
# header the live host-FS passthrough (vms-5eb) does not carry, so DIRECTORY/FULL
# omits them rather than printing a plausible-looking fake. The EXPECT_NOT below
# locks that no fabricated "File ID:" line ever appears.
#
# --- DIRECTORY/FULL: the authentic multi-line per-file block ---
# EXPECT: regex:Size:[ ]+[0-9]+/[0-9]+[ ]+Owner:[ ]+\[[0-9]+,[0-9]+\]
# EXPECT: regex:Created:[ ]+[ 0-9][0-9]-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}
# EXPECT: regex:Revised:[ ]+[ 0-9][0-9]-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}
# EXPECT: contains:Expired:  <None specified>
# EXPECT: contains:Backup:   <No backup recorded>
# EXPECT: regex:File protection:[ ]+System:RWED, Owner:R.*Group:.*World:
# EXPECT_NOT: contains:File ID:
#
# --- /SIZE modes: USED (default), ALLOCATION, ALL ---
# EXPECT: contains:SIZE_ALL_COL_OK
# EXPECT: contains:SIZE_ALL_TRAILER_OK
# EXPECT: contains:SIZE_ALLOC_TRAILER_OK
# EXPECT: contains:SIZE_USED_TRAILER_OK
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
# A real file with deterministic protection (owner rw, group r, world none) so
# the long-form protection decode is stable.
printf 'line one\nline two\n' > "$TDIR/fullrec.dat"
chmod 640 "$TDIR/fullrec.dat"

# DIRECTORY/FULL — the multi-line block is asserted by the EXPECT regexes above.
FULL=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY/FULL FULLREC.DAT\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$FULL"

# /SIZE=ALL — the file line carries a "used/allocated" column, and the trailer
# is "Total of 1 file, U/A blocks."
SALL=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY/SIZE=ALL FULLREC.DAT\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$SALL"
if printf '%s\n' "$SALL" | grep -Eq 'FULLREC\.DAT;1[ ]+[0-9]+/[0-9]+'; then
    echo "SIZE_ALL_COL_OK"; else echo "SIZE_ALL_COL_BAD"; fi
if printf '%s\n' "$SALL" | grep -Eq 'Total of 1 file, [0-9]+/[0-9]+ blocks?\.'; then
    echo "SIZE_ALL_TRAILER_OK"; else echo "SIZE_ALL_TRAILER_BAD"; fi

# /SIZE=ALLOCATION — trailer shows allocated blocks (no slash).
SALO=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY/SIZE=ALLOCATION FULLREC.DAT\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$SALO"
if printf '%s\n' "$SALO" | grep -Eq 'Total of 1 file, [0-9]+ blocks?\.'; then
    echo "SIZE_ALLOC_TRAILER_OK"; else echo "SIZE_ALLOC_TRAILER_BAD"; fi

# Bare /SIZE (=USED) — unchanged: used blocks, no slash.
SUSE=$(printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY/SIZE FULLREC.DAT\n' "$TDIR" | $VMSDCL 2>&1)
printf '%s\n' "$SUSE"
if printf '%s\n' "$SUSE" | grep -Eq 'Total of 1 file, [0-9]+ blocks?\.'; then
    echo "SIZE_USED_TRAILER_OK"; else echo "SIZE_USED_TRAILER_BAD"; fi

rm -rf "$TDIR"
