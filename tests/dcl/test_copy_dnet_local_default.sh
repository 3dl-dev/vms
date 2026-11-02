#!/bin/bash
# TEST: DCL COPY node:: is RMS remote access, never a DECNETD activation (vms-ea8, vms-0a7)
#
# COPY with a node:: input or output goes through RMS ($SEARCH/$OPEN/$GET on
# the node's FAL, $CREATE/$PUT on the local side completed with the process
# default directory -- the vms-ea8 rule). A stand-in SYS$SYSTEM:DECNETD.EXE
# prints any argv it gets: it must never run, and no password may reach a
# message. On a host with no _NET: device the remote side fails honestly.
# NEGCTL: before rd vms-0a7 DCL activated DECNETD.EXE --copy ... --password-fd.
#
# EXPECT: contains:%COPY-E-OPENIN, error opening 1.1"SYSTEM password"::A.TXT as input
# EXPECT: contains:-RMS-E-FND
# EXPECT: contains:$STATUS == "%X10000908"
# EXPECT_NOT: contains:FAKE-DECNETD-ARGV
# EXPECT_NOT: contains:--password-fd
# EXPECT_NOT: contains:secret
VMSDCL="${VMSDCL:-vmsdcl}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf '#!/bin/sh\necho "FAKE-DECNETD-ARGV: $*"\n' > "$T/DECNETD.EXE"
chmod +x "$T/DECNETD.EXE"
printf 'DEFINE SYS$SYSTEM "%s/"\nCOPY 1.1"SYSTEM secret"::A.TXT B.TXT\nSHOW SYMBOL $STATUS\nCOPY C.TXT 1.1"SYSTEM secret"::D.TXT\n' "$T" | $VMSDCL 2>&1
