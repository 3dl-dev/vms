/*
 * dcl_symbol.c - DCL Symbol Table Implementation
 *
 * Provides the symbol table used by DCL for variables.
 * Supports local and global scopes with case-insensitive lookup
 * using a hash table with separate chaining.
 *
 * Also implements symbol substitution ('symbol' and &symbol).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include "dcl/symbol.h"

#define SYM_TABLE_SIZE  256
#define SYM_MAX_NAME    255
#define SYM_MAX_VALUE   4096

/* Symbol entry */
struct sym_entry {
    char name[SYM_MAX_NAME + 1];
    char value[SYM_MAX_VALUE + 1];
    int  scope;     /* DCL_SYM_LOCAL or DCL_SYM_GLOBAL */
    int  type;      /* DCL_SYM_STRING or DCL_SYM_INTEGER */
    struct sym_entry *next;
};

/* Two hash tables: local and global */
static struct sym_entry *local_table[SYM_TABLE_SIZE];
static struct sym_entry *global_table[SYM_TABLE_SIZE];

/*
 * Case-insensitive DJB2 hash.
 */
static unsigned int sym_hash(const char *name)
{
    unsigned int h = 5381;
    for (const char *p = name; *p; p++) {
        h = ((h << 5) + h) + (unsigned int)toupper((unsigned char)*p);
    }
    return h % SYM_TABLE_SIZE;
}

/*
 * Initialize symbol tables.
 */
void dcl_sym_init(void)
{
    memset(local_table, 0, sizeof(local_table));
    memset(global_table, 0, sizeof(global_table));
}

/*
 * Clean up symbol tables - free all entries.
 */
void dcl_sym_cleanup(void)
{
    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        struct sym_entry *e = local_table[i];
        while (e) {
            struct sym_entry *next = e->next;
            free(e);
            e = next;
        }
        local_table[i] = NULL;

        e = global_table[i];
        while (e) {
            struct sym_entry *next = e->next;
            free(e);
            e = next;
        }
        global_table[i] = NULL;
    }
}

/*
 * Find an entry in a specific table.
 */
static struct sym_entry *sym_find(struct sym_entry **table, const char *name)
{
    unsigned int h = sym_hash(name);
    struct sym_entry *e = table[h];
    while (e) {
        if (strcasecmp(e->name, name) == 0)
            return e;
        e = e->next;
    }
    return NULL;
}

/*
 * Set a symbol value (string).
 * Returns 0 on success.
 */
int dcl_sym_set(const char *name, const char *value, int scope)
{
    if (!name || !value) return -1;

    struct sym_entry **table = (scope == DCL_SYM_GLOBAL) ?
                               global_table : local_table;

    /* Check if already exists */
    struct sym_entry *e = sym_find(table, name);
    if (e) {
        strncpy(e->value, value, SYM_MAX_VALUE);
        e->value[SYM_MAX_VALUE] = '\0';
        e->type = DCL_SYM_STRING;
        return 0;
    }

    /* Create new entry */
    e = calloc(1, sizeof(*e));
    if (!e) return -1;

    /* Store name in uppercase */
    size_t i;
    for (i = 0; i < SYM_MAX_NAME && name[i]; i++) {
        e->name[i] = (char)toupper((unsigned char)name[i]);
    }
    e->name[i] = '\0';

    strncpy(e->value, value, SYM_MAX_VALUE);
    e->value[SYM_MAX_VALUE] = '\0';
    e->scope = scope;
    e->type = DCL_SYM_STRING;

    /* Insert at head of chain */
    unsigned int h = sym_hash(name);
    e->next = table[h];
    table[h] = e;

    return 0;
}

/*
 * Set a symbol to an integer value.
 */
int dcl_sym_set_int(const char *name, int32_t value, int scope)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);

    if (dcl_sym_set(name, buf, scope) != 0) return -1;

    /* Mark as integer type */
    struct sym_entry **table = (scope == DCL_SYM_GLOBAL) ?
                               global_table : local_table;
    struct sym_entry *e = sym_find(table, name);
    if (e) e->type = DCL_SYM_INTEGER;

    return 0;
}

/*
 * Get a symbol value. Searches local first, then global.
 * Returns NULL if not found.
 */
const char *dcl_sym_get(const char *name)
{
    if (!name) return NULL;

    /* Search local first */
    struct sym_entry *e = sym_find(local_table, name);
    if (e) return e->value;

    /* Then global */
    e = sym_find(global_table, name);
    if (e) return e->value;

    return NULL;
}

/*
 * Get a symbol as an integer value.
 * Returns 0 on success, -1 if not found or not convertible.
 */
int dcl_sym_get_int(const char *name, int32_t *value)
{
    const char *str = dcl_sym_get(name);
    if (!str) return -1;

    char *endp;
    long v = strtol(str, &endp, 0);
    if (*endp != '\0' && *endp != ' ') return -1;

    *value = (int32_t)v;
    return 0;
}

/*
 * Delete a symbol from the specified scope.
 * Returns 0 on success, -1 if not found.
 */
int dcl_sym_delete(const char *name, int scope)
{
    if (!name) return -1;

    struct sym_entry **table = (scope == DCL_SYM_GLOBAL) ?
                               global_table : local_table;
    unsigned int h = sym_hash(name);
    struct sym_entry **pp = &table[h];

    while (*pp) {
        if (strcasecmp((*pp)->name, name) == 0) {
            struct sym_entry *del = *pp;
            *pp = del->next;
            free(del);
            return 0;
        }
        pp = &(*pp)->next;
    }

    return -1;
}

/*
 * Perform symbol substitution on a DCL command line.
 *
 * VMS DCL substitution rules:
 *   'symbol'  - Replace with symbol value (apostrophe substitution)
 *   &symbol   - Replace with symbol value (ampersand substitution)
 *   ''symbol' - Iterative substitution (substitute, then substitute again)
 *   'f$func() - Lexical function (handled elsewhere)
 *
 * Returns 0 on success.
 */
int dcl_sym_substitute(const char *input, char *output, size_t outlen)
{
    if (!input || !output || outlen == 0) return -1;

    size_t out = 0;
    size_t in_len = strlen(input);
    size_t i = 0;

    while (i < in_len && out < outlen - 1) {
        /* Check for quoted string - don't substitute inside */
        if (input[i] == '"') {
            output[out++] = input[i++];
            while (i < in_len && out < outlen - 1) {
                output[out++] = input[i];
                if (input[i] == '"') {
                    i++;
                    /* Check for doubled quote */
                    if (i < in_len && input[i] == '"') {
                        output[out++] = input[i++];
                    } else {
                        break;
                    }
                } else {
                    i++;
                }
            }
            continue;
        }

        /* Apostrophe substitution: 'symbolname' */
        if (input[i] == '\'') {
            i++; /* skip opening ' */

            /* Check for '' (iterative substitution) */
            int iterative = 0;
            if (i < in_len && input[i] == '\'') {
                iterative = 1;
                i++;
            }

            /* Collect symbol name until closing ' or end of valid chars */
            char symname[256];
            size_t si = 0;
            while (i < in_len && si < sizeof(symname) - 1) {
                char c = input[i];
                if (c == '\'') {
                    i++; /* skip closing ' */
                    break;
                }
                if (isalnum((unsigned char)c) || c == '_' || c == '$') {
                    symname[si++] = c;
                    i++;
                } else {
                    break;
                }
            }
            symname[si] = '\0';

            if (si > 0) {
                const char *val = dcl_sym_get(symname);
                if (val) {
                    if (iterative) {
                        /* Do another round of substitution on the value */
                        char temp[4096];
                        dcl_sym_substitute(val, temp, sizeof(temp));
                        size_t vlen = strlen(temp);
                        if (out + vlen < outlen) {
                            memcpy(output + out, temp, vlen);
                            out += vlen;
                        }
                    } else {
                        size_t vlen = strlen(val);
                        if (out + vlen < outlen) {
                            memcpy(output + out, val, vlen);
                            out += vlen;
                        }
                    }
                }
                /* If symbol not found, substitute nothing (empty string) */
            } else {
                /* Lone apostrophe - keep it */
                if (out < outlen - 1) output[out++] = '\'';
            }
            continue;
        }

        /* Ampersand substitution: &symbolname */
        if (input[i] == '&') {
            i++; /* skip & */

            char symname[256];
            size_t si = 0;
            while (i < in_len && si < sizeof(symname) - 1) {
                char c = input[i];
                if (isalnum((unsigned char)c) || c == '_' || c == '$') {
                    symname[si++] = c;
                    i++;
                } else {
                    break;
                }
            }
            symname[si] = '\0';

            if (si > 0) {
                const char *val = dcl_sym_get(symname);
                if (val) {
                    size_t vlen = strlen(val);
                    if (out + vlen < outlen) {
                        memcpy(output + out, val, vlen);
                        out += vlen;
                    }
                }
                /* Consume trailing . if present (VMS convention) */
                if (i < in_len && input[i] == '.') i++;
            } else {
                if (out < outlen - 1) output[out++] = '&';
            }
            continue;
        }

        /* Normal character */
        output[out++] = input[i++];
    }

    output[out] = '\0';
    return 0;
}

/*
 * Enumerate symbols in a scope (or both if scope == -1).
 */
int dcl_sym_enumerate(int scope, dcl_sym_callback cb, void *ctx)
{
    if (!cb) return -1;

    if (scope == DCL_SYM_LOCAL || scope == -1) {
        for (int i = 0; i < SYM_TABLE_SIZE; i++) {
            struct sym_entry *e = local_table[i];
            while (e) {
                if (cb(e->name, e->value, DCL_SYM_LOCAL, ctx) != 0)
                    return 0;
                e = e->next;
            }
        }
    }

    if (scope == DCL_SYM_GLOBAL || scope == -1) {
        for (int i = 0; i < SYM_TABLE_SIZE; i++) {
            struct sym_entry *e = global_table[i];
            while (e) {
                if (cb(e->name, e->value, DCL_SYM_GLOBAL, ctx) != 0)
                    return 0;
                e = e->next;
            }
        }
    }

    return 0;
}
