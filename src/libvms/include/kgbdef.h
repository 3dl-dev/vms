/*
 * KGBDEF.H - VMS Known Good Block / Rights Identifier Definitions
 *
 * OpenVMX compatibility layer - Defines the KGB$M_ bit-mask constants
 * used with rights identifier attributes returned by sys$find_held,
 * sys$find_holder, and sys$idtoasc.
 *
 * KGB stands for "Known Good Block" — the internal VMS structure for
 * a rights identifier entry in the rights database.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Guide to System Security
 */

#ifndef __KGBDEF_H
#define __KGBDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * KGB$M_ — Rights identifier attribute bit masks
 *
 * Returned in the "attribs" argument of sys$find_held and
 * sys$find_holder.  Used to determine the properties of a
 * rights identifier.
 * ================================================================ */

#define KGB$M_DYNAMIC       0x00000001  /* Dynamic identifier (can be granted) */
#define KGB$M_NOACCESS      0x00000002  /* No access — subsystem use */
#define KGB$M_RESOURCE      0x00000004  /* Resource identifier */
#define KGB$M_HOLDER_HIDDEN 0x00000008  /* Holders are hidden */

/* ================================================================
 * KGB$C_ — Identifier type codes
 * ================================================================ */

#define KGB$C_UIC           1   /* UIC-based identifier */
#define KGB$C_GENERAL       2   /* General identifier */
#define KGB$C_SUBSYSTEM     3   /* Subsystem identifier */
#define KGB$C_OBSOLETE      4   /* Obsolete identifier */

#ifdef __cplusplus
}
#endif

#endif /* __KGBDEF_H */
