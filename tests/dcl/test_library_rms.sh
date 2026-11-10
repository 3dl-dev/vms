#!/bin/bash
# TEST: LIBRARY reaches libraries, modules and output files through RMS: a file RMS cannot reach is OPENIN/OPENOUT with the RMS reason, and /EXTRACT/OUTPUT writes a text file DCL reads back
# EXPECT: contains:%LIBRARIAN-E-OPENIN, error opening library - NOSUCH.TLB
# EXPECT: contains:%LIBRARIAN-E-OPENIN, error opening source file - NOSRC.TXT
# EXPECT: contains:%LIBRARIAN-E-OPENIN, error opening object file - NOOBJ.OBJ
# EXPECT: contains:-RMS-E-FNF, file not found
# EXPECT: contains:LIBRMS-FNF-COUNT=3
# EXPECT: regex:%LIBRARIAN-E-OPENOUT, error creating library - SYS\$SYSDEVICE:\[DCL_LIB_RMS_[0-9]+\.NODIR\]X\.TLB
# EXPECT: contains:LIBRMS-OPENOUT-RMS-REASON-OK
# EXPECT: contains:%LIBRARIAN-S-EXTRACTED, module RMSMOD extracted to OUT.TXT
# EXPECT: contains:LIBRMS-LINE1-FIRST RECORD
# EXPECT: contains:LIBRMS-LINE2-SECOND RECORD
# EXPECT_NOT: contains:No such file
# EXPECT_NOT: contains:LIBRMS-OBJ-LIBRARY-MADE
#
# libc-drop vms-f8cd: LIBRARY opens its libraries, source modules and objects
# with RMS ($OPEN, block $READ / record $GET) and writes libraries and /OUTPUT
# files with RMS ($CREATE, $PUT). A file RMS cannot reach is reported as VMS
# does -- the LIBRARIAN message, then the RMS reason on its own line
# (-RMS-E-FNF; for a library in a directory that does not exist, OPENOUT and
# the status RMS's $CREATE returned), never a host strerror() text -- and a
# refused object input makes no library. The module a text library holds is
# its source's records; /EXTRACT/OUTPUT writes them as records that DCL's
# OPEN/READ reads back line for line.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_lib_rms_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$VDIR"
printf 'FIRST RECORD\nSECOND RECORD\n' > "/vms/$VDIR/rmssrc.txt"

OUT=$(sed "s/@VDIR@/$VDIR/g" <<'DCL' | $VMSDCL 2>&1
SET DEFAULT SYS$SYSDEVICE:[@VDIR@]
LIBRARY /LIST NOSUCH.TLB
LIBRARY /CREATE R.TLB
LIBRARY R.TLB NOMOD NOSRC.TXT
LIBRARY /CREATE R.OLB NOOBJ.OBJ
LIBRARY /CREATE SYS$SYSDEVICE:[@VDIR@.NODIR]X.TLB
LIBRARY R.TLB RMSMOD RMSSRC.TXT
LIBRARY /EXTRACT=RMSMOD /OUTPUT=OUT.TXT R.TLB
OPEN/READ F OUT.TXT
READ F L1
READ F L2
CLOSE F
WRITE SYS$OUTPUT "LIBRMS-LINE1-''L1'"
WRITE SYS$OUTPUT "LIBRMS-LINE2-''L2'"
DCL
)
printf '%s\n' "$OUT"
printf '%s\n' "$OUT" | grep -A1 -- '-E-OPENOUT, error creating library' | tail -1 |
    grep -qE '^-RMS-[WEF]-[A-Z]+, ' && echo "LIBRMS-OPENOUT-RMS-REASON-OK"
echo "LIBRMS-FNF-COUNT=$(printf '%s\n' "$OUT" | grep -c -- '-RMS-E-FNF, file not found')"
if find "/vms/$VDIR" -iname 'R.OLB*' | grep -q .; then echo "LIBRMS-OBJ-LIBRARY-MADE"; fi

rm -rf "/vms/$VDIR"
