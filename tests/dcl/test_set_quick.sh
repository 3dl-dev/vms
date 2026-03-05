#!/bin/bash
# TEST: SET quick-win subcommands produce correct VMS-format output
# EXPECT: contains:%SET-I-NOTAVAIL, DECnet is not available on this system
# EXPECT: contains:%SET-I-INTSET, auditing enabled
# EXPECT: contains:security auditing is currently enabled.
# EXPECT: contains:%SET-I-INTSET, auditing disabled
# EXPECT: contains:%SET-I-INTSET, accounting enabled
# EXPECT: contains:Accounting is currently enabled.
# EXPECT: contains:%SET-I-INTSET, accounting disabled
# EXPECT: contains:%SET-I-NOTIMPL, SET VOLUME requires a mounted VMSFS volume
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'SET HOST\nSET AUDIT /ENABLE\nSHOW AUDIT\nSET AUDIT /DISABLE\nSET ACCOUNTING /ENABLE\nSHOW ACCOUNTING\nSET ACCOUNTING /DISABLE\nSET VOLUME\n' | $VMSDCL 2>&1
