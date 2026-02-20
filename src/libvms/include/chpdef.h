/*
 * CHPDEF.H - VMS Check Protection Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the CHP$_ item codes used
 * with sys$check_access and sys$chkpro to specify what access rights
 * and protection information to check.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __CHPDEF_H
#define __CHPDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CHP$_ — Item codes for sys$check_access item list
 *
 * These codes are used as the item code field (func/code) in the
 * item list passed to sys$check_access and sys$chkpro.
 * ================================================================ */

#define CHP$_ACCESS         1   /* Access rights to check (ARM bitmask) */
#define CHP$_FLAGS          2   /* Flags controlling the check */
#define CHP$_OWNER          3   /* Owner UIC of the object */
#define CHP$_PROT           4   /* Protection mask of the object */
#define CHP$_ACL            5   /* ACL to check against */
#define CHP$_USERNAME       6   /* Username to check access for */
#define CHP$_PRIV           7   /* Privilege mask of the accessor */

/* ================================================================
 * CHP$M_ — Flag bits for CHP$_FLAGS item
 * ================================================================ */

#define CHP$M_READ          0x00000001  /* Check read access */
#define CHP$M_WRITE         0x00000002  /* Check write access */
#define CHP$M_NOPRIV        0x00000004  /* Do not consider privileges */

#ifdef __cplusplus
}
#endif

#endif /* __CHPDEF_H */
