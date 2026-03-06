/*
 * vms_string.c - String and memory primitives (no glibc dependency)
 *
 * These replace the glibc string.h and ctype.h functions used
 * throughout OVMX.  Optimized for correctness over speed; the
 * compiler will auto-vectorize the obvious loops.
 */

#include "vms_string.h"

/* ================================================================
 * Memory operations
 * ================================================================ */

void *vms_memcpy(void *dest, const void *src, vms_size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *vms_memmove(void *dest, const void *src, vms_size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

void *vms_memset(void *s, int c, vms_size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

int vms_memcmp(const void *s1, const void *s2, vms_size_t n)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    while (n--) {
        if (*a != *b)
            return *a - *b;
        a++;
        b++;
    }
    return 0;
}

void *vms_memchr(const void *s, int c, vms_size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

/* ================================================================
 * String operations
 * ================================================================ */

vms_size_t vms_strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (vms_size_t)(p - s);
}

vms_size_t vms_strnlen(const char *s, vms_size_t maxlen)
{
    const char *p = s;
    while (maxlen-- && *p)
        p++;
    return (vms_size_t)(p - s);
}

char *vms_strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *vms_strncpy(char *dest, const char *src, vms_size_t n)
{
    char *d = dest;
    while (n && (*d = *src)) {
        d++;
        src++;
        n--;
    }
    while (n--)
        *d++ = '\0';
    return dest;
}

char *vms_strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *vms_strncat(char *dest, const char *src, vms_size_t n)
{
    char *d = dest;
    while (*d)
        d++;
    while (n-- && (*d = *src)) {
        d++;
        src++;
    }
    *d = '\0';
    return dest;
}

int vms_strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int vms_strncmp(const char *s1, const char *s2, vms_size_t n)
{
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *vms_strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *vms_strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0')
        return (char *)s;
    return (char *)last;
}

char *vms_strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

/* ================================================================
 * Character classification / conversion
 * ================================================================ */

int vms_toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int vms_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

int vms_isalpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int vms_isdigit(int c)
{
    return c >= '0' && c <= '9';
}

int vms_isalnum(int c)
{
    return vms_isalpha(c) || vms_isdigit(c);
}

int vms_isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int vms_isprint(int c)
{
    return c >= 0x20 && c <= 0x7E;
}

int vms_isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

int vms_islower(int c)
{
    return c >= 'a' && c <= 'z';
}

int vms_isxdigit(int c)
{
    return vms_isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

/* ================================================================
 * Case-insensitive comparison
 * ================================================================ */

int vms_strcasecmp(const char *s1, const char *s2)
{
    while (*s1 && vms_tolower((unsigned char)*s1) == vms_tolower((unsigned char)*s2)) {
        s1++;
        s2++;
    }
    return vms_tolower((unsigned char)*s1) - vms_tolower((unsigned char)*s2);
}

int vms_strncasecmp(const char *s1, const char *s2, vms_size_t n)
{
    while (n && *s1 && vms_tolower((unsigned char)*s1) == vms_tolower((unsigned char)*s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return vms_tolower((unsigned char)*s1) - vms_tolower((unsigned char)*s2);
}

/* ================================================================
 * String-to-number conversion
 * ================================================================ */

unsigned long vms_strtoul(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    unsigned long result = 0;
    int digit;

    /* Skip leading whitespace */
    while (vms_isspace((unsigned char)*s))
        s++;

    /* Skip optional '+' */
    if (*s == '+')
        s++;

    /* Auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') {
                base = 16;
                s++;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
    }

    while (*s) {
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;

        if (digit >= base)
            break;

        /* Overflow detection */
        if (result > ((unsigned long)-1 - (unsigned long)digit) / (unsigned long)base) {
            result = (unsigned long)-1;  /* saturate to ULONG_MAX */
            /* Advance past remaining valid digits */
            s++;
            while (*s) {
                int d = -1;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
                else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
                else break;
                if (d >= base) break;
                s++;
            }
            if (endptr)
                *endptr = (char *)s;
            return result;
        }

        result = result * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)s;
    return result;
}

long vms_strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    int neg = 0;

    while (vms_isspace((unsigned char)*s))
        s++;

    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    char *ep;
    unsigned long val = vms_strtoul(s, &ep, base);

    /* If no digits were consumed, endptr must point to original nptr */
    if (ep == s) {
        if (endptr)
            *endptr = (char *)nptr;
        return 0;
    }

    if (endptr)
        *endptr = ep;

    if (neg)
        return -(long)val;
    return (long)val;
}

int vms_atoi(const char *s)
{
    return (int)vms_strtol(s, NULL, 10);
}

long vms_atol(const char *s)
{
    return vms_strtol(s, NULL, 10);
}
