#!/bin/bash
# TEST: DIRECTORY and TYPE of a DECnet node:: spec go to the FAL client (rd vms-6a4)
#
# `DIRECTORY VAX1"SYSTEM pw"::` and `TYPE VAX1"SYSTEM pw"::LOGIN.COM` name files
# on a remote node. DCL must hand them to SYS$SYSTEM:DECNETD.EXE (--directory /
# --type), never search the local disk for them. On a host with no DECNETD.EXE
# staged that is the honest NETNOTAVAIL, and the password is never echoed.
#
# EXPECT: contains:%DIRECT-I-NETNOTAVAIL
# EXPECT: contains:%TYPE-I-NETNOTAVAIL
# EXPECT_NOT: contains:%DIRECT-W-NOFILES
# EXPECT_NOT: contains:%TYPE-W-SEARCHFAIL
# EXPECT_NOT: contains:secret
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DIRECTORY VAX1"SYSTEM secret"::\nTYPE VAX1"SYSTEM secret"::LOGIN.COM\n' | $VMSDCL 2>&1
