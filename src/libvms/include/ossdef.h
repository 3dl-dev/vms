/*
 * OSSDEF.H - VMS Object Security Service Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the OSS$_ item codes used with
 * sys$get_security and sys$set_security to query and modify the
 * security profile (owner, protection, ACL) of protected objects.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Guide to System Security
 */

#ifndef __OSSDEF_H
#define __OSSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * OSS$_ — Item codes for sys$get_security / sys$set_security
 * ================================================================ */

#define OSS$_OWNER          1   /* Owner UIC (longword) */
#define OSS$_PROTECTION     2   /* Protection mask (longword) */
#define OSS$_ACL_LENGTH     3   /* Total ACL length in bytes (longword) */
#define OSS$_ACL_READ       4   /* Read entire ACL into buffer */
#define OSS$_ACL_ADD_ENTRY  5   /* Add an ACE to the ACL */
#define OSS$_ACL_DELETE_ENTRY 6 /* Delete an ACE from the ACL */
#define OSS$_ACL_CLEAR      7   /* Delete all ACEs from the ACL */
#define OSS$_CLASS_PROT     8   /* Information classification protection */
#define OSS$_PRIVS          9   /* Privilege requirements */

/* ================================================================
 * OSS$M_ — Flags for sys$get_security / sys$set_security
 * ================================================================ */

#define OSS$M_RELAX_ACCESS  0x00000001  /* Relax normal access restrictions */
#define OSS$M_WLOCK         0x00000002  /* Write-lock the object */

/* ================================================================
 * OSS$C_ — Object class codes (for the "objclass" argument)
 * ================================================================ */

#define OSS$C_FILE          1   /* File object */
#define OSS$C_DEVICE        2   /* Device object */
#define OSS$C_VOLUME        3   /* Volume object */
#define OSS$C_QUEUE         4   /* Queue object */
#define OSS$C_SYMBIONT      5   /* Symbiont object */
#define OSS$C_GROUP_GLOBAL  6   /* Group global section */
#define OSS$C_SYSTEM_GLOBAL 7   /* System global section */

#ifdef __cplusplus
}
#endif

#endif /* __OSSDEF_H */
