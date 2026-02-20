/*
 * GEN64DEF.H - VMS Generic 64-Bit Type Definitions
 *
 * OpenVMX compatibility layer - Defines the GENERIC_64 union and
 * associated 64-bit portability types used throughout the VMS
 * system service interfaces.
 *
 * GENERIC_64 is a union that allows a 64-bit quantity to be
 * accessed as a quadword, two longwords, four words, or eight
 * bytes.  It is used wherever VMS APIs accept or return 64-bit
 * values (times, privileges, region identifiers, etc.).
 *
 * The __int64 and unsigned __int64 types are GCC/DECC extensions
 * that map to 64-bit integers.  On x86_64 and aarch64 Linux with
 * GCC/Clang, __int64 is a compiler built-in synonym for long long.
 *
 * Reference: OpenVMS Programming Concepts Manual
 *            OpenVMS Calling Standard
 *            HP OpenVMS Alpha/I64 Porting and User's Guide
 */

#ifndef __GEN64DEF_H
#define __GEN64DEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 64-bit integer type aliases
 *
 * VMS C (DECC) and porting guides use __int64 / unsigned __int64.
 * GCC already provides these as built-in types on 64-bit targets,
 * but we provide fallback typedefs for portability.
 * ================================================================ */

#ifndef __int64
/* GCC defines __int64 as a built-in on 64-bit targets; if somehow
 * not present, map to the equivalent standard type. */
#  define __int64  long long
#endif

#ifndef __uint64
#  define __uint64  unsigned long long
#endif

/* ================================================================
 * GENERIC_64 — Generic 64-bit union
 *
 * Allows a 64-bit value to be accessed as:
 *   gen64$q_quadword   — the whole 64-bit quadword (uint64_t)
 *   gen64$l_longword[] — two 32-bit longwords [0]=low, [1]=high
 *   gen64$w_word[]     — four 16-bit words
 *   gen64$b_byte[]     — eight 8-bit bytes
 *
 * The struct tag is _generic_64 to match the VMS convention of
 * using struct _generic_64 in casts.
 * ================================================================ */

struct _generic_64 {
    union {
        uint64_t    gen64$q_quadword;       /* Full 64-bit quadword */
        uint32_t    gen64$l_longword[2];    /* Two 32-bit longwords */
        uint16_t    gen64$w_word[4];        /* Four 16-bit words */
        uint8_t     gen64$b_byte[8];        /* Eight bytes */
    };
};

typedef struct _generic_64  GENERIC_64;

/* ================================================================
 * Convenience macros
 * ================================================================ */

/*
 * $IS_DESC64 - Test whether a descriptor is a 64-bit descriptor
 *
 * On VMS, 64-bit descriptors have DSC$K_CLASS_D64 or similar class.
 * This macro tests bit 15 of the class byte (the DSC$V_CLASS_64 bit).
 * In OVMX we provide the macro for source compatibility; the
 * distinction is typically unused in Linux-hosted code.
 */
#define $is_desc64(dsc)     (((dsc)->dsc$b_class & 0x80) != 0)

#ifdef __cplusplus
}
#endif

#endif /* __GEN64DEF_H */
