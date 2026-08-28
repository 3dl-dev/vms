#!/bin/bash
# TEST: SHOW SYMBOL with a wildcard (* or %) lists matching local symbols
# EXPECT: contains:WILDTEST_ONE
# EXPECT: contains:WILDTEST_TWO
# EXPECT_NOT: contains:%DCL-W-NOLCL
# EXPECT_NOT: contains:no symbol "WILD*"
#
# vms-9344c: "SHOW SYMBOL *" used to be a LITERAL lookup of a symbol named
# "*", which failed %DCL-W-NOLCL. VMS treats * / % as a wildcard pattern
# matched against the symbol table. This is fully checkable on a host with no
# executive because DCL symbols live in the DCL session itself. Two local
# symbols are defined and a prefix wildcard must list both; a bare "*" must
# not raise NOLCL.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WILDTEST_ONE = "alpha"\nWILDTEST_TWO = "beta"\nSHOW SYMBOL WILD*\nSHOW SYMBOL *\n' | $VMSDCL 2>&1
