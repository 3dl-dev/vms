/*
 * C_ASM.H - VMS Inline Assembly Intrinsics
 *
 * OpenVMX compatibility layer - Provides compatibility stubs for the
 * OpenVMS compiler inline assembly support header.
 *
 * On real OpenVMS Alpha/IA64, this header provides the asm() macro and
 * related Alpha-specific inline assembly intrinsics used with the
 * HP/DEC C compiler.  On OVMX (Linux/GCC/Clang), standard GCC inline
 * assembly syntax is used instead; this stub header satisfies the
 * #include directive for corpus programs that include it.
 *
 * Note: Programs using VMS-specific Alpha PAL calls and CHMU instructions
 * cannot run on OVMX without modification — this header only allows them
 * to compile.
 *
 * Reference: HP C User's Guide for OpenVMS Systems
 *            OpenVMS Alpha Architecture Reference Manual
 */

#ifndef __C_ASM_H
#define __C_ASM_H

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * On GCC/Clang (OVMX host), inline asm uses the standard GCC syntax.
 * The VMS asm() macro is a no-op stub here for compilation purposes.
 * ================================================================ */

#ifndef __DECC
/* On non-DEC compilers, provide a harmless asm alias if not already defined */
#ifndef asm
/* Standard GCC/Clang asm is used directly; no redefinition needed */
#endif
#endif /* __DECC */

#ifdef __cplusplus
}
#endif

#endif /* __C_ASM_H */
