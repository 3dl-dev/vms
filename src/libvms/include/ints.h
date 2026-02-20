/*
 * INTS.H - VMS Integer Type Definitions
 *
 * OpenVMX compatibility layer - Defines the VMS-style fixed-width
 * integer type names used in Alpha/IA64 kernel-mode code.
 *
 * On real OpenVMS, these types are provided by the DEC C compiler's
 * built-in type system.  On OVMX (GCC/Clang), they are mapped to
 * standard C99 stdint types.
 *
 * Reference: HP C User's Guide for OpenVMS Systems
 */

#ifndef __INTS_H
#define __INTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * VMS fixed-width integer types
 *
 * These are the types used in VMS system software and kernel code,
 * particularly in Alpha-specific code (e.g., sys_cmkrnl.c).
 * ================================================================ */

typedef int8_t    int8;
typedef int16_t   int16;
typedef int32_t   int32;
typedef int64_t   int64;

typedef uint8_t   uint8;
typedef uint16_t  uint16;
typedef uint32_t  uint32;
typedef uint64_t  uint64;

#ifdef __cplusplus
}
#endif

#endif /* __INTS_H */
