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

/* Mandatory-access (classified) synonyms. ORACLE-PINNED 2026-08-13 on
 * OpenVMS VAX V7.3 (lab-2): $CHPDEF assembled as GLOBAL symbols, values
 * read from the object GSD via ANALYZE/OBJECT/GSD (Rule 8). On real VMS
 * CHP$M_OBSERVE aliases CHP$M_READ (1) and CHP$M_ALTER aliases
 * CHP$M_WRITE (2) -- OVMX's existing READ/WRITE bits already match the
 * oracle, so these are the authentic values, not a private choice. */
#define CHP$M_OBSERVE       0x00000001  /* Observe access (== CHP$M_READ) */
#define CHP$M_ALTER         0x00000002  /* Alter access   (== CHP$M_WRITE) */

#ifdef __cplusplus
}
#endif

#endif /* __CHPDEF_H */
