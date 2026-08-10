/*
 * LCKDEF.H - VMS Lock Mode and Flag Definitions
 *
 * OpenVMX compatibility layer - Defines the LCK$K_ lock mode constants
 * and LCK$M_ flag bit constants used with sys$enq, sys$enqw, sys$deq,
 * sys$getlki, and sys$getlkiw (distributed lock manager services).
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual — Lock Management
 *
 * ================================================================
 * GROUNDING (clean-room Rule 8) — every LCK$M_/LCK$K_ value below is the
 * authentic OpenVMS $LCKDEF layout, pinned by TWO independent documented-tool
 * methods on the OpenVMS VAX V7.3 reference oracle (lab-2 vaxlab-7), 2026-08-10.
 * These bit values are architecture-independent ($LCKDEF is identical on VAX
 * and Alpha), so a VAX oracle is authoritative for them.
 *
 *   Method A — LIBRARIAN extract of the definition macro:
 *     $ LIBRARY/EXTRACT=$LCKDEF/OUTPUT=SYS$SCRATCH:LCKDEF.MAR SYS$LIBRARY:STARLET.MLB
 *     $ TYPE SYS$SCRATCH:LCKDEF.MAR
 *       $EQU LCK$M_VALBLK 1   $EQU LCK$M_CONVERT 2   $EQU LCK$M_NOQUEUE 4
 *       $EQU LCK$M_SYNCSTS 8  $EQU LCK$M_SYSTEM 16   $EQU LCK$M_NOQUOTA 32
 *       $EQU LCK$M_CVTSYS 64  $EQU LCK$M_RECOVER 128 $EQU LCK$M_PROTECT 256
 *       $EQU LCK$M_NODLCKWT 512   $EQU LCK$M_NODLCKBLK 1024
 *       $EQU LCK$M_EXPEDITE 2048  $EQU LCK$M_QUECVT 4096  $EQU LCK$M_BYPASS 8192
 *       $EQU LCK$M_DEQALL 1   $EQU LCK$M_CANCEL 2   $EQU LCK$M_INVVALBLK 4
 *
 *   Method B — MACRO-32 assembler symbol table (independent of LIBRARIAN):
 *     $ CREATE SYS$SCRATCH:LCKT.MAR   ["<TAB>$LCKDEF GLOBAL" / "<TAB>.END"]
 *     $ MACRO/LIST=SYS$SCRATCH:LCKT.LIS SYS$SCRATCH:LCKT.MAR
 *     $ SEARCH SYS$SCRATCH:LCKT.LIS "LCK$M_"
 *       LCK$M_VALBLK=00000001 LCK$M_CONVERT=00000002 LCK$M_NOQUEUE=00000004
 *       LCK$M_SYNCSTS=00000008 LCK$M_SYSTEM=00000010 LCK$M_NOQUOTA=00000020
 *       LCK$M_CVTSYS=00000040 LCK$M_RECOVER=00000080 LCK$M_PROTECT=00000100
 *       LCK$M_NODLCKWT=00000200 LCK$M_NODLCKBLK=00000400 LCK$M_EXPEDITE=00000800
 *       LCK$M_QUECVT=00001000 LCK$M_BYPASS=00002000
 *       LCK$M_DEQALL=00000001 LCK$M_CANCEL=00000002 LCK$M_INVVALBLK=00000004
 *
 * Both methods agree exactly. Values are spelled identically to the second
 * (authoritative) copy in <starlet.h> so a translation unit that includes both
 * headers sees a benign (identical) macro redefinition, never a conflict.
 *
 * HISTORY: this header previously carried NINE wrong bits (vms-982) — several
 * SWAPPED, not near-misses: NOQUEUE/NODLCKWT/SYNCSTS/EXPEDITE/PROTECT held
 * wrong values, LCK$M_CLUSTER (0x1000) and LCK$M_RQJOBIDFL (0x2000) were NOT
 * real $LCKDEF symbols and squatted the QUECVT/BYPASS slots, and DEQALL/
 * INVVALBLK carried invented $ENQ-field values. Corrected against the oracle.
 * ================================================================
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
 * LCK$M_ — $ENQ/$ENQW flags bit masks
 *
 * Passed as the "flags" argument to sys$enq and sys$enqw. Bit positions
 * (LCK$V_) run 0..13 in declaration order; each mask is 1 << position.
 * ================================================================ */

#define LCK$M_VALBLK    0x0001  /* bit 0  Lock has a 16-byte value block */
#define LCK$M_CONVERT   0x0002  /* bit 1  Convert the existing lock named by lkid */
#define LCK$M_NOQUEUE   0x0004  /* bit 2  Fail immediately (SS$_NOTQUEUED) if not grantable */
#define LCK$M_SYNCSTS   0x0008  /* bit 3  Synchronous completion (return SS$_SYNCH) */
#define LCK$M_SYSTEM    0x0010  /* bit 4  System-wide (system-owned) resource */
#define LCK$M_NOQUOTA   0x0020  /* bit 5  Do not charge against enqueue quota */
#define LCK$M_CVTSYS    0x0040  /* bit 6  Convert to a system lock */
#define LCK$M_RECOVER   0x0080  /* bit 7  Recovery lock */
#define LCK$M_PROTECT   0x0100  /* bit 8  Protected against forced dequeue */
#define LCK$M_NODLCKWT  0x0200  /* bit 9  Exclude from deadlock-wait detection */
#define LCK$M_NODLCKBLK 0x0400  /* bit 10 Exclude from deadlock-block detection */
#define LCK$M_EXPEDITE  0x0800  /* bit 11 Expedite grant */
#define LCK$M_QUECVT    0x1000  /* bit 12 Queued conversion */
#define LCK$M_BYPASS    0x2000  /* bit 13 Bypass fast-path */

/* ================================================================
 * LCK$M_ — $DEQ flags bit masks
 *
 * Passed as the "flags" argument to sys$deq. These form a SEPARATE bit
 * field that restarts at bit 0, so they legitimately share numeric values
 * with the low $ENQ flags above (this is authentic $LCKDEF, not an alias
 * bug — see the oracle extract in the header comment).
 * ================================================================ */

#define LCK$M_DEQALL    0x0001  /* bit 0  Dequeue all locks for the access mode */
#define LCK$M_CANCEL    0x0002  /* bit 1  Cancel a queued lock/conversion request */
#define LCK$M_INVVALBLK 0x0004  /* bit 2  Invalidate the value block on dequeue */

/* ================================================================
 * LCK$C_ — Lock value block length
 * ================================================================ */

#define LCK$C_VALBLK_LEN    16  /* Size of the lock value block (bytes) */

#ifdef __cplusplus
}
#endif

#endif /* __LCKDEF_H */
