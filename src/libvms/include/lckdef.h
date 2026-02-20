/*
 * LCKDEF.H - VMS Lock Mode and Flag Definitions
 *
 * OpenVMX compatibility layer - Defines the LCK$K_ lock mode constants
 * and LCK$M_ flag bit constants used with sys$enq, sys$enqw, sys$deq,
 * sys$getlki, and sys$getlkiw (distributed lock manager services).
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual — Lock Management
 */

#ifndef __LCKDEF_H
#define __LCKDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LCK$K_ — Lock grant mode codes
 *
 * Passed as the "lkmode" argument to sys$enq and sys$enqw.
 * Mode values form a hierarchy; higher values are more exclusive.
 * ================================================================ */

#define LCK$K_NLMODE    0   /* Null lock (no access granted) */
#define LCK$K_CRMODE    1   /* Concurrent read */
#define LCK$K_CWMODE    2   /* Concurrent write */
#define LCK$K_PRMODE    3   /* Protected read (shared) */
#define LCK$K_PWMODE    4   /* Protected write */
#define LCK$K_EXMODE    5   /* Exclusive */

/* ================================================================
 * LCK$M_ — ENQ flags bit masks
 *
 * Passed as the "flags" argument to sys$enq and sys$enqw.
 * ================================================================ */

#define LCK$M_VALBLK    0x00000001  /* Lock value block present */
#define LCK$M_CONVERT   0x00000002  /* Convert existing lock (no new grant) */
#define LCK$M_NODLCKWT  0x00000004  /* No deadlock wait */
#define LCK$M_NOQUEUE   0x00000008  /* Do not queue if cannot be granted */
#define LCK$M_SYSTEM    0x00000010  /* System-owned lock */
#define LCK$M_NOQUOTA   0x00000020  /* Do not charge against quota */
#define LCK$M_EXPEDITE  0x00000040  /* Expedite conversion */
#define LCK$M_PROTECT   0x00000080  /* Protected (only owner can dequeue) */
#define LCK$M_DEQALL    0x00000100  /* Dequeue all locks with this resource */
#define LCK$M_INVVALBLK 0x00000200  /* Invalidate value block */
#define LCK$M_SYNCSTS   0x00000400  /* Synchronous status */
#define LCK$M_CLUSTER   0x00001000  /* Cluster-wide lock */
#define LCK$M_RQJOBIDFL 0x00002000  /* Request from job-wide context */

/* ================================================================
 * LCK$C_ — Lock value block length
 * ================================================================ */

#define LCK$C_VALBLK_LEN    16  /* Size of the lock value block (bytes) */

#ifdef __cplusplus
}
#endif

#endif /* __LCKDEF_H */
