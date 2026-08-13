#!/bin/bash
# TEST: CREATE mints a new version (never truncates), DELETE ;0/;-1 relative
#       selectors, and PURGE keeps the highest version (vms-73b).
#
# vms-73b closed the last raw-fopen("w") file command: CREATE. It used to
# resolve its target to the HIGHEST existing version and fopen("w") it, which
# TRUNCATED that version's contents in place — a silent data-destroying
# non-VMS behavior. VMS CREATE, like every RMS file creation, mints a NEW,
# higher version and never touches the existing one. COPY/DELETE/RENAME/PURGE
# version semantics are covered by test_copy_delete_rename_versions.sh; this
# test locks down CREATE plus the two version selectors that test did not
# exercise (;0 = highest, ;-n = relative-to-highest) and PURGE keep-highest.
#
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary —
#   CREATE : "creates a sequential disk file" from SYS$INPUT; a create of a
#            name.type that already exists produces the next-higher version,
#            never an overwrite (VSI OpenVMS User's Manual, "Version Numbers").
#   DELETE : ";0" (or ";") selects the highest version; a negative ";-n"
#            selects the version n below the highest (";-1" = second highest —
#            VSI OpenVMS Wiki, "File version"; VSI OpenVMS User's Manual).
#   PURGE  : deletes all but the highest version (default /KEEP=1).
#
# --- A) CREATE over an existing version mints ;2 and PRESERVES ;1's bytes ---
#     (the exact regression: the old code truncated CRVER.TXT;1 to empty)
# EXPECT: contains:CRVER.TXT;1
# EXPECT: contains:CRVER.TXT;2
# EXPECT: contains:ORIGINAL_V1_BYTES
# EXPECT: contains:SECOND_VERSION_BYTES
#
# --- B) DELETE ;0 removes exactly the highest version ---
# EXPECT: contains:DZERO.TXT;1
# EXPECT: contains:DZERO.TXT;2
# EXPECT_NOT: contains:DZERO.TXT;3
#
# --- C) DELETE ;-1 removes exactly the version below the highest ---
# EXPECT: contains:EREL.TXT;1
# EXPECT: contains:EREL.TXT;3
# EXPECT_NOT: contains:EREL.TXT;2
#
# --- D) PURGE keeps only the highest version ---
# EXPECT: contains:PKEEP.TXT;3
# EXPECT_NOT: contains:PKEEP.TXT;1
# EXPECT_NOT: contains:PKEEP.TXT;2
VMSDCL="${VMSDCL:-vmsdcl}"

# --- A) CREATE mints a new version, leaving the prior version intact.
#     A seeded CRVER.TXT;1 holds known bytes; CREATE (fed SECOND_VERSION_BYTES
#     via a DECK/EOD block, the bounded SYS$INPUT idiom) must produce ;2 and
#     leave ;1 readable and unchanged. Uses the proven @proc + DECK pattern
#     (see test_scripting_status_call_deck.sh) so CREATE's SYS$INPUT read is
#     cleanly bounded by EOD rather than swallowing following commands. The
#     procedure lives under /vms and is invoked relative to the set default,
#     exactly as that reference test does. ---
ATD="cre_test_$$"
AVD="$(echo "$ATD" | tr a-z A-Z)"
mkdir -p "/vms/$ATD"
printf 'ORIGINAL_V1_BYTES\n' > "/vms/$ATD/crver.txt;1"
cat > "/vms/$ATD/cre.com" << 'EOF'
$ CREATE CRVER.TXT
$ DECK
SECOND_VERSION_BYTES
$ EOD
$ DIRECTORY CRVER.TXT
$ TYPE CRVER.TXT;1
$ TYPE CRVER.TXT;2
EOF
{ echo "SET DEFAULT SYS\$SYSDEVICE:[$AVD]"; echo "@cre.com"; } | $VMSDCL 2>&1
rm -rf "/vms/$ATD"

# --- B) DELETE ;0 removes the highest version only ---
B=$(mktemp -d); for v in 1 2 3; do echo "v$v" > "$B/dzero.txt;$v"; done
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDELETE DZERO.TXT;0\nDIRECTORY DZERO.TXT\n' "$B" | $VMSDCL 2>&1
rm -rf "$B"

# --- C) DELETE ;-1 removes the version below the highest only ---
C=$(mktemp -d); for v in 1 2 3; do echo "v$v" > "$C/erel.txt;$v"; done
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDELETE EREL.TXT;-1\nDIRECTORY EREL.TXT\n' "$C" | $VMSDCL 2>&1
rm -rf "$C"

# --- D) PURGE keeps only the highest version ---
D=$(mktemp -d); for v in 1 2 3; do echo "v$v" > "$D/pkeep.txt;$v"; done
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nPURGE PKEEP.TXT\nDIRECTORY PKEEP.TXT\n' "$D" | $VMSDCL 2>&1
rm -rf "$D"
