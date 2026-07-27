/*
 * lib_symbol.c - LIB$ CLI Symbol Table Routines
 *
 * Implements LIB$SET_SYMBOL, LIB$DELETE_SYMBOL, and LIB$GET_SYMBOL -
 * the RTL routines that define, remove, and read DCL command-language
 * symbols (LIB$K_CLI_LOCAL_SYM / LIB$K_CLI_GLOBAL_SYM).
 *
 * OVMX design choice (not a VMS wire format, so outside the clean-room
 * RE scope): on real VMS these routines call back into the invoking
 * CLI's symbol table over the CLI$ callback interface, which requires
 * the calling image to be running under DCL. A standalone image linked
 * and run directly (as the conformance corpus programs are) has no
 * such CLI context. Rather than unconditionally return LIB$_NOCLI
 * (which is the byte-accurate VMS answer for a bare image with no CLI,
 * but makes the routines untestable/useless outside DCL), OVMX
 * implements a process-local symbol table keyed by (name, table type)
 * that lives for the lifetime of the calling process. This preserves
 * the documented external contract (define/read/delete a symbol) for
 * any caller, DCL-hosted or not, while honestly not claiming to
 * replicate VMS's actual inter-process CLI callback mechanism.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual -
 *            LIB$SET_SYMBOL, LIB$DELETE_SYMBOL, LIB$GET_SYMBOL
 */

#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

#define SYM_MAX_NAME  256
#define SYM_MAX_VALUE 1024
#define SYM_MAX_COUNT 256

struct cli_symbol {
    char     name[SYM_MAX_NAME];
    char     value[SYM_MAX_VALUE];
    uint16_t value_len;
    uint32_t table_type;   /* LIB$K_CLI_LOCAL_SYM or LIB$K_CLI_GLOBAL_SYM */
    int      in_use;
};

static struct cli_symbol symbol_table[SYM_MAX_COUNT];
static pthread_mutex_t symbol_mutex = PTHREAD_MUTEX_INITIALIZER;
static int symbol_table_seeded = 0;

/*
 * DCL reserves a handful of symbol names that are always defined for
 * a running command procedure/image (see OpenVMS DCL Concepts Manual,
 * "Reserved Global Symbols"). $STATUS holds the completion status of
 * the most recently executed command, formatted as "%Xhhhhhhhh". This
 * process-local table seeds it once to SS$_NORMAL so LIB$GET_SYMBOL
 * behaves per documented DCL convention even before any command has
 * run in-process; it is a fixed default, not a live-updated tracker
 * of the last status (OVMX doesn't have a CLI in this context to
 * update it from - see the file header comment).
 */
static void seed_reserved_symbols_locked(void) {
    if (symbol_table_seeded)
        return;
    symbol_table_seeded = 1;

    struct cli_symbol *entry = &symbol_table[0];
    strncpy(entry->name, "$STATUS", sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    static const char default_status[] = "%X00000001";
    memcpy(entry->value, default_status, sizeof(default_status) - 1);
    entry->value_len = sizeof(default_status) - 1;
    entry->table_type = LIB$K_CLI_LOCAL_SYM;
    entry->in_use = 1;
}

static struct cli_symbol *find_symbol_locked(const char *name, uint32_t table_type) {
    seed_reserved_symbols_locked();
    for (int i = 0; i < SYM_MAX_COUNT; i++) {
        if (symbol_table[i].in_use &&
            symbol_table[i].table_type == table_type &&
            strcasecmp(symbol_table[i].name, name) == 0) {
            return &symbol_table[i];
        }
    }
    return NULL;
}

/*
 * lib$set_symbol - Define a CLI symbol.
 *
 * @param symbol      Descriptor of the symbol name (required).
 * @param value       Descriptor of the symbol's new value (required).
 * @param table_type  Optional pointer to LIB$K_CLI_LOCAL_SYM or
 *                     LIB$K_CLI_GLOBAL_SYM. Defaults to
 *                     LIB$K_CLI_LOCAL_SYM if not supplied.
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if a required
 *          argument is missing, LIB$_INSCLIMEM if the symbol name or
 *          value is too long or the table is full.
 */
uint32_t lib$set_symbol(
    const struct dsc$descriptor_s *symbol,
    const struct dsc$descriptor_s *value,
    const uint32_t *table_type)
{
    if (!symbol || !symbol->dsc$a_pointer || !value)
        return SS$_BADPARAM;

    if (symbol->dsc$w_length == 0 || symbol->dsc$w_length >= SYM_MAX_NAME)
        return LIB$_INSCLIMEM;
    if (value->dsc$w_length >= SYM_MAX_VALUE)
        return LIB$_INSCLIMEM;

    uint32_t ttype = table_type ? *table_type : LIB$K_CLI_LOCAL_SYM;

    char name[SYM_MAX_NAME];
    memcpy(name, symbol->dsc$a_pointer, symbol->dsc$w_length);
    name[symbol->dsc$w_length] = '\0';

    pthread_mutex_lock(&symbol_mutex);

    struct cli_symbol *entry = find_symbol_locked(name, ttype);
    if (!entry) {
        for (int i = 0; i < SYM_MAX_COUNT; i++) {
            if (!symbol_table[i].in_use) {
                entry = &symbol_table[i];
                break;
            }
        }
    }

    if (!entry) {
        pthread_mutex_unlock(&symbol_mutex);
        return LIB$_INSCLIMEM;
    }

    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';

    uint16_t vlen = value->dsc$w_length;
    if (vlen > 0 && value->dsc$a_pointer)
        memcpy(entry->value, value->dsc$a_pointer, vlen);
    entry->value_len = vlen;
    entry->table_type = ttype;
    entry->in_use = 1;

    pthread_mutex_unlock(&symbol_mutex);
    return SS$_NORMAL;
}

/*
 * lib$delete_symbol - Delete a CLI symbol.
 *
 * @param symbol      Descriptor of the symbol name (required).
 * @param table_type  Optional pointer to LIB$K_CLI_LOCAL_SYM or
 *                     LIB$K_CLI_GLOBAL_SYM. Defaults to
 *                     LIB$K_CLI_LOCAL_SYM if not supplied.
 *
 * @return  SS$_NORMAL on success, LIB$_NOSUCHSYM if not found,
 *          SS$_BADPARAM if symbol is missing.
 */
uint32_t lib$delete_symbol(
    const struct dsc$descriptor_s *symbol,
    const uint32_t *table_type)
{
    if (!symbol || !symbol->dsc$a_pointer)
        return SS$_BADPARAM;
    if (symbol->dsc$w_length == 0 || symbol->dsc$w_length >= SYM_MAX_NAME)
        return LIB$_NOSUCHSYM;

    uint32_t ttype = table_type ? *table_type : LIB$K_CLI_LOCAL_SYM;

    char name[SYM_MAX_NAME];
    memcpy(name, symbol->dsc$a_pointer, symbol->dsc$w_length);
    name[symbol->dsc$w_length] = '\0';

    pthread_mutex_lock(&symbol_mutex);
    struct cli_symbol *entry = find_symbol_locked(name, ttype);
    if (!entry) {
        pthread_mutex_unlock(&symbol_mutex);
        return LIB$_NOSUCHSYM;
    }
    entry->in_use = 0;
    pthread_mutex_unlock(&symbol_mutex);

    return SS$_NORMAL;
}

/*
 * lib$get_symbol - Read the value of a CLI symbol.
 *
 * @param symbol      Descriptor of the symbol name (required).
 * @param value       Descriptor to receive the value. For a CLASS_S
 *                     (fixed) descriptor, the value is copied and
 *                     truncated to the buffer's declared length. For
 *                     a CLASS_D (dynamic) descriptor, a new buffer is
 *                     allocated to exactly fit the value.
 * @param value_len   Optional pointer to receive the untruncated
 *                     value length.
 * @param table_type  Optional pointer to LIB$K_CLI_LOCAL_SYM or
 *                     LIB$K_CLI_GLOBAL_SYM. If NULL, LOCAL is tried
 *                     first, then GLOBAL (standard VMS symbol lookup
 *                     order).
 *
 * @return  SS$_NORMAL on success, LIB$_NOSUCHSYM if not found,
 *          SS$_BADPARAM if a required argument is missing.
 */
uint32_t lib$get_symbol(
    const struct dsc$descriptor_s *symbol,
    struct dsc$descriptor_s *value,
    uint16_t *value_len,
    uint32_t *table_type)
{
    if (!symbol || !symbol->dsc$a_pointer || !value)
        return SS$_BADPARAM;
    if (symbol->dsc$w_length == 0 || symbol->dsc$w_length >= SYM_MAX_NAME)
        return LIB$_NOSUCHSYM;

    char name[SYM_MAX_NAME];
    memcpy(name, symbol->dsc$a_pointer, symbol->dsc$w_length);
    name[symbol->dsc$w_length] = '\0';

    pthread_mutex_lock(&symbol_mutex);

    struct cli_symbol *entry = NULL;
    uint32_t matched_type = 0;
    if (table_type && *table_type != 0) {
        entry = find_symbol_locked(name, *table_type);
        matched_type = *table_type;
    } else {
        entry = find_symbol_locked(name, LIB$K_CLI_LOCAL_SYM);
        matched_type = LIB$K_CLI_LOCAL_SYM;
        if (!entry) {
            entry = find_symbol_locked(name, LIB$K_CLI_GLOBAL_SYM);
            matched_type = LIB$K_CLI_GLOBAL_SYM;
        }
    }

    if (!entry) {
        pthread_mutex_unlock(&symbol_mutex);
        return LIB$_NOSUCHSYM;
    }

    uint16_t full_len = entry->value_len;

    if (value->dsc$b_class == DSC$K_CLASS_D) {
        struct dsc$descriptor_d *dyn = (struct dsc$descriptor_d *)value;
        vms_desc_free(dyn);
        if (vms_desc_alloc(dyn, full_len) != 0) {
            pthread_mutex_unlock(&symbol_mutex);
            return SS$_INSFMEM;
        }
        if (full_len > 0)
            memcpy(dyn->dsc$a_pointer, entry->value, full_len);
    } else {
        uint16_t copy_len = full_len;
        if (copy_len > value->dsc$w_length)
            copy_len = value->dsc$w_length;
        if (copy_len > 0 && value->dsc$a_pointer)
            memcpy(value->dsc$a_pointer, entry->value, copy_len);
        value->dsc$w_length = copy_len;
    }

    if (value_len)
        *value_len = full_len;
    if (table_type)
        *table_type = matched_type;

    pthread_mutex_unlock(&symbol_mutex);
    return SS$_NORMAL;
}
