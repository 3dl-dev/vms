/*
 * lib_charscan.c - LIB$ character scan / classify / convert routines
 *
 * Implements:
 *
 *   LIB$SCANC          - Scan a string for a character in a set
 *   LIB$SKPC           - Skip leading occurrences of a character
 *   LIB$CHAR           - Convert a byte value to a one-character string
 *   LIB$ANALYZE_SDESC  - Return the length/address a descriptor describes
 *   LIB$ANALYZE_SDESC_64 - 64-bit form of LIB$ANALYZE_SDESC
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 *              - LIB$SCANC (scan characters; VAX SCANC jacket),
 *              - LIB$SKPC (skip equal characters; VAX SKPC jacket),
 *              - LIB$CHAR (byte -> one-character string),
 *              - LIB$ANALYZE_SDESC / LIB$ANALYZE_SDESC_64 (return the data
 *                length and address described by any class of descriptor).
 */

#include <stdint.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

/*
 * lib$scanc - Scan a string for a member of a character set.
 *
 * Each character of the string indexes a 256-byte classification table;
 * the character matches if (table[char] & mask) is non-zero.  Returns
 * the 1-based position of the first matching character, or 0 if none
 * matched (matching the relative-position convention the LIB$ jacket
 * exposes for the VAX SCANC instruction).
 */
uint32_t lib$scanc(const struct dsc$descriptor_s *string,
                   const uint8_t *table, const uint8_t *mask)
{
    if (!string || !table || !mask)
        return 0;

    const uint8_t *s = (const uint8_t *)string->dsc$a_pointer;
    uint16_t len = string->dsc$w_length;
    uint8_t m = *mask;

    for (uint16_t i = 0; i < len; i++) {
        if (table[s[i]] & m)
            return (uint32_t)(i + 1);
    }
    return 0;
}

/*
 * lib$skpc - Skip equal characters.
 *
 * Scans the string for the first character that is NOT equal to the
 * single character supplied in the first descriptor.  Returns the
 * 1-based position of that first non-matching character, or 0 if every
 * character of the string equals it.
 */
uint32_t lib$skpc(const struct dsc$descriptor_s *character,
                  const struct dsc$descriptor_s *string)
{
    if (!character || !string || character->dsc$w_length == 0)
        return 0;

    char c = character->dsc$a_pointer[0];
    const char *s = string->dsc$a_pointer;
    uint16_t len = string->dsc$w_length;

    for (uint16_t i = 0; i < len; i++) {
        if (s[i] != c)
            return (uint32_t)(i + 1);
    }
    return 0;
}

/*
 * lib$char - Convert a byte value to a one-character string.
 *
 * Writes the character into the destination string.  For a fixed-length
 * (CLASS_S) destination longer than one byte, the remaining positions
 * are space-filled, following the VMS string copy-out semantics; the
 * descriptor length is left unchanged.  For a dynamic (CLASS_D)
 * destination the length is set to 1.
 */
uint32_t lib$char(struct dsc$descriptor_s *destination,
                  const uint8_t *ascii_code)
{
    if (!destination || !ascii_code || !destination->dsc$a_pointer)
        return SS$_BADPARAM;

    if (destination->dsc$w_length == 0)
        return LIB$_STRTRU;

    destination->dsc$a_pointer[0] = (char)*ascii_code;

    if (destination->dsc$b_class == DSC$K_CLASS_D) {
        destination->dsc$w_length = 1;
    } else {
        for (uint16_t i = 1; i < destination->dsc$w_length; i++)
            destination->dsc$a_pointer[i] = ' ';
    }
    return SS$_NORMAL;
}

/*
 * lib$analyze_sdesc - Return the data length and address described by a
 *                     string descriptor of any class.
 *
 * OVMX descriptors (including those produced by $DESCRIPTOR64) all use
 * the 32-bit dsc$descriptor_s layout, so the length and pointer come
 * straight from the standard fields.
 */
uint32_t lib$analyze_sdesc(const void *input_descriptor,
                           uint16_t *data_length, void **data_address)
{
    if (!input_descriptor || !data_length || !data_address)
        return SS$_BADPARAM;

    const struct dsc$descriptor_s *d =
        (const struct dsc$descriptor_s *)input_descriptor;

    *data_length  = d->dsc$w_length;
    *data_address = d->dsc$a_pointer;
    return SS$_NORMAL;
}

/*
 * lib$analyze_sdesc_64 - 64-bit form: same analysis, quadword length.
 */
uint32_t lib$analyze_sdesc_64(const void *input_descriptor,
                              uint64_t *data_length, void **data_address)
{
    if (!input_descriptor || !data_length || !data_address)
        return SS$_BADPARAM;

    const struct dsc$descriptor_s *d =
        (const struct dsc$descriptor_s *)input_descriptor;

    *data_length  = (uint64_t)d->dsc$w_length;
    *data_address = d->dsc$a_pointer;
    return SS$_NORMAL;
}
