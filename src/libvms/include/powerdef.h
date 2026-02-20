/*
 * POWERDEF.H - VMS Power Control Definitions
 *
 * OpenVMX compatibility layer - Defines the POWER$C_ constants used
 * with sys$power_control on IA64 (Integrity Server) systems to query
 * or change the CPU power management mode.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            HP Integrity Server iLO documentation
 */

#ifndef __POWERDEF_H
#define __POWERDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * POWER$C_ — Power control mode codes
 *
 * Passed as the "new_setting" argument to sys$power_control, and
 * returned via the "old_setting" output argument.
 * ================================================================ */

#define POWER$C_HIGH_PERF   1   /* High performance mode */
#define POWER$C_LOW_POWER   2   /* Low power / energy saving mode */
#define POWER$C_EFFICIENCY  3   /* Balanced efficiency mode */

#ifdef __cplusplus
}
#endif

#endif /* __POWERDEF_H */
