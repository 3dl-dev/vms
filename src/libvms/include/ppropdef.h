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
#define PPROP$C_PARSE_STYLE_TEMP    2   /* Temporary filename parse style */

/* ================================================================
 * PPROP$K_ — Process property value codes
 *
 * Used as values for PPROP$C_PARSE_STYLE_* properties.
 * ================================================================ */

#define PPROP$K_PARSE_TRADITIONAL   0   /* Traditional VMS filename parsing */
#define PPROP$K_PARSE_EXTENDED      1   /* Extended (ODS-5) filename parsing */

/* ================================================================
 * PPROP$M_ — Flag bits for sys$set_process_properties flags argument
 * ================================================================ */

#define PPROP$M_NOAUDIT             0x00000001  /* Suppress audit record */

#ifdef __cplusplus
}
#endif

#endif /* __PPROPDEF_H */
