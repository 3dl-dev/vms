#!/bin/bash
# TEST: OPEN on SYS$OUTPUT/SYS$INPUT acts on the process-permanent file through RMS; @ reaches a procedure only through RMS (vms-28db, vms-20c94)
# EXPECT: contains:to-terminal
# EXPECT: contains:after-close
# EXPECT: contains:IN=line-one
# EXPECT: contains:FILE=via-channel
# EXPECT: contains:%DCL-E-OPENIN
# EXPECT_NOT: contains:HOST-PATH-RAN
#
# Negative controls for libc drops: a channel OPENed on SYS$OUTPUT that went
# through dup()+fdopen() of the process stream would not follow a DEFINE of
# SYS$OUTPUT to a file (FILE=via-channel must come back through RMS $GET), and
# an @ that fell back to a host fopen() would run a procedure that exists only
# in a host directory outside the system disk (HOST-PATH-RAN).
VMSDCL="${VMSDCL:-vmsdcl}"
TNAME="dcl_ppf_$$"
TDIR="/vms/$TNAME"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
HOST=$(mktemp -d)
mkdir -p "$TDIR"
trap 'rm -rf "$TDIR" "$HOST"' EXIT
printf 'line-one\n' > "$TDIR/IN.TXT"
printf '$ WRITE SYS$OUTPUT "HOST-PATH-RAN"\n' > "$HOST/HP.COM"
cat > "$TDIR/T.COM" <<'EOC'
$ OPEN/WRITE OUT SYS$OUTPUT
$ WRITE OUT "to-terminal"
$ CLOSE OUT
$ WRITE SYS$OUTPUT "after-close"
$ DEFINE SYS$INPUT IN.TXT
$ OPEN/READ INP SYS$INPUT
$ READ INP L
$ CLOSE INP
$ DEASSIGN SYS$INPUT
$ WRITE SYS$OUTPUT "IN=''L'"
$ DEFINE SYS$OUTPUT OUT.TXT
$ OPEN/WRITE OUT SYS$OUTPUT
$ WRITE OUT "via-channel"
$ CLOSE OUT
$ DEASSIGN SYS$OUTPUT
$ OPEN/READ F OUT.TXT
$ READ F R
$ CLOSE F
$ WRITE SYS$OUTPUT "FILE=''R'"
EOC
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@T.COM\nDEFINE HOSTDIR "%s"\n@HOSTDIR:[000000]HP.COM\n' "$VDIR" "$HOST" | $VMSDCL 2>&1
