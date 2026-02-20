/*
 * IOSBDEF.H - VMS I/O Status Block Type Definitions
 *
 * OpenVMX compatibility layer - Defines the IOSB type used with
 * QIO-family services and system information services (GETDVIW,
 * GETJPIW, GETSYIW, etc.).
 *
 * The IOSB (I/O Status Block) is an 8-byte structure that receives
 * completion status from asynchronous system services.
 *
 * This header provides the full union form of struct _iosb that
 * real VMS iosbdef.h exposes, including the iosb$l_getxxi_status
 * field used by GETJPIW, GETSYIW, GETDVIW, and related services.
 *
 * Include order: this header may be included before or after iodef.h
 * (and before starlet.h which includes iodef.h).  The _IOSB_STRUCT_DEFINED
 * guard ensures only one definition of struct _iosb is compiled.
 * When iosbdef.h is included first, the extended struct with the
 * getxxi_status union is used; iodef.h's simpler struct is suppressed.
 * When iodef.h is included first, the simpler struct is used and
 * iosb$l_getxxi_status is not available as a named field.
 *
 * Recommended inclusion order for programs using item-list services:
 *   #include <iosbdef.h>    // extended IOSB with getxxi_status union
 *   #include <starlet.h>    // system services (pulls in iodef.h, etc.)
 *
 * IOSB struct layout (8 bytes):
 *
 *   Bytes 0-1:  iosb$w_status           completion status word
 *   Bytes 2-3:  iosb$w_bcnt             byte/transfer count word
 *   Bytes 4-7:  union {
 *                   iosb$l_dev_depend    device-dependent longword
 *                   iosb$l_getxxi_status GETJPIW/GETSYIW/GETDVIW
 *                                        per-item status code
 *               }
 *
 * Usage:
 *   static IOSB iosb;
 *   r0_status = sys$getjpiw(EFN$C_ENF, &pid, 0, itmlst, &iosb, 0, 0);
 *   errchk_sig(iosb.iosb$l_getxxi_status);
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS I/O User's Reference Manual
 */

#ifndef __IOSBDEF_H
#define __IOSBDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * struct _iosb — Extended I/O Status Block with getxxi_status union
 *
 * The _IOSB_STRUCT_DEFINED guard allows this header and iodef.h
 * to coexist: whichever is included first wins.  iosbdef.h must
 * be included first in programs that need iosb$l_getxxi_status.
 *
 * Layout (8 bytes):
 *   iosb$w_status       [0-1]  completion status
 *   iosb$w_bcnt         [2-3]  byte / transfer count
 *   union {
 *     iosb$l_dev_depend  [4-7]  device-dependent (QIO)
 *     iosb$l_getxxi_status [4-7] item-list service status
 *   }
 *
 * Note on VMS byte layout: on real VMS Alpha/I64, iosb$l_getxxi_status
 * occupies bytes 2-5 (overlaying w_bcnt).  On OVMX we place it at
 * bytes 4-7 (a union with l_dev_depend) to achieve natural alignment
 * and an exact 8-byte size.  OVMX system service implementations write
 * to iosb$l_getxxi_status at bytes 4-7, consistent with this layout.
 * ================================================================ */

#ifndef _IOSB_STRUCT_DEFINED
#define _IOSB_STRUCT_DEFINED

struct _iosb {
    uint16_t  iosb$w_status;          /* Completion status word */
    uint16_t  iosb$w_bcnt;            /* Byte / transfer count */
    union {
        uint32_t  iosb$l_dev_depend;      /* Device-dependent info (QIO) */
        uint32_t  iosb$l_getxxi_status;   /* GETJPIW/GETSYIW/GETDVIW
                                           * per-item completion status */
    };
};

typedef struct _iosb IOSB;

#endif /* _IOSB_STRUCT_DEFINED */

/* ================================================================
 * Size assertion
 * ================================================================ */

_Static_assert(sizeof(struct _iosb) == 8,
               "struct _iosb must be 8 bytes (2+2+4)");

#ifdef __cplusplus
}
#endif

#endif /* __IOSBDEF_H */
