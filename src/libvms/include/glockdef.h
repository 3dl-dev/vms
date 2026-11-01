/*
 * GLOCKDEF.H - VMS Galaxy Lock Table Definitions
 *
 * OpenVMX compatibility layer - Defines the GLCKTBL$C_ constants used
 * with sys$create_galaxy_lock_table for creating galaxy lock tables
 * on Alpha Galaxy (partitioned) systems.
 *
 * Reference: OpenVMS Alpha Partitioning and Galaxy Guide
 *            OpenVMS System Services Reference Manual
 */

#ifndef __GLOCKDEF_H
#define __GLOCKDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * GLCKTBL$C_ — Galaxy lock table type codes
 *
 * Passed as the "flags" argument to sys$create_galaxy_lock_table.
 * ================================================================ */

#define GLCKTBL$C_SYSTEM    1   /* System-wide galaxy lock table */
#define GLCKTBL$C_GROUP     2   /* Group-specific galaxy lock table */

/* ================================================================
 * GLCKTBL$M_ — Galaxy lock table flag bits
 * ================================================================ */

#define GLCKTBL$M_PERMANENT 0x00000001  /* Persistent across image activation */

#ifdef __cplusplus
}
#endif

#endif /* __GLOCKDEF_H */
