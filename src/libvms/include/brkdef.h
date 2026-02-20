/*
 * BRKDEF.H - VMS Broadcast Message Definitions
 *
 * OpenVMX compatibility layer - Defines the BRK$_ constants used with
 * the sys$brkthruw (broadcast through) system service to send messages
 * to terminals and processes.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __BRKDEF_H
#define __BRKDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * BRK$C_ — Broadcast target type codes
 *
 * Passed as the "sendtype" argument to sys$brkthruw to specify
 * how the target is identified.
 * ================================================================ */

#define BRK$C_DEVICE        1   /* Send to a specific device */
#define BRK$C_USERNAME      2   /* Send to all terminals of a username */
#define BRK$C_ALLTERMS      3   /* Send to all terminals */
#define BRK$C_USER1         4   /* Send to user terminals (type 1) */
#define BRK$C_USER2         5   /* Send to user terminals (type 2) */

/* ================================================================
 * BRK$M_ — Broadcast flag bits
 *
 * Passed as the "flags" argument to sys$brkthruw.
 * ================================================================ */

#define BRK$M_CLUSTER       0x00000001  /* Send to all nodes in cluster */
#define BRK$M_NOQUEUE       0x00000002  /* Do not queue if terminal is busy */

#ifdef __cplusplus
}
#endif

#endif /* __BRKDEF_H */
