/*
 * lib_strconv.c - LIB$ string translation / conversion / locale routines
 * (vms-801 R2.2 batch 2).
 *
 * Implements:
 *   lib$movtc                       - move translated characters (VAX MOVTC)
 *   lib$tra_asc_ebc / lib$tra_ebc_asc - ASCII<->EBCDIC translation
 *   lib$scopy_r_dx                  - copy string by reference to descriptor
 *   lib$cvt_dx_dx                   - convert (numeric/text) descriptor to text
 *   lib$currency / lib$digit_sep / lib$radix_point - locale symbols
 *
 * Grounded in the public OpenVMS RTL Library (LIB$) Manual descriptions
 * of these routines.  Clean-room (Rule 8): no VSI source consulted.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "ssdef.h"
#include "stsdef.h"
#include "descrip.h"
#include "lib$routines.h"   /* pulls in libdef.h: LIB$_*, LIB$AB_* externs */

/*
 * copy_to_desc - place `srclen` bytes of `src` into descriptor `dst`.
 *
 * For a dynamic (CLASS_D) descriptor the buffer is (re)allocated to
 * exactly srclen bytes and dsc$w_length is set accordingly.  For a
 * fixed (CLASS_S) descriptor the string is copied up to the field
 * width and the remainder space-filled (VMS fixed-string semantics);
 * LIB$_STRTRU is returned if the source did not fit.
 */
static uint32_t copy_to_desc(struct dsc$descriptor_s *dst,
                             const char *src, uint16_t srclen) {
    if (!dst) return SS$_BADPARAM;

    if (dst->dsc$b_class == DSC$K_CLASS_D) {
        struct dsc$descriptor_d *dd = (struct dsc$descriptor_d *)dst;
        if (vms_desc_alloc(dd, srclen) != 0) return LIB$_INSVIRMEM;
        if (srclen && src) memcpy(dd->dsc$a_pointer, src, srclen);
        return SS$_NORMAL;
    }

    if (!dst->dsc$a_pointer) return SS$_BADPARAM;
    uint16_t field = dst->dsc$w_length;
    uint16_t n = (srclen < field) ? srclen : field;
    if (n && src) memcpy(dst->dsc$a_pointer, src, n);
    if (n < field) memset(dst->dsc$a_pointer + n, ' ', field - n);
    return (srclen > field) ? LIB$_STRTRU : SS$_NORMAL;
}

/*
 * lib$scopy_r_dx - Copy a source string (length by reference, address by
 * reference) into a destination descriptor of any class.
 */
uint32_t lib$scopy_r_dx(const uint16_t *srclen, const char *srcadr,
                        struct dsc$descriptor_s *dst) {
    if (!srclen || !dst) return SS$_BADPARAM;
    return copy_to_desc(dst, srcadr, *srclen);
}

/*
 * lib$cvt_dx_dx - Convert a source descriptor to a destination
 * descriptor.
 *
 * Supported source data types: the atomic integer types (B/W/L/Q,
 * signed and unsigned) are formatted as decimal text; a text (T)
 * source is copied verbatim.  The destination is produced as text.
 * The resulting length is returned through retlen.  Unsupported source
 * data types return LIB$_INVARG (honest failure, not a silent no-op).
 */
uint32_t lib$cvt_dx_dx(const void *src_desc, void *dst_desc,
                       uint16_t *retlen) {
    const struct dsc$descriptor_s *src =
        (const struct dsc$descriptor_s *)src_desc;
    struct dsc$descriptor_s *dst = (struct dsc$descriptor_s *)dst_desc;
    if (!src || !dst || !src->dsc$a_pointer) return SS$_BADPARAM;

    char buf[32];
    const char *out = buf;
    uint16_t outlen = 0;

    switch (src->dsc$b_dtype) {
        case DSC$K_DTYPE_BU:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%u",
                        (unsigned)*(const uint8_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_WU:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%u",
                        (unsigned)*(const uint16_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_LU:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%u",
                        *(const uint32_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_QU:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%llu",
                        (unsigned long long)*(const uint64_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_B:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%d",
                        (int)*(const int8_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_W:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%d",
                        (int)*(const int16_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_L:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%d",
                        *(const int32_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_Q:
            outlen = (uint16_t)snprintf(buf, sizeof(buf), "%lld",
                        (long long)*(const int64_t *)src->dsc$a_pointer);
            break;
        case DSC$K_DTYPE_T:
            out = src->dsc$a_pointer;
            outlen = src->dsc$w_length;
            break;
        default:
            return LIB$_INVARG;
    }

    uint32_t st = copy_to_desc(dst, out, outlen);
    if (retlen) {
        uint16_t field = (dst->dsc$b_class == DSC$K_CLASS_D)
                             ? outlen : dst->dsc$w_length;
        *retlen = (outlen < field) ? outlen : field;
    }
    return st;
}

/*
 * lib$movtc - Move translated characters (VAX MOVTC semantics).
 *
 * Each source byte is translated through the 256-entry table and stored
 * in the destination.  If the destination is longer than the source,
 * the remaining destination bytes are set to the (untranslated) fill
 * character.  If shorter, the source is truncated and LIB$_STRTRU is
 * returned.
 */
uint32_t lib$movtc(const struct dsc$descriptor_s *src,
                   const struct dsc$descriptor_s *fill,
                   const struct dsc$descriptor_s *table,
                   struct dsc$descriptor_s *dst) {
    if (!src || !fill || !table || !dst) return SS$_BADPARAM;
    if (!src->dsc$a_pointer || !table->dsc$a_pointer || !dst->dsc$a_pointer)
        return SS$_BADPARAM;

    const unsigned char *t = (const unsigned char *)table->dsc$a_pointer;
    char fillc = (fill->dsc$w_length > 0 && fill->dsc$a_pointer)
                     ? fill->dsc$a_pointer[0] : ' ';

    uint16_t n = (src->dsc$w_length < dst->dsc$w_length)
                     ? src->dsc$w_length : dst->dsc$w_length;
    for (uint16_t i = 0; i < n; i++) {
        dst->dsc$a_pointer[i] =
            (char)t[(unsigned char)src->dsc$a_pointer[i]];
    }
    for (uint16_t i = n; i < dst->dsc$w_length; i++) {
        dst->dsc$a_pointer[i] = fillc;
    }
    return (src->dsc$w_length > dst->dsc$w_length) ? LIB$_STRTRU : SS$_NORMAL;
}

/*
 * tra_via_table - translate src through a 256-byte table into dst.
 */
static uint32_t tra_via_table(const struct dsc$descriptor_s *src,
                              struct dsc$descriptor_s *dst,
                              const unsigned char *t) {
    if (!src || !dst || !src->dsc$a_pointer || !dst->dsc$a_pointer)
        return SS$_BADPARAM;
    uint16_t n = (src->dsc$w_length < dst->dsc$w_length)
                     ? src->dsc$w_length : dst->dsc$w_length;
    for (uint16_t i = 0; i < n; i++) {
        dst->dsc$a_pointer[i] =
            (char)t[(unsigned char)src->dsc$a_pointer[i]];
    }
    return (src->dsc$w_length > dst->dsc$w_length) ? LIB$_STRTRU : SS$_NORMAL;
}

/*
 * lib$tra_asc_ebc - Translate an ASCII string to EBCDIC (CP037).
 */
uint32_t lib$tra_asc_ebc(const struct dsc$descriptor_s *src,
                         struct dsc$descriptor_s *dst) {
    return tra_via_table(src, dst, (const unsigned char *)&LIB$AB_ASC_EBC);
}

/*
 * lib$tra_ebc_asc - Translate an EBCDIC string to ASCII (CP037).
 */
uint32_t lib$tra_ebc_asc(const struct dsc$descriptor_s *src,
                         struct dsc$descriptor_s *dst) {
    return tra_via_table(src, dst, (const unsigned char *)&LIB$AB_EBC_ASC);
}

/*
 * return_symbol - copy a locale symbol string into dst and report its
 * length through retlen.
 */
static uint32_t return_symbol(struct dsc$descriptor_s *dst,
                              uint16_t *retlen, const char *sym) {
    if (!dst) return SS$_BADPARAM;
    uint16_t slen = (uint16_t)strlen(sym);
    uint16_t field = (dst->dsc$b_class == DSC$K_CLASS_D)
                         ? slen : dst->dsc$w_length;
    uint32_t st = copy_to_desc(dst, sym, slen);
    if (!$VMS_STATUS_SUCCESS(st) && st != LIB$_STRTRU) return st;
    if (retlen) *retlen = (slen < field) ? slen : field;
    return SS$_NORMAL;
}

/*
 * lib$currency - Return the system currency symbol (default "$").
 * lib$digit_sep - Return the digit-separator symbol (default ",").
 * lib$radix_point - Return the radix-point symbol (default ".").
 *
 * OpenVMS derives these from the SYS$CURRENCY / SYS$DIGIT_SEP /
 * SYS$RADIX_POINT logical names; when undefined the documented defaults
 * (US locale) are returned.
 */
uint32_t lib$currency(struct dsc$descriptor_s *dst, uint16_t *retlen) {
    return return_symbol(dst, retlen, "$");
}

uint32_t lib$digit_sep(struct dsc$descriptor_s *dst, uint16_t *retlen) {
    return return_symbol(dst, retlen, ",");
}

uint32_t lib$radix_point(struct dsc$descriptor_s *dst, uint16_t *retlen) {
    return return_symbol(dst, retlen, ".");
}
