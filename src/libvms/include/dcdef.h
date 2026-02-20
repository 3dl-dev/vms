/*
 * DCDEF.H - VMS Device Class Definitions
 *
 * OpenVMX compatibility layer - Defines the DC$_ device class constants
 * used with DVI$_DEVCLASS in sys$getdvi item lists and with
 * sys$device_scan to filter devices by class.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS I/O User's Reference Manual
 */

#ifndef __DCDEF_H
#define __DCDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DC$_ — Device class codes
 *
 * Returned by DVI$_DEVCLASS and used to filter in DVS$_DEVCLASS.
 * ================================================================ */

#define DC$_UNKNOWN         0   /* Unknown device class */
#define DC$_DISK            1   /* Disk device */
#define DC$_TAPE            2   /* Magnetic tape device */
#define DC$_SCOM            3   /* Serial communications device */
#define DC$_CARD            4   /* Card reader */
#define DC$_LP              5   /* Line printer */
#define DC$_TERM            6   /* Terminal */
#define DC$_MAILBOX         7   /* Mailbox */
#define DC$_NET             8   /* Network */
#define DC$_REALTIME        9   /* Real-time device */
#define DC$_WORKSTATION    10   /* Workstation */
#define DC$_SCANNER        11   /* Scanner */
#define DC$_PRINTER        12   /* Printer */

#ifdef __cplusplus
}
#endif

#endif /* __DCDEF_H */
