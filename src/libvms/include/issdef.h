/*
 * ISSDEF.H - VMS Impersonate System Service Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the ISS$_ item codes used with
 * sys$persona_create, sys$persona_assume, sys$persona_clone, and related
 * persona system services (impersonation).
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Guide to System Security
 */

#ifndef __ISSDEF_H
#define __ISSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ISS$_ — Impersonate (persona) system service item codes
 *
 * Used in the item list passed to sys$persona_create and related
 * persona system services.
 * ================================================================ */

#define ISS$_NOAUDIT        11   /* Suppress audit records for this persona */
#define ISS$_USERNAME       9   /* Username for the new persona */
#define ISS$_UIC            22   /* UIC (User Identification Code) */
#define ISS$_PERSONA_ID     20   /* Persona identifier */

/* ================================================================
 * ISS$C_ — Persona-id constants
 *
 * ORACLE-PINNED 2026-08-13 on OpenVMS VAX V7.3 (lab-2 node VAX1):
 * $ISSDEF assembled as GLOBAL symbols, value read from the object GSD
 * via ANALYZE/OBJECT/GSD (documented tool output, Rule 8).  This is a
 * standalone persona-id constant, independent of OVMX's ISS$_ item-code
 * numbering, so the authentic value is used directly.
 * ================================================================ */

#define ISS$C_ID_NATURAL    1   /* The natural (login) persona id */

#ifdef __cplusplus
}
#endif

#endif /* __ISSDEF_H */
