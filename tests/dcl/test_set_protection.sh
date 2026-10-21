#!/bin/bash
# TEST: with no executive, SET PROTECTION / SET FILE refuse honestly and touch no host file (vms-1fb7)
#
# A file's protection and owner live in its Files-11 header and only the
# executive ACP changes them (IO$_MODIFY, CONTROL access) -- proven against a
# real /dev/vms by tests/qemu/test_syssvc_dcl_set_protection.c, which sets a
# protection and reads it back through F$FILE_ATTRIBUTES and
# DIRECTORY/PROTECTION. This host run has no executive, so the honest answer is
# %SYSTEM-W-NOSUCHDEV, SS$_NOSUCHDEV (%X00000908), and the backing file's mode
# must NOT change: the old code chmod()ed the host file here, a change no
# VMS-side protection read ever saw (vms-1fb7, AGENTS.md "one runtime").
#
# EXPECT: contains:%SYSTEM-W-NOSUCHDEV, no such device available
# EXPECT: contains:$STATUS = "%X00000908"
# EXPECT: contains:REALMODE_ALPHA=600
# EXPECT: contains:REALMODE_BETA=640
# EXPECT_NOT: contains:%RMS-E-PRV
# EXPECT_NOT: contains:$STATUS = "%X00000001"
#
# A protection code naming an unknown category is CLI$_IVKEYW, naming it
# (VAX1, tests/lab/captures/set-protection-20261021/: SET PROTECTION=(Q:R) ->
# "%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling" / " \Q\",
# $STATUS %X00038060). /DEFAULT parses the code before anything else, so it
# shows here without an executive.
# EXPECT: contains:%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling
# EXPECT: contains: \Q\
# EXPECT: contains:$STATUS = "%X00038060"
# EXPECT_NOT: contains:BADPROT
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR=$(mktemp -d)
touch "$TDIR/alpha.txt"; chmod 600 "$TDIR/alpha.txt"
touch "$TDIR/beta.txt";  chmod 640 "$TDIR/beta.txt"
printf 'DEFINE TD "%s"\n'\
'SET DEFAULT TD:[000000]\n'\
'SET PROTECTION=(S:RWED,O:RWED,G:RE,W:) ALPHA.TXT\n'\
'SHOW SYMBOL $STATUS\n'\
'SET FILE/PROTECTION=(W:R) BETA.TXT\n'\
'SHOW SYMBOL $STATUS\n'\
'SET PROTECTION=(Q:R)/DEFAULT\n'\
'SHOW SYMBOL $STATUS\n' "$TDIR" | $VMSDCL 2>&1
echo "REALMODE_ALPHA=$(stat -c '%a' "$TDIR/alpha.txt")"
echo "REALMODE_BETA=$(stat -c '%a' "$TDIR/beta.txt")"
rm -rf "$TDIR"
