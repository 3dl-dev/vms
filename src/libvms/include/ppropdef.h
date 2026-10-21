/*
 * PPROPDEF.H - VMS Process Property Definitions
 *
 * OpenVMX compatibility layer - Defines the PPROP$C_ constants used
 * with sys$set_process_properties and sys$set_process_propertiesw
 * to control process-specific behavior such as filename parsing style.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __PPROPDEF_H
#define __PPROPDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PPROP$C_ — Process property item codes
 *
 * Passed as the "property" argument to sys$set_process_propertiesw.
 * ================================================================ */

#define PPROP$C_PARSE_STYLE_PERM    1   /* Permanent filename parse style */
#define PPROP$C_PARSE_STYLE_TEMP    0   /* Temporary filename parse style */

#ifdef __cplusplus
}
#endif

#endif /* __PPROPDEF_H */
