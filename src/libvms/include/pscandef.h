/*
 * PSCANDEF.H - $PROCESS_SCAN item codes and item-specific flags.
 *
 * Every value is the $PSCANDEF of OpenVMS VAX V7.3 (docs/oracle/
 * vax73-starlet-defs/PSCANDEF.txt, LIBRARY/EXTRACT of the node's own
 * STARLET.MLB). The symbols are case-blind on VMS; this header spells them in
 * upper case. (The former header carried invented values for every item code
 * and most flags: PSCAN$_USERNAME was 6, the system's is 23.)
 */

#ifndef __PSCANDEF_H
#define __PSCANDEF_H

/* Item codes ($PROCESS_SCAN item list) */
#define PSCAN$_BEGIN                     0
#define PSCAN$_ACCOUNT                   1
#define PSCAN$_AUTHPRI                   2
#define PSCAN$_CURPRIV                   3
#define PSCAN$_GRP                       4
#define PSCAN$_HW_MODEL                  5
#define PSCAN$_HW_NAME                   6
#define PSCAN$_JOBPRCCNT                 7
#define PSCAN$_JOBTYPE                   8
#define PSCAN$_MASTER_PID                9
#define PSCAN$_MEM                       10
#define PSCAN$_MODE                      11
#define PSCAN$_NODE_CSID                 12
#define PSCAN$_NODENAME                  13
#define PSCAN$_OWNER                     14
#define PSCAN$_PRCCNT                    15
#define PSCAN$_PRCNAM                    16
#define PSCAN$_PRI                       17
#define PSCAN$_PRIB                      18
#define PSCAN$_STATE                     19
#define PSCAN$_STS                       20
#define PSCAN$_TERMINAL                  21
#define PSCAN$_UIC                       22
#define PSCAN$_USERNAME                  23
#define PSCAN$_GETJPI_BUFFER_SIZE        24
#define PSCAN$_PSCAN_CONTROL_FLAGS       25
#define PSCAN$_RESERVED2                 26
#define PSCAN$_RESERVED3                 27
#define PSCAN$_SCHED_CLASS_NAME          28
#define PSCAN$_END                       29

/* Item-specific flags (the item list's fourth longword) */
#define PSCAN$M_OR                       0x00000001
#define PSCAN$M_BIT_ALL                  0x00000002
#define PSCAN$M_BIT_ANY                  0x00000004
#define PSCAN$M_GEQ                      0x00000008
#define PSCAN$M_GTR                      0x00000010
#define PSCAN$M_LEQ                      0x00000020
#define PSCAN$M_LSS                      0x00000040
#define PSCAN$M_PREFIX_MATCH             0x00000080
#define PSCAN$M_WILDCARD                 0x00000100
#define PSCAN$M_CASE_BLIND               0x00000200
#define PSCAN$M_EQL                      0x00000400
#define PSCAN$M_NEQ                      0x00000800

#define PSCAN$K_TYPE                     129
#define PSCAN$S_ITEM_SPECIFIC_FLAGS      2
#define PSCAN$V_OR                       0
#define PSCAN$V_BIT_ALL                  1
#define PSCAN$V_BIT_ANY                  2
#define PSCAN$V_GEQ                      3
#define PSCAN$V_GTR                      4
#define PSCAN$V_LEQ                      5
#define PSCAN$V_LSS                      6
#define PSCAN$V_PREFIX_MATCH             7
#define PSCAN$V_WILDCARD                 8
#define PSCAN$V_CASE_BLIND               9
#define PSCAN$V_EQL                      10
#define PSCAN$V_NEQ                      11

#endif /* __PSCANDEF_H */
