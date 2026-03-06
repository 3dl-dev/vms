/*
 * str_util.h - Shared VMS string utility functions
 *
 * Provides common string operations used across OVMX tools and libraries.
 */

#ifndef STR_UTIL_H
#define STR_UTIL_H

#include <stddef.h>

/*
 * str_upcase - Convert string to uppercase in-place.
 */
void str_upcase(char *s);

/*
 * str_upcase_copy - Copy src to dst, converting to uppercase.
 * Copies at most maxlen-1 characters and null-terminates.
 */
void str_upcase_copy(char *dst, const char *src, size_t maxlen);

/*
 * str_trim - Trim trailing whitespace and newlines in-place.
 */
void str_trim(char *s);

#endif /* STR_UTIL_H */
