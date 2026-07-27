/*
 * CAPDEF.H - VMS CPU Capabilities Definitions
 *
 * OpenVMX compatibility layer - Defines the CAP$ symbols used with
 * sys$cpu_capabilities, sys$process_capabilities, sys$get_user_capability,
 * sys$free_user_capability, sys$process_affinity, and
 * sys$set_implicit_affinity.
 *
 * OpenVMS defines 16 user-assignable CPU "capability" bits (CAP$M_USER1
 * through CAP$M_USER16) in a quadword mask (GENERIC_64), plus symbolic
 * constants for the $PROCESS_AFFINITY "state" argument and the
 * $SET_IMPLICIT_AFFINITY "state" argument.
 *
 * PROVENANCE: the *existence* and *purpose* of CAP$M_USER1..USER16,
 * CAP$K_ALL_CPU_ADD/REMOVE, and CAP$M_IMPLICIT_AFFINITY_SET/CLEAR are
 * documented in the public OpenVMS System Services Reference Manual
 * ($CPU_CAPABILITIES, $PROCESS_AFFINITY, $SET_IMPLICIT_AFFINITY). The
 * exact bit positions / numeric encodings below could NOT be confirmed
 * against a fetchable public source in this session — they are OVMX's
 * own sequential assignment (clean-room, CLAUDE.md rule 8) and are
 * flagged in vms-531 findings for operator sign-off before being relied
 * on for wire-level VMS interop.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            ($CPU_CAPABILITIES, $PROCESS_CAPABILITIES,
 *             $GET_USER_CAPABILITY, $FREE_USER_CAPABILITY,
 *             $PROCESS_AFFINITY, $SET_IMPLICIT_AFFINITY)
 */

#ifndef __CAPDEF_H
#define __CAPDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CAP$M_USERn — user-definable CPU capability bits (quadword mask)
 * OVMX placeholder bit assignment — see PROVENANCE note above.
 * ================================================================ */

#define CAP$M_USER1           0x0000000000000001ULL
#define CAP$M_USER2           0x0000000000000002ULL
#define CAP$M_USER3           0x0000000000000004ULL
#define CAP$M_USER4           0x0000000000000008ULL
#define CAP$M_USER5           0x0000000000000010ULL
#define CAP$M_USER6           0x0000000000000020ULL
#define CAP$M_USER7           0x0000000000000040ULL
#define CAP$M_USER8           0x0000000000000080ULL
#define CAP$M_USER9           0x0000000000000100ULL
#define CAP$M_USER10          0x0000000000000200ULL
#define CAP$M_USER11          0x0000000000000400ULL
#define CAP$M_USER12          0x0000000000000800ULL
#define CAP$M_USER13          0x0000000000001000ULL
#define CAP$M_USER14          0x0000000000002000ULL
#define CAP$M_USER15          0x0000000000004000ULL
#define CAP$M_USER16          0x0000000000008000ULL

/* ================================================================
 * CAP$K_ — $PROCESS_AFFINITY "state" argument values
 * ================================================================ */

#define CAP$K_ALL_CPU_ADD      1   /* Add CPUs in mask to affinity set */
#define CAP$K_ALL_CPU_REMOVE   2   /* Remove CPUs in mask from affinity set */

/* ================================================================
 * CAP$M_ — $SET_IMPLICIT_AFFINITY "state" argument values
 * ================================================================ */

#define CAP$M_IMPLICIT_AFFINITY_SET     0x00000001  /* Set implicit affinity to CPU */
#define CAP$M_IMPLICIT_AFFINITY_CLEAR   0x00000002  /* Clear implicit affinity */

#ifdef __cplusplus
}
#endif

#endif /* __CAPDEF_H */
