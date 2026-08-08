/*
 * sys_logical.c - Logical Name System Services
 *
 * Implements the VMS logical name services $CRELNM, $DELLNM, and $TRNLNM.
 *
 * Table placement (vms-d37):
 *   LNM$PROCESS_TABLE - process-private, kept in logical_table[] here
 *   LNM$JOB           - process-private for now (executive residency deferred)
 *   LNM$GROUP         - process-private for now (executive residency deferred)
 *   LNM$SYSTEM        - EXECUTIVE-RESIDENT: create/delete go through /dev/vms
 *                       (vms_kif_lnm_define/_delete) and translate reads the
 *                       read-only arena (vms_kif_lnm_translate). A name defined
 *                       in LNM$SYSTEM by one process is therefore visible to
 *                       every other process on the node.
 *
 * NO PER-PROCESS FALLBACK for LNM$SYSTEM (CLAUDE.md Rule 9 / INV-6): if
 * /dev/vms is absent, the SYSTEM path fails honestly with SS$_NOSUCHDEV; it
 * does NOT fall back to logical_table[].
 *
 * Logical names are stored case-insensitively (uppercased).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * LNM$PROCESS/JOB/GROUP still answer from logical_table[] (a process-private
 * array). LNM$SYSTEM is executive-resident as of vms-d37: the SYSTEM branch of
 * each service reaches a vms_kif_* entry point, so these are PARTIAL, not
 * USERSPACE.
 *
 * OVMX-PARTIAL: sys$crelnm (vms-d37) -- exec: LNM$SYSTEM storage is the
 *     executive's (vms_kif_lnm_define); PROCESS/JOB/GROUP stay in logical_table[].
 * OVMX-LOCAL: sys$crelnm -- LNM$PROCESS_TABLE, LNM$JOB and LNM$GROUP creates
 *     are still process-private in logical_table[] (JOB/GROUP executive
 *     residency is the deferred half of vms-d37).
 * OVMX-PARTIAL: sys$dellnm (vms-d37) -- exec: LNM$SYSTEM delete is the
 *     executive's (vms_kif_lnm_delete); other tables stay process-private.
 * OVMX-LOCAL: sys$dellnm -- LNM$PROCESS/JOB/GROUP deletes are served from the
 *     process-private logical_table[], not the executive.
 * OVMX-PARTIAL: sys$trnlnm (vms-d37) -- exec: LNM$SYSTEM translate reads the
 *     executive's mmap arena (vms_kif_lnm_translate); other tables are local.
 * OVMX-LOCAL: sys$trnlnm -- the LNM$PROCESS/JOB/GROUP steps of the search read
 *     the process-private logical_table[]; only the LNM$SYSTEM step is executive.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include "starlet.h"
#include "str_util.h"
#include "vms_kif.h"

/*
 * Map a VMS table-name string to an executive-resident table id, or 0 when
 * the table is NOT executive-resident (PROCESS, JOB, GROUP -- kept local).
 * Only LNM$SYSTEM moves to the executive in the vms-d37 core.
 */
static uint32_t lnm_exec_table_id(const char *table)
{
    if (strcasecmp(table, "LNM$SYSTEM") == 0 ||
        strcasecmp(table, "LNM$SYSTEM_TABLE") == 0 ||
        strcasecmp(table, "SYSTEM") == 0)
        return VMS_LNM_TBL_SYSTEM;
    return 0;
}

#define MAX_LOGICALS 1024
#define MAX_EQUIV_LEN 256

/* Logical name entry */
struct logical_entry {
    char     name[LNM$C_NAMLENGTH + 1];
    char     equiv[MAX_EQUIV_LEN];
    char     table[LNM$C_TABNAMLEN + 1];
    uint32_t attr;
    int      in_use;
};

static struct logical_entry logical_table[MAX_LOGICALS];
static pthread_mutex_t lnm_mutex = PTHREAD_MUTEX_INITIALIZER;

/* str_upcase_copy() now provided by str_util.h */

/* Find a logical name in a specific table */
static struct logical_entry *find_logical(const char *table, const char *name) {
    for (int i = 0; i < MAX_LOGICALS; i++) {
        if (logical_table[i].in_use &&
            strcasecmp(logical_table[i].name, name) == 0 &&
            strcasecmp(logical_table[i].table, table) == 0) {
            return &logical_table[i];
        }
    }
    return NULL;
}

/*
 * sys$crelnm - Create a logical name.
 *
 * Creates or supersedes a logical name in the specified table.
 * The equivalence string is extracted from the item list (LNM$_STRING).
 *
 * Parameters:
 *   attr   - Logical name attributes (or NULL)
 *   tabnam - Table name descriptor (e.g. "LNM$PROCESS_TABLE")
 *   lognam - Logical name descriptor
 *   acmode - Access mode (ignored)
 *   itmlst - Item list containing LNM$_STRING with equivalence value
 */
uint32_t sys$crelnm(const uint32_t *attr,
                    const struct dsc$descriptor_s *tabnam,
                    const struct dsc$descriptor_s *lognam,
                    const uint8_t *acmode,
                    const struct item_list_3 *itmlst) {
    (void)acmode;
    if (!tabnam || !lognam) return SS$_BADPARAM;
    if (!tabnam->dsc$a_pointer || !lognam->dsc$a_pointer) return SS$_BADPARAM;

    /* Validate logical name length */
    if (lognam->dsc$w_length == 0 || lognam->dsc$w_length > LNM$C_NAMLENGTH)
        return SS$_IVLOGNAM;

    char table[LNM$C_TABNAMLEN + 1];
    char name[LNM$C_NAMLENGTH + 1];
    dsc$strncpy(table, tabnam, sizeof(table));
    dsc$strncpy(name, lognam, sizeof(name));

    /* Extract equivalence string from item list */
    char equiv[MAX_EQUIV_LEN] = "";
    if (itmlst) {
        for (const struct item_list_3 *item = itmlst;
             item->buflen != 0 || item->item_code != 0; item++) {
            if (item->item_code == LNM$_STRING && item->bufaddr) {
                uint16_t len = item->buflen;
                if (len >= sizeof(equiv)) len = sizeof(equiv) - 1;
                memcpy(equiv, item->bufaddr, len);
                equiv[len] = '\0';
                break;
            }
        }
    }

    /*
     * LNM$SYSTEM is executive-resident: the storage lives in vms.ko, not in
     * this process. Route the mutation through /dev/vms so the name is
     * visible system-wide. No fallback -- SS$_NOSUCHDEV if the executive is
     * absent (vms-d37, INV-6).
     */
    uint32_t exec_tbl = lnm_exec_table_id(table);
    if (exec_tbl) {
        const char *vals[1] = { equiv };
        return vms_kif_lnm_define(exec_tbl, name, vals, 1,
                                  attr ? *attr : 0,
                                  acmode ? *acmode : LNM$C_USER);
    }

    pthread_mutex_lock(&lnm_mutex);

    /* Check for existing entry (supersede) */
    uint32_t ret_status = SS$_NORMAL;
    struct logical_entry *entry = find_logical(table, name);
    if (entry) {
        ret_status = SS$_SUPERSEDE;
    } else {
        /* Find a free slot */
        for (int i = 0; i < MAX_LOGICALS; i++) {
            if (!logical_table[i].in_use) {
                entry = &logical_table[i];
                break;
            }
        }
    }

    if (!entry) {
        pthread_mutex_unlock(&lnm_mutex);
        return SS$_EXQUOTA;
    }

    /* Store the logical name (uppercased) */
    str_upcase_copy(entry->name, name, sizeof(entry->name));
    str_upcase_copy(entry->table, table, sizeof(entry->table));
    strncpy(entry->equiv, equiv, sizeof(entry->equiv) - 1);
    entry->equiv[sizeof(entry->equiv) - 1] = '\0';
    entry->attr = attr ? *attr : 0;
    entry->in_use = 1;

    pthread_mutex_unlock(&lnm_mutex);
    return ret_status;
}

/*
 * sys$dellnm - Delete a logical name.
 *
 * Removes the specified logical name from the specified table.
 */
uint32_t sys$dellnm(const struct dsc$descriptor_s *tabnam,
                    const struct dsc$descriptor_s *lognam,
                    const uint8_t *acmode) {
    (void)acmode;
    if (!tabnam || !lognam) return SS$_BADPARAM;
    if (!tabnam->dsc$a_pointer || !lognam->dsc$a_pointer) return SS$_BADPARAM;

    char table[LNM$C_TABNAMLEN + 1];
    char name[LNM$C_NAMLENGTH + 1];
    dsc$strncpy(table, tabnam, sizeof(table));
    dsc$strncpy(name, lognam, sizeof(name));

    /* LNM$SYSTEM is executive-resident (vms-d37): delete through /dev/vms. */
    uint32_t exec_tbl = lnm_exec_table_id(table);
    if (exec_tbl)
        return vms_kif_lnm_delete(exec_tbl, name, acmode ? *acmode : LNM$C_USER);

    pthread_mutex_lock(&lnm_mutex);

    struct logical_entry *entry = find_logical(table, name);
    if (!entry) {
        pthread_mutex_unlock(&lnm_mutex);
        return SS$_NOLOGNAM;
    }

    entry->in_use = 0;
    pthread_mutex_unlock(&lnm_mutex);
    return SS$_NORMAL;
}

/*
 * sys$trnlnm - Translate a logical name.
 *
 * Looks up a logical name and returns its equivalence string and
 * attributes via the item list. If no table is specified, searches
 * the standard VMS table order: PROCESS, JOB, GROUP, SYSTEM.
 *
 * Supported item codes:
 *   LNM$_STRING     - Equivalence string
 *   LNM$_LENGTH     - Length of equivalence string
 *   LNM$_ATTRIBUTES - Logical name attributes
 *   LNM$_MAX_INDEX  - Maximum translation index (always 0 for single-valued)
 */
uint32_t sys$trnlnm(const uint32_t *attr,
                    const struct dsc$descriptor_s *tabnam,
                    const struct dsc$descriptor_s *lognam,
                    const uint8_t *acmode,
                    const struct item_list_3 *itmlst) {
    (void)attr; (void)acmode;
    if (!lognam || !lognam->dsc$a_pointer) return SS$_BADPARAM;

    char name[LNM$C_NAMLENGTH + 1];
    dsc$strncpy(name, lognam, sizeof(name));

    /*
     * Standard table search order when no table is specified. LNM$SYSTEM is
     * NOT in this list: it is executive-resident (vms-d37) and is consulted
     * separately, after the process-private tables miss.
     */
    static const char *search_order[] = {
        "LNM$PROCESS_TABLE", "LNM$JOB", "LNM$GROUP", NULL
    };

    /*
     * Resolve into local storage (equiv value + attributes), from either the
     * executive-resident LNM$SYSTEM arena or a process-private table, then
     * fill the item list from those. Copying out of the in-process entry
     * before dropping the mutex keeps the fill lock-free and lets both
     * sources share one code path.
     */
    char equiv[MAX_EQUIV_LEN];
    equiv[0] = '\0';
    uint32_t found_attr = 0;
    int have = 0;   /* 1 = found */

    if (tabnam && tabnam->dsc$a_pointer) {
        char table[LNM$C_TABNAMLEN + 1];
        dsc$strncpy(table, tabnam, sizeof(table));

        uint32_t exec_tbl = lnm_exec_table_id(table);
        if (exec_tbl) {
            /* LNM$SYSTEM: read the executive arena. No fallback (INV-6). */
            int r = vms_kif_lnm_translate(exec_tbl, name, equiv,
                                          sizeof(equiv), NULL, &found_attr);
            if (r < 0) return SS$_NOSUCHDEV;   /* executive absent */
            if (r == 0) return SS$_NOLOGNAM;
            have = 1;
        } else {
            pthread_mutex_lock(&lnm_mutex);
            struct logical_entry *e = find_logical(table, name);
            if (e) {
                strncpy(equiv, e->equiv, sizeof(equiv) - 1);
                equiv[sizeof(equiv) - 1] = '\0';
                found_attr = e->attr;
                have = 1;
            }
            pthread_mutex_unlock(&lnm_mutex);
            if (!have) return SS$_NOLOGNAM;
        }
    } else {
        /* Process-private tables first, in standard order. */
        pthread_mutex_lock(&lnm_mutex);
        for (int i = 0; search_order[i]; i++) {
            struct logical_entry *e = find_logical(search_order[i], name);
            if (e) {
                strncpy(equiv, e->equiv, sizeof(equiv) - 1);
                equiv[sizeof(equiv) - 1] = '\0';
                found_attr = e->attr;
                have = 1;
                break;
            }
        }
        pthread_mutex_unlock(&lnm_mutex);

        /* Then LNM$SYSTEM, from the executive arena. */
        if (!have) {
            int r = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, name, equiv,
                                          sizeof(equiv), NULL, &found_attr);
            if (r < 0) return SS$_NOSUCHDEV;   /* executive absent */
            if (r == 0) return SS$_NOLOGNAM;
            have = 1;
        }
    }

    if (!have)
        return SS$_NOLOGNAM;

    /* Fill in item list results from the resolved value. */
    if (itmlst) {
        for (const struct item_list_3 *item = itmlst;
             item->buflen != 0 || item->item_code != 0; item++) {
            switch (item->item_code) {
                case LNM$_STRING:
                    if (item->bufaddr) {
                        uint16_t len = (uint16_t)strlen(equiv);
                        if (len > item->buflen) len = item->buflen;
                        memcpy(item->bufaddr, equiv, len);
                        if (item->retlen) *item->retlen = len;
                    }
                    break;

                case LNM$_LENGTH:
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                        *(uint32_t *)item->bufaddr = (uint32_t)strlen(equiv);
                    }
                    if (item->retlen) *item->retlen = sizeof(uint32_t);
                    break;

                case LNM$_ATTRIBUTES:
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                        *(uint32_t *)item->bufaddr = found_attr;
                    }
                    if (item->retlen) *item->retlen = sizeof(uint32_t);
                    break;

                case LNM$_MAX_INDEX:
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                        *(uint32_t *)item->bufaddr = 0;  /* Single-valued */
                    }
                    if (item->retlen) *item->retlen = sizeof(uint32_t);
                    break;

                default:
                    break;
            }
        }
    }

    return SS$_NORMAL;
}
