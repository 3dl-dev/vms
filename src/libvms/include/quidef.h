/*
 * QUIDEF.H - VMS Queue Information (QUI$) Item Codes and Constants
 *
 * OpenVMX compatibility layer - Defines the QUI$_ function/item codes
 * and QUI$M_ status flag bits used with sys$getqui/sys$getquiw and
 * lib$getqui (batch/print queue and job information).
 *
 * PROVENANCE: item/function-code mechanism documented in the public
 * OpenVMS System Services Reference Manual ($GETQUI). Exact numeric
 * values not confirmed against a fetchable public source this
 * session — sequential OVMX assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($GETQUI)
 */

#ifndef __QUIDEF_H
#define __QUIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * QUI$_ function codes (passed as the "function" argument)
 * ================================================================ */

#define QUI$_DISPLAY_QUEUE      0x6  /* Display queue information */
#define QUI$_CANCEL_OPERATION   0x1  /* Cancel outstanding GETQUI context */

/* ================================================================
 * QUI$_ item codes (used both as the "itmlst"/"item" argument and
 * within GETQUI item lists)
 * ================================================================ */

#define QUI$_PROTECTION      0x44  /* Queue protection mask (W) */
#define QUI$_SEARCH_NAME     0x4D  /* Wildcard queue name to search for (T) */
#define QUI$_SEARCH_FLAGS    0x4C  /* Search filter flags (L) — see QUI$M_ below */
#define QUI$_QUEUE_NAME      0x46  /* Queue name (T) */
#define QUI$_SCSNODE_NAME    0x4B  /* SCS node name owning the queue (T) */
#define QUI$_QUEUE_STATUS    0x47  /* Queue status flags (L) — see QUI$M_ below */

/* ================================================================
 * QUI$M_ — SYS$GETQUI(W) search-flags and queue-status bit masks
 * ================================================================ */

#define QUI$M_SEARCH_BATCH      0x4  /* Restrict search to batch queues */
#define QUI$M_QUEUE_IDLE        0x2  /* Queue is idle (no job executing) */
#define QUI$M_QUEUE_AVAILABLE   0x40000  /* Queue is available to accept jobs */
#define QUI$M_QUEUE_CLOSED      0x8000  /* Queue is closed */

#ifdef __cplusplus
}
#endif

#endif /* __QUIDEF_H */
