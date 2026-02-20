/*
 * PSLDEF.H - VMS Processor Status Longword (PSL) Definitions
 *
 * OpenVMX compatibility layer - Defines the PSL$C_ access mode
 * constants, PSL$M_ bit-mask constants, and PSL$V_ bit position
 * constants for the VMS Processor Status Longword.
 *
 * The PSL is a 32-bit register that encodes:
 *   - Current access mode (kernel, executive, supervisor, user)
 *   - Previous access mode (mode before most recent change)
 *   - Interrupt priority level (IPL)
 *   - Condition codes (N, Z, V, C flags)
 *   - Trace and debug bits
 *
 * The PSL access mode constants (PSL$C_*) are widely used in
 * system service calls to specify the privilege level at which
 * an operation should execute or which memory regions are accessible.
 *
 * Access mode numeric encoding (2 bits):
 *   0 = Kernel     — highest privilege
 *   1 = Executive  — second highest
 *   2 = Supervisor — third (used for DCL shell)
 *   3 = User       — lowest privilege (normal application code)
 *
 * Higher numeric value = less privilege.  A process can only
 * change to a more privileged mode via system services.
 *
 * Reference: OpenVMS Alpha Architecture Reference Manual
 *            OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual — Process Privilege
 */

#ifndef __PSLDEF_H
#define __PSLDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * PSL$C_ — Access mode numeric constants
 *
 * Used as arguments to system services that require an access mode,
 * such as sys$crembx, sys$cretva_64, sys$setprt_64, sys$trnlnm, etc.
 * Also used to express the mode in which a logical name is accessible
 * or to specify the protection level for memory regions.
 * ================================================================ */

#define PSL$C_KERNEL    0   /* Kernel mode — highest privilege */
#define PSL$C_EXEC      1   /* Executive mode */
#define PSL$C_SUPER     2   /* Supervisor mode (DCL shell runs here) */
#define PSL$C_USER      3   /* User mode — normal application code */

/* Maximum access mode value */
#define PSL$C_MAX_MODE  3   /* Most privileged = 0, least = 3 */

/* ================================================================
 * PSL$M_ — Bit-mask constants for the Processor Status Longword
 *
 * These masks select specific fields within the PSL register.
 * They are used in kernel-mode code and PAL call results.
 * ================================================================ */

/* Condition code bits (bits 0-3) — VAX PSL */
#define PSL$M_C         0x00000001  /* Carry flag */
#define PSL$M_V         0x00000002  /* Overflow flag */
#define PSL$M_Z         0x00000004  /* Zero flag */
#define PSL$M_N         0x00000008  /* Negative flag */

/* Decimal overflow enable and floating underflow enable (VAX) */
#define PSL$M_DV        0x00000080  /* Decimal overflow enable */
#define PSL$M_FU        0x00000040  /* Floating underflow enable */
#define PSL$M_IV        0x00000020  /* Integer overflow enable */
#define PSL$M_T         0x00000010  /* Trace pending */

/* Current access mode field (bits 22-23 on VAX, encoded in PSL) */
#define PSL$M_CURMOD    0x00C00000  /* Current mode field mask */

/* Previous access mode field (bits 20-21 on VAX) */
#define PSL$M_PRVMOD    0x00300000  /* Previous mode field mask */

/* Interrupt priority level (bits 16-19 on VAX) */
#define PSL$M_IPL       0x001F0000  /* IPL field mask */

/* ================================================================
 * PSL$V_ — Bit position constants (shift counts for field extraction)
 * ================================================================ */

#define PSL$V_C         0   /* Carry flag bit position */
#define PSL$V_V         1   /* Overflow flag bit position */
#define PSL$V_Z         2   /* Zero flag bit position */
#define PSL$V_N         3   /* Negative flag bit position */
#define PSL$V_IPL       16  /* IPL field start bit */
#define PSL$V_PRVMOD    20  /* Previous mode field start bit */
#define PSL$V_CURMOD    22  /* Current mode field start bit */

/* ================================================================
 * PSL$S_ — Field width constants
 * ================================================================ */

#define PSL$S_CURMOD    2   /* Current mode field is 2 bits wide */
#define PSL$S_PRVMOD    2   /* Previous mode field is 2 bits wide */
#define PSL$S_IPL       5   /* IPL field is 5 bits wide */

/* ================================================================
 * Access mode field extraction macros
 * ================================================================ */

/**
 * PSL$CURMOD - Extract current access mode from PSL value
 */
#define PSL$CURMOD(psl) (((psl) & PSL$M_CURMOD) >> PSL$V_CURMOD)

/**
 * PSL$PRVMOD - Extract previous access mode from PSL value
 */
#define PSL$PRVMOD(psl) (((psl) & PSL$M_PRVMOD) >> PSL$V_PRVMOD)

/**
 * PSL$IPL - Extract interrupt priority level from PSL value
 */
#define PSL$IPL(psl)    (((psl) & PSL$M_IPL) >> PSL$V_IPL)

#ifdef __cplusplus
}
#endif

#endif /* __PSLDEF_H */
