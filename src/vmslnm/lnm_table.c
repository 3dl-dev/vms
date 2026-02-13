/*
 * lnm_table.c - Logical Name Table Data Structure
 *
 * Implements the in-memory hash table for logical name storage.
 * Each table (process, job, group, system) is a separate hash table
 * with case-insensitive name matching using djb2 hash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "vms/logical.h"
#include "ssdef.h"

/*
 * djb2 hash function, case-folded to uppercase for case-insensitive matching.
 */
static uint32_t lnm_hash(const char *name, uint32_t num_buckets)
{
    uint32_t hash = 5381;
    while (*name) {
        hash = ((hash << 5) + hash) + (uint32_t)toupper((unsigned char)*name);
        name++;
    }
    return hash % num_buckets;
}

/*
 * lnm_table_create - Allocate and initialize a logical name table.
 *
 * @name:        Table name (e.g., "LNM$PROCESS_TABLE")
 * @num_buckets: Number of hash buckets
 *
 * Returns a new table, or NULL on allocation failure.
 */
lnm_table_t *lnm_table_create(const char *name, uint32_t num_buckets)
{
    if (!name || num_buckets == 0)
        return NULL;

    lnm_table_t *table = calloc(1, sizeof(lnm_table_t));
    if (!table)
        return NULL;

    strncpy(table->name, name, sizeof(table->name) - 1);
    table->name[sizeof(table->name) - 1] = '\0';

    table->num_buckets = num_buckets;
    table->count = 0;
    table->parent = NULL;

    table->buckets = calloc(num_buckets, sizeof(lnm_entry_t *));
    if (!table->buckets) {
        free(table);
        return NULL;
    }

    return table;
}

/*
 * lnm_table_destroy - Free all entries and the table itself.
 *
 * @table: Table to destroy (may be NULL).
 */
void lnm_table_destroy(lnm_table_t *table)
{
    if (!table)
        return;

    if (table->buckets) {
        for (uint32_t i = 0; i < table->num_buckets; i++) {
            lnm_entry_t *entry = table->buckets[i];
            while (entry) {
                lnm_entry_t *next = entry->next;
                free(entry);
                entry = next;
            }
        }
        free(table->buckets);
    }

    free(table);
}

/*
 * lnm_table_insert - Insert a logical name entry into a table.
 *
 * If an entry with the same name already exists, it is superseded
 * (replaced) by the new entry.
 *
 * @table: Target table
 * @entry: Entry to insert (the table takes ownership of this pointer)
 *
 * Returns SS$_NORMAL on success, SS$_SUPERSEDE if an existing entry
 * was replaced, or an error status.
 */
uint32_t lnm_table_insert(lnm_table_t *table, lnm_entry_t *entry)
{
    if (!table || !entry)
        return SS$_BADPARAM;

    uint32_t bucket = lnm_hash(entry->name, table->num_buckets);

    /* Check for existing entry with the same name (supersede) */
    lnm_entry_t **pp = &table->buckets[bucket];
    while (*pp) {
        if (strcasecmp((*pp)->name, entry->name) == 0) {
            /* Supersede: replace the old entry */
            entry->next = (*pp)->next;
            free(*pp);
            *pp = entry;
            return SS$_SUPERSEDE;
        }
        pp = &(*pp)->next;
    }

    /* Insert at head of bucket chain */
    entry->next = table->buckets[bucket];
    table->buckets[bucket] = entry;
    table->count++;

    return SS$_NORMAL;
}

/*
 * lnm_table_lookup - Find an entry by name (case-insensitive).
 *
 * @table: Table to search
 * @name:  Logical name to find
 *
 * Returns pointer to the entry, or NULL if not found.
 * The returned pointer is owned by the table - do not free it.
 */
lnm_entry_t *lnm_table_lookup(lnm_table_t *table, const char *name)
{
    if (!table || !name)
        return NULL;

    uint32_t bucket = lnm_hash(name, table->num_buckets);

    lnm_entry_t *entry = table->buckets[bucket];
    while (entry) {
        if (strcasecmp(entry->name, name) == 0)
            return entry;
        entry = entry->next;
    }

    return NULL;
}

/*
 * lnm_table_remove - Remove an entry by name.
 *
 * @table: Table to modify
 * @name:  Logical name to remove
 *
 * Returns SS$_NORMAL if removed, SS$_NOLOGNAM if not found.
 */
uint32_t lnm_table_remove(lnm_table_t *table, const char *name)
{
    if (!table || !name)
        return SS$_BADPARAM;

    uint32_t bucket = lnm_hash(name, table->num_buckets);

    lnm_entry_t **pp = &table->buckets[bucket];
    while (*pp) {
        if (strcasecmp((*pp)->name, name) == 0) {
            lnm_entry_t *victim = *pp;
            *pp = victim->next;
            free(victim);
            table->count--;
            return SS$_NORMAL;
        }
        pp = &(*pp)->next;
    }

    return SS$_NOLOGNAM;
}

/*
 * lnm_table_enumerate - Iterate all entries in a table.
 *
 * @table:    Table to enumerate
 * @callback: Function called for each entry; return 0 to continue, non-zero to stop
 * @ctx:      Opaque context passed to callback
 *
 * Returns SS$_NORMAL on success (all entries visited or callback stopped).
 */
uint32_t lnm_table_enumerate(lnm_table_t *table,
                              lnm_enum_callback_t callback, void *ctx)
{
    if (!table || !callback)
        return SS$_BADPARAM;

    for (uint32_t i = 0; i < table->num_buckets; i++) {
        lnm_entry_t *entry = table->buckets[i];
        while (entry) {
            if (callback(entry->name, entry, ctx) != 0)
                return SS$_NORMAL;
            entry = entry->next;
        }
    }

    return SS$_NORMAL;
}
