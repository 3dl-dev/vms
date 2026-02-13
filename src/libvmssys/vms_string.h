/*
 * vms_string.h - String and memory primitive declarations
 */

#ifndef _VMS_STRING_H
#define _VMS_STRING_H

#include "vms_types.h"

/* Memory operations */
void *vms_memcpy(void *dest, const void *src, vms_size_t n);
void *vms_memmove(void *dest, const void *src, vms_size_t n);
void *vms_memset(void *s, int c, vms_size_t n);
int   vms_memcmp(const void *s1, const void *s2, vms_size_t n);
void *vms_memchr(const void *s, int c, vms_size_t n);

/* String operations */
vms_size_t vms_strlen(const char *s);
vms_size_t vms_strnlen(const char *s, vms_size_t maxlen);
char *vms_strcpy(char *dest, const char *src);
char *vms_strncpy(char *dest, const char *src, vms_size_t n);
char *vms_strcat(char *dest, const char *src);
char *vms_strncat(char *dest, const char *src, vms_size_t n);
int   vms_strcmp(const char *s1, const char *s2);
int   vms_strncmp(const char *s1, const char *s2, vms_size_t n);
char *vms_strchr(const char *s, int c);
char *vms_strrchr(const char *s, int c);
char *vms_strstr(const char *haystack, const char *needle);

/* Character classification */
int vms_toupper(int c);
int vms_tolower(int c);
int vms_isalpha(int c);
int vms_isdigit(int c);
int vms_isalnum(int c);
int vms_isspace(int c);
int vms_isprint(int c);
int vms_isupper(int c);
int vms_islower(int c);
int vms_isxdigit(int c);

/* Case-insensitive comparison */
int vms_strcasecmp(const char *s1, const char *s2);
int vms_strncasecmp(const char *s1, const char *s2, vms_size_t n);

/* String-to-number */
unsigned long vms_strtoul(const char *nptr, char **endptr, int base);
long vms_strtol(const char *nptr, char **endptr, int base);
int  vms_atoi(const char *s);
long vms_atol(const char *s);

#endif /* _VMS_STRING_H */
