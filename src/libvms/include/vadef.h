/*
 * VADEF.H - VMS Virtual Address Space Region Definitions
 *
 * OpenVMX compatibility layer - Defines the VA$C_ region ID constants,
 * VA$_ item codes, and REGSUM structure used with sys$get_region_info
 * to query information about virtual address space regions (P0, P1, P2).
 *
 * On Alpha/IA64/x86_64 VMS, P2 space is a 64-bit address space region
 * used by privileged images and the VMS runtime.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual — Memory Management
 */

#ifndef __VADEF_H
#define __VADEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * VA$C_ — Virtual address space region ID constants
 *
 * Used as the value in the region_id (GENERIC_64) argument to
 * sys$get_region_info.
 * ================================================================ */

#define VA$C_P0         0   /* Process (user) space (P0) */
#define VA$C_P1         1   /* Control (supervisor) space (P1) */
#define VA$C_P2         2   /* 64-bit virtual address space (P2) */
#define VA$C_S0         3   /* System space (S0) */

/* ================================================================
 * VA$_ — Item codes for sys$get_region_info function parameter
 * ================================================================ */

#define VA$_REGSUM_BY_ID    1   /* Get region summary by region ID */
#define VA$_REGSUM_BY_VA    2   /* Get region summary by virtual address */
#define VA$_REGMAP_BY_ID    3   /* Get region map by region ID */
#define VA$_REGMAP_BY_VA    4   /* Get region map by virtual address */

/* ================================================================
 * REGSUM — Region Summary structure
 *
 * Returned by sys$get_region_info when using VA$_REGSUM_BY_ID or
 * VA$_REGSUM_BY_VA.  Fields accessed by corpus programs are included.
 * ================================================================ */

struct _regsum {
    uint32_t va$l_flags;                /* Region flags */
    uint32_t va$l_region_protection;    /* Default page protection */
    void    *va$pq_start_va;            /* Start virtual address of region */
    void    *va$pq_first_free_va;       /* First free virtual address */
    int64_t  va$q_region_size;          /* Size of region in bytes */
    uint32_t va$l_region_pages;         /* Number of pages in region */
    uint32_t va$l_reserved;             /* Reserved */
};

typedef struct _regsum REGSUM;

/* ================================================================
 * VA$M_ — Region flag bit masks (va$l_flags)
 * ================================================================ */

#define VA$M_NO_CALL_GATE   0x00000001  /* No call gate access */
#define VA$M_PROTECTED      0x00000002  /* Protected region */
#define VA$M_64BIT          0x00000004  /* 64-bit address region */

#ifdef __cplusplus
}
#endif

#endif /* __VADEF_H */
