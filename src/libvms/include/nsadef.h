/*
 * NSADEF.H - VMS Security Audit (NSA$) Item Codes and Constants
 *
 * OpenVMX compatibility layer - Defines the NSA$_ item codes used
 * with sys$audit_eventw, and the NSA$C_/NSA$M_ constants used with
 * sys$format_audit and sys$audit_eventw.
 *
 * PROVENANCE: the item-list mechanism and general purpose of these
 * symbols are documented in the public OpenVMS Guide to System
 * Security / System Services Reference Manual ($AUDIT_EVENT,
 * $FORMAT_AUDIT). Exact numeric values could NOT be confirmed against
 * a fetchable public source in this session — sequential OVMX
 * assignment (clean-room, CLAUDE.md rule 8), matching the numbering
 * style already used elsewhere in this tree (e.g. dvidef.h). Flagged
 * in vms-531 findings for operator sign-off.
 *
 * Reference: OpenVMS Guide to System Security
 *            OpenVMS System Services Reference Manual ($AUDIT_EVENT,
 *            $FORMAT_AUDIT)
 */

#ifndef __NSADEF_H
#define __NSADEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * NSA$_ item codes for SYS$AUDIT_EVENTW item lists
 * ================================================================ */

#define NSA$_IMAGE_NAME         0x0001  /* Image name (T) */
#define NSA$_EVENT_TYPE         0x0002  /* Event type code (L) */
#define NSA$_EVENT_SUBTYPE      0x0003  /* Event subtype code (L) */
#define NSA$_ALARM_NAME         0x0004  /* Alarm journal name (T) */
#define NSA$_AUDIT_NAME         0x0005  /* Audit journal name (T) */
#define NSA$_OBJECT_CLASS       0x0006  /* Object class name (T) */
#define NSA$_ACCESS_DESIRED     0x0007  /* Access mask requested (L) */
#define NSA$_FINAL_STATUS       0x0008  /* Final completion status (L) */
#define NSA$_DEVICE_NAME        0x0009  /* Device name (T) */

/* ================================================================
 * NSA$C_ — event/subtype and format-style class codes
 * ================================================================ */

#define NSA$C_MSG_OBJ_ACCESS       1   /* Object-access message class */
#define NSA$C_OBJ_ACCESS           1   /* Object-access event subtype */
#define NSA$C_FORMAT_STYLE_FULL    1   /* Full (verbose) audit record format */

/* ================================================================
 * NSA$M_ — SYS$AUDIT_EVENTW flags bit masks
 * ================================================================ */

#define NSA$M_NOEVTCHECK    0x00000001  /* Skip alarm/audit-enabled check */
#define NSA$M_FLUSH         0x00000002  /* Flush record to journal immediately */

#ifdef __cplusplus
}
#endif

#endif /* __NSADEF_H */
