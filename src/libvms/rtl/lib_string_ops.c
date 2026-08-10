/*
 * lib_string_ops.c - LIB$ String and Character Operation Routines
 *
 * Implements string/character routines from the VMS RTL:
 *
 *   LIB$ICHAR    - Integer value of first character in descriptor
 *   LIB$INDEX    - Find position of substring (1-based, 0 if not found)
 *   LIB$LEN      - Length of string excluding trailing spaces
 *   LIB$LOCC     - Locate character in string (1-based, 0 if not found)
 *   LIB$MATCHC   - Find substring in string (1-based, 0 if not found)
 *   LIB$MOVC3    - Move (copy) characters, 3-argument form
 *   LIB$MOVC5    - Move characters with fill, 5-argument form
 *   LIB$SPANC    - Span characters matching a table/mask
 *
 * All string parameters use VMS string descriptors (struct dsc$descriptor_s).
 * MOVC3/MOVC5 use (length, address) pairs per VMS calling convention.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 */

#include <stdint.h>
#include <string.h>
#include "ssdef.h"
#include "descrip.h"
#include "libdef.h"
#include "lib$routines.h"

/*
 * lib$ichar - Integer value of first character.
 *
 * Returns the ASCII value of the first character of the descriptor.
 * Returns 0 if the string is empty or the descriptor is invalid.
 *
 * Parameters:
 *   str - Pointer to string descriptor
 *
 * Returns: ASCII value of first character (not a VMS status code)
 */
uint32_t lib$ichar(const struct dsc$descriptor_s *str)
{
    if (!str || !str->dsc$a_pointer || str->dsc$w_length == 0)
        return 0;

    return (uint32_t)(unsigned char)str->dsc$a_pointer[0];
}

/*
 * lib$index - Find position of substring in string.
 *
 * Searches str for the first occurrence of sub. Returns the 1-based
 * position of the first match, or 0 if not found.
 *
 * Parameters:
 *   str - Pointer to descriptor of string to search
 *   sub - Pointer to descriptor of substring to find
 *
 * Returns: 1-based position of first match, or 0 if not found
 */
uint32_t lib$index(const struct dsc$descriptor_s *str,
                   const struct dsc$descriptor_s *sub)
{
    if (!str || !sub) return 0;
    if (!str->dsc$a_pointer || !sub->dsc$a_pointer) return 0;
    if (sub->dsc$w_length == 0) return 1;  /* empty substring matches at 1 */
    if (sub->dsc$w_length > str->dsc$w_length) return 0;

    uint16_t limit = str->dsc$w_length - sub->dsc$w_length;

    for (uint16_t i = 0; i <= limit; i++) {
        if (memcmp(str->dsc$a_pointer + i, sub->dsc$a_pointer,
                   sub->dsc$w_length) == 0) {
            return (uint32_t)(i + 1);  /* VMS positions are 1-based */
        }
    }

    return 0;
}

/*
 * lib$len - Length of string excluding trailing spaces.
 *
 * Returns the number of characters in the string descriptor, not
 * counting trailing space (0x20) characters.
 *
 * Parameters:
 *   str - Pointer to string descriptor
 *
 * Returns: length without trailing spaces (not a VMS status code)
 */
uint32_t lib$len(const struct dsc$descriptor_s *str)
{
    if (!str || !str->dsc$a_pointer) return 0;

    int len = str->dsc$w_length;
    while (len > 0 && str->dsc$a_pointer[len - 1] == ' ')
        len--;

    return (uint32_t)len;
}

/*
 * lib$locc - Locate character in string.
 *
 * Searches str for the first character that matches any character in
 * the single-character descriptor char_to_find. Returns the 1-based
 * position of the first match, or 0 if not found.
 *
 * On VMS, lib$locc takes a single character (the first character of
 * the char descriptor) and scans str for it.
 *
 * Parameters:
 *   char_to_find - Descriptor whose first character is searched for
 *   str          - Descriptor of string to search
 *
 * Returns: 1-based position of first match, or 0 if not found
 */
uint32_t lib$locc(const struct dsc$descriptor_s *char_to_find,
                  const struct dsc$descriptor_s *str)
{
    if (!char_to_find || !str) return 0;
    if (!char_to_find->dsc$a_pointer || char_to_find->dsc$w_length == 0)
        return 0;
    if (!str->dsc$a_pointer) return 0;

    char target = char_to_find->dsc$a_pointer[0];

    for (uint16_t i = 0; i < str->dsc$w_length; i++) {
        if (str->dsc$a_pointer[i] == target)
            return (uint32_t)(i + 1);
    }

    return 0;
}

/*
 * lib$matchc - Match characters (substring search).
 *
 * Searches str for the first occurrence of sub. Returns the 1-based
 * position immediately after the end of the match (i.e., position of
 * the character after the match), or 0 if not found.
 *
 * Note: lib$matchc returns the position of the character AFTER the
 * match, unlike lib$index which returns the start. If sub is found
 * at position p with length n, lib$matchc returns p + n.
 *
 * Parameters:
 *   sub - Descriptor of substring to search for
 *   str - Descriptor of string to search
 *
 * Returns: 1-based position after match end, or 0 if not found
 */
uint32_t lib$matchc(const struct dsc$descriptor_s *sub,
                    const struct dsc$descriptor_s *str)
{
    if (!sub || !str) return 0;
    if (!sub->dsc$a_pointer || !str->dsc$a_pointer) return 0;
    if (sub->dsc$w_length == 0) return 1;
    if (sub->dsc$w_length > str->dsc$w_length) return 0;

    uint16_t limit = str->dsc$w_length - sub->dsc$w_length;

    for (uint16_t i = 0; i <= limit; i++) {
        if (memcmp(str->dsc$a_pointer + i, sub->dsc$a_pointer,
                   sub->dsc$w_length) == 0) {
            /* Return position after the match */
            return (uint32_t)(i + sub->dsc$w_length + 1);
        }
    }

    return 0;
}

/*
 * lib$movc3 - Move (copy) characters, 3-argument form.
 *
 * Copies *len bytes from src to dst. Source and destination may
 * overlap (uses memmove). This is the simple 3-argument copy form.
 *
 * Parameters:
 *   len - Pointer to word (uint16_t) containing number of bytes
 *   src - Pointer to source buffer
 *   dst - Pointer to destination buffer
 *
 * Returns: SS$_NORMAL (callers often ignore the return value)
 */
uint32_t lib$movc3(const uint16_t *len,
                   const void *src,
                   void *dst)
{
    if (!len || !src || !dst) return SS$_BADPARAM;

    memmove(dst, src, (size_t)*len);

    return SS$_NORMAL;
}

/*
 * lib$movc5 - Move characters with fill, 5-argument form.
 *
 * Copies min(src_len, dst_len) bytes from src to dst. If dst_len >
 * src_len, the remaining bytes in dst are filled with *fill_char.
 *
 * Parameters:
 *   src_len   - Pointer to word (uint16_t) containing source length
 *   src       - Pointer to source buffer
 *   fill_char - Pointer to fill character (used when dst is longer)
 *   dst_len   - Pointer to word (uint16_t) containing destination length
 *   dst       - Pointer to destination buffer
 *
 * Returns: SS$_NORMAL (callers often ignore the return value)
 */
uint32_t lib$movc5(const uint16_t *src_len,
                   const void *src,
                   const char *fill_char,
                   const uint16_t *dst_len,
                   void *dst)
{
    if (!src_len || !src || !fill_char || !dst_len || !dst)
        return SS$_BADPARAM;

    uint16_t slen = *src_len;
    uint16_t dlen = *dst_len;
    uint16_t copylen = slen < dlen ? slen : dlen;

    memmove(dst, src, (size_t)copylen);

    if (dlen > slen) {
        memset((char *)dst + slen, (unsigned char)*fill_char,
               (size_t)(dlen - slen));
    }

    return SS$_NORMAL;
}

/*
 * lib$spanc - Span characters matching a table and mask.
 *
 * Scans str from the beginning, advancing as long as each character
 * satisfies: (table[char] & mask) != 0. Returns the 1-based position
 * of the first character that does NOT match, or 0 if all characters
 * matched (no non-matching character found).
 *
 * Parameters:
 *   str   - Pointer to string descriptor
 *   table - Pointer to 256-byte translation table (indexed by character)
 *   mask  - Pointer to byte mask; character accepted if table[c] & *mask != 0
 *
 * Returns: 1-based position of first non-matching character, or 0 if
 *          all characters matched
 */
uint32_t lib$spanc(const struct dsc$descriptor_s *str,
                   const unsigned char *table,
                   const unsigned char *mask)
{
    if (!str || !table || !mask) return 0;
    if (!str->dsc$a_pointer) return 0;

    unsigned char m = *mask;

    for (uint16_t i = 0; i < str->dsc$w_length; i++) {
        unsigned char c = (unsigned char)str->dsc$a_pointer[i];
        if ((table[c] & m) == 0) {
            /* First non-matching character */
            return (uint32_t)(i + 1);
        }
    }

    /* All characters matched - return 0 (no non-matching char found) */
    return 0;
}

/*
 * lib$scopy_dxdx - Copy a source string (passed by descriptor) to a
 *                  destination string (passed by descriptor).
 *
 * Handles any source class (fixed CLASS_S, dynamic CLASS_D, or varying
 * CLASS_VS) and delivers the bytes to a fixed, dynamic, or varying
 * destination:
 *
 *   CLASS_D  - the destination buffer is (re)allocated to the source
 *              length and dsc$w_length is set to the copied length.
 *   CLASS_S  - the destination keeps its fixed length; the string is
 *              copied and space-filled if the source is shorter, or
 *              truncated (returning LIB$_STRTRU) if the source is longer.
 *   CLASS_VS - up to dsc$w_maxstrlen bytes are copied into the varying
 *              body and its current-length word (CURLEN) is updated;
 *              LIB$_STRTRU is returned if the source was truncated.
 *
 * Returns SS$_NORMAL, LIB$_STRTRU if the source was truncated to fit a
 * fixed/varying destination, SS$_BADPARAM on a bad descriptor, or
 * LIB$_INSVIRMEM if a dynamic (re)allocation fails.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual - LIB$SCOPY_DXDX,
 *            "Copy a Source String Passed by Descriptor to a
 *            Destination String".
 */
uint32_t lib$scopy_dxdx(const struct dsc$descriptor_s *src,
                        struct dsc$descriptor_s *dest) {
    if (!dest)
        return SS$_BADPARAM;

    /* Resolve the source bytes and length, honoring a varying source. */
    const char *sptr = NULL;
    uint16_t slen = 0;
    if (src && src->dsc$a_pointer) {
        if (src->dsc$b_class == DSC$K_CLASS_VS) {
            const struct dsc$varying_string *vs =
                (const struct dsc$varying_string *)src->dsc$a_pointer;
            sptr = vs->dsc$a_body;
            slen = vs->dsc$w_curlen;
        } else {
            sptr = src->dsc$a_pointer;
            slen = src->dsc$w_length;
        }
    }

    switch (dest->dsc$b_class) {
    case DSC$K_CLASS_D: {
        struct dsc$descriptor_d *dd = (struct dsc$descriptor_d *)dest;
        /* Reallocate only when the current size differs. */
        if (dd->dsc$w_length != slen || dd->dsc$a_pointer == NULL) {
            vms_desc_free(dd);
            if (slen > 0 && vms_desc_alloc(dd, slen) != 0)
                return LIB$_INSVIRMEM;
        }
        if (slen > 0 && sptr)
            memcpy(dd->dsc$a_pointer, sptr, slen);
        dd->dsc$w_length = slen;
        return SS$_NORMAL;
    }

    case DSC$K_CLASS_VS: {
        struct dsc$descriptor_vs *dv = (struct dsc$descriptor_vs *)dest;
        struct dsc$varying_string *vs =
            (struct dsc$varying_string *)dv->dsc$a_pointer;
        if (!vs)
            return SS$_BADPARAM;
        uint16_t cap = dv->dsc$w_maxstrlen;
        uint16_t n = (slen <= cap) ? slen : cap;
        if (n > 0 && sptr)
            memcpy(vs->dsc$a_body, sptr, n);
        vs->dsc$w_curlen = n;
        return (slen > cap) ? LIB$_STRTRU : SS$_NORMAL;
    }

    case DSC$K_CLASS_S:
    default: {
        if (!dest->dsc$a_pointer)
            return SS$_BADPARAM;
        uint16_t cap = dest->dsc$w_length;
        uint16_t n = (slen <= cap) ? slen : cap;
        if (n > 0 && sptr)
            memcpy(dest->dsc$a_pointer, sptr, n);
        if (cap > n)                    /* space-fill the remainder */
            memset(dest->dsc$a_pointer + n, ' ', cap - n);
        return (slen > cap) ? LIB$_STRTRU : SS$_NORMAL;
    }
    }
}
