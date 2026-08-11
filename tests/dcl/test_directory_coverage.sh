#!/bin/bash
# TEST: DIRECTORY qualifier COVERAGE (vms-7543) - /EXCLUDE, /VERSIONS, and
#       /PROTECTION each do REAL work, not just parse (INV-DCL: implemented to
#       VMS semantics, never a declared-and-ignored facade). Names/value-types
#       are grounded in the public VSI OpenVMS DCL Dictionary DIRECTORY entry.
#
# --- /EXCLUDE=*.LOG: BETA.LOG omitted, BETA.TXT still listed ---
# EXPECT: contains:BETA.TXT
# EXPECT_NOT: contains:BETA.LOG
#
# --- /VERSIONS=2: newest two versions of VER.DAT listed, oldest (;1) dropped ---
# EXPECT: contains:VER.DAT;3
# EXPECT: contains:VER.DAT;2
# EXPECT_NOT: contains:VER.DAT;1
#
# --- /PROTECTION: the VMS protection column is rendered on the file line ---
# EXPECT: regex:ALPHA\.TXT;1.*\(S:.*O:.*G:.*W:.*\)
#
# THE FINDING THIS GATES (docs/design-vms-parity-map.md sec 3): DIRECTORY
# carried 10 of ~30 Dictionary qualifiers. TRIPWIRE: revert cmd_directory()'s
# /EXCLUDE skip, the /VERSIONS group counter, or the /PROTECTION column
# (src/vmsdcl/dcl_cmd_file.c) and the matching assertion goes red - the
# excluded file reappears, the old version reappears, or the protection column
# vanishes. Also proves the qualifiers are accepted (not IVQUAL) - they are in
# q_directory (src/vmsdcl/dcl_builtin.c).
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/alpha.txt" "$TDIR/beta.txt" "$TDIR/beta.log"
touch "$TDIR/ver.dat;1" "$TDIR/ver.dat;2" "$TDIR/ver.dat;3"
# Each DIRECTORY is scoped to a controlled pattern so the three assertions
# don't cross-contaminate (e.g. an unscoped /EXCLUDE listing would also print
# VER.DAT;1 and break the /VERSIONS EXPECT_NOT).
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDIRECTORY/EXCLUDE=*.LOG BETA.*\nDIRECTORY/VERSIONS=2 VER.DAT\nDIRECTORY/PROTECTION ALPHA.TXT\n' "$TDIR" | $VMSDCL 2>&1
rm -rf "$TDIR"
