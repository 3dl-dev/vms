/*
 * ARMDEF.H - VMS Access Rights Mask Definitions
 *
 * OpenVMX compatibility layer - Defines the ARM$M_ bit-mask constants
 * used with sys$check_access and sys$audit_event to specify access rights.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __ARMDEF_H
#define __ARMDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * ARM$M_ — Access rights mask bit constants
 *
 * Used in the ARM (Access Rights Mask) parameter of sys$check_access
 * and in NSA$_ACCESS_DESIRED for sys$audit_event.
 * ================================================================ */

#define ARM$M_READ          0x00000001  /* Read access */
#define ARM$M_WRITE         0x00000002  /* Write access */
#define ARM$M_EXECUTE       0x00000004  /* Execute access */
#define ARM$M_DELETE        0x00000008  /* Delete access */
#define ARM$M_CONTROL       0x00000010  /* Control (change protection) access */
#define ARM$M_EXTEND        0x00000020  /* Extend access */

/* ================================================================
 * ARM$K_ — Access rights mask size constant
 * ================================================================ */

#define ARM$K_LENGTH        4   /* Size of access rights mask (bytes) */

#ifdef __cplusplus
}
#endif

#endif /* __ARMDEF_H */
