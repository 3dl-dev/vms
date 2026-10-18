#!/bin/bash
# TEST: MAIL refuses an unrecognized qualifier before MAIL.EXE is entered, and forwards
# the one it honours as /NAME=value (not the bare name) (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQBOGUS\
# EXPECT: contains:MAILARG=/SUBJECT=hello world
# EXPECT: contains:MAILARG=SOMEUSER
# EXPECT_NOT: contains:MAILARG=SUBJECT
# EXPECT_NOT: contains:MAILARG=NOSUCHUSR
VMSDCL="${VMSDCL:-vmsdcl}"
SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
mkdir -p "$SYSEXE"
STUB="$SYSEXE/MAIL.EXE"
if [ -e "$STUB" ]; then echo "SKIP-FOREIGN-MAIL-EXE-PRESENT"; exit 0; fi
printf '#!/bin/sh\nfor a in "$@"; do echo "MAILARG=$a"; done\n' > "$STUB"
chmod +x "$STUB"
printf 'MAIL/ZQBOGUS NOSUCHUSR\nMAIL/SUBJECT="hello world" SOMEUSER\n' | $VMSDCL 2>&1
rm -f "$STUB"
