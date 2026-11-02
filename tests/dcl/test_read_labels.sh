#!/bin/bash
# TEST: READ validates its qualifiers and honours /END_OF_FILE and /ERROR labels (vms-097).
# EXPECT: contains:first=one
# EXPECT: contains:at-eof
# EXPECT_NOT: contains:NOT-REACHED
# EXPECT_NOT: contains:wrong-err
# EXPECT: contains: \BOGUS\
# EXPECT: contains: \DELETE\
VMSDCL="${VMSDCL:-vmsdcl}"
# The file is staged on the system disk (/vms) so READ reaches it through RMS
# $OPEN/$GET like any VMS file -- a file-redirected SYS$INPUT is never read
# with host stdio (vms-5cc).
TDIR="dcl_rdlab_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
d="/vms/$TDIR"
mkdir -p "$d"
trap 'rm -rf "$d"' EXIT
printf 'one\n' > "$d/F.TXT"
cat > "$d/T.COM" <<'EOC'
$ DEFINE SYS$INPUT "F.TXT"
$ READ/END_OF_FILE=EOFL/ERROR=ERRL SYS$INPUT REC
$ WRITE SYS$OUTPUT "first=''REC'"
$ READ/END_OF_FILE=EOFL/ERROR=ERRL SYS$INPUT REC
$ WRITE SYS$OUTPUT "NOT-REACHED"
$ EXIT
$ EOFL:
$ WRITE SYS$OUTPUT "at-eof"
$ EXIT
$ ERRL:
$ WRITE SYS$OUTPUT "wrong-err"
EOC
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@T.COM\nREAD/BOGUS SYS$INPUT X\nREAD/DELETE SYS$INPUT X\n' "$VDIR" | $VMSDCL 2>&1
