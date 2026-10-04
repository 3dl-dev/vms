#!/bin/bash
# TEST: a DECnet node access string is ONE parameter (rd vms-a8a lab)
#
# `COPY VAX1"SYSTEM pw"::T1.TXT X.TXT` -- the quoted access string sits INSIDE
# the first parameter with no blank around it, and DCL keeps adjacent tokens as
# one parameter. The lexer split it at the quotes into three parameters, so
# COPY never saw a NODE:: spec and tried a LOCAL copy of a file named "VAX1"
# (%RMS-E-FNF -- found live on a booted image against a real VAX). With the
# parameter intact, COPY routes to the DECnet FAL client; on a host with no
# SYS$SYSTEM:DECNETD.EXE that is the honest %COPY-I-NETNOTAVAIL.
#
# EXPECT: contains:%COPY-I-NETNOTAVAIL
# EXPECT_NOT: contains:%RMS-E-FNF
# EXPECT_NOT: contains:- VAX1
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'COPY VAX1"SYSTEM secret"::T1.TXT X.TXT\n' | $VMSDCL 2>&1
