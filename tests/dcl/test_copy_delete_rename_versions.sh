#!/bin/bash
# TEST: COPY / DELETE / RENAME wildcard + version semantics (vms-1c6, final slice)
#
# This slice of vms-1c6 (File/RMS user-visible fidelity) locks down the file
# commands a VMS user runs constantly — COPY, DELETE and RENAME — against the
# real on-disk vmsfs version store (name.type;N) and the single VMS wildcard
# matcher vmsfs_wildcard_match() (shared with DIRECTORY/PURGE). INV-6: every
# copy/unlink/rename is a real backing-store operation; counts are real.
#
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary —
#   DELETE : "you must include the version number in each file specification";
#            omitting it yields %DELETE-E-DELVER, "explicit version number or
#            wild card required". Accepted version fields: ;n ; ;0/; (highest);
#            ;* (all versions); ;-n (relative). /LOG reports %DELETE-I-FILDEL.
#   COPY   : a wildcard input copies each matching file; the output version
#            defaults to one greater than the highest existing version of the
#            same name.type (never a silent overwrite); a multi-file copy
#            reports %COPY-S-NEWFILES, n files created; /LOG lists each file
#            with %COPY-S-COPIED.
#   RENAME : wildcard input + output-field substitution (*.TXT -> *.BAK); a
#            directory in the output moves the file; /LOG reports
#            %RENAME-I-RENAMED.
#
# --- DELETE with no version field is refused (DELVER) ---
# EXPECT: contains:%DELETE-E-DELVER
# EXPECT: regex:explicit version number or wild card required
#
# --- DELETE ;n removes exactly that version, leaving the others ---
# EXPECT: contains:BPICK.TXT;3
# EXPECT: contains:BPICK.TXT;1
# EXPECT_NOT: contains:BPICK.TXT;2
#
# --- DELETE ;* removes every version; authentic FILDEL identifier ---
# EXPECT: contains:%DELETE-I-FILDEL
# EXPECT: contains:%DIRECT-W-NOFILES
#
# --- COPY of a versioned file onto an existing name bumps the version ---
# EXPECT: contains:%COPY-S-COPIED
# EXPECT: contains:DCOPY.TXT;4
#
# --- wildcard COPY into a directory: per-file copy + NEWFILES summary ---
# EXPECT: contains:%COPY-S-NEWFILES
# EXPECT: regex:2 files created
# EXPECT: contains:E1.DAT
# EXPECT: contains:E2.DAT
#
# --- RENAME cross-directory move (authentic RENAMED identifier) ---
# EXPECT: contains:%RENAME-I-RENAMED
# EXPECT: contains:FRN.TXT
#
# --- wildcard RENAME with output field substitution (*.TXT -> *.BAK) ---
# EXPECT: contains:G1.BAK
# EXPECT: contains:G2.BAK
# EXPECT_NOT: contains:rename failed
VMSDCL="${VMSDCL:-vmsdcl}"

# --- A) DELETE requires an explicit version field ---
A=$(mktemp -d); touch "$A/avers.txt;1"
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDELETE AVERS.TXT\n' "$A" | $VMSDCL 2>&1
rm -rf "$A"

# --- B) DELETE ;n removes exactly that version ---
B=$(mktemp -d); for v in 1 2 3; do echo "v$v" > "$B/bpick.txt;$v"; done
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDELETE BPICK.TXT;2\nDIRECTORY BPICK.TXT\n' "$B" | $VMSDCL 2>&1
rm -rf "$B"

# --- C) DELETE ;* removes every version ---
C=$(mktemp -d); for v in 1 2; do echo "v$v" > "$C/cwild.txt;$v"; done
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nDELETE/LOG CWILD.TXT;*\nDIRECTORY CWILD.TXT\n' "$C" | $VMSDCL 2>&1
rm -rf "$C"

# --- D) COPY of a versioned file onto an existing name bumps the version ---
D=$(mktemp -d); echo "v3" > "$D/dcopy.txt;3"
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nCOPY/LOG DCOPY.TXT;3 DCOPY.TXT\nDIRECTORY DCOPY.TXT\n' "$D" | $VMSDCL 2>&1
rm -rf "$D"

# --- E) wildcard COPY into a directory + NEWFILES summary ---
E=$(mktemp -d); mkdir "$E/ebk"; echo a > "$E/e1.dat;1"; echo b > "$E/e2.dat;1"
printf 'DEFINE TD "%s"\nDEFINE EBK "%s/ebk"\nSET DEFAULT TD:[000000]\nCOPY/LOG *.DAT EBK:\nDIRECTORY EBK:\n' "$E" "$E" | $VMSDCL 2>&1
rm -rf "$E"

# --- F) RENAME cross-directory move ---
F=$(mktemp -d); mkdir "$F/fbk"; echo hi > "$F/frn.txt;1"
printf 'DEFINE TD "%s"\nDEFINE FBK "%s/fbk"\nSET DEFAULT TD:[000000]\nRENAME/LOG FRN.TXT FBK:FRN.TXT\nDIRECTORY FBK:\n' "$F" "$F" | $VMSDCL 2>&1
rm -rf "$F"

# --- G) wildcard RENAME with output field substitution (*.TXT -> *.BAK) ---
G=$(mktemp -d); echo 1 > "$G/g1.txt;1"; echo 2 > "$G/g2.txt;1"
printf 'DEFINE TD "%s"\nSET DEFAULT TD:[000000]\nRENAME/LOG *.TXT *.BAK\nDIRECTORY *.BAK\n' "$G" | $VMSDCL 2>&1
rm -rf "$G"
