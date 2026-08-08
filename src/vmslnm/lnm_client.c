/*
 * lnm_client.c - Logical Name Client Library
 *
 * Provides the public API for logical name operations.
 *
 * LNM$SYSTEM is EXECUTIVE-RESIDENT (vms-d37/#193, wired here by vms-96e2):
 * lnm_create/_delete/_translate route the SYSTEM table through vms_kif -> the
 * /dev/vms arena, so a name DEFINE/SYSTEM'd by one process is visible to every
 * other process on the node (real OpenVMS behaviour). LNM$PROCESS/JOB/GROUP
 * stay in the process-private tables below (their executive residency is the
 * deferred half of vms-d37). There is no daemon to talk to; the socket/daemon
 * client was deleted (vms-a4b) -- do not reintroduce it.
 *
 * NO PER-PROCESS FALLBACK for LNM$SYSTEM (vms-48ab, CLAUDE.md Rule 9 / INV-6):
 * vms-96e2 shipped a transitional fallback to a process-local SYSTEM table
 * when /dev/vms was absent, so host ctest could still exercise DEFINE/SYSTEM.
 * That was itself the silent-per-process-fake shape Rule 9 forbids, and
 * vms-48ab removed it: create/delete/translate against LNM$SYSTEM now return
 * SS$_NOSUCHDEV honestly with no executive, exactly like sys$crelnm/$trnlnm/
 * $dellnm (src/libvms/syssvc/sys_logical.c) already did. The host tests that
 * relied on the fallback moved to tests/qemu/test_syssvc_lnm_system.c, which
 * proves this path against a real /dev/vms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>

#include "vms/logical.h"
#include "ssdef.h"
#include "vms_kif.h"

/* Default number of hash buckets per table */
#define LNM_DEFAULT_BUCKETS 256

/*
 * is_system_table - true when table_name names the executive-resident
 * LNM$SYSTEM table.
 *
 * vms-96e2 (on top of vms-d37/#193): LNM$SYSTEM is executive-resident. A name
 * defined in it by one process must be visible to every other process on the
 * node -- that is the whole reason SYS$UPDATE, DEFINE/SYSTEM'd by
 * SYS$MANAGER:STARTUP.COM at boot, has to be seen by a later login process for
 * @SYS$UPDATE:PARTS_SETUP.COM to resolve (the 0.2 demo blocker). So lnm_create,
 * lnm_delete and lnm_translate route the SYSTEM table through vms_kif -> the
 * /dev/vms executive arena (the #193 client), NOT the process-private tables
 * kept here. Every consumer of this manager (DCL DEFINE/SHOW LOGICAL/F$TRNLNM
 * and vmsfs filespec resolution) therefore reaches the one executive-resident
 * table without a caller change and with no split-brain.
 *
 * vmsfs cannot call sys$crelnm/$trnlnm directly (libvms links vmsfs -- a cycle),
 * so this manager, which sits below libvms and which both DCL and vmsfs already
 * use, is where the executive routing goes; it reaches the SAME arena as
 * libvms's sys$ services via the shared vms_kif client.
 *
 * No fallback (vms-48ab): when /dev/vms is absent, callers below return
 * vms_kif's SS$_NOSUCHDEV unchanged rather than silently using the
 * process-local system_table.
 */
static int is_system_table(const char *table_name)
{
    return table_name &&
           (strcasecmp(table_name, LNM_SYSTEM_TABLE) == 0 ||
            strcasecmp(table_name, "LNM$SYSTEM_TABLE") == 0 ||
            strcasecmp(table_name, "SYSTEM") == 0);
}

/* Internal table functions from lnm_table.c */
extern lnm_table_t *lnm_table_create(const char *name, uint32_t num_buckets);
extern void          lnm_table_destroy(lnm_table_t *table);
extern uint32_t      lnm_table_insert(lnm_table_t *table, lnm_entry_t *entry);
extern lnm_entry_t  *lnm_table_lookup(lnm_table_t *table, const char *name);
extern uint32_t      lnm_table_remove(lnm_table_t *table, const char *name);
extern uint32_t      lnm_table_enumerate(lnm_table_t *table,
                                          lnm_enum_callback_t callback, void *ctx);

/* Global static manager instance, protected by pthread_once */
static lnm_manager_t *g_manager = NULL;
static pthread_once_t g_manager_once = PTHREAD_ONCE_INIT;

/*
 * validate_logical_name - Check that a logical name is valid.
 *
 * VMS logical names must be 1-255 characters and may not contain
 * certain control characters.
 */
static uint32_t validate_logical_name(const char *name)
{
    if (!name)
        return SS$_IVLOGNAM;

    size_t len = strlen(name);
    if (len == 0 || len > LNM_MAX_NAME)
        return SS$_IVLOGNAM;

    /* Check for invalid characters (control chars except $ and _) */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 && c != '\t')
            return SS$_IVLOGNAM;
    }

    return SS$_NORMAL;
}

/*
 * make_entry - Allocate and populate a logical name entry.
 *
 * The name is stored in uppercase.
 */
static lnm_entry_t *make_entry(const char *name, const char *equiv,
                                uint32_t attributes, uint8_t acmode)
{
    lnm_entry_t *entry = calloc(1, sizeof(lnm_entry_t));
    if (!entry)
        return NULL;

    /* Store name in uppercase */
    size_t i;
    for (i = 0; i < LNM_MAX_NAME && name[i]; i++)
        entry->name[i] = (char)toupper((unsigned char)name[i]);
    entry->name[i] = '\0';
    entry->name_length = (uint16_t)i;

    entry->attributes = attributes;
    entry->acmode = acmode;
    entry->next = NULL;

    if (equiv) {
        size_t elen = strlen(equiv);
        if (elen > LNM_MAX_VALUE)
            elen = LNM_MAX_VALUE;
        memcpy(entry->translations[0].value, equiv, elen);
        entry->translations[0].value[elen] = '\0';
        entry->translations[0].length = (uint16_t)elen;
        entry->translations[0].index = 0;
        entry->num_translations = 1;
    }

    return entry;
}

/*
 * lnm_find_table - Find a table by name within a manager.
 *
 * Accepts the standard table names:
 *   LNM$PROCESS_TABLE, LNM$JOB, LNM$GROUP, LNM$SYSTEM
 *
 * Also accepts the short forms: PROCESS, JOB, GROUP, SYSTEM
 * and the alternate form LNM$SYSTEM_TABLE.
 */
lnm_table_t *lnm_find_table(lnm_manager_t *mgr, const char *table_name)
{
    if (!mgr || !table_name)
        return NULL;

    if (strcasecmp(table_name, LNM_PROCESS_TABLE) == 0 ||
        strcasecmp(table_name, "PROCESS") == 0)
        return mgr->process_table;

    if (strcasecmp(table_name, LNM_JOB_TABLE) == 0 ||
        strcasecmp(table_name, "JOB") == 0)
        return mgr->job_table;

    if (strcasecmp(table_name, LNM_GROUP_TABLE) == 0 ||
        strcasecmp(table_name, "GROUP") == 0)
        return mgr->group_table;

    if (strcasecmp(table_name, LNM_SYSTEM_TABLE) == 0 ||
        strcasecmp(table_name, "LNM$SYSTEM_TABLE") == 0 ||
        strcasecmp(table_name, "SYSTEM") == 0)
        return mgr->system_table;

    return NULL;
}

/*
 * lnm_init - Create a new logical name manager with 4 standard tables.
 *
 * The table hierarchy is:
 *   process -> job -> group -> system (parent chain)
 *
 * Returns a new manager, or NULL on failure.
 */
lnm_manager_t *lnm_init(void)
{
    lnm_manager_t *mgr = calloc(1, sizeof(lnm_manager_t));
    if (!mgr)
        return NULL;

    mgr->system_table  = lnm_table_create(LNM_SYSTEM_TABLE,  LNM_DEFAULT_BUCKETS);
    mgr->group_table   = lnm_table_create(LNM_GROUP_TABLE,   LNM_DEFAULT_BUCKETS);
    mgr->job_table     = lnm_table_create(LNM_JOB_TABLE,     LNM_DEFAULT_BUCKETS);
    mgr->process_table = lnm_table_create(LNM_PROCESS_TABLE, LNM_DEFAULT_BUCKETS);

    if (!mgr->system_table || !mgr->group_table ||
        !mgr->job_table || !mgr->process_table) {
        lnm_shutdown(mgr);
        return NULL;
    }

    /* Set up the parent chain for search ordering */
    mgr->process_table->parent = mgr->job_table;
    mgr->job_table->parent     = mgr->group_table;
    mgr->group_table->parent   = mgr->system_table;
    mgr->system_table->parent  = NULL;

    return mgr;
}

/*
 * lnm_shutdown - Destroy all tables and free the manager.
 */
void lnm_shutdown(lnm_manager_t *mgr)
{
    if (!mgr)
        return;

    lnm_table_destroy(mgr->process_table);
    lnm_table_destroy(mgr->job_table);
    lnm_table_destroy(mgr->group_table);
    lnm_table_destroy(mgr->system_table);

    if (g_manager == mgr) {
        g_manager = NULL;
        /* Reset so lnm_get_manager() can re-initialize if called again */
        g_manager_once = PTHREAD_ONCE_INIT;
    }

    free(mgr);
}

/*
 * lnm_init_once - pthread_once callback to initialize the global manager.
 */
static void lnm_init_once(void)
{
    g_manager = lnm_init();
}

/*
 * lnm_get_manager - Return the global/default manager instance.
 *
 * Initializes on first call using pthread_once for thread safety.
 * Concurrent first calls will not double-init.
 * Returns NULL if initialization fails.
 */
lnm_manager_t *lnm_get_manager(void)
{
    pthread_once(&g_manager_once, lnm_init_once);
    return g_manager;
}

/*
 * lnm_create - Create a single-valued logical name.
 *
 * @mgr:          Manager instance
 * @table_name:   Target table
 * @logical_name: Name to create
 * @equivalence:  Equivalence string
 * @attributes:   Logical name attributes (LNM_ATTR_*)
 * @acmode:       Access mode (LNM_MODE_*)
 *
 * Returns SS$_NORMAL on success, SS$_SUPERSEDE if an existing name
 * was replaced, or an error code.
 */
uint32_t lnm_create(lnm_manager_t *mgr, const char *table_name,
                     const char *logical_name, const char *equivalence,
                     uint32_t attributes, uint8_t acmode)
{
    if (!mgr)
        return SS$_BADPARAM;

    uint32_t status = validate_logical_name(logical_name);
    if (!$VMS_STATUS_SUCCESS(status))
        return status;

    if (!equivalence)
        return SS$_BADPARAM;

    /*
     * LNM$SYSTEM is executive-resident (see is_system_table): the storage
     * lives in vms.ko, not in this process, so a DEFINE/SYSTEM here is
     * visible system-wide. NO FALLBACK (vms-48ab, CLAUDE.md Rule 9 / INV-6):
     * on SS$_NOSUCHDEV the executive is unreachable -- at OVMX runtime that
     * cannot happen (PID 1 pins it), so it means host BUILD/TEST tooling with
     * no /dev/vms. There USED to be a process-local SYSTEM table fallback
     * here so the ctest suite that seeds SYS$SYSDEVICE etc. could still run;
     * that was itself the silent-per-process-fake shape Rule 9 forbids, and
     * it is gone. The host tests that depended on it were migrated to
     * tests/qemu/test_syssvc_lnm_system.c, which proves this path against a
     * real /dev/vms (docs/design-lnm-executive-surface.md).
     */
    if (is_system_table(table_name)) {
        const char *vals[1] = { equivalence };
        return vms_kif_lnm_define(VMS_LNM_TBL_SYSTEM, logical_name,
                                  vals, 1, attributes, acmode);
    }

    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;

    lnm_entry_t *entry = make_entry(logical_name, equivalence, attributes, acmode);
    if (!entry)
        return SS$_INSFMEM;

    return lnm_table_insert(table, entry);
}

/*
 * lnm_create_multi - Create a multi-valued logical name.
 *
 * @mgr:          Manager instance
 * @table_name:   Target table
 * @logical_name: Name to create
 * @equivalences: Array of equivalence strings
 * @num_equiv:    Number of equivalence strings (1..LNM_MAX_INDEX)
 * @attributes:   Logical name attributes
 * @acmode:       Access mode
 *
 * Returns SS$_NORMAL or SS$_SUPERSEDE on success, or an error code.
 */
uint32_t lnm_create_multi(lnm_manager_t *mgr, const char *table_name,
                           const char *logical_name,
                           const char **equivalences, int num_equiv,
                           uint32_t attributes, uint8_t acmode)
{
    if (!mgr)
        return SS$_BADPARAM;

    uint32_t status = validate_logical_name(logical_name);
    if (!$VMS_STATUS_SUCCESS(status))
        return status;

    if (!equivalences || num_equiv <= 0)
        return SS$_BADPARAM;

    if (num_equiv > LNM_MAX_INDEX)
        num_equiv = LNM_MAX_INDEX;

    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;

    /* Allocate the entry (calloc zeroes the translation array) */
    lnm_entry_t *entry = calloc(1, sizeof(lnm_entry_t));
    if (!entry)
        return SS$_INSFMEM;

    /* Store name in uppercase */
    size_t i;
    for (i = 0; i < LNM_MAX_NAME && logical_name[i]; i++)
        entry->name[i] = (char)toupper((unsigned char)logical_name[i]);
    entry->name[i] = '\0';
    entry->name_length = (uint16_t)i;

    entry->attributes = attributes;
    entry->acmode = acmode;
    entry->next = NULL;

    /* Populate each equivalence string */
    entry->num_translations = (uint8_t)num_equiv;
    for (int idx = 0; idx < num_equiv; idx++) {
        if (!equivalences[idx])
            continue;
        size_t elen = strlen(equivalences[idx]);
        if (elen > LNM_MAX_VALUE)
            elen = LNM_MAX_VALUE;
        memcpy(entry->translations[idx].value, equivalences[idx], elen);
        entry->translations[idx].value[elen] = '\0';
        entry->translations[idx].length = (uint16_t)elen;
        entry->translations[idx].index = (uint8_t)idx;
    }

    return lnm_table_insert(table, entry);
}

/*
 * lnm_delete - Delete a logical name from a table.
 *
 * @mgr:          Manager instance
 * @table_name:   Table containing the name
 * @logical_name: Name to delete
 * @acmode:       Access mode of the caller (must be >= entry's acmode)
 *
 * Returns SS$_NORMAL on success, SS$_NOLOGNAM if not found.
 */
uint32_t lnm_delete(lnm_manager_t *mgr, const char *table_name,
                     const char *logical_name, uint8_t acmode)
{
    if (!mgr)
        return SS$_BADPARAM;

    uint32_t status = validate_logical_name(logical_name);
    if (!$VMS_STATUS_SUCCESS(status))
        return status;

    /* LNM$SYSTEM is executive-resident: delete through vms_kif. NO FALLBACK
     * (vms-48ab, INV-6): SS$_NOSUCHDEV means the executive is unreachable
     * (host tooling only) and is returned honestly, never masked by a
     * process-local delete (see is_system_table / lnm_create). */
    if (is_system_table(table_name))
        return vms_kif_lnm_delete(VMS_LNM_TBL_SYSTEM, logical_name, acmode);

    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;

    /* Check access mode: caller's mode must be <= entry's mode (more privileged) */
    lnm_entry_t *entry = lnm_table_lookup(table, logical_name);
    if (!entry)
        return SS$_NOLOGNAM;

    if (acmode > entry->acmode)
        return SS$_NOPRIV;

    return lnm_table_remove(table, logical_name);
}

/*
 * lnm_enumerate - Walk all entries in a table via callback.
 *
 * @mgr:        Manager instance
 * @table_name: Table to enumerate
 * @callback:   Function called for each entry
 * @ctx:        Opaque context passed to callback
 *
 * Returns SS$_NORMAL on success, SS$_NOLOGTAB if table not found.
 */
uint32_t lnm_enumerate(lnm_manager_t *mgr, const char *table_name,
                        lnm_enum_callback_t callback, void *ctx)
{
    if (!mgr || !callback)
        return SS$_BADPARAM;

    lnm_table_t *table = lnm_find_table(mgr, table_name);
    if (!table)
        return SS$_NOLOGTAB;

    return lnm_table_enumerate(table, callback, ctx);
}
