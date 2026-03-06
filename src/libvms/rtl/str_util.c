/*
 * str_util.c - Shared VMS string utility functions
 *
 * Provides common string operations used across OVMX tools and libraries.
 * Consolidates duplicate implementations from vms_login.c, vms_authorize.c,
 * vms_mail.c, vmssshd.c, sysuaf.c, vmsfs_translate.c, and dcl_parser.c.
 */

#include <ctype.h>
#include <string.h>
#include "str_util.h"

/*
 * str_upcase - Convert string to uppercase in-place.
 */
void str_upcase(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

/*
 * str_upcase_copy - Copy src to dst, converting to uppercase.
 * Copies at most maxlen-1 characters and null-terminates.
 */
void str_upcase_copy(char *dst, const char *src, size_t maxlen)
{
    if (maxlen == 0) return;
    size_t i;
    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

/*
 * str_trim - Trim trailing whitespace and newlines in-place.
 */
void str_trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    s[len] = '\0';
}
