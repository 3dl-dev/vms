/*
 * VA_RANGEDEF.H - VMS Virtual Address Range Definition
 *
 * OpenVMX compatibility layer - Defines the VA_RANGE structure used
 * to specify or receive a range of virtual addresses.
 *
 * VA_RANGE is passed to and returned from virtual memory system
 * services that operate on address ranges:
 *   sys$cretva  — create virtual address space
 *   sys$deltva  — delete virtual address space
 *   sys$expreg  — expand region (allocate pages at end of P0/P1 space)
 *   sys$crmpsc  — create and map section
 *   sys$lckpag  — lock pages in working set
 *   sys$ulkpag  — unlock pages from working set
 *   sys$lkwset  — lock pages in working set
 *   sys$ulwset  — unlock pages from working set
 *   sys$purgws  — purge working set
 *   sys$create_bufobj — create buffered I/O object (Fast I/O)
 *
 * On input (inadr), the caller supplies the start and end virtual
 * addresses of the range to operate on.  On output (retadr / outadr),
 * the system returns the actual start and end addresses affected.
 *
 * The end address is inclusive: the range covers [ps_start_va, ps_end_va].
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$CRETVA,
 *            SYS$DELTVA, SYS$EXPREG, SYS$CRMPSC)
 *            OpenVMS Programming Concepts Manual, Chapter 12
 */

#ifndef __VA_RANGEDEF_H
#define __VA_RANGEDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * VA_RANGE structure
 *
 * A pair of virtual address pointers describing a contiguous range
 * of virtual address space.  Both pointers are byte addresses.
 *
 * va_range$ps_start_va — first (lowest) byte of the range
 * va_range$ps_end_va   — last  (highest) byte of the range (inclusive)
 * ================================================================ */

typedef struct {
    void *va_range$ps_start_va;     /* Start virtual address (inclusive) */
    void *va_range$ps_end_va;       /* End virtual address (inclusive) */
} VA_RANGE;

/* ================================================================
 * Convenience macro for declaring an initialized VA_RANGE
 * ================================================================ */

#define VA_RANGE_INIT(start, end) \
    { (void *)(start), (void *)(end) }

#ifdef __cplusplus
}
#endif

#endif /* __VA_RANGEDEF_H */
