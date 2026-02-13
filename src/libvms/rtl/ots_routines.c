/*
 * ots_routines.c - OTS$ (Object Time System) Routines
 *
 * Integer/string conversion utilities from the VMS OTS runtime.
 * These convert between binary integer values and their textual
 * representations stored in VMS descriptors.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ssdef.h"
#include "descrip.h"

/*
 * ots$cvt_l_ti - Convert longword integer to decimal text in a descriptor.
 *
 * Formats the integer value as a right-justified decimal string in the
 * descriptor's buffer, padding with leading spaces.
 *
 * Parameters:
 *   value      - Pointer to integer value to convert
 *   dest       - Descriptor to receive the text
 *   min_digits - Minimum number of digits (ignored for now)
 *   size       - Field size (ignored for now)
 *   flags      - Conversion flags (ignored for now)
 */
uint32_t ots$cvt_l_ti(const int32_t *value,
                      struct dsc$descriptor_s *dest,
                      const int32_t *min_digits,
                      const int32_t *size,
                      const uint32_t *flags) {
    (void)min_digits; (void)size; (void)flags;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", *value);

    uint16_t copylen = (uint16_t)len;
    if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;

    /* Right-justify in field, pad with leading spaces */
    if (copylen < dest->dsc$w_length) {
        memset(dest->dsc$a_pointer, ' ', dest->dsc$w_length - copylen);
        memcpy(dest->dsc$a_pointer + dest->dsc$w_length - copylen,
               buf, copylen);
    } else {
        memcpy(dest->dsc$a_pointer, buf + (len - copylen), copylen);
    }

    return SS$_NORMAL;
}

/*
 * ots$cvt_ti_l - Convert decimal text in a descriptor to a longword integer.
 *
 * Parses the text in the descriptor as a decimal integer, skipping
 * leading whitespace.
 *
 * Parameters:
 *   src   - Descriptor containing the text to convert
 *   value - Receives the converted integer value
 *   flags - Conversion flags (ignored for now)
 */
uint32_t ots$cvt_ti_l(const struct dsc$descriptor_s *src,
                      int32_t *value,
                      const uint32_t *flags) {
    (void)flags;
    if (!src || !value || !src->dsc$a_pointer) return SS$_BADPARAM;

    char buf[64];
    dsc$strncpy(buf, src, sizeof(buf));

    /* Skip leading whitespace */
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    char *endp;
    long result = strtol(p, &endp, 10);

    /* Check that at least one digit was converted */
    if (endp == p) return SS$_BADPARAM;

    *value = (int32_t)result;
    return SS$_NORMAL;
}

/*
 * ots$cvt_l_tz - Convert longword to hexadecimal text.
 */
uint32_t ots$cvt_l_tz(const int32_t *value,
                      struct dsc$descriptor_s *dest,
                      const int32_t *min_digits) {
    (void)min_digits;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%X", (uint32_t)*value);

    uint16_t copylen = (uint16_t)len;
    if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;

    /* Right-justify with leading spaces */
    if (copylen < dest->dsc$w_length) {
        memset(dest->dsc$a_pointer, ' ', dest->dsc$w_length - copylen);
        memcpy(dest->dsc$a_pointer + dest->dsc$w_length - copylen,
               buf, copylen);
    } else {
        memcpy(dest->dsc$a_pointer, buf, copylen);
    }

    return SS$_NORMAL;
}

/*
 * ots$cvt_l_to - Convert longword to octal text.
 */
uint32_t ots$cvt_l_to(const int32_t *value,
                      struct dsc$descriptor_s *dest,
                      const int32_t *min_digits) {
    (void)min_digits;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%o", (uint32_t)*value);

    uint16_t copylen = (uint16_t)len;
    if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;

    /* Right-justify with leading spaces */
    if (copylen < dest->dsc$w_length) {
        memset(dest->dsc$a_pointer, ' ', dest->dsc$w_length - copylen);
        memcpy(dest->dsc$a_pointer + dest->dsc$w_length - copylen,
               buf, copylen);
    } else {
        memcpy(dest->dsc$a_pointer, buf, copylen);
    }

    return SS$_NORMAL;
}
