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
#include "vms_kif.h"

/* Internal table functions from lnm_table.c */
extern lnm_entry_t *lnm_table_lookup(lnm_table_t *table, const char *name);

/* From lnm_client.c */
extern lnm_table_t *lnm_find_table(lnm_manager_t *mgr, const char *table_name);

/* Copy an equivalence value + attributes into the caller's result buffers. */
static uint32_t fill_result(const char *value, uint32_t attr,
                            char *result, size_t result_size,
                            uint16_t *result_length, uint32_t *attributes)
{
    size_t vallen = strlen(value);
    if (vallen >= result_size)
        vallen = result_size - 1;
    memcpy(result, value, vallen);
    result[vallen] = '\0';
    if (result_length)
        *result_length = (uint16_t)vallen;
    if (attributes)
        *attributes = attr;
    return SS$_NORMAL;
}

/* Copy a found entry's index-0 equivalence into the result buffers. */
static uint32_t fill_from_entry(const lnm_entry_t *entry,
                                char *result, size_t result_size,
                                uint16_t *result_length, uint32_t *attributes)
{
    if (!entry || entry->num_translations == 0)
        return SS$_NOLOGNAM;
    return fill_result(entry->translations[0].value, entry->attributes,
                       result, result_size, result_length, attributes);
}

/*
 * translate_system - Translate a name in the executive-resident LNM$SYSTEM.
 *
 * vms-96e2: LNM$SYSTEM lives in vms.ko (vms-d37/#193), reached through the
 * read-only mmap arena via vms_kif_lnm_translate. This is why SYS$UPDATE,
 * DEFINE/SYSTEM'd at boot by one process, resolves in a later login process
 * (@SYS$UPDATE:PARTS_SETUP.COM).
 *
 * vms_kif_lnm_translate returns 1 (found), 0 (executive present, name absent),
 * or -1 (executive unavailable). At OVMX runtime the executive is always
 * present (PID 1 pins it, Rule 9), so -1 is a host BUILD/TEST-tooling state
 * only; there we consult the process-local SYSTEM table so the ctest suite
 * that seeds SYS$SYSDEVICE etc. still runs. Migrating those host tests to the
 * QEMU path (and dropping this tooling branch) is vms-96e2's tracked follow-up.
 */
static uint32_t translate_system(lnm_manager_t *mgr, const char *name,
                                 char *result, size_t result_size,
                                 uint16_t *result_length, uint32_t *attributes)
{
    char equiv[LNM_MAX_VALUE + 1];
    uint32_t attr = 0;
    int r = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, name, equiv,
                                  sizeof(equiv), NULL, &attr);
    if (r == 1)
        return fill_result(equiv, attr, result, result_size,
                           result_length, attributes);
    if (r == 0)
        return SS$_NOLOGNAM;      /* executive present, name absent */

    /* r < 0: no executive (host tooling) -- local SYSTEM table */
    if (mgr && mgr->system_table) {
        lnm_entry_t *e = lnm_table_lookup(mgr->system_table, name);
        if (e)
            return fill_from_entry(e, result, result_size,
                                   result_length, attributes);
    }
    return SS$_NOLOGNAM;
}

/*
 * lnm_translate - Look up a logical name in the specified table.
 *
 * If table_name is a search list (LNM$FILE_DEV / LNM$DCL_LOGICAL) or NULL,
 * searches process/job/group (process-private) then LNM$SYSTEM (executive-
 * resident). If found, returns the first equivalence string (index 0).
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

    int is_searchlist = (!table_name ||
                         strcasecmp(table_name, LNM_FILE_DEV) == 0 ||
                         strcasecmp(table_name, LNM_DCL_LOGICAL) == 0);
    int is_system = (table_name &&
                     (strcasecmp(table_name, LNM_SYSTEM_TABLE) == 0 ||
                      strcasecmp(table_name, "LNM$SYSTEM_TABLE") == 0 ||
                      strcasecmp(table_name, "SYSTEM") == 0));

    if (is_system)
        return translate_system(mgr, logical_name, result, result_size,
                                result_length, attributes);

    if (is_searchlist) {
        /* Process-private tables first, in the standard order. */
        lnm_table_t *tables[3];
        tables[0] = mgr->process_table;
        tables[1] = mgr->job_table;
        tables[2] = mgr->group_table;
        for (int i = 0; i < 3; i++) {
            if (!tables[i])
                continue;
            lnm_entry_t *entry = lnm_table_lookup(tables[i], logical_name);
            if (entry && entry->num_translations > 0)
                return fill_from_entry(entry, result, result_size,
                                       result_length, attributes);
        }
        /* Then LNM$SYSTEM, from the executive. */
        return translate_system(mgr, logical_name, result, result_size,
                                result_length, attributes);
    }

    /* A specific, non-system table. */
    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;
    return fill_from_entry(lnm_table_lookup(table, logical_name),
                           result, result_size, result_length, attributes);
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
