#!/bin/bash
# TEST: vms-f89 - SYS$NODE is a live system logical (config from logicals,
#       parent vms-704). F$TRNLNM("SYS$NODE") returns the node name in DECnet
#       full-name form (trailing "::"), sourced from the identity SSOT, and a
#       DEFINE overrides it live.
#
# --- Seeded default: SYS$NODE resolves to a non-empty node name ending "::".
#     On origin/main SYS$NODE is ABSENT, so F$TRNLNM returns "" and this fails.
# EXPECT: regex:NODE = "[A-Za-z0-9_$]+::"
#
# --- A DEFINE takes effect live at the next F$TRNLNM.
# EXPECT: contains:NODE2 = "REDEFNODE::"
#
# THE FINDING THIS GATES: config that should come FROM a logical was absent, so
# there was no SYS$NODE to translate or redefine. Doc pin (VSI OpenVMS DCL
# Dictionary, F$GETSYI / SYS$NODE): SYS$NODE is the local node name in DECnet
# full-name form; the idiom NODE = F$TRNLNM("SYS$NODE") - "::" relies on the
# "::" delimiter. Identity stays OVMX (node from SYSGEN SCSNODE).
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'NODE = F$TRNLNM("SYS$NODE")\nSHOW SYMBOL NODE\nDEFINE SYS$NODE "REDEFNODE::"\nNODE2 = F$TRNLNM("SYS$NODE")\nSHOW SYMBOL NODE2\n' | $VMSDCL 2>&1
