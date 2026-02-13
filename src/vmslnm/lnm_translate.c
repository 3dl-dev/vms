/*
 * lnm_translate.c - Logical Name Translation Logic
 *
 * Implements single-level and iterative logical name translation.
 * VMS logical names can be chained: A -> B -> C -> final_value.
 * Translation iterates until a terminal value or max depth is reached.
 *
 * The LNM$FILE_DEV search list translates through:
 *   process -> job -> group -> system tables in order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "vms/logical.h"
#include "ssdef.h"

/* Internal table functions from lnm_table.c */
extern lnm_entry_t *lnm_table_lookup(lnm_table_t *table, const char *name);

/* From lnm_client.c */
extern lnm_table_t *lnm_find_table(lnm_manager_t *mgr, const char *table_name);

/*
 * search_file_dev - Search through the LNM$FILE_DEV search list.
 *
 * This implements the standard VMS logical name search order:
 *   1. LNM$PROCESS_TABLE
 *   2. LNM$JOB
 *   3. LNM$GROUP
 *   4. LNM$SYSTEM
 *
 * @mgr:  The logical name manager
 * @name: Logical name to search for
 *
 * Returns the entry if found, NULL otherwise.
 */
static lnm_entry_t *search_file_dev(lnm_manager_t *mgr, const char *name)
{
    lnm_table_t *tables[4];
    tables[0] = mgr->process_table;
    tables[1] = mgr->job_table;
    tables[2] = mgr->group_table;
    tables[3] = mgr->system_table;

    for (int i = 0; i < 4; i++) {
        if (!tables[i])
            continue;
        lnm_entry_t *entry = lnm_table_lookup(tables[i], name);
        if (entry)
            return entry;
    }

    return NULL;
}

/*
 * lnm_translate - Look up a logical name in the specified table.
 *
 * If table_name is LNM$FILE_DEV, searches process/job/group/system.
 * If found, returns the first equivalence string (index 0).
 *
 * @mgr:           Logical name manager
 * @table_name:    Table to search (or LNM$FILE_DEV for search list)
 * @logical_name:  Name to translate
 * @result:        Buffer to receive the equivalence string
 * @result_size:   Size of the result buffer
 * @result_length: Receives the length of the result (may be NULL)
 * @attributes:    Receives the entry attributes (may be NULL)
 *
 * Returns SS$_NORMAL on success, SS$_NOLOGNAM if not found.
 */
uint32_t lnm_translate(lnm_manager_t *mgr, const char *table_name,
                        const char *logical_name, char *result,
                        size_t result_size, uint16_t *result_length,
                        uint32_t *attributes)
{
    if (!mgr || !logical_name || !result || result_size == 0)
        return SS$_BADPARAM;

    /* Validate logical name */
    size_t namelen = strlen(logical_name);
    if (namelen == 0 || namelen > LNM_MAX_NAME)
        return SS$_IVLOGNAM;

    lnm_entry_t *entry = NULL;

    if (!table_name || strcasecmp(table_name, LNM_FILE_DEV) == 0) {
        /* Search through the standard table hierarchy */
        entry = search_file_dev(mgr, logical_name);
    } else {
        /* Search in a specific table */
        lnm_table_t *table = lnm_find_table(mgr, table_name);
        if (!table)
            return SS$_NOLOGTAB;
        entry = lnm_table_lookup(table, logical_name);
    }

    if (!entry)
        return SS$_NOLOGNAM;

    if (entry->num_translations == 0)
        return SS$_NOLOGNAM;

    /* Return the first (index 0) equivalence string */
    size_t vallen = entry->translations[0].length;
    if (vallen >= result_size)
        vallen = result_size - 1;

    memcpy(result, entry->translations[0].value, vallen);
    result[vallen] = '\0';

    if (result_length)
        *result_length = (uint16_t)vallen;
    if (attributes)
        *attributes = entry->attributes;

    return SS$_NORMAL;
}

/*
 * lnm_translate_iterative - Translate a logical name iteratively.
 *
 * Translates the logical name, then if the result is itself a logical
 * name, translates again, up to LNM_MAX_DEPTH times.
 *
 * Iteration stops when:
 *   - The result is not a logical name (not found in any table)
 *   - The LNM_ATTR_TERMINAL attribute is set on the entry
 *   - Maximum depth (LNM_MAX_DEPTH) is exceeded
 *
 * Uses the LNM$FILE_DEV search list at each level.
 *
 * @mgr:           Logical name manager
 * @table_name:    Initial table to search (or LNM$FILE_DEV)
 * @logical_name:  Name to translate
 * @result:        Buffer to receive the final equivalence string
 * @result_size:   Size of the result buffer
 * @result_length: Receives the length of the result (may be NULL)
 *
 * Returns SS$_NORMAL on success, SS$_NOLOGNAM if the initial name
 * is not found, SS$_RESULTOVF if max depth is exceeded.
 */
uint32_t lnm_translate_iterative(lnm_manager_t *mgr, const char *table_name,
                                  const char *logical_name, char *result,
                                  size_t result_size, uint16_t *result_length)
{
    if (!mgr || !logical_name || !result || result_size == 0)
        return SS$_BADPARAM;

    char current[LNM_MAX_VALUE + 1];
    char next_val[LNM_MAX_VALUE + 1];
    uint32_t attrs;

    strncpy(current, logical_name, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (int depth = 0; depth < LNM_MAX_DEPTH; depth++) {
        uint16_t len = 0;
        attrs = 0;

        uint32_t status = lnm_translate(mgr, table_name, current,
                                         next_val, sizeof(next_val),
                                         &len, &attrs);

        if (status != SS$_NORMAL && status != SS$_SUPERSEDE) {
            if (depth == 0) {
                /* The very first name was not found */
                return SS$_NOLOGNAM;
            }
            /*
             * Previous translation gave us 'current' which is not
             * itself a logical name -- that is the final value.
             */
            size_t curlen = strlen(current);
            if (curlen >= result_size)
                curlen = result_size - 1;
            memcpy(result, current, curlen);
            result[curlen] = '\0';
            if (result_length)
                *result_length = (uint16_t)curlen;
            return SS$_NORMAL;
        }

        /* Got a translation */
        if (attrs & LNM_ATTR_TERMINAL) {
            /* Terminal -- do not translate further */
            size_t vlen = strlen(next_val);
            if (vlen >= result_size)
                vlen = result_size - 1;
            memcpy(result, next_val, vlen);
            result[vlen] = '\0';
            if (result_length)
                *result_length = (uint16_t)vlen;
            return SS$_NORMAL;
        }

        /* The result might be another logical name; continue iterating */
        strncpy(current, next_val, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';

        /*
         * Heuristic: if the result contains a '/' or starts with '.',
         * it is a filesystem path, not a logical name. Stop iterating.
         */
        if (strchr(current, '/') != NULL || current[0] == '.') {
            size_t curlen = strlen(current);
            if (curlen >= result_size)
                curlen = result_size - 1;
            memcpy(result, current, curlen);
            result[curlen] = '\0';
            if (result_length)
                *result_length = (uint16_t)curlen;
            return SS$_NORMAL;
        }
    }

    /* Max depth exceeded -- return whatever we have */
    {
        size_t curlen = strlen(current);
        if (curlen >= result_size)
            curlen = result_size - 1;
        memcpy(result, current, curlen);
        result[curlen] = '\0';
        if (result_length)
            *result_length = (uint16_t)curlen;
    }

    return SS$_RESULTOVF;
}
