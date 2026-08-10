#!/bin/bash
# TEST: DEFINE name val1,val2,val3 creates a search-list logical; SHOW LOGICAL
#       displays every equivalence string (vms-420 -- previously BAZ, and
#       anything after the first value, was silently dropped: DEFINE FOO
#       BAR,BAZ created "FOO" = "BAR" with no trace of BAZ). Shape is
#       oracle-pinned against OpenVMS VAX V7.3 (lab-2/vaxlab-0, 2026-08-10):
#       the first value shares the name/table line, each further value gets
#       its own "        = \"value\"" continuation line.
# EXPECT: contains:"FOO" = "BAR" (LNM$PROCESS_TABLE)
# EXPECT: contains:        = "BAZ"
# EXPECT: contains:        = "QUX"
# EXPECT_NOT: contains:"BAR,BAZ"
# EXPECT_NOT: contains:"BAR,BAZ,QUX"
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DEFINE FOO BAR,BAZ,QUX\nSHOW LOGICAL FOO\n' | $VMSDCL 2>&1
