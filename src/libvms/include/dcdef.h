/*
 * DCDEF.H - VMS Device Class Definitions
 *
 * OpenVMX compatibility layer - Defines the DC$_ device class constants
 * used with DVI$_DEVCLASS in sys$getdvi item lists and with
 * sys$device_scan to filter devices by class.
 *
 * Values are the authentic OpenVMS $DCDEF constants (non-sequential),
 * grounded clean-room from two independent documented sources; see
 * docs/oracle/vax73-device-class.md (vms-8f7b). Reference: OpenVMS System
 * Services Reference Manual, OpenVMS I/O User's Reference Manual.
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
 * Authentic $DCDEF (SYS$LIBRARY:STARLET.MLB) values — non-sequential,
 * ordered by value. Grounded 02-SEP-2026 from two independent documented
 * sources (docs/oracle/vax73-device-class.md, vms-8f7b).
 *
 * NOTE (multi-source drift caught): DC$_LP is 67, NOT 64 — 64 appeared in an
 * intermediate hand-note but neither documented $DCDEF source carries any
 * class at 64.
 *
 * A LAN/Ethernet controller reports DC$_SCOM (there is no DC$_NET class in
 * real VMS); printers report DC$_LP (there is no DC$_PRINTER/DC$_SCANNER
 * class). The former fabricated symbols DC$_NET/DC$_SCANNER/DC$_PRINTER had
 * no consumers and were removed rather than renumbered (INV-6: a symbol that
 * does not exist in $DCDEF must not be presented as if it does).
 * ================================================================ */

#define DC$_UNKNOWN         0   /* Unknown device class (a.k.a. DC$_ANY) */
#define DC$_DISK            1   /* Disk device */
#define DC$_TAPE            2   /* Magnetic tape device */
#define DC$_SCOM           32   /* Serial-communications / LAN device */
#define DC$_CARD           65   /* Card reader */
#define DC$_TERM           66   /* Terminal */
#define DC$_LP             67   /* Line printer */
#define DC$_WORKSTATION    70   /* Workstation */
#define DC$_REALTIME       96   /* Real-time device */
#define DC$_MAILBOX       160   /* Mailbox */

#ifdef __cplusplus
}
#endif

#endif /* __DCDEF_H */
