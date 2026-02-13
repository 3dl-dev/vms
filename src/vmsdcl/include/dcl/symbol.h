#ifndef __DCL_SYMBOL_H
#define __DCL_SYMBOL_H

#include <stdint.h>
#include <stddef.h>

/* Symbol scope */
#define DCL_SYM_LOCAL   0
#define DCL_SYM_GLOBAL  1

/* Symbol types */
#define DCL_SYM_STRING  0
#define DCL_SYM_INTEGER 1

/* Initialize/cleanup symbol tables */
void dcl_sym_init(void);
void dcl_sym_cleanup(void);

/* Set a symbol value */
int dcl_sym_set(const char *name, const char *value, int scope);
int dcl_sym_set_int(const char *name, int32_t value, int scope);

/* Get a symbol value (returns NULL if not found) */
const char *dcl_sym_get(const char *name);
int dcl_sym_get_int(const char *name, int32_t *value);

/* Delete a symbol */
int dcl_sym_delete(const char *name, int scope);

/* Substitute symbols in a string ('symbol' and &symbol) */
int dcl_sym_substitute(const char *input, char *output, size_t outlen);

/* Enumerate symbols */
typedef int (*dcl_sym_callback)(const char *name, const char *value,
                                 int scope, void *ctx);
int dcl_sym_enumerate(int scope, dcl_sym_callback cb, void *ctx);

#endif /* __DCL_SYMBOL_H */
