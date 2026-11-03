#!/bin/bash
# TEST: a DECnet node access string is ONE parameter (rd vms-a8a lab)
#
# `COPY VAX1"SYSTEM pw"::T1.TXT X.TXT` -- the quoted access string sits INSIDE
# the first parameter with no blank around it, and DCL keeps adjacent tokens as
# one parameter. The lexer split it at the quotes into three parameters, so
# COPY never saw a NODE:: spec and tried a LOCAL copy of a file named "VAX1"
# (%RMS-E-FNF -- found live on a booted image against a real VAX). With the
# parameter intact, COPY's input is RMS's remote access to node VAX1 (rd
# vms-0a7); on a host with no _NET: device the connect fails honestly:
# %COPY-E-OPENIN on the whole node spec, -RMS-E-FND and the $ASSIGN failure.
#
# EXPECT: contains:%COPY-E-OPENIN, error opening VAX1"SYSTEM password"::T1.TXT as input
# EXPECT: contains:-RMS-E-FND
# EXPECT: contains:$STATUS == "%X10000908"
# EXPECT_NOT: contains:%RMS-E-FNF
# EXPECT_NOT: contains:- VAX1
# EXPECT_NOT: contains:secret
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'COPY VAX1"SYSTEM secret"::T1.TXT X.TXT\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
