/*
 * STENVDEF.H - VMS Static Environment Variable Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the STENV$K_ constants used
 * with sys$getenv and sys$setenv to query and modify static environment
 * variables (boot parameters and firmware settings) on Alpha systems.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Alpha Boot Management Manual
 */

#ifndef __STENVDEF_H
#define __STENVDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * STENV$K_ — Environment variable function/type codes
 *
 * Passed as the "function" field in the item list to sys$getenv
 * to specify which environment variable to retrieve.
 * ================================================================ */

#define STENV$K_BOOTED_DEV      1   /* Device the system was booted from */
#define STENV$K_BOOTDEF_DEV     2   /* Default boot device (saved in NVRAM) */
#define STENV$K_BOOTED_FILE     3   /* Boot file used */
#define STENV$K_BOOTED_FLAGS    4   /* Boot flags used */
#define STENV$K_BOOTED_OSFLAGS  5   /* OS-specific boot flags */
#define STENV$K_BOOT_RESET      6   /* Reset to factory boot defaults */
#define STENV$K_RESTART_DEV     7   /* Restart device name */
#define STENV$K_CONSOLE_TYPE    8   /* Console type (serial, graphic, etc.) */

#ifdef __cplusplus
}
#endif

#endif /* __STENVDEF_H */
