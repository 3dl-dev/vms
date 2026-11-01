/*
 * SYSEVTDEF.H - VMS System Event Definitions
 *
 * OpenVMX compatibility layer - Defines the SYSEVT$C_ event type codes
 * and SYSEVT$M_ flag bits used with sys$set_system_event and
 * sys$clear_system_event to register for system-level event ASTs.
 *
 * System events include CPU state transitions, volume mount/dismount,
 * and other system-wide events.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __SYSEVTDEF_H
#define __SYSEVTDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * SYSEVT$C_ — System event type codes
 *
 * Passed as the "event" argument to sys$set_system_event.
 * ================================================================ */

#define SYSEVT$C_ADD_ACTIVE_CPU     4   /* CPU added to active set */
#define SYSEVT$C_DEL_ACTIVE_CPU     5   /* CPU removed from active set */

/* ================================================================
 * SYSEVT$M_ — System event flag bits
 *
 * Passed as the "flags" argument to sys$set_system_event.
 * ================================================================ */

#define SYSEVT$M_REPEAT_NOTIFY  0x00000001  /* Re-register after each event */

#ifdef __cplusplus
}
#endif

#endif /* __SYSEVTDEF_H */
