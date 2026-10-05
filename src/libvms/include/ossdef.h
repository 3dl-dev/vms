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

#define OSS$_OWNER          21   /* Owner UIC (longword) */
#define OSS$_PROTECTION     22   /* Protection mask (longword) */
#define OSS$_ACL_LENGTH     11   /* Total ACL length in bytes (longword) */
#define OSS$_ACL_READ       17   /* Read entire ACL into buffer */
#define OSS$_ACL_ADD_ENTRY  3   /* Add an ACE to the ACL */
#define OSS$_ACL_DELETE_ENTRY 4 /* Delete an ACE from the ACL */
#define OSS$_ACL_CLEAR      7   /* Delete all ACEs from the ACL */
#define OSS$_CLASS_PROT     8   /* Information classification protection */
#define OSS$_PRIVS          9   /* Privilege requirements */

/* ================================================================
 * OSS$M_ — Flags for sys$get_security / sys$set_security
 * ================================================================ */

#define OSS$M_RELAX_ACCESS  0x80000000  /* [OVMX] relax normal access restrictions (not in V7.3 $OSSDEF) */
#define OSS$M_WLOCK         0x1  /* Write-lock the object */
/* OVMX-private bit (vms-f16, Rule 8 design choice).  The 2026-08-13
 * $OSSDEF oracle dump (OpenVMS VAX V7.3, lab-2) shows real VMS uses
 * OSS$M_RELCTX=2 and OSS$M_WLOCK=1 -- but OVMX has already assigned
 * bit 0x02 to OSS$M_WLOCK, so the authentic value would collide.
 * OVMX therefore assigns OSS$M_RELCTX the next free bit and labels it
 * as an OVMX representation, not a VMS-authentic value. */
#define OSS$M_RELCTX        0x2  /* Release object security context */
/* OSS$M_LOCAL is 4 on the OpenVMS VAX V7.3 oracle; OVMX already spent 0x04 on
 * OSS$M_RELCTX (above), so, like RELCTX, it takes the next free bit and is an OVMX
 * representation, not a VMS-authentic value (vms-619). */
#define OSS$M_LOCAL         0x4  /* Operate on the local (non-cluster) object */

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
