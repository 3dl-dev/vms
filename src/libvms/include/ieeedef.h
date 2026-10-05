/*
 * IEEEDEF.H - VMS IEEE Floating-Point Control Definitions
 *
 * OpenVMX compatibility layer - Defines the IEEE structure and IEEE$_
 * constants used with sys$ieee_set_fp_control, sys$ieee_set_precision_mode,
 * and sys$ieee_set_rounding_mode on Alpha and IA64/x86_64 systems.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Alpha Floating-Point Programming Guide
 */

#ifndef __IEEEDEF_H
#define __IEEEDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * IEEE — Floating-point control register structure
 *
 * Used with sys$ieee_set_fp_control.  The ieee$q_flags field is a
 * 64-bit mask where each bit controls a specific IEEE exception trap.
 * ================================================================ */

struct _ieee {
    uint64_t ieee$q_flags;  /* IEEE floating-point control flags */
};

typedef struct _ieee IEEE;

/* ================================================================
 * IEEE$M_ — Floating-point exception trap enable/status bit masks
 *
 * Used as bits in ieee$q_flags to enable or disable IEEE traps.
 * ================================================================ */

#define IEEE$M_TRAP_ENABLE_INV  0x0000000000000002ULL  /* Invalid operation */
#define IEEE$M_TRAP_ENABLE_DZE  0x0000000000000004ULL  /* Divide by zero */
#define IEEE$M_TRAP_ENABLE_OVF  0x0000000000000008ULL  /* Overflow */
#define IEEE$M_TRAP_ENABLE_UNF  0x0000000000000010ULL  /* Underflow */
#define IEEE$M_TRAP_ENABLE_INE  0x0000000000000020ULL  /* Inexact */
#define IEEE$M_TRAP_ENABLE_DNOE 0x0000000000000040ULL  /* Denormal operand (V8.4 $IEEEDEF) */
#define IEEE$M_TRAP_ENABLE_DNO  IEEE$M_TRAP_ENABLE_DNOE

/* ================================================================
 * IEEE$C_PM_ — Precision mode constants
 *
 * Used with sys$ieee_set_precision_mode.
 * ================================================================ */

#define IEEE$C_PM_NO_CHANGE     (-1)   /* Do not change precision mode */
#define IEEE$C_PM_SINGLE        0   /* Single precision (24-bit mantissa) */
#define IEEE$C_PM_DOUBLE        2   /* Double precision (53-bit mantissa) */
#define IEEE$C_PM_EXTENDED      3   /* Extended precision (64-bit mantissa) */

/* ================================================================
 * IEEE$C_RM_ — Rounding mode constants
 *
 * Used with sys$ieee_set_rounding_mode.
 * ================================================================ */

#define IEEE$C_RM_NO_CHANGE     (-1)   /* Do not change rounding mode */
#define IEEE$C_RM_NEAREST       0   /* Round to nearest (default) */
#define IEEE$C_RM_DOWN          1   /* Round toward negative infinity */
#define IEEE$C_RM_UP            2   /* Round toward positive infinity */
#define IEEE$C_RM_TRUNCATE      3   /* Round toward zero (truncate) */

#ifdef __cplusplus
}
#endif

#endif /* __IEEEDEF_H */
