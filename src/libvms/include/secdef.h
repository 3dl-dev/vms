/*
 * SECDEF.H - VMS Section (Memory Mapping) Definitions
 *
 * OpenVMX compatibility layer - Defines the SEC$M_ bit-mask constants,
 * SEC$C_ codes, and SECID structure used with sys$crmpsc, sys$mgblsc,
 * sys$create_gpfn, sys$cretva_64, sys$deltva, and related
 * memory section / global section system services.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual — Memory Management
 */

#ifndef __SECDEF_H
#define __SECDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * SEC$M_ — Section flag bit masks
 *
 * Passed as the "flags" argument to sys$crmpsc, sys$create_gpfn,
 * and related section creation services.
 * ================================================================ */

#define SEC$M_GBL           0x00000001  /* Global (shared) section */
#define SEC$M_CRF           0x00000002  /* Copy-on-reference */
#define SEC$M_DZRO          0x00000004  /* Demand-zero pages */
#define SEC$M_WRT           0x00000008  /* Writable section */
#define SEC$M_PERM          0x00000010  /* Permanent (survives image exit) */
#define SEC$M_SYSGBL        0x00000020  /* System global section */
#define SEC$M_PFNMAP        0x00000040  /* PFN-mapped section */
#define SEC$M_EXPREG        0x00000100  /* Expand region (P1 space) */
#define SEC$M_OVERLAYD      0x00000200  /* Overlaid section */
#define SEC$M_PROTECT       0x00000400  /* Protected section */
#define SEC$M_PAGFIL        0x00001000  /* Page file backed */
#define SEC$M_EXECUTE       0x00002000  /* Execute access */
#define SEC$M_NOCOPY        0x00004000  /* No copy on fork */
#define SEC$M_64BIT         0x00008000  /* 64-bit mapped section */

/* ================================================================
 * SEC$C_ — Section type / access mode codes
 * ================================================================ */

#define SEC$C_PRIVATE           0   /* Private section */
#define SEC$C_GLOBAL            1   /* Global section */
#define SEC$C_DISK              2   /* Disk-file backed section */

/* ================================================================
 * SECID — Section identifier structure
 *
 * Used to identify a global section by number and sequence.
 * Passed to sys$create_gpfn and sys$crmpsc section services.
 * ================================================================ */

struct _secid {
    uint32_t secid$l_ident;   /* Section version / sequence number */
    uint32_t secid$l_minor;   /* Minor version */
};

typedef struct _secid SECID;

#ifdef __cplusplus
}
#endif

#endif /* __SECDEF_H */
