#!/bin/bash
# TEST: DCL COPY node:: is RMS remote access, never a NETACP activation (vms-ea8, vms-0a7)
#
# COPY with a node:: input or output goes through RMS ($SEARCH/$OPEN/$GET on
# the node's FAL, $CREATE/$PUT on the local side completed with the process
# default directory -- the vms-ea8 rule). A stand-in SYS$SYSTEM:NETACP.EXE
# prints any argv it gets: it must never run, and no password may reach a
# message. On a host with no _NET: device the remote side fails honestly.
# NEGCTL: before rd vms-0a7 DCL activated the retired DECnet daemon image --copy ... --password-fd.
#
# EXPECT: contains:%COPY-E-OPENIN, error opening 1.1"SYSTEM password"::A.TXT as input
# EXPECT: contains:-RMS-E-FND
# EXPECT: contains:$STATUS == "%X10000908"
# EXPECT_NOT: contains:FAKE-NETACP-ARGV
# EXPECT_NOT: contains:--password-fd
# EXPECT_NOT: contains:secret
VMSDCL="${VMSDCL:-vmsdcl}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# RMS remote access is $ASSIGN _NET: + $QIO (rd vms-0a7 S1); it never execs a
# DECnet image with a command line. Plant an argv-echoing file under every
# DECnet image name SYS$SYSTEM holds (NETACP, FAL, RTPAD): none may run.
for img in NETACP.EXE FAL.EXE RTPAD.EXE; do
    printf '#!/bin/sh\necho "FAKE-NETACP-ARGV: $*"\n' > "$T/$img"
    chmod +x "$T/$img"
done
printf 'DEFINE SYS$SYSTEM "%s/"\nCOPY 1.1"SYSTEM secret"::A.TXT B.TXT\nSHOW SYMBOL $STATUS\nCOPY C.TXT 1.1"SYSTEM secret"::D.TXT\n' "$T" | $VMSDCL 2>&1
