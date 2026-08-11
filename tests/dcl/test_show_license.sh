#!/bin/bash
# TEST: LMF grant-all compatibility surface -- SHOW LICENSE renders the real
#       VMS-format licensed state (not the old fabricated rows) and F$LICENSE
#       grants any queried product (vms-79b3 / vms-fdf, licensing-stance-grant-all).
#
# WHAT THIS GATES, AND WHY IT IS A FAIL-ON-FACADE TRIPWIRE.
#
# Operator ruling 2026-08-11: OVMX has no reason to gate/meter/enforce
# licenses. The License Management Facility exists ONLY so software that
# queries a license and refuses to run without one passes -- so OVMX
# implements it as grant-all. The GOVERNING PRINCIPLE (parity program
# vms-8ad) is that OPERATION must be VMS-exact while IDENTITY stays honestly
# OVMX: the SHOW LICENSE column layout and the F$LICENSE return semantics
# match real VMS byte-for-byte, but the product/producer strings say OVMX and
# the always-grant POLICY is labeled an OVMX design choice.
#
# SHOW LICENSE format is oracle-pinned. The banner "Active licenses on node
# <n>:", its blank line, the "------- Product ID --------    ---- Rating
# ----- -- Version --" rule, and the "Product ... Producer ... Units Avail
# Activ Version Release ... Termination" column header reproduce a real
# OpenVMS VAX V7.3 `SHOW LICENSE` transcript captured off the reference lab
# (lab-2 node VAX1, 11-AUG-2026), Rule 8 clean-room.
#
# THE FACADE THIS REPLACES (dcl_cmd_show.c cmd_show_license, pre-change)
# printed "Active licenses on this node:" and a column header reading
# "Product Name          Producer  Units  Avail  Actv  Version  Termination"
# -- note "this node", "Product Name", the misspelled "Actv", and NO
# "Release" column. None of those tokens appear in real VMS output. Restore
# the facade and the EXPECTs below (real tokens) go missing and the
# EXPECT_NOTs (facade-only tokens) trip. So this is a discriminating
# assertion about cmd_show_license()'s own source, not decoration.
#
# F$LICENSE is a modern-VMS lexical (VSI OpenVMS DCL Dictionary: returns 1
# when the named license is loaded, 0 otherwise). It is absent on VAX V7.3 --
# the lab-2 VAX1 oracle answers %DCL-W-IVFNAM -- so a tree WITHOUT the new
# lex_license would answer IVFNAM here too; the EXPECT_NOT on IVFNAM plus the
# GRANT_* EXPECTs make this fail before the change and pass after. Grant-all
# means ANY product name returns 1, so two arbitrary made-up names are
# queried to prove it is grant-by-query, not a fixed list.
#
# EXPECT: contains:Active licenses on node
# EXPECT: contains:------- Product ID --------    ---- Rating ----- -- Version --
# EXPECT: contains:Activ Version Release
# EXPECT: contains:OVMX-VMSCLUSTER
# EXPECT: contains:GRANT_BOGUS_OK
# EXPECT: contains:GRANT_OTHER_OK
# EXPECT_NOT: contains:Active licenses on this node:
# EXPECT_NOT: contains:Product Name
# EXPECT_NOT: contains:Actv
# EXPECT_NOT: contains:GRANT_BOGUS_FAIL
# EXPECT_NOT: contains:%DCL-W-IVFNAM
VMSDCL="${VMSDCL:-vmsdcl}"

# SHOW LICENSE (format-exact) + F$LICENSE grant-by-query for two arbitrary
# product names that are deliberately not any real PAK. F$LICENSE returns an
# integer (1 = loaded), so it is compared as an integer (.EQ./.NE.).
printf 'SHOW LICENSE\n%s\n%s\n%s\n%s\n%s\n' \
  'GOT = F$LICENSE("BOGUS-PRODUCT-9Z")' \
  'OTHER = F$LICENSE("SOME-OTHER-THING-42")' \
  'IF GOT .EQ. 1 THEN WRITE SYS$OUTPUT "GRANT_BOGUS_OK"' \
  'IF OTHER .EQ. 1 THEN WRITE SYS$OUTPUT "GRANT_OTHER_OK"' \
  'IF GOT .NE. 1 THEN WRITE SYS$OUTPUT "GRANT_BOGUS_FAIL"' \
  | $VMSDCL 2>&1
