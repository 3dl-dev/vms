#!/bin/bash
# TEST: READ validates its qualifiers and honours /END_OF_FILE and /ERROR labels (vms-097).
# EXPECT: contains:first=one
# EXPECT: contains:at-eof
# EXPECT_NOT: contains:NOT-REACHED
# EXPECT_NOT: contains:wrong-err
# EXPECT: contains: \BOGUS\
# EXPECT: contains: \DELETE\
VMSDCL="${VMSDCL:-vmsdcl}"
d=$(mktemp -d)
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
printf 'DEFINE RDDIR "%s"\nSET DEFAULT RDDIR:[000000]\n@T.COM\nREAD/BOGUS SYS$INPUT X\nREAD/DELETE SYS$INPUT X\n' "$d" | $VMSDCL 2>&1
