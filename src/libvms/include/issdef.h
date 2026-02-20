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

#define ISS$_NOAUDIT        1   /* Suppress audit records for this persona */
#define ISS$_USERNAME       2   /* Username for the new persona */
#define ISS$_PASSWORD       3   /* Password for authentication */
#define ISS$_UIC            4   /* UIC (User Identification Code) */
#define ISS$_PRIVILEGES     5   /* Privilege mask */
#define ISS$_AUTH_PRIVS     6   /* Authorized privilege mask */
#define ISS$_DEFPRIV        7   /* Default privilege mask */
#define ISS$_PERSONA_ID     8   /* Persona identifier */
#define ISS$_RIGHTS_LIST    9   /* Rights identifiers list */

/* ================================================================
 * ISS$M_ — Flag bits for ISS$_NOAUDIT and other flag items
 * ================================================================ */

#define ISS$M_NOAUDIT       0x00000001  /* Suppress audit logging */

#ifdef __cplusplus
}
#endif

#endif /* __ISSDEF_H */
