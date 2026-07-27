/*
 * LKIDEF.H - VMS Lock Information (LKI$) Item Codes and Constants
 *
 * OpenVMX compatibility layer - Defines the LKI$_ item codes used
 * with sys$getlki/sys$getlkiw (Get Lock Information) item lists, and
 * the LKI$C_ lock-state constants returned via LKI$_STATE.
 *
 * PROVENANCE: item-code mechanism documented in the public OpenVMS
 * System Services Reference Manual ($GETLKI). Exact numeric values
 * not confirmed against a fetchable public source this session —
 * sequential OVMX assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($GETLKI)
 *            OpenVMS Programming Concepts Manual, Lock Management
 */

#ifndef __LKIDEF_H
#define __LKIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LKI$_ item codes for SYS$GETLKI/SYS$GETLKIW item lists
 * ================================================================ */

#define LKI$_PID           0x0001  /* PID of lock holder (L) */
#define LKI$_RESNAM        0x0002  /* Resource name (T) */
#define LKI$_STATE         0x0003  /* Lock state (B) — see LKI$C_ below */
#define LKI$_GRANTCOUNT    0x0004  /* Number of granted locks on resource (L) */

/* ================================================================
 * LKI$C_ — lock state values returned via LKI$_STATE
 * ================================================================ */

#define LKI$C_GRANTED    1   /* Lock is granted */
#define LKI$C_CONVERT    2   /* Lock is converting (waiting for a new mode) */

#ifdef __cplusplus
}
#endif

#endif /* __LKIDEF_H */
