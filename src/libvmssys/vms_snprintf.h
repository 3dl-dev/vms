/*
 * vms_snprintf.h - Minimal formatted output declarations
 */

#ifndef _VMS_SNPRINTF_H
#define _VMS_SNPRINTF_H

#include "vms_types.h"

int vms_vsnprintf(char *buf, vms_size_t size, const char *fmt, va_list ap);
int vms_snprintf(char *buf, vms_size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vms_sprintf(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* _VMS_SNPRINTF_H */
