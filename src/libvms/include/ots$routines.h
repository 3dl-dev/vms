/*
 * OTS$ROUTINES.H - VMS Object Time System (OTS$) Routine Prototypes
 *
 * OpenVMX compatibility layer - Declares the OTS$ run-time library
 * routines.  OTS$ (Object Time System) provides compiler support
 * routines for data type conversion, string operations, complex
 * arithmetic, and power functions.
 *
 * These routines are typically called by compiler-generated code
 * rather than directly by application programs, but they are also
 * available for direct use.
 *
 * Calling conventions: All routines follow the VMS calling standard.
 * String parameters use VMS descriptors.  Return values are either
 * 32-bit condition codes or the computed result (for functions that
 * return a value rather than a status).
 *
 * Note: The complex arithmetic routines (ots$divcx_r3, ots$mulcx_r3,
 * ots$powcxcx_r3, ots$powcxj_r3) return their result in registers
 * rather than as a status code.  Their exact calling convention is
 * architecture-specific and they are normally invoked only by
 * compiler-generated code.  Direct callers should use the __unknown_params
 * convention (as seen in corpus examples) or rely on compiler builtins.
 *
 * Reference: OpenVMS RTL Object Time System (OTS$) Manual
 */

#ifndef __OTS_ROUTINES_H
#define __OTS_ROUTINES_H

#include <stdint.h>
#include <complex.h>
#include "descrip.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * __unknown_params - on the native VMS C compiler this is a builtin macro
 * meaning "the parameter list is unspecified" (an empty prototype, K&R
 * style).  The Eight-Cubed corpus declares the register-return complex
 * routines with it, e.g.
 *     extern double complex ots$divct_r3 (__unknown_params);
 * so provide the same expansion here (an empty parameter list) when the
 * host compiler has not already defined it.
 */
#ifndef __unknown_params
#define __unknown_params
#endif

/* ================================================================
 * Text-to-integer conversion routines
 *
 * These convert ASCII text in a VMS string descriptor to a binary
 * integer value.  The "t" in the name denotes the source type
 * (text), and the trailing letter denotes the base or format:
 *   u = unsigned decimal
 *   i = signed decimal (with optional leading sign)
 *   l = logical (FORTRAN: .TRUE./.FALSE.)
 *   o = octal
 *   z = hexadecimal (leading "Z" or "0X" prefix notation)
 *   b = binary (base 2)
 * ================================================================ */

/**
 * ots$cvt_tu_l - Convert unsigned decimal text to longword
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive unsigned longword result
 * @param size      Size in bytes of the destination (4 for longword)
 * @param flags     Conversion flags (0 = default)
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM on invalid input
 */
uint32_t ots$cvt_tu_l(
    const struct dsc$descriptor_s *src,
    uint32_t *dest,
    const uint32_t size,
    const uint32_t flags
);

/**
 * ots$cvt_ti_l - Convert signed decimal text to longword
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive signed longword result
 * @param size      Size in bytes of the destination (4 for longword)
 * @param flags     Conversion flags (0 = default)
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM on invalid input
 */
uint32_t ots$cvt_ti_l(
    const struct dsc$descriptor_s *src,
    uint32_t *dest,
    const uint32_t size,
    const uint32_t flags
);

/**
 * ots$cvt_tl_l - Convert FORTRAN logical text to longword
 *
 * Accepts .TRUE., .FALSE., T, F, 1, 0 (case-insensitive).
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive logical result (non-zero = true)
 * @param size      Size in bytes of the destination
 * @param flags     Conversion flags (0 = default)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_tl_l(
    const struct dsc$descriptor_s *src,
    uint32_t *dest,
    const uint32_t size,
    const uint32_t flags
);

/**
 * ots$cvt_to_l - Convert octal text to longword
 *
 * @param src       Pointer to descriptor of source octal text string
 * @param dest      Pointer to receive unsigned longword result
 * @param size      Size in bytes of the destination
 * @param flags     Conversion flags (0 = default)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_to_l(
    const struct dsc$descriptor_s *src,
    uint32_t *dest,
    const uint32_t size,
    const uint32_t flags
);

/**
 * ots$cvt_tz_l - Convert hexadecimal text to longword
 *
 * @param src       Pointer to descriptor of source hex text string
 * @param dest      Pointer to receive unsigned longword result
 * @param size      Size in bytes of the destination
 * @param flags     Conversion flags (0 = default)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_tz_l(
    const struct dsc$descriptor_s *src,
    uint32_t *dest,
    const uint32_t size,
    const uint32_t flags
);

/**
 * ots$cvt_tb_l - Convert binary text to longword
 *
 * @param src       Pointer to descriptor of source binary text string
 * @param dest      Pointer to receive unsigned longword result
 * @param size      Size in bytes of the destination
 * @param flags     Conversion flags (0 = default)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_tb_l(
    const struct dsc$descriptor_s *src,
    uint32_t *dest,
    const uint32_t size,
    const uint32_t flags
);

/* ================================================================
 * Text-to-floating-point conversion routines
 *
 * These convert ASCII text to floating-point values.  The trailing
 * letter(s) denote the floating-point format:
 *   f = F_floating (32-bit VAX F format)
 *   d = D_floating (64-bit VAX D format)
 *   g = G_floating (64-bit VAX G format)
 *   h = H_floating (128-bit VAX H format)
 *   t = IEEE T (64-bit double)
 *   s = IEEE S (32-bit single)
 * ================================================================ */

/**
 * ots$cvt_t_f - Convert text to F_floating
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive F_floating result (float)
 * @param ...       Optional flags and precision arguments
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_t_f(
    const struct dsc$descriptor_s *src,
    float *dest,
    ...
);

/**
 * ots$cvt_t_d - Convert text to D_floating
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive D_floating result (double)
 * @param ...       Optional flags and precision arguments
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_t_d(
    const struct dsc$descriptor_s *src,
    double *dest,
    ...
);

/**
 * ots$cvt_t_g - Convert text to G_floating
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive G_floating result (double)
 * @param ...       Optional flags and precision arguments
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_t_g(
    const struct dsc$descriptor_s *src,
    double *dest,
    ...
);

/**
 * ots$cvt_t_h - Convert text to H_floating (128-bit)
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive H_floating result (long double)
 * @param ...       Optional flags and precision arguments
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_t_h(
    const struct dsc$descriptor_s *src,
    long double *dest,
    ...
);

/**
 * ots$cvt_t_t - Convert text to IEEE T_floating (double)
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive IEEE double result
 * @param ...       Optional flags and precision arguments
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_t_t(
    const struct dsc$descriptor_s *src,
    double *dest,
    ...
);

/**
 * ots$cvt_t_s - Convert text to IEEE S_floating (float)
 *
 * @param src       Pointer to descriptor of source text string
 * @param dest      Pointer to receive IEEE float result
 * @param ...       Optional flags and precision arguments
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_t_s(
    const struct dsc$descriptor_s *src,
    float *dest,
    ...
);

/* ================================================================
 * Integer-to-text conversion routines
 *
 * These convert binary integer values to ASCII text in a VMS
 * string descriptor.  The trailing letters denote the output base:
 *   l  = signed decimal
 *   u  = unsigned decimal (tu = to unsigned)
 *   o  = octal
 *   z  = hexadecimal
 *   i  = signed decimal with sign char
 *   b  = unsigned decimal (binary integer to text)
 * ================================================================ */

/**
 * ots$cvt_l_tl - Convert signed longword to decimal text
 *
 * @param value     Pointer to signed longword value to convert
 * @param dest      Pointer to descriptor to receive text
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_l_tl(
    const int32_t *value,
    struct dsc$descriptor_s *dest
);

/**
 * ots$cvt_l_tu - Convert unsigned longword to unsigned decimal text
 *
 * @param value     Pointer to unsigned longword value to convert
 * @param dest      Pointer to descriptor to receive text
 * @param min_digits Minimum digits to output (0 = no minimum)
 * @param size      Source size in bytes
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_l_tu(
    const uint32_t *value,
    struct dsc$descriptor_s *dest,
    const uint32_t min_digits,
    const uint32_t size
);

/**
 * ots$cvt_l_ti - Convert integer to signed decimal text (with sign)
 *
 * @param value     Pointer to unsigned longword (value interpreted as signed)
 * @param dest      Pointer to descriptor to receive text
 * @param min_digits Minimum digits to output
 * @param size      Source size in bytes
 * @param flags     Conversion flags (1 = always show sign)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_l_ti(
    const uint32_t *value,
    struct dsc$descriptor_s *dest,
    const uint32_t min_digits,
    const uint32_t size,
    const uint32_t flags
);

/**
 * ots$cvt_l_to - Convert longword to octal text
 *
 * @param value     Pointer to unsigned longword value to convert
 * @param dest      Pointer to descriptor to receive text
 * @param min_digits Minimum digits to output
 * @param size      Source size in bytes
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_l_to(
    const int32_t *value,
    struct dsc$descriptor_s *dest,
    const int32_t *min_digits,
    const uint32_t size
);

/**
 * ots$cvt_l_tz - Convert longword to hexadecimal text
 *
 * @param value     Pointer to unsigned longword value to convert
 * @param dest      Pointer to descriptor to receive text
 * @param min_digits Minimum digits to output
 * @param size      Source size in bytes
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_l_tz(
    const int32_t *value,
    struct dsc$descriptor_s *dest,
    const int32_t *min_digits,
    const uint32_t size
);

/**
 * ots$cvt_l_tb - Convert longword to unsigned decimal text (byte-width result)
 *
 * @param value     Pointer to unsigned longword value to convert
 * @param dest      Pointer to descriptor to receive text
 * @param min_digits Minimum digits to output
 * @param size      Source size in bytes
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cvt_l_tb(
    const uint32_t *value,
    struct dsc$descriptor_s *dest,
    const uint32_t min_digits,
    const uint32_t size
);

/* ================================================================
 * Floating-point-to-text conversion routines
 *
 * ots$cnvout_x converts a floating-point value to its text
 * representation with a specified precision.
 * ================================================================ */

/**
 * ots$cnvout_f - Convert F_floating to text
 *
 * @param value     Pointer to F_floating value to convert
 * @param dest      Pointer to descriptor to receive text
 * @param precision Number of significant digits
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cnvout_f(
    const float *value,
    struct dsc$descriptor_s *dest,
    const uint32_t precision
);

/**
 * ots$cnvout_d - Convert D_floating to text
 *
 * @param value     Pointer to D_floating value (double) to convert
 * @param dest      Pointer to descriptor to receive text
 * @param precision Number of significant digits
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cnvout_d(
    const double *value,
    struct dsc$descriptor_s *dest,
    const uint32_t precision
);

/**
 * ots$cnvout_g - Convert G_floating to text
 *
 * @param value     Pointer to G_floating value (double) to convert
 * @param dest      Pointer to descriptor to receive text
 * @param precision Number of significant digits
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cnvout_g(
    const double *value,
    struct dsc$descriptor_s *dest,
    const uint32_t precision
);

/**
 * ots$cnvout_t - Convert IEEE T_floating (double) to text
 *
 * @param value     Pointer to IEEE double value to convert
 * @param dest      Pointer to descriptor to receive text
 * @param precision Number of significant digits
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$cnvout_t(
    const double *value,
    struct dsc$descriptor_s *dest,
    const uint32_t precision
);

/* ================================================================
 * String copy routines
 *
 * These copy string data between VMS descriptors or between a
 * raw buffer and a descriptor.
 * ================================================================ */

/**
 * ots$scopy_dxdx - Copy string from any descriptor to any descriptor
 *
 * Copies src to dest.  Handles CLASS_S (fixed), CLASS_D (dynamic),
 * and CLASS_VS (varying string) descriptors.  For CLASS_S, truncates
 * or space-fills as necessary.  For CLASS_D, resizes the string.
 *
 * @param src   Pointer to source descriptor
 * @param dest  Pointer to destination descriptor
 *
 * @return  Number of bytes copied (not a status code), or negative on error
 */
int32_t ots$scopy_dxdx(
    const struct dsc$descriptor_s *src,
    struct dsc$descriptor_s *dest
);

/**
 * ots$scopy_r_dx - Copy string by reference (length + address) to descriptor
 *
 * Copies the string at (srcadr, srclen) to dest.
 *
 * @param srclen  Length of source string in bytes
 * @param srcadr  Pointer to source string data
 * @param dest    Pointer to destination descriptor
 *
 * @return  0 on success, non-zero on error
 */
int32_t ots$scopy_r_dx(
    const uint32_t srclen,
    const char *srcadr,
    struct dsc$descriptor_s *dest
);

/* ================================================================
 * Dynamic string management routines
 *
 * These allocate and free dynamic (CLASS_D) string descriptors.
 * The descriptor is passed as an unsigned __int64 pointer to match
 * the VMS calling convention for 64-bit descriptor handles.
 * ================================================================ */

/**
 * ots$sget1_dd - Allocate a dynamic string descriptor of given length
 *
 * @param length  Length in bytes to allocate
 * @param desc    Pointer to dynamic descriptor (as 64-bit handle)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$sget1_dd(
    const uint32_t length,
    uint64_t *desc
);

/**
 * ots$sfree1_dd - Free a single dynamic string descriptor
 *
 * @param desc  Pointer to dynamic descriptor (as 64-bit handle)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$sfree1_dd(
    uint64_t *desc
);

/**
 * ots$sfreen_dd - Free an array of n dynamic string descriptors
 *
 * @param count  Number of descriptors in the array
 * @param desc   Pointer to first dynamic descriptor (as 64-bit handle)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t ots$sfreen_dd(
    const uint32_t count,
    uint64_t *desc
);

/* ================================================================
 * Memory move routines
 *
 * Analogous to memmove/memset, used by compiler-generated code.
 * ================================================================ */

/**
 * ots$move3 - Move memory block (3-argument form)
 *
 * Copies count bytes from src to dest.  Handles overlapping regions.
 *
 * @param count  Number of bytes to copy
 * @param src    Pointer to source
 * @param dest   Pointer to destination
 */
void ots$move3(
    const uint32_t count,
    const void *src,
    void *dest
);

/**
 * ots$move5 - Move memory block with fill (5-argument form)
 *
 * Copies srclen bytes from src to dest.  If destlen > srclen, the
 * remaining bytes of dest are filled with fill.
 *
 * @param srclen   Number of bytes in source
 * @param src      Pointer to source
 * @param fill     Fill character for remaining destination bytes
 * @param destlen  Total length of destination
 * @param dest     Pointer to destination
 */
void ots$move5(
    const uint32_t srclen,
    const void *src,
    const uint8_t fill,
    const uint32_t destlen,
    void *dest
);

/* ================================================================
 * Integer power routine
 * ================================================================ */

/**
 * ots$powjj - Raise integer to integer power
 *
 * Computes base ** exponent for integer operands.
 *
 * @param base      Integer base
 * @param exponent  Integer exponent
 *
 * @return  base raised to the power of exponent
 */
int32_t ots$powjj(
    const int32_t base,
    const int32_t exponent
);

/* ================================================================
 * Complex arithmetic routines
 *
 * These are normally invoked only by compiler-generated code for
 * FORTRAN COMPLEX arithmetic.  Their return type is double complex
 * (the result is returned in floating-point registers).
 *
 * The naming convention is:
 *   ots$divcx_r3   - divide complex (result in R3 register pair)
 *   ots$mulcx_r3   - multiply complex
 *   ots$powcxcx_r3 - raise complex to complex power
 *   ots$powcxj_r3  - raise complex to integer power
 *
 * where x is the floating-point format: g (G_float) or t (IEEE T).
 *
 * Direct callers should declare these with __unknown_params as shown
 * in the corpus examples, e.g.:
 *   extern double complex ots$divct_r3 (__unknown_params);
 * ================================================================ */

/* G_floating complex variants (VAX G_float format) */
extern double complex ots$divcg_r3(__unknown_params);   /* divide complex G */
extern double complex ots$mulcg_r3(__unknown_params);   /* multiply complex G */
extern double complex ots$powcgcg_r3(__unknown_params); /* complex G ** complex G */
extern double complex ots$powcgj_r3(__unknown_params);  /* complex G ** integer */

/* IEEE T_floating complex variants (IEEE double) */
extern double complex ots$divct_r3(__unknown_params);   /* divide complex T */
extern double complex ots$mulct_r3(__unknown_params);   /* multiply complex T */
extern double complex ots$powctct_r3(__unknown_params); /* complex T ** complex T */
extern double complex ots$powctj_r3(__unknown_params);  /* complex T ** integer */

#ifdef __cplusplus
}
#endif

#endif /* __OTS_ROUTINES_H */
