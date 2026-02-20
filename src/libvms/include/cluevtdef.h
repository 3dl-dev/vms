/*
 * CLUEVTDEF.H - VMS Cluster Event Definitions
 *
 * OpenVMX compatibility layer - Defines the CLUEVT$C_ constants used
 * with sys$setcluevt, sys$tstcluevt, and sys$clrcluevt to register
 * and manage cluster event ASTs.
 *
 * Cluster events notify processes when nodes join or leave the cluster.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __CLUEVTDEF_H
#define __CLUEVTDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CLUEVT$C_ — Cluster event type codes
 *
 * Passed to sys$setcluevt as the "event" argument to specify which
 * cluster event to receive notifications for.
 * ================================================================ */

#define CLUEVT$C_ADD        1   /* A node was added to the cluster */
#define CLUEVT$C_REMOVE     2   /* A node was removed from the cluster */

/* ================================================================
 * CLUEVT$M_ — Cluster event flag bits
 * ================================================================ */

#define CLUEVT$M_REPEAT     0x00000001  /* Repeat notification after each event */

#ifdef __cplusplus
}
#endif

#endif /* __CLUEVTDEF_H */
