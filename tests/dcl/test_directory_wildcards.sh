#!/bin/bash
# TEST: DIRECTORY wildcard + ellipsis uniformity and multi-directory rollup (vms-1c6)
#
# This slice of vms-1c6 (File/RMS user-visible fidelity) locks down the file-
# name/directory wildcard surface a VMS user drives constantly:
#
#   *   matches zero or more characters
#   %   matches exactly one character
#   ... (ellipsis) recurses THIS directory and every subdirectory below it
#
# All three run through the single filename matcher vmsfs_wildcard_match()
# (src/vmsfs/vmsfs_translate.c); the ellipsis directory walk is a real,
# on-disk depth-first traversal of the vmsfs tree (INV-6: no faked recursion —
# a listed subdirectory file exists on disk).
#
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY command:
#   - "*"/"%" filename wildcards;
#   - the "..." ellipsis directory wildcard = "this directory and all
#     subdirectories below it";
#   - a multi-directory listing prints a per-directory header + "Total of N
#     files" subtotal, then one "Grand total of D directories, F files[, M
#     blocks]." line;
#   - a search that matches nothing yields "%DIRECT-W-NOFILES, no files found"
#     (a warning), NOT an empty success / "Total of 0 files."
#
# --- "*" filename wildcard: *.TXT matches only the .TXT file ---
# EXPECT: contains:APPLE.TXT;1
# EXPECT_NOT: contains:BANANA.DAT
#
# --- "%" single-char wildcard: %.LOG matches the 1-char name, not the 2-char ---
# EXPECT: contains:A.LOG;1
# EXPECT_NOT: contains:AB.LOG
#
# --- "..." ellipsis: recurse base + subdirectory, per-dir subtotals + grand total ---
# EXPECT: contains:CHERRY.TXT;1
# EXPECT: regex:Grand total of 2 directories, 2 files\.
#
# --- zero matches: the authentic %DIRECT-W-NOFILES warning ---
# EXPECT: contains:%DIRECT-W-NOFILES, no files found
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)

# Base directory files.
touch "$TDIR/apple.txt;1"
touch "$TDIR/banana.dat;1"
touch "$TDIR/a.log;1"
touch "$TDIR/ab.log;1"

# A real subdirectory with its own matching file (ellipsis must reach it).
mkdir "$TDIR/logs"
touch "$TDIR/logs/cherry.txt;1"

# 1) "*" wildcard — *.TXT in the base directory only.
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY *.TXT\n' "$TDIR" | $VMSDCL 2>&1

# 2) "%" single-char wildcard — %.LOG matches a.log;1 but not ab.log;1.
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY %%.LOG\n' "$TDIR" | $VMSDCL 2>&1

# 3) "..." ellipsis — recurse base + logs, expect a two-directory grand total.
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY [...]*.TXT\n' "$TDIR" | $VMSDCL 2>&1

# 4) zero matches — the authentic NOFILES warning, not "Total of 0 files."
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY NOSUCH.XYZ\n' "$TDIR" | $VMSDCL 2>&1

rm -rf "$TDIR"
