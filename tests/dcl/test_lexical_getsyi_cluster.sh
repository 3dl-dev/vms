#!/bin/bash
# TEST: F$GETSYI cluster items are ANSWERED, not left to the "0" fall-through
# (rd vms-b6f). On a host with no connection manager a real standalone VMS
# reports CLUSTER_NODES 0 and CLUSTER_MEMBER FALSE; the item names must be
# recognised (CLUSTER_MEMBER used to read "0", which is not a DCL boolean).
# The values on a real member come from the CLUB (test_cluster_api_getsyi).
# EXPECT: contains:M = "FALSE"
# EXPECT: regex:N = 0 +Hex
# EXPECT: regex:Q = [0-9]+ +Hex
# EXPECT: regex:V = [0-9]+ +Hex
# EXPECT: regex:E = [0-9]+ +Hex
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'M = F$GETSYI("CLUSTER_MEMBER")\nN = F$GETSYI("CLUSTER_NODES")\nQ = F$GETSYI("CLUSTER_QUORUM")\nV = F$GETSYI("CLUSTER_VOTES")\nE = F$GETSYI("EXPECTED_VOTES")\nSHOW SYMBOL M\nSHOW SYMBOL N\nSHOW SYMBOL Q\nSHOW SYMBOL V\nSHOW SYMBOL E\n' | $VMSDCL 2>&1
