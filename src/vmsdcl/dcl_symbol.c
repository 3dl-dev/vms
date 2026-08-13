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

/*
 * Local symbols are per command-procedure level. VMS gives each command
 * level (each @ invocation) its own local symbol table, discards it when
 * control returns to the caller, lets an inner level READ an outer level's
 * locals, but sends every assignment to the CURRENT level (so a write in an
 * inner procedure creates/updates a symbol at that level and never modifies
 * the caller's). See:
 *   - OpenVMS User's Manual, "Symbols" / command-level symbol scope: a local
 *     symbol table exists per command level and is deleted on RETURN; inner
 *     levels can read outer local symbols; an assignment defines the symbol at
 *     the current command level.
 *   - OpenVMS DCL Dictionary, SET SYMBOL /SCOPE=([NO]LOCAL,[NO]GLOBAL):
 *     NOLOCAL makes local symbols from OUTER levels inaccessible to the current
 *     and inner levels; NOGLOBAL makes global symbols inaccessible. Default:
 *     both accessible.
 *
 * We model this as a stack of frames. Frame 0 is the interactive / top level;
 * each @ pushes a fresh frame and each EXIT/end-of-procedure pops it.
 */
#define SYM_MAX_FRAMES  64   /* base + DCL_MAX_NEST(32) levels, with headroom */

struct sym_frame {
    struct sym_entry *buckets[SYM_TABLE_SIZE];
    int hide_outer_local;   /* SET SYMBOL/SCOPE=NOLOCAL at/above this level */
    int hide_global;        /* SET SYMBOL/SCOPE=NOGLOBAL at/above this level */
};

static struct sym_frame sym_frames[SYM_MAX_FRAMES];
static int sym_top;   /* index of current (innermost) local frame; base = 0 */

/* Global symbols are shared across all command levels. */
static struct sym_entry *global_table[SYM_TABLE_SIZE];

/* Current-level local bucket array. */
static struct sym_entry **cur_local(void)
{
    return sym_frames[sym_top].buckets;
}

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
    memset(sym_frames, 0, sizeof(sym_frames));
    memset(global_table, 0, sizeof(global_table));
    sym_top = 0;
}

/* Free every entry in one local frame's buckets. */
static void free_frame(struct sym_frame *fr)
{
    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        struct sym_entry *e = fr->buckets[i];
        while (e) {
            struct sym_entry *next = e->next;
            free(e);
            e = next;
        }
        fr->buckets[i] = NULL;
    }
    fr->hide_outer_local = 0;
    fr->hide_global = 0;
}

/*
 * Clean up symbol tables - free all entries in every local frame and global.
 */
void dcl_sym_cleanup(void)
{
    for (int f = 0; f < SYM_MAX_FRAMES; f++)
        free_frame(&sym_frames[f]);
    sym_top = 0;

    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        struct sym_entry *e = global_table[i];
        while (e) {
            struct sym_entry *next = e->next;
            free(e);
            e = next;
        }
        global_table[i] = NULL;
    }
}

/*
 * Push a fresh local symbol frame on @ / procedure entry.
 *
 * The new (inner) level inherits the caller's /SCOPE flags, so a NOLOCAL /
 * NOGLOBAL scope set by an outer level applies to "the current or inner
 * command levels" as the SET SYMBOL/SCOPE documentation specifies. An inner
 * level can re-open access with its own SET SYMBOL/SCOPE=(LOCAL,GLOBAL).
 * Returns 0 on success, -1 if the frame stack is full (level is not pushed).
 */
int dcl_sym_push_frame(void)
{
    if (sym_top >= SYM_MAX_FRAMES - 1)
        return -1;
    int parent = sym_top;
    sym_top++;
    free_frame(&sym_frames[sym_top]);   /* start empty */
    sym_frames[sym_top].hide_outer_local = sym_frames[parent].hide_outer_local;
    sym_frames[sym_top].hide_global = sym_frames[parent].hide_global;
    return 0;
}

/*
 * Pop the current local frame on EXIT / end-of-procedure, discarding every
 * local symbol defined at this level. The base frame (0) is never popped.
 */
void dcl_sym_pop_frame(void)
{
    if (sym_top <= 0)
        return;
    free_frame(&sym_frames[sym_top]);
    sym_top--;
}

/*
 * SET SYMBOL/SCOPE support. Establishes, for the current command level,
 * whether outer-level local symbols and global symbols are accessible.
 * hide_outer_local / hide_global: 1 = inaccessible (NOLOCAL / NOGLOBAL),
 * 0 = accessible (LOCAL / GLOBAL, the default).
 */
void dcl_sym_scope_set(int hide_outer_local, int hide_global)
{
    sym_frames[sym_top].hide_outer_local = hide_outer_local ? 1 : 0;
    sym_frames[sym_top].hide_global = hide_global ? 1 : 0;
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

    /* A local assignment always targets the CURRENT command level: if the
     * symbol already exists at this level it is updated, otherwise it is
     * created here. An identically-named local at an OUTER level is shadowed,
     * never modified (OpenVMS: an assignment defines the symbol at the current
     * command level). Global assignments go to the shared global table. */
    struct sym_entry **table = (scope == DCL_SYM_GLOBAL) ?
                               global_table : cur_local();

    /* Check if already exists at the target table */
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

    /* Mark as integer type (in the table dcl_sym_set just wrote to) */
    struct sym_entry **table = (scope == DCL_SYM_GLOBAL) ?
                               global_table : cur_local();
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

    /* Search the current command level's locals first. */
    struct sym_entry *e = sym_find(cur_local(), name);
    if (e) return e->value;

    /* Then each enclosing (outer) command level, innermost first — unless the
     * current level set /SCOPE=NOLOCAL, which hides all outer-level locals. */
    if (!sym_frames[sym_top].hide_outer_local) {
        for (int f = sym_top - 1; f >= 0; f--) {
            e = sym_find(sym_frames[f].buckets, name);
            if (e) return e->value;
        }
    }

    /* Then the shared global table — unless /SCOPE=NOGLOBAL. */
    if (!sym_frames[sym_top].hide_global) {
        e = sym_find(global_table, name);
        if (e) return e->value;
    }

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

    /* Delete at the current command level for locals (matching where an
     * assignment would land), or from the shared table for globals. */
    struct sym_entry **table = (scope == DCL_SYM_GLOBAL) ?
                               global_table : cur_local();
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
        /* Quoted string.  Inside a "..." string a SINGLE apostrophe is a
         * literal character, but a DOUBLED apostrophe ''symbol' performs
         * symbol substitution (VMS DCL User's Manual, "Symbol Substitution
         * Within Character Strings").  So copy the string verbatim except for
         * the ''symbol' form, which we expand. */
        if (input[i] == '"') {
            output[out++] = input[i++];      /* opening " */
            while (i < in_len && out < outlen - 1) {
                /* Doubled apostrophe: ''symbol' -> value of symbol */
                if (input[i] == '\'' && i + 1 < in_len && input[i + 1] == '\'') {
                    i += 2;                  /* skip the two opening apostrophes */
                    char symname[256];
                    size_t si = 0;
                    while (i < in_len && si < sizeof(symname) - 1) {
                        char c = input[i];
                        if (c == '\'') { i++; break; }   /* closing ' */
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
                        /* undefined symbol -> substitute nothing */
                    }
                    continue;
                }

                output[out++] = input[i];
                if (input[i] == '"') {
                    i++;
                    /* Check for doubled quote (escaped ") */
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
        /* SHOW SYMBOL lists the current command level's local symbols. */
        struct sym_entry **local_table = cur_local();
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
