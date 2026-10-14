#!/bin/bash
# TEST: DEFINE/ASSIGN/DEASSIGN /TABLE=name reach a REAL logical-name table,
#       and a table this node does not hold is refused (rd vms-aae)
# EXPECT: contains:"AAE_DEF" = "FROM_DEFINE" (LNM$PROCESS_TABLE)
# EXPECT: contains:"AAE_ASG" = "FROM_ASSIGN" (LNM$PROCESS_TABLE)
# EXPECT: contains:%SYSTEM-F-NOLOGTAB
# EXPECT: contains:%DCL-W-NOTIMPL, DEASSIGN /ALL with /TABLE=
# EXPECT_NOT: contains:%DCL-W-IVQUAL
# EXPECT_NOT: contains:ASSIGN /TABLE is not yet implemented
#
# WHAT THIS GATES (rd vms-aae; fire G's prediction H4,
# tests/lab/captures/vms-94a-cwlnm-oracle-20261013/PREDICTION.md).
# /TABLE=name is the GENERAL form of the logical-name commands -- /PROCESS,
# /JOB, /GROUP and /SYSTEM are shorthands for four well-known tables, and the
# executive now also holds the CLUSTERWIDE tables (rd vms-94a). DEFINE and
# DEASSIGN rejected /TABLE at the parser (%DCL-W-IVQUAL) and ASSIGN refused it
# %DCL-W-NOTIMPL, so on a booted OVMX in a real cluster there was NO way to
# say DEFINE/TABLE=LNM$SYSCLUSTER at all: lab fire G could not even reach the
# executive's own SS$_UNSUPPORTED check, which is what it was trying to prove.
#
# The three commands now resolve the table once (dcl_lnm_table_of in
# src/vmsdcl/dcl_cmd_io.c) and hand the NAME to the logical-name manager,
# which routes it to the executive table it names -- including a clusterwide
# one -- or refuses SS$_NOLOGTAB. Nothing here invents a table: a name that is
# no table on this node is refused with the same %SYSTEM-F-NOLOGTAB that SHOW
# LOGICAL/TABLE= already gives, and nothing is written.
#
# A clusterwide table cannot be exercised here (it needs /dev/vms and a
# cluster); LNM$PROCESS_TABLE is the table this environment really holds, so
# that is what the positive half uses. The executive-side behaviour for a
# clusterwide table -- served alone, refused SS$_UNSUPPORTED with other
# members or while this node's copy is short -- is pinned by
# tests/cluster/host/test_lnm_cluster.c.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'DEFINE/TABLE=LNM$PROCESS_TABLE AAE_DEF "FROM_DEFINE"\nSHOW LOGICAL AAE_DEF\nASSIGN/TABLE=PROCESS FROM_ASSIGN AAE_ASG\nSHOW LOGICAL AAE_ASG\nDEASSIGN/TABLE=PROCESS AAE_DEF\nSHOW LOGICAL AAE_DEF\nDEFINE/TABLE=NO_SUCH_TABLE AAE_BAD "X"\nSHOW LOGICAL AAE_BAD\nDEASSIGN/ALL/TABLE=PROCESS\n' | $VMSDCL 2>&1
