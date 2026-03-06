/*
 * STR$ROUTINES.H - VMS String (STR$) Routine Prototypes
 *
 * OpenVMX compatibility layer - Declares the STR$ run-time library
 * routines for string manipulation.  STR$ routines operate on VMS
 * string descriptors and handle dynamic memory allocation for
 * CLASS_D (dynamic) descriptors automatically.
 *
 * All STR$ routines accept any descriptor class for input strings,
 * but output descriptors must be either CLASS_S (fixed-length, may
 * truncate) or CLASS_D (dynamic, will be resized as needed).
 *
 * Calling conventions: All routines follow the VMS calling standard.
 * Return values are 32-bit condition codes.  Strings are passed
 * by descriptor.
 *
 * Reference: OpenVMS RTL String Manipulation (STR$) Manual
 */

#ifndef __STR_ROUTINES_H
#define __STR_ROUTINES_H

#include <stdint.h>
#include "descrip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * String Copy Routines
 * ================================================================ */

/**
 * str$copy_dx - Copy source string to destination descriptor
 *
 * @param dest  Pointer to destination descriptor (CLASS_S or CLASS_D)
 * @param src   Pointer to source descriptor
 *
 * @return  SS$_NORMAL on success,
 *          STR$_TRU if truncated (CLASS_S dest too short)
 *
 * Copies the source string to the destination.  If dest is CLASS_D,
 * it is reallocated to match the source length.  If dest is CLASS_S,
 * the string is copied with space-fill or truncation.
 */
uint32_t str$copy_dx(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src
);

/**
 * str$copy_r - Copy string by reference (length + address)
 *
 * @param dest    Pointer to destination descriptor
 * @param srclen  Pointer to source string length
 * @param srcadr  Pointer to source string data
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$copy_r(
    struct dsc$descriptor_s *dest,
    const uint16_t *srclen,
    const char *srcadr
);

/* ================================================================
 * String Concatenation
 * ================================================================ */

/**
 * str$concat - Concatenate two or more strings
 *
 * @param dest  Pointer to destination descriptor
 * @param src1  Pointer to first source descriptor
 * @param src2  Pointer to second source descriptor
 * @param ...   Additional source descriptors (NULL-terminated list)
 *
 * @return  SS$_NORMAL on success,
 *          STR$_TRU if result truncated
 *
 * Concatenates src1 and src2 (and any additional sources) into dest.
 * The VMS calling convention passes a counted argument list; in this
 * implementation, additional sources are passed as varargs terminated
 * by a NULL pointer.
 */
uint32_t str$concat(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src1,
    const struct dsc$descriptor_s *src2,
    ...
);

/**
 * str$append - Append source string to destination
 *
 * @param dest  Pointer to destination descriptor (CLASS_D recommended)
 * @param src   Pointer to source descriptor to append
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$append(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src
);

/**
 * str$prefix - Prepend source string to destination
 *
 * @param dest  Pointer to destination descriptor (CLASS_D recommended)
 * @param src   Pointer to source descriptor to prepend
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$prefix(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src
);

/* ================================================================
 * String Comparison Routines
 * ================================================================ */

/**
 * str$compare - Compare two strings (alphabetic)
 *
 * @param str1  Pointer to first string descriptor
 * @param str2  Pointer to second string descriptor
 *
 * @return  -1 if str1 < str2, 0 if equal, +1 if str1 > str2
 *
 * Performs a lexicographic comparison using the ASCII collating
 * sequence.  Shorter strings are effectively padded with spaces.
 */
int32_t str$compare(
    const struct dsc$descriptor_s *str1,
    const struct dsc$descriptor_s *str2
);

/**
 * str$compare_eql - Compare two strings for equality
 *
 * @param str1  Pointer to first string descriptor
 * @param str2  Pointer to second string descriptor
 *
 * @return  0 if strings are equal, non-zero if different
 *
 * Tests exact equality: same length and same content.
 * Unlike str$compare, does NOT pad shorter strings with spaces.
 */
int32_t str$compare_eql(
    const struct dsc$descriptor_s *str1,
    const struct dsc$descriptor_s *str2
);

/**
 * str$case_blind_compare - Compare strings ignoring case
 *
 * @param str1  Pointer to first string descriptor
 * @param str2  Pointer to second string descriptor
 *
 * @return  -1 if str1 < str2, 0 if equal, +1 if str1 > str2
 */
int32_t str$case_blind_compare(
    const struct dsc$descriptor_s *str1,
    const struct dsc$descriptor_s *str2
);

/**
 * str$compare_multi - Compare strings with multinational character support
 *
 * @param str1      Pointer to first string descriptor
 * @param str2      Pointer to second string descriptor
 * @param flags     Optional pointer to comparison flags
 * @param locale    Optional pointer to locale descriptor
 *
 * @return  -1, 0, or +1
 */
int32_t str$compare_multi(
    const struct dsc$descriptor_s *str1,
    const struct dsc$descriptor_s *str2,
    const uint32_t *flags,
    const struct dsc$descriptor_s *locale
);

/* ================================================================
 * String Extraction and Manipulation
 * ================================================================ */

/**
 * str$left - Extract left portion of string
 *
 * @param dest     Pointer to destination descriptor
 * @param src      Pointer to source descriptor
 * @param end_pos  Pointer to ending character position (1-based)
 *
 * @return  SS$_NORMAL on success
 *
 * Copies characters 1 through *end_pos from src to dest.
 */
uint32_t str$left(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src,
    const uint16_t *end_pos
);

/**
 * str$right - Extract right portion of string
 *
 * @param dest       Pointer to destination descriptor
 * @param src        Pointer to source descriptor
 * @param start_pos  Pointer to starting character position (1-based)
 *
 * @return  SS$_NORMAL on success
 *
 * Copies characters from *start_pos through the end of src to dest.
 */
uint32_t str$right(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src,
    const uint16_t *start_pos
);

/**
 * str$len_extr - Extract substring by position and length
 *
 * @param dest   Pointer to destination descriptor
 * @param src    Pointer to source descriptor
 * @param start  Pointer to starting position (1-based)
 * @param len    Pointer to number of characters to extract
 *
 * @return  SS$_NORMAL on success
 *
 * Extracts *len characters from src starting at position *start.
 */
uint32_t str$len_extr(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src,
    const uint32_t *start,
    const uint32_t *len
);

/**
 * str$element - Extract delimited element from string
 *
 * @param dest       Pointer to destination descriptor
 * @param element    Pointer to element number (0-based)
 * @param delimiter  Pointer to descriptor of delimiter character(s)
 * @param src        Pointer to source descriptor
 *
 * @return  SS$_NORMAL on success, STR$_NOELEM if element not found
 */
uint32_t str$element(
    struct dsc$descriptor_s *dest,
    const uint32_t *element,
    const struct dsc$descriptor_s *delimiter,
    const struct dsc$descriptor_s *src
);

/* ================================================================
 * String Search Routines
 * ================================================================ */

/**
 * str$find_first_substring - Find first occurrence of any substring
 *
 * @param src        Pointer to source string descriptor
 * @param index      Pointer to receive position of found substring (1-based)
 * @param sub_index  Pointer to receive which substring was found (1-based)
 * @param sub        Pointer to first substring descriptor to search for
 * @param ...        Additional substring descriptors (NULL-terminated)
 *
 * @return  STR$_MATCH if found, SS$_NORMAL if not found
 *
 * Searches src for the first occurrence of any of the specified
 * substrings.  Returns the position and identity of the one found
 * first (leftmost match).
 */
uint32_t str$find_first_substring(
    const struct dsc$descriptor_s *src,
    uint32_t *index,
    uint32_t *sub_index,
    const struct dsc$descriptor_s *sub,
    ...
);

/**
 * str$position - Find position of substring
 *
 * @param src    Pointer to source string descriptor
 * @param sub    Pointer to substring descriptor to find
 * @param start  Optional pointer to starting position (1-based, default 1)
 *
 * @return  Position (1-based) of substring, or 0 if not found
 *
 * Searches src for the first occurrence of sub, starting at
 * position *start.
 */
uint32_t str$position(
    const struct dsc$descriptor_s *src,
    const struct dsc$descriptor_s *sub,
    const uint32_t *start
);

/**
 * str$match_wild - Match string against wildcard pattern
 *
 * @param candidate  Pointer to descriptor of string to test
 * @param pattern    Pointer to descriptor of wildcard pattern
 *
 * @return  STR$_MATCH if matches, STR$_NOMATCH if no match
 *
 * Tests if candidate matches pattern.  Pattern may contain
 * '*' (match any sequence) and '%' (match any single character).
 */
uint32_t str$match_wild(
    const struct dsc$descriptor_s *candidate,
    const struct dsc$descriptor_s *pattern
);

/* ================================================================
 * String Modification Routines
 * ================================================================ */

/**
 * str$trim - Remove trailing spaces and tabs
 *
 * @param dest        Pointer to destination descriptor
 * @param src         Pointer to source descriptor
 * @param result_len  Optional pointer to receive result length
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$trim(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src,
    uint16_t *result_len
);

/**
 * str$upcase - Convert string to uppercase
 *
 * @param dest  Pointer to destination descriptor
 * @param src   Pointer to source descriptor
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$upcase(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src
);

/**
 * str$translate - Translate characters using tables
 *
 * @param dest         Pointer to destination descriptor
 * @param src          Pointer to source descriptor
 * @param trans_table  Pointer to descriptor of translation table (256 bytes)
 * @param match_table  Optional pointer to descriptor of match table
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$translate(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src,
    const struct dsc$descriptor_s *trans_table,
    const struct dsc$descriptor_s *match_table
);

/**
 * str$replace - Replace portion of string
 *
 * @param dest       Pointer to destination descriptor
 * @param src        Pointer to source descriptor
 * @param start_pos  Pointer to starting position of replacement (1-based)
 * @param end_pos    Pointer to ending position of replacement (1-based)
 * @param rep        Pointer to descriptor of replacement string
 *
 * @return  SS$_NORMAL on success
 */
uint32_t str$replace(
    struct dsc$descriptor_s *dest,
    const struct dsc$descriptor_s *src,
    const uint32_t *start_pos,
    const uint32_t *end_pos,
    const struct dsc$descriptor_s *rep
);

/* ================================================================
 * Dynamic String Management
 * ================================================================ */

/**
 * str$free1_dx - Free one dynamic string descriptor
 *
 * @param desc  Pointer to dynamic (CLASS_D) descriptor to free
 *
 * @return  SS$_NORMAL on success
 *
 * Releases the storage associated with a dynamic string descriptor,
 * resetting it to zero length with a null pointer.
 */
uint32_t str$free1_dx(
    struct dsc$descriptor_d *desc
);

/**
 * str$get1_dx - Allocate dynamic string of specified length
 *
 * @param length  Pointer to desired string length
 * @param desc    Pointer to dynamic descriptor to allocate
 *
 * @return  SS$_NORMAL on success, STR$_INSVIRMEM if insufficient memory
 */
uint32_t str$get1_dx(
    const uint16_t *length,
    struct dsc$descriptor_d *desc
);

/* ================================================================
 * String Analysis Routines
 * ================================================================ */

/**
 * str$analyze_sdesc - Analyze a string descriptor
 *
 * @param desc    Pointer to descriptor to analyze
 * @param length  Pointer to receive string length
 * @param addr    Pointer to receive string data address
 *
 * @return  SS$_NORMAL on success, STR$_ILLSTRCLA for invalid class
 *
 * Given any class of string descriptor, extracts the length
 * and address of the actual string data.
 */
uint32_t str$analyze_sdesc(
    const struct dsc$descriptor_s *desc,
    uint16_t *length,
    char **addr
);

/* ================================================================
 * STR$ condition value definitions
 * ================================================================ */

#define STR$_NORMAL     0x00801001  /* Normal completion */
#define STR$_TRU        0x00801008  /* String truncated (warning) */
#define STR$_MATCH      0x00801011  /* String matched */
#define STR$_NOMATCH    0x00801018  /* No match */
#define STR$_NOELEM     0x00801020  /* No such element */
#define STR$_INVDELIM   0x00801028  /* Invalid delimiter */
#define STR$_STRTRU     0x00801030  /* String truncated */
#define STR$_FATINTERR  0x0080103C  /* Fatal internal error */
#define STR$_ILLSTRCLA  0x00801044  /* Illegal string class */
#define STR$_INSVIRMEM  0x0080104C  /* Insufficient virtual memory */
#define STR$_NEGSTRLEN  0x00801054  /* Negative string length */
#define STR$_WRONUMARG  0x0080105C  /* Wrong number of arguments */
#define STR$_STRTOOLON  0x00801064  /* String too long */

#ifdef __cplusplus
}
#endif

#endif /* __STR_ROUTINES_H */
