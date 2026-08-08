#!/bin/bash
# TEST: DIRECTORY/lookup finds VMS-versioned files by bare name, by ;n, and by wildcard (vms-1c8)
#
# Root cause of vms-1c8 (the PARTS 0.2 demo's last blocker): sys$create()
# (src/vmsrms/rms_core.c) names files on disk with a real ";N" suffix --
# "PARTS.DAT" is stored as "parts.dat;1" -- but DCL's DIRECTORY and filespec
# lookup had NO version-number handling in either exact-match or wildcard
# lookup, so `$ DIRECTORY SYS$SCRATCH:PARTS.DAT` (and even `$ DIRECTORY
# *.DAT`) reported "Total of 0 files" against a file that had just been
# created and used in the same session.
#
# The two on-disk versions here are created directly with the same literal
# "name.type;N" convention sys$create() uses (touch, not DCL CREATE),
# isolating exactly the lookup path vms-1c8 is about: given real versioned
# files on disk, does DIRECTORY find them correctly? The explicit-version
# case is typed as a quoted string ("VEROVMX.DAT;1") rather than a bare
# word -- the DCL lexer treats a bare, unquoted ';' as a comment
# introducer (see test_parser_comments.sh; that is deliberate, tested
# behavior, unrelated to this fix and out of this item's lane), but
# preserves ';' verbatim inside a quoted string, which is how a real
# filespec value flows in from a program (e.g. a FAB's fab$l_fna) anyway.
#
#   - DIRECTORY VEROVMX.DAT     (bare, no version) -> ALL versions,
#     highest first (VMS: a bare name is not a request for "the" version,
#     it lists every one)
#   - DIRECTORY "VEROVMX.DAT;1" (explicit version) -> that version ONLY
#   - DIRECTORY *.DAT           (wildcard, no version) -> ALL versions of
#     every matching name
#
# EXPECT: contains:VEROVMX.DAT;1
# EXPECT: contains:VEROVMX.DAT;2
# EXPECT: regex:Total of 2 files, 0 blocks\.
# EXPECT: regex:Total of 1 file, 0 blocks\.
# EXPECT_NOT: contains:Total of 0 files
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/verovmx.dat;1"
touch "$TDIR/verovmx.dat;2"
printf 'DEFINE TESTDIR "%s"\nSET DEFAULT TESTDIR:[000000]\nDIRECTORY VEROVMX.DAT\nDIRECTORY "VEROVMX.DAT;1"\nDIRECTORY *.DAT\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
