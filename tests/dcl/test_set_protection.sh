#!/bin/bash
# TEST: SET PROTECTION parses a VMS protection code, applies it to the REAL
#       backing file, and DIRECTORY/PROTECTION renders the authentic
#       (S:RWED,O:RWED,G:RE,W:) string back (vms-1c6, file-protection slice).
#
# Syntax + the (class:code) protection format are grounded in the public VSI
# OpenVMS DCL Dictionary "SET PROTECTION" entry (SET PROTECTION[=(ownership:
# access[,...])] filespec); the RWED bit meanings and the "unspecified
# categories are unchanged" merge rule are from that same entry. The bit
# layout of the 16-bit protection word is pinned in
# src/libvms/include/ovmx_fileprot.h (vms-f81). No protection value is a
# fabricated constant: DIRECTORY/PROTECTION renders the word that
# vmsfs_mode_to_protection() derives from the file's live st_mode, and the
# REALMODE_* lines below read that same backing file with stat(1), so a
# SET PROTECTION that did nothing would fail these assertions (INV-6).
#
# --- multi-category list: the comma list is NOT mis-split (was %RMS-E-FNF) ---
# EXPECT: regex:ALPHA\.TXT;1 +\(S:RWED,O:RWED,G:RE,W:\)
# EXPECT: contains:REALMODE_ALPHA=750
#
# --- partial spec: unspecified categories keep their CURRENT value (VMS merge),
#     they are NOT wiped to fully-denied. BETA starts 640 = (S:RWED,O:RWD,G:R,W:);
#     SET PROTECTION=(W:R) must leave O:RWD and G:R intact and only add W:R ---
# EXPECT: regex:BETA\.TXT;1 +\(S:RWED,O:RWD,G:R,W:R\)
# EXPECT: contains:REALMODE_BETA=644
# EXPECT_NOT: regex:BETA\.TXT;1 +\(S:RWED,O:,G:,W:R\)
#
# --- single category without parentheses: SET PROTECTION=W:RE ---
# EXPECT: regex:GAMMA\.TXT;1 +\(S:RWED,O:RWD,G:R,W:RE\)
# EXPECT: contains:REALMODE_GAMMA=645
#
# --- a missing file is reported %RMS-E-PRV, never a silent success ---
# EXPECT: contains:%RMS-E-PRV
#
# TRIPWIRE: revert cmd_set_protection() to reading cmd->params[] (instead of
# cmd->raw_tail) and the multi-category ALPHA case goes red with %RMS-E-FNF;
# revert vmsfs_parse_protection() to forcing *prot=0xFFFF and the BETA merge
# case goes red (O:/G: emptied). Both in the vms-1c6 protection slice.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/alpha.txt"; chmod 600 "$TDIR/alpha.txt"
touch "$TDIR/beta.txt";  chmod 640 "$TDIR/beta.txt"
touch "$TDIR/gamma.txt"; chmod 640 "$TDIR/gamma.txt"
# Each DIRECTORY is scoped to a single file so the four protection strings do
# not cross-contaminate the regex assertions above.
printf 'DEFINE TD "%s"\n'\
'SET DEFAULT TD:[000000]\n'\
'SET PROTECTION=(S:RWED,O:RWED,G:RE,W:) ALPHA.TXT\n'\
'DIRECTORY/PROTECTION ALPHA.TXT\n'\
'SET PROTECTION=(W:R) BETA.TXT\n'\
'DIRECTORY/PROTECTION BETA.TXT\n'\
'SET PROTECTION=W:RE GAMMA.TXT\n'\
'DIRECTORY/PROTECTION GAMMA.TXT\n'\
'SET PROTECTION=(O:RWED) NOSUCH_PROT_QWERTY.TXT\n' "$TDIR" | $VMSDCL 2>&1
# Prove the REAL backing files changed (INV-6), independent of the display path.
echo "REALMODE_ALPHA=$(stat -c '%a' "$TDIR/alpha.txt")"
echo "REALMODE_BETA=$(stat -c '%a' "$TDIR/beta.txt")"
echo "REALMODE_GAMMA=$(stat -c '%a' "$TDIR/gamma.txt")"
rm -rf "$TDIR"
