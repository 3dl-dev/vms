#!/bin/bash
# TEST: DIRECTORY command lists files
#
# Hermetic (vms-1c6): list a real temporary directory holding one real file so
# the smoke test is deterministic regardless of the process default directory.
# A DIRECTORY that resolves to a real, populated directory must produce the VMS
# "Directory ...] / Total of N files." shape, never a raw Unix "ls:"/"total N"
# error. (A bare DIRECTORY against a non-existent default dir now yields the
# authentic %RMS-E-DNF / %DIRECT-W-NOFILES with no header — see
# test_directory_wildcards.sh — so this smoke test lists a directory that
# actually exists.)
# EXPECT: regex:(Directory|Total of|files)
# EXPECT_NOT: regex:^(ls:|total [0-9])
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/smoke.txt;1"
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
