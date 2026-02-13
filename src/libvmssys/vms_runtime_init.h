/*
 * vms_runtime_init.h - Runtime initialization declarations
 */

#ifndef _VMS_RUNTIME_INIT_H
#define _VMS_RUNTIME_INIT_H

#include "vms_types.h"

/* Called by crt0.S before main() */
void __vms_runtime_init(int argc, char **argv, char **envp);

/* Access to parsed auxv values */
unsigned long vms_getauxval(unsigned long type);

/* Access to environment */
extern char **vms_environ;
char *vms_getenv(const char *name);
int   vms_setenv(const char *name, const char *value);

/* Page size (from auxv) */
extern unsigned long vms_page_size;

#endif /* _VMS_RUNTIME_INIT_H */
