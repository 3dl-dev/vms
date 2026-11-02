#!/bin/bash
# TEST: DIRECTORY and TYPE of a DECnet node:: spec are RMS remote access (rd vms-6a4, vms-0a7)
#
# `DIRECTORY VAX1"SYSTEM pw"::` and `TYPE VAX1"SYSTEM pw"::LOGIN.COM` name files
# on a remote node. RMS takes them to the node's FAL ($PARSE/$SEARCH/$OPEN over
# _NET:), never to the local disk and never to another image. On a host with no
# _NET: device the connect fails honestly, in VMS's words, on the spec with the
# password shown as the word "password" -- the password itself is never echoed.
# The form is the real VAX's for a refused connect (TYPE ... -RMS-E-FND then the
# connect failure, tests/lab/captures/decnet-fal-verbs-20261008/vax1-console.txt;
# DIRECTORY's %DIRECT-E-OPENIN, rd vms-c18 live VAX); here the connect failure is
# the $ASSIGN _NET: one, and $STATUS is it with STS$M_INHIB_MSG.
#
# EXPECT: contains:%DIRECT-E-OPENIN, error opening VAX1"SYSTEM password"::*.*;* as input
# EXPECT: contains:%TYPE-W-SEARCHFAIL, error searching for VAX1"SYSTEM password"::LOGIN.COM;
# EXPECT: contains:-RMS-E-FND, ACP file or directory lookup failed
# EXPECT: contains:-SYSTEM-W-NOSUCHDEV, no such device available
# EXPECT: contains:$STATUS == "%X10000908"
# EXPECT_NOT: contains:$STATUS == "%X00000001"
# EXPECT_NOT: contains:%DIRECT-W-NOFILES
# EXPECT_NOT: contains:NETNOTAVAIL
# EXPECT_NOT: contains:DECNETD
# EXPECT_NOT: contains:secret
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DIRECTORY VAX1"SYSTEM secret"::\nSHOW SYMBOL $STATUS\nTYPE VAX1"SYSTEM secret"::LOGIN.COM\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
