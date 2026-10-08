/*
 * str_routines.c - STR$ String Routines
 *
 * VMS Runtime Library string manipulation routines. These operate
 * on VMS descriptors and handle both static (Class S) and dynamic
 * (Class D) descriptors transparently.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ssdef.h"
#include "descrip.h"
#include "str$routines.h"

/* Imported from descrip.c */
extern uint32_t dsc$alloc_d(struct dsc$descriptor_d *desc, uint16_t length);
extern uint32_t dsc$free_d(struct dsc$descriptor_d *desc);
extern uint32_t dsc$copy(struct dsc$descriptor *dst,
                          const struct dsc$descriptor *src);

/*
 * str$copy_dx - Copy descriptor contents from src to dest.
 *
 * If dest is dynamic (Class D), allocates/reallocates as needed.
 * If dest is static (Class S), truncates or pads with spaces.
 */
uint32_t str$copy_dx(struct dsc$descriptor_s *dest,
                     const struct dsc$descriptor_s *src) {
    if (!dest || !src) return SS$_BADPARAM;
    uint32_t st = dsc$copy((struct dsc$descriptor *)dest,
                           (const struct dsc$descriptor *)src);
    /* A fixed-length destination shorter than the source: the copy is
     * truncated and STR$_TRU (a warning) says so (observed
     * STR.COPY_DX.FIXED.TRUNC, docs/oracle/semantics/rtl/). */
    if ((st & 1) && dest->dsc$b_class != DSC$K_CLASS_D &&
        src->dsc$w_length > dest->dsc$w_length)
        return STR$_TRU;
    return st;
}

/*
 * str$copy_r - Copy a raw buffer into a descriptor.
 *
 * Copies *src_len bytes from src into dest.
 */
uint32_t str$copy_r(struct dsc$descriptor_s *dest,
                    const uint16_t *src_len, const char *src) {
    if (!dest || !src_len || !src) return SS$_BADPARAM;

    uint16_t len = *src_len;
    if (dest->dsc$b_class == DSC$K_CLASS_D) {
        struct dsc$descriptor_d *ddest = (struct dsc$descriptor_d *)dest;
        uint32_t status = dsc$alloc_d(ddest, len);
        if (!(status & 1)) return status;
        memcpy(ddest->dsc$a_pointer, src, len);
    } else {
        if (!dest->dsc$a_pointer) return SS$_BADPARAM;
        uint16_t copylen = len;
        if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;
        memcpy(dest->dsc$a_pointer, src, copylen);
        if (copylen < dest->dsc$w_length) {
            memset(dest->dsc$a_pointer + copylen, ' ',
                   dest->dsc$w_length - copylen);
        }
    }
    return SS$_NORMAL;
}

/*
 * str$concat - Concatenate two or more source strings into dest.
 *
 * OpenVMS passes a counted argument list (up to 254 sources); OVMX's C
 * calling convention ends the list with a NULL pointer (str$routines.h).
 * Every source is used (observed STR.CONCAT, three sources -> "abcdef").
 */
uint32_t (str$concat)(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *src1,
                    const struct dsc$descriptor_s *src2, ...) {
    if (!dest || !src1) return SS$_BADPARAM;

    const struct dsc$descriptor_s *srcs[254];
    int n = 0;
    srcs[n++] = src1;
    if (src2) {
        srcs[n++] = src2;
        va_list ap;
        va_start(ap, src2);
        const struct dsc$descriptor_s *d;
        while (n < 254 && (d = va_arg(ap, const struct dsc$descriptor_s *)) != NULL)
            srcs[n++] = d;
        va_end(ap);
    }

    uint32_t total = 0;
    for (int i = 0; i < n; i++)
        total += srcs[i]->dsc$w_length;
    if (total > UINT16_MAX) return STR$_STRTOOLON;

    char *buf = malloc(total ? total : 1);
    if (!buf) return STR$_INSVIRMEM;
    uint32_t pos = 0;
    for (int i = 0; i < n; i++) {
        if (srcs[i]->dsc$a_pointer && srcs[i]->dsc$w_length)
            memcpy(buf + pos, srcs[i]->dsc$a_pointer, srcs[i]->dsc$w_length);
        pos += srcs[i]->dsc$w_length;
    }
    struct dsc$descriptor_s all = { (uint16_t)total, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf };
    uint32_t st = str$copy_dx(dest, &all);
    free(buf);
    return st;
}

/*
 * str$compare - Compare two descriptors (case-sensitive).
 *
 * Returns: -1 if str1 < str2, 0 if equal, 1 if str1 > str2.
 * Shorter strings are considered less than longer ones when
 * the common prefix matches.
 */
int32_t str$compare(const struct dsc$descriptor_s *str1,
                    const struct dsc$descriptor_s *str2) {
    if (!str1 || !str2) return 0;

    /* The shorter string compares as if blank-filled to the longer one's
     * length (observed STR.COMPARE.PAD: "abc" = "abc  "). */
    uint16_t l1 = str1->dsc$w_length, l2 = str2->dsc$w_length;
    uint16_t len = l1 > l2 ? l1 : l2;
    for (uint16_t i = 0; i < len; i++) {
        unsigned char c1 = i < l1 ? (unsigned char)str1->dsc$a_pointer[i] : ' ';
        unsigned char c2 = i < l2 ? (unsigned char)str2->dsc$a_pointer[i] : ' ';
        if (c1 != c2) return c1 < c2 ? -1 : 1;
    }
    return 0;
}

/*
 * str$compare_eql - Compare two descriptors for equality.
 *
 * Returns: 0 if equal, 1 if not equal.
 * Note: VMS convention - 0 means strings ARE equal.
 */
int32_t str$compare_eql(const struct dsc$descriptor_s *str1,
                         const struct dsc$descriptor_s *str2) {
    if (!str1 || !str2) return 1;
    if (str1->dsc$w_length != str2->dsc$w_length) return 1;
    return memcmp(str1->dsc$a_pointer, str2->dsc$a_pointer,
                  str1->dsc$w_length) == 0 ? 0 : 1;
}

/*
 * str$case_blind_compare - Compare two descriptors (case-insensitive).
 *
 * Returns: -1 if str1 < str2, 0 if equal, 1 if str1 > str2.
 */
int32_t str$case_blind_compare(const struct dsc$descriptor_s *str1,
                                const struct dsc$descriptor_s *str2) {
    if (!str1 || !str2) return 0;

    /* blank-filled like STR$COMPARE */
    uint16_t l1 = str1->dsc$w_length, l2 = str2->dsc$w_length;
    uint16_t len = l1 > l2 ? l1 : l2;
    for (uint16_t i = 0; i < len; i++) {
        int c1 = toupper(i < l1 ? (unsigned char)str1->dsc$a_pointer[i] : ' ');
        int c2 = toupper(i < l2 ? (unsigned char)str2->dsc$a_pointer[i] : ' ');
        if (c1 != c2) return c1 < c2 ? -1 : 1;
    }
    return 0;
}

/*
 * str$free1_dx - Free a dynamic descriptor's storage.
 *
 * Releases the heap allocation and resets length to 0.
 */
uint32_t str$free1_dx(struct dsc$descriptor_d *desc) {
    return dsc$free_d(desc);
}

/*
 * str$get1_dx - Allocate storage for a dynamic descriptor.
 *
 * Allocates *length bytes for the descriptor.
 */
uint32_t str$get1_dx(const uint16_t *length, struct dsc$descriptor_d *desc) {
    if (!length || !desc) return SS$_BADPARAM;
    return dsc$alloc_d(desc, *length);
}

/*
 * str$trim - Copy source to destination, trimming trailing spaces.
 *
 * Removes all trailing space characters and sets result_len to
 * the trimmed length.
 */
uint32_t str$trim(struct dsc$descriptor_s *dest,
                  const struct dsc$descriptor_s *src,
                  uint16_t *result_len) {
    if (!dest || !src) return SS$_BADPARAM;
    if (!src->dsc$a_pointer) return SS$_BADPARAM;

    /* Trailing blanks AND tabs go (observed STR.TRIM: "  ab  <TAB> " -> "  ab") */
    int len = src->dsc$w_length;
    while (len > 0 && (src->dsc$a_pointer[len - 1] == ' ' || src->dsc$a_pointer[len - 1] == '\t'))
        len--;

    struct dsc$descriptor_s trimmed = {
        .dsc$w_length = (uint16_t)len,
        .dsc$b_dtype = DSC$K_DTYPE_T,
        .dsc$b_class = DSC$K_CLASS_S,
        .dsc$a_pointer = src->dsc$a_pointer
    };

    if (result_len) *result_len = (uint16_t)len;
    return str$copy_dx(dest, &trimmed);
}

/*
 * str$upcase - Copy source to destination, converting to uppercase.
 */
uint32_t str$upcase(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *src) {
    if (!dest || !src) return SS$_BADPARAM;

    uint32_t status = str$copy_dx(dest, src);
    if (!(status & 1) && status != STR$_TRU) return status;

    uint16_t len = dest->dsc$w_length;
    for (uint16_t i = 0; i < len; i++) {
        dest->dsc$a_pointer[i] =
            (char)toupper((unsigned char)dest->dsc$a_pointer[i]);
    }

    return status;
}

/*
 * str$find_first_substring - Find the first occurrence of a substring.
 *
 * Searches src for sub. Sets *index to the 1-based position of the
 * first match (0 if not found). Sets *sub_index to identify which
 * of the varargs substrings matched (1-based).
 */
uint32_t str$find_first_substring(const struct dsc$descriptor_s *src,
                                   uint32_t *index, uint32_t *sub_index,
                                   const struct dsc$descriptor_s *sub, ...) {
    if (!src || !index) return SS$_BADPARAM;

    *index = 0;
    if (sub_index) *sub_index = 0;

    if (!sub || !sub->dsc$a_pointer || sub->dsc$w_length == 0)
        return SS$_NORMAL;

    if (sub->dsc$w_length > src->dsc$w_length) return SS$_NORMAL;

    for (int i = 0; i <= src->dsc$w_length - sub->dsc$w_length; i++) {
        if (memcmp(src->dsc$a_pointer + i, sub->dsc$a_pointer,
                   sub->dsc$w_length) == 0) {
            *index = (uint32_t)(i + 1);  /* VMS is 1-based */
            if (sub_index) *sub_index = 1;
            return SS$_NORMAL;
        }
    }

    return SS$_NORMAL;
}

/*
 * str$position - Return position of substring in source.
 *
 * Searches src for sub starting at position *start (1-based).
 * Returns the 1-based position, or 0 if not found.
 */
uint32_t str$position(const struct dsc$descriptor_s *src,
                      const struct dsc$descriptor_s *sub,
                      const uint32_t *start) {
    if (!src || !sub) return 0;

    int32_t s = (start && *start > 0) ? (int32_t)(*start - 1) : 0;
    /* An empty substring is found where the search starts (observed
     * STR.POSITION.EMPTYSUB -> 1). */
    if (!sub->dsc$a_pointer || sub->dsc$w_length == 0)
        return (uint32_t)(s + 1);
    if (s >= src->dsc$w_length) return 0;
    if (sub->dsc$w_length > (uint16_t)(src->dsc$w_length - s)) return 0;

    for (int i = s; i <= src->dsc$w_length - sub->dsc$w_length; i++) {
        if (memcmp(src->dsc$a_pointer + i, sub->dsc$a_pointer,
                   sub->dsc$w_length) == 0) {
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

/*
 * str$left - Extract left substring (positions 1 through end_pos).
 */
uint32_t str$left(struct dsc$descriptor_s *dest,
                  const struct dsc$descriptor_s *src,
                  const uint16_t *end_pos) {
    if (!dest || !src || !end_pos) return SS$_BADPARAM;

    int32_t len = (int32_t)*end_pos;
    if (len < 0) len = 0;
    if (len > src->dsc$w_length) len = src->dsc$w_length;

    struct dsc$descriptor_s sub = {
        .dsc$w_length = (uint16_t)len,
        .dsc$b_dtype = DSC$K_DTYPE_T,
        .dsc$b_class = DSC$K_CLASS_S,
        .dsc$a_pointer = src->dsc$a_pointer
    };
    return str$copy_dx(dest, &sub);
}

/*
 * str$right - Extract right substring (positions start_pos through end).
 *
 * start_pos is 1-based.
 */
uint32_t str$right(struct dsc$descriptor_s *dest,
                   const struct dsc$descriptor_s *src,
                   const uint16_t *start_pos) {
    if (!dest || !src || !start_pos) return SS$_BADPARAM;

    int32_t start = (int32_t)*start_pos - 1;  /* Convert to 0-based */
    if (start < 0) start = 0;
    if (start >= src->dsc$w_length) {
        struct dsc$descriptor_s empty = {
            0, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)""
        };
        return str$copy_dx(dest, &empty);
    }

    struct dsc$descriptor_s sub = {
        .dsc$w_length = (uint16_t)(src->dsc$w_length - start),
        .dsc$b_dtype = DSC$K_DTYPE_T,
        .dsc$b_class = DSC$K_CLASS_S,
        .dsc$a_pointer = src->dsc$a_pointer + start
    };
    return str$copy_dx(dest, &sub);
}

/*
 * str$len_extr - Extract substring by position and length.
 *
 * start is 1-based.
 */
uint32_t str$len_extr(struct dsc$descriptor_s *dest,
                      const struct dsc$descriptor_s *src,
                      const uint32_t *start_pos,
                      const uint32_t *length) {
    if (!dest || !src || !start_pos || !length) return SS$_BADPARAM;

    int32_t start = (int32_t)*start_pos - 1;  /* Convert to 0-based */
    int32_t len = (int32_t)*length;
    /* Out-of-range positions are clamped and reported with a warning-level
     * status, the substring still delivered (observed STR.LEN_EXTR.NEGSTART:
     * STR$_ILLSTRPOS "abc"; STR.LEN_EXTR.PAST_END: STR$_ILLSTRSPE "ef"). */
    uint32_t warn = SS$_NORMAL;
    if (start < 0) { start = 0; warn = STR$_ILLSTRPOS; }
    if (start >= src->dsc$w_length || len <= 0) {
        struct dsc$descriptor_s empty = {
            0, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)""
        };
        uint32_t st = str$copy_dx(dest, &empty);
        return (st & 1) && warn != SS$_NORMAL ? warn : st;
    }
    if (start + len > src->dsc$w_length) {
        len = src->dsc$w_length - start;
        if (warn == SS$_NORMAL) warn = STR$_ILLSTRSPE;
    }

    struct dsc$descriptor_s sub = {
        .dsc$w_length = (uint16_t)len,
        .dsc$b_dtype = DSC$K_DTYPE_T,
        .dsc$b_class = DSC$K_CLASS_S,
        .dsc$a_pointer = src->dsc$a_pointer + start
    };
    uint32_t st = str$copy_dx(dest, &sub);
    return (st & 1) && warn != SS$_NORMAL ? warn : st;
}

/*
 * str$element - Extract a delimited element from a string.
 */
uint32_t str$element(struct dsc$descriptor_s *dest,
                     const uint32_t *element_num,
                     const struct dsc$descriptor_s *delimiter,
                     const struct dsc$descriptor_s *src) {
    if (!dest || !element_num || !delimiter || !src) return SS$_BADPARAM;
    if (!delimiter->dsc$a_pointer || delimiter->dsc$w_length == 0)
        return SS$_BADPARAM;

    char delim = delimiter->dsc$a_pointer[0];
    uint32_t target = *element_num;
    uint32_t current = 0;
    int start = 0;

    for (int i = 0; i <= src->dsc$w_length; i++) {
        if (i == src->dsc$w_length || src->dsc$a_pointer[i] == delim) {
            if (current == target) {
                struct dsc$descriptor_s elem = {
                    .dsc$w_length = (uint16_t)(i - start),
                    .dsc$b_dtype = DSC$K_DTYPE_T,
                    .dsc$b_class = DSC$K_CLASS_S,
                    .dsc$a_pointer = src->dsc$a_pointer + start
                };
                return str$copy_dx(dest, &elem);
            }
            current++;
            start = i + 1;
        }
    }

    /* No such element: STR$_NOELEM, and the destination is left as it was
     * (observed STR.ELEMENT.5). */
    return STR$_NOELEM;
}

/*
 * str$translate - Copy src to dest, replacing each character found in the
 * match string by the character at the same position of the translation
 * string (a translation string shorter than the match string translates the
 * rest to blanks). Observed STR.TRANSLATE: "abcabc", "XY", "ab" -> "XYcXYc".
 */
uint32_t str$translate(struct dsc$descriptor_s *dest,
                       const struct dsc$descriptor_s *src,
                       const struct dsc$descriptor_s *trans_table,
                       const struct dsc$descriptor_s *match) {
    if (!dest || !src || !trans_table || !match) return SS$_BADPARAM;
    uint16_t n = src->dsc$w_length;
    char *buf = malloc(n ? n : 1);
    if (!buf) return STR$_INSVIRMEM;
    for (uint16_t i = 0; i < n; i++) {
        char c = src->dsc$a_pointer[i];
        for (uint16_t k = 0; k < match->dsc$w_length; k++) {
            if (match->dsc$a_pointer[k] == c) {
                c = k < trans_table->dsc$w_length ? trans_table->dsc$a_pointer[k] : ' ';
                break;
            }
        }
        buf[i] = c;
    }
    struct dsc$descriptor_s out = { n, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf };
    uint32_t st = str$copy_dx(dest, &out);
    free(buf);
    return st;
}

/*
 * str$prefix - Prepend a prefix string to a dynamic descriptor.
 *
 * Note: The header declares dest as dsc$descriptor_s for compatibility,
 * but this operation is only meaningful on CLASS_D descriptors since
 * it reallocates the storage.
 */
uint32_t str$prefix(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *prefix) {
    if (!dest || !prefix) return SS$_BADPARAM;
    if (dest->dsc$b_class != DSC$K_CLASS_D) return SS$_BADPARAM;

    struct dsc$descriptor_d *ddest = (struct dsc$descriptor_d *)dest;

    uint32_t new_len32 = (uint32_t)prefix->dsc$w_length + (uint32_t)ddest->dsc$w_length;
    if (new_len32 > UINT16_MAX) return STR$_STRTOOLON;
    uint16_t new_len = (uint16_t)new_len32;

    char *new_buf = malloc(new_len);
    if (!new_buf) return SS$_INSFMEM;

    if (prefix->dsc$a_pointer && prefix->dsc$w_length > 0) {
        memcpy(new_buf, prefix->dsc$a_pointer, prefix->dsc$w_length);
    }
    if (ddest->dsc$a_pointer && ddest->dsc$w_length > 0) {
        memcpy(new_buf + prefix->dsc$w_length, ddest->dsc$a_pointer,
               ddest->dsc$w_length);
    }

    if (ddest->dsc$a_pointer) free(ddest->dsc$a_pointer);
    ddest->dsc$a_pointer = new_buf;
    ddest->dsc$w_length = new_len;

    return SS$_NORMAL;
}

/*
 * str$append - Append a suffix string to a dynamic descriptor.
 *
 * Note: The header declares dest as dsc$descriptor_s for compatibility,
 * but this operation is only meaningful on CLASS_D descriptors since
 * it reallocates the storage.
 */
uint32_t str$append(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *suffix) {
    if (!dest || !suffix) return SS$_BADPARAM;
    if (dest->dsc$b_class != DSC$K_CLASS_D) return SS$_BADPARAM;

    struct dsc$descriptor_d *ddest = (struct dsc$descriptor_d *)dest;

    /* Appending nothing is a no-op */
    if (suffix->dsc$w_length == 0) return SS$_NORMAL;

    uint32_t new_len32 = (uint32_t)ddest->dsc$w_length + (uint32_t)suffix->dsc$w_length;
    if (new_len32 > UINT16_MAX) return STR$_STRTOOLON;
    uint16_t new_len = (uint16_t)new_len32;

    char *old_buf = ddest->dsc$a_pointer;
    char *new_buf = realloc(old_buf, new_len);
    if (!new_buf) return SS$_INSFMEM;

    if (suffix->dsc$a_pointer) {
        memcpy(new_buf + ddest->dsc$w_length, suffix->dsc$a_pointer,
               suffix->dsc$w_length);
    }
    ddest->dsc$a_pointer = new_buf;
    ddest->dsc$w_length = new_len;

    return SS$_NORMAL;
}

/*
 * str$match_wild - Match string against wildcard pattern.
 *
 * Pattern may contain '*' (match any sequence) and '%' (match any
 * single character). Returns STR$_MATCH if matches, STR$_NOMATCH if not.
 *
 * Parameters:
 *   candidate - Descriptor of string to test
 *   pattern   - Descriptor of wildcard pattern
 */
uint32_t str$match_wild(const struct dsc$descriptor_s *candidate,
                        const struct dsc$descriptor_s *pattern) {
    if (!candidate || !pattern) return STR$_NOMATCH;
    if (!candidate->dsc$a_pointer || !pattern->dsc$a_pointer)
        return STR$_NOMATCH;

    const char *c = candidate->dsc$a_pointer;
    const char *p = pattern->dsc$a_pointer;
    uint16_t clen = candidate->dsc$w_length;
    uint16_t plen = pattern->dsc$w_length;
    uint16_t ci = 0, pi = 0;
    uint16_t star_ci = 0, star_pi = 0;
    int has_star = 0;

    while (ci < clen) {
        if (pi < plen && p[pi] == '*') {
            /* Remember star position for backtracking */
            has_star = 1;
            star_ci = ci;
            star_pi = pi;
            pi++;
        } else if (pi < plen && (p[pi] == '%' || p[pi] == c[ci])) {
            /* Match single character */
            ci++;
            pi++;
        } else if (has_star) {
            /* Backtrack to last star and try matching from next position */
            star_ci++;
            ci = star_ci;
            pi = star_pi + 1;
        } else {
            /* No match */
            return STR$_NOMATCH;
        }
    }

    /* Consume trailing stars in pattern */
    while (pi < plen && p[pi] == '*') {
        pi++;
    }

    /* Both must be at end for a match */
    return (pi == plen) ? STR$_MATCH : STR$_NOMATCH;
}

/*
 * str$analyze_sdesc - Analyze a string descriptor.
 *
 * Extracts the length and data address from any class of descriptor.
 * Returns SS$_NORMAL on success, STR$_ILLSTRCLA for unsupported class.
 *
 * Parameters:
 *   desc   - Descriptor to analyze
 *   length - Receives string length
 *   addr   - Receives string data address
 */
uint32_t str$analyze_sdesc(const struct dsc$descriptor_s *desc,
                           uint16_t *length, char **addr) {
    if (!desc || !length || !addr) return SS$_BADPARAM;

    switch (desc->dsc$b_class) {
        case DSC$K_CLASS_S:
        case DSC$K_CLASS_D:
        case DSC$K_CLASS_A:
            *length = desc->dsc$w_length;
            *addr = desc->dsc$a_pointer;
            return SS$_NORMAL;

        default:
            /* Unsupported descriptor class */
            *length = 0;
            *addr = NULL;
            return STR$_ILLSTRCLA;
    }
}

/*
 * str$compare_multi - Compare strings with multinational character support.
 *
 * This is a stub that falls back to str$compare. Full implementation
 * would handle locale-specific collation sequences.
 *
 * Parameters:
 *   str1   - First string descriptor
 *   str2   - Second string descriptor
 *   flags  - Comparison flags (ignored)
 *   locale - Locale descriptor (ignored)
 */
int32_t str$compare_multi(const struct dsc$descriptor_s *str1,
                          const struct dsc$descriptor_s *str2,
                          const uint32_t *flags,
                          const struct dsc$descriptor_s *locale) {
    (void)flags;
    (void)locale;
    return str$compare(str1, str2);
}

/*
 * str$replace - Replace portion of string.
 *
 * Replaces characters from *start_pos through *end_pos in src with rep.
 * Positions are 1-based.
 *
 * Parameters:
 *   dest      - Destination descriptor
 *   src       - Source descriptor
 *   start_pos - Starting position of replacement (1-based)
 *   end_pos   - Ending position of replacement (1-based)
 *   rep       - Replacement string descriptor
 */
uint32_t str$replace(struct dsc$descriptor_s *dest,
                     const struct dsc$descriptor_s *src,
                     const uint32_t *start_pos,
                     const uint32_t *end_pos,
                     const struct dsc$descriptor_s *rep) {
    if (!dest || !src || !start_pos || !end_pos || !rep)
        return SS$_BADPARAM;

    /* Convert to 0-based indices */
    int32_t start = (int32_t)(*start_pos - 1);
    int32_t end = (int32_t)(*end_pos - 1);

    /* Clamp to valid range */
    if (start < 0) start = 0;
    if (end < start) end = start - 1;  /* Empty range */
    if (start > src->dsc$w_length) start = src->dsc$w_length;
    if (end >= src->dsc$w_length) end = src->dsc$w_length - 1;

    /* Calculate result length */
    int32_t before_len = start;
    int32_t after_start = end + 1;
    int32_t after_len = src->dsc$w_length - after_start;
    if (after_len < 0) after_len = 0;

    uint16_t result_len = (uint16_t)(before_len + rep->dsc$w_length + after_len);

    /* Allocate or use static dest */
    if (dest->dsc$b_class == DSC$K_CLASS_D) {
        struct dsc$descriptor_d *ddest = (struct dsc$descriptor_d *)dest;
        if (ddest->dsc$a_pointer) free(ddest->dsc$a_pointer);
        ddest->dsc$a_pointer = (char *)malloc(result_len);
        if (!ddest->dsc$a_pointer && result_len > 0) {
            ddest->dsc$w_length = 0;
            return SS$_INSFMEM;
        }
        ddest->dsc$w_length = result_len;

        /* Copy before part */
        if (before_len > 0) {
            memcpy(ddest->dsc$a_pointer, src->dsc$a_pointer, before_len);
        }
        /* Copy replacement */
        if (rep->dsc$w_length > 0 && rep->dsc$a_pointer) {
            memcpy(ddest->dsc$a_pointer + before_len,
                   rep->dsc$a_pointer, rep->dsc$w_length);
        }
        /* Copy after part */
        if (after_len > 0) {
            memcpy(ddest->dsc$a_pointer + before_len + rep->dsc$w_length,
                   src->dsc$a_pointer + after_start, after_len);
        }
    } else {
        /* Static dest - truncate/pad as needed */
        if (!dest->dsc$a_pointer) return SS$_BADPARAM;
        uint16_t pos = 0;

        /* Copy before part */
        if (before_len > 0) {
            uint16_t copylen = before_len;
            if (pos + copylen > dest->dsc$w_length) {
                copylen = dest->dsc$w_length - pos;
            }
            memcpy(dest->dsc$a_pointer + pos, src->dsc$a_pointer, copylen);
            pos += copylen;
        }

        /* Copy replacement */
        if (pos < dest->dsc$w_length && rep->dsc$w_length > 0 && rep->dsc$a_pointer) {
            uint16_t copylen = rep->dsc$w_length;
            if (pos + copylen > dest->dsc$w_length) {
                copylen = dest->dsc$w_length - pos;
            }
            memcpy(dest->dsc$a_pointer + pos, rep->dsc$a_pointer, copylen);
            pos += copylen;
        }

        /* Copy after part */
        if (pos < dest->dsc$w_length && after_len > 0) {
            uint16_t copylen = after_len;
            if (pos + copylen > dest->dsc$w_length) {
                copylen = dest->dsc$w_length - pos;
            }
            memcpy(dest->dsc$a_pointer + pos,
                   src->dsc$a_pointer + after_start, copylen);
            pos += copylen;
        }

        /* Pad with spaces */
        if (pos < dest->dsc$w_length) {
            memset(dest->dsc$a_pointer + pos, ' ', dest->dsc$w_length - pos);
        }
    }

    return SS$_NORMAL;
}

/*
 * str$pos_extr - Extract the substring from start_pos through end_pos
 * (1-based, inclusive). Observed STR.POS_EXTR: "abcdef", 2, 4 -> "bcd".
 */
uint32_t str$pos_extr(struct dsc$descriptor_s *dest, const struct dsc$descriptor_s *src,
                      const int32_t *start_pos, const int32_t *end_pos) {
    if (!dest || !src || !start_pos || !end_pos) return SS$_BADPARAM;
    int32_t s = *start_pos, e = *end_pos;
    uint32_t warn = SS$_NORMAL;
    if (s < 1) { s = 1; warn = STR$_ILLSTRPOS; }
    if (e > src->dsc$w_length) { e = src->dsc$w_length; if (warn == SS$_NORMAL) warn = STR$_ILLSTRSPE; }
    int32_t len = e - s + 1;
    if (len < 0) len = 0;
    struct dsc$descriptor_s sub = { (uint16_t)len, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                    src->dsc$a_pointer + (len ? s - 1 : 0) };
    uint32_t st = str$copy_dx(dest, &sub);
    return (st & 1) && warn != SS$_NORMAL ? warn : st;
}

/*
 * str$dupl_char - dest = <length> copies of one character (default 1 blank).
 * Observed STR.DUPL_CHAR: 5, "x" -> "xxxxx".
 */
uint32_t str$dupl_char(struct dsc$descriptor_s *dest, const int32_t *length, const char *character) {
    if (!dest) return SS$_BADPARAM;
    int32_t n = length ? *length : 1;
    if (n < 0) return STR$_NEGSTRLEN;
    if (n > UINT16_MAX) return STR$_STRTOOLON;
    char *buf = malloc(n ? (size_t)n : 1);
    if (!buf) return STR$_INSVIRMEM;
    memset(buf, character ? *character : ' ', (size_t)n);
    struct dsc$descriptor_s out = { (uint16_t)n, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf };
    uint32_t st = str$copy_dx(dest, &out);
    free(buf);
    return st;
}

/*
 * str$find_first_in_set / str$find_first_not_in_set - the 1-based position of
 * the first character of src that is (is not) in set, 0 when there is none.
 * Observed: "hello" in "xyl" -> 3; "hello" not in "h" -> 2.
 */
static int32_t str_first_set(const struct dsc$descriptor_s *src, const struct dsc$descriptor_s *set, int want)
{
    if (!src || !set) return 0;
    for (uint16_t i = 0; i < src->dsc$w_length; i++) {
        int in = set->dsc$w_length && memchr(set->dsc$a_pointer, src->dsc$a_pointer[i], set->dsc$w_length) != NULL;
        if (in == want) return (int32_t)i + 1;
    }
    return 0;
}

int32_t str$find_first_in_set(const struct dsc$descriptor_s *src, const struct dsc$descriptor_s *set) {
    return str_first_set(src, set, 1);
}

int32_t str$find_first_not_in_set(const struct dsc$descriptor_s *src, const struct dsc$descriptor_s *set) {
    return str_first_set(src, set, 0);
}
