/*
 * LIBVMDEF.H - VMS LIB$VM (Virtual Memory) Zone Constants
 *
 * OpenVMX compatibility layer - Defines the LIB$K_VM_ algorithm codes
 * and LIB$M_VM_ flag bits used with lib$create_vm_zone, lib$get_vm,
 * lib$free_vm, lib$delete_vm_zone, lib$show_vm_zone, lib$find_vm_zone,
 * lib$stat_vm, and lib$verify_vm_zone.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 */

#ifndef __LIBVMDEF_H
#define __LIBVMDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LIB$K_VM_ — Zone allocation algorithm codes
 *
 * Passed as the "algorithm" argument to lib$create_vm_zone.
 * ================================================================ */

#define LIB$K_VM_FIXED          1   /* Fixed-size block allocation */
#define LIB$K_VM_VARIABLE       2   /* Variable-size block allocation */
#define LIB$K_VM_FIRST_FIT      3   /* First-fit variable allocation */
#define LIB$K_VM_BEST_FIT       4   /* Best-fit variable allocation */
#define LIB$K_VM_QUICK_FIT      5   /* Quick-fit with lookaside lists */

/* ================================================================
 * LIB$M_VM_ — Zone creation flag bits
 *
 * Passed as the "flags" argument to lib$create_vm_zone.
 * ================================================================ */

#define LIB$M_VM_BOUNDARY_TAGS  0x00000001  /* Use boundary tags for blocks */
#define LIB$M_VM_EXTEND         0x00000002  /* Allow zone to extend */
#define LIB$M_VM_GET_FILL0      0x00000004  /* Fill allocated blocks with zero */
#define LIB$M_VM_FREE_FILL_PAT  0x00000008  /* Fill freed blocks with pattern */
#define LIB$M_VM_CHECK_BOUNDS   0x00000010  /* Check boundary tags on free */

/* ================================================================
 * LIB$K_ — Float format constants (for lib$wait)
 *
 * Also defined here as this is part of the LIB$ VM / utility header
 * family used by corpus programs.
 * ================================================================ */

#define LIB$K_VAX_F     1   /* VAX F_floating (single precision) */
#define LIB$K_VAX_D     2   /* VAX D_floating (double precision) */
#define LIB$K_VAX_G     3   /* VAX G_floating (double precision, wider range) */
#define LIB$K_VAX_H     4   /* VAX H_floating (quad precision) */
#define LIB$K_IEEE_S    5   /* IEEE single precision */
#define LIB$K_IEEE_T    6   /* IEEE double precision */

#ifdef __cplusplus
}
#endif

#endif /* __LIBVMDEF_H */
