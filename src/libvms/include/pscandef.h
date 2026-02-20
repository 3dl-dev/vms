/*
 * PSCANDEF.H - VMS Process Scan Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the PSCAN$_ item codes and
 * PSCAN$M_ flag bits used with sys$process_scan to enumerate processes
 * with filtering criteria.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __PSCANDEF_H
#define __PSCANDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PSCAN$_ — Item codes for the sys$process_scan item list
 *
 * Each item in the process scan item list specifies a filter
 * criterion.  The "flags" field contains comparison operators
 * (PSCAN$M_EQL, PSCAN$M_NEQ, etc.).
 * ================================================================ */

#define PSCAN$_RESERVED         0   /* Reserved */
#define PSCAN$_PID              1   /* Process identifier (PID) */
#define PSCAN$_GRP              2   /* Group number (UIC group) */
#define PSCAN$_MEM              3   /* Member number (UIC member) */
#define PSCAN$_STATE            4   /* Process state code */
#define PSCAN$_PRCNAM           5   /* Process name */
#define PSCAN$_USERNAME         6   /* Username */
#define PSCAN$_NODE_CSID        7   /* Cluster system ID (CSID) of node */
#define PSCAN$_OWNER            8   /* Owner PID */
#define PSCAN$_JOBTYPE          9   /* Job type code */
#define PSCAN$_PRCCNT          10   /* Subprocess count */
#define PSCAN$_TERMINAL        11   /* Terminal name */
#define PSCAN$_UIC             12   /* Full UIC */
#define PSCAN$_MODE            13   /* Access mode */
#define PSCAN$_CURSRV          14   /* Current server process */
#define PSCAN$_MASTER_PID      15   /* Master (top-level) PID */
#define PSCAN$_NODE_VERSION    16   /* VMS version on node */

/* ================================================================
 * PSCAN$M_ — Comparison operator flag bits
 *
 * Used in the "flags" field of each process scan item list entry
 * to specify how the criterion value is compared to the process attribute.
 * ================================================================ */

#define PSCAN$M_EQL     0x00000001  /* Equal to */
#define PSCAN$M_NEQ     0x00000002  /* Not equal to */
#define PSCAN$M_GTR     0x00000004  /* Greater than */
#define PSCAN$M_GEQ     0x00000008  /* Greater than or equal to */
#define PSCAN$M_LSS     0x00000010  /* Less than */
#define PSCAN$M_LEQ     0x00000020  /* Less than or equal to */
#define PSCAN$M_CASE    0x00000040  /* Case-sensitive string comparison */

#ifdef __cplusplus
}
#endif

#endif /* __PSCANDEF_H */
