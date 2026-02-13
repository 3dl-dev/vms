/*
 * sys_logical.c - Logical Name System Services
 *
 * Implements the VMS logical name services $CRELNM, $DELLNM, and $TRNLNM.
 * On real VMS these would communicate with the logical name daemon; this
 * implementation uses a simple in-process table (linked list per table)
 * until the vmslnm daemon is built.
 *
 * Supported tables:
 *   LNM$PROCESS_TABLE - Process-private logical names
 *   LNM$JOB           - Job-wide logical names
 *   LNM$GROUP          - Group-wide logical names
 *   LNM$SYSTEM_TABLE  - System-wide logical names
 *
 * Logical names are stored case-insensitively (uppercased).
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include "starlet.h"

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

/* Upcase a string into a buffer */
static void str_upcase(char *dst, const char *src, size_t maxlen) {
    size_t i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

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
    str_upcase(entry->name, name, sizeof(entry->name));
    str_upcase(entry->table, table, sizeof(entry->table));
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

    /* Standard table search order when no table is specified */
    static const char *search_order[] = {
        "LNM$PROCESS_TABLE", "LNM$JOB", "LNM$GROUP",
        "LNM$SYSTEM_TABLE", NULL
    };

    pthread_mutex_lock(&lnm_mutex);

    struct logical_entry *entry = NULL;
    if (tabnam && tabnam->dsc$a_pointer) {
        char table[LNM$C_TABNAMLEN + 1];
        dsc$strncpy(table, tabnam, sizeof(table));
        entry = find_logical(table, name);
    } else {
        /* Search all tables in standard order */
        for (int i = 0; search_order[i]; i++) {
            entry = find_logical(search_order[i], name);
            if (entry) break;
        }
    }

    if (!entry) {
        pthread_mutex_unlock(&lnm_mutex);
        return SS$_NOLOGNAM;
    }

    /* Fill in item list results */
    if (itmlst) {
        for (const struct item_list_3 *item = itmlst;
             item->buflen != 0 || item->item_code != 0; item++) {
            switch (item->item_code) {
                case LNM$_STRING:
                    if (item->bufaddr) {
                        uint16_t len = (uint16_t)strlen(entry->equiv);
                        if (len > item->buflen) len = item->buflen;
                        memcpy(item->bufaddr, entry->equiv, len);
                        if (item->retlen) *item->retlen = len;
                    }
                    break;

                case LNM$_LENGTH:
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                        *(uint32_t *)item->bufaddr =
                            (uint32_t)strlen(entry->equiv);
                    }
                    if (item->retlen) *item->retlen = sizeof(uint32_t);
                    break;

                case LNM$_ATTRIBUTES:
                    if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                        *(uint32_t *)item->bufaddr = entry->attr;
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

    pthread_mutex_unlock(&lnm_mutex);
    return SS$_NORMAL;
}
