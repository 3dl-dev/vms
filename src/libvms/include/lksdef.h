/*
 * LKSDEF.H - VMS Lock Status Block (LKSB) Layout
 *
 * OpenVMS compatibility layer - Defines the caller-visible Lock Status
 * Block passed to sys$enq/sys$enqw/sys$deq (the "lksb" parameter).
 *
 * Reference: OpenVMS Programming Concepts Manual — Lock Management
 *            (the $ENQ/$ENQW description documents the LKSB as: a status
 *            word, a reserved word, a longword lock ID, and — when
 *            LCK$M_VALBLK is set — a 16-byte value block). VSI/HPE do not
 *            publish a byte-offset table for this structure; the layout
 *            below reproduces src/libvms/syssvc/sys_lock.c's existing,
 *            already-implemented field order (status, reserved, lkid,
 *            valblk[16]) as a public header so external callers (tests,
 *            future DCL/RTL code) have one shared definition instead of
 *            each call site guessing the private struct sys_lock.c used
 *            internally. Zero behavior change: this is the same layout
 *            that sys_lock.c already read/wrote before this header existed.
 */

#ifndef __LKSDEF_H
#define __LKSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lksb {
    uint16_t lksb$w_status;     /* Completion status (SS$_xxx) */
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;       /* Lock ID, assigned by the lock manager */
    char     lksb$b_valblk[16]; /* Lock value block (valid iff LCK$M_VALBLK) */
};

#ifdef __cplusplus
}
#endif

#endif /* __LKSDEF_H */
