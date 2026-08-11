#ifndef __VMS_LOGICAL_H
#define __VMS_LOGICAL_H

#include <stdint.h>
#include <stddef.h>

/* Table names */
#define LNM_PROCESS_TABLE "LNM$PROCESS_TABLE"
#define LNM_JOB_TABLE     "LNM$JOB"
#define LNM_GROUP_TABLE   "LNM$GROUP"
#define LNM_SYSTEM_TABLE  "LNM$SYSTEM"

/* Search list for default translation */
#define LNM_FILE_DEV     "LNM$FILE_DEV"
#define LNM_DCL_LOGICAL  "LNM$DCL_LOGICAL"

/* Max values */
#define LNM_MAX_NAME     255
#define LNM_MAX_VALUE    255
#define LNM_MAX_DEPTH    10
#define LNM_MAX_INDEX    128

/*
 * LNM_MAX_SEARCHLIST - the number of equivalence strings lnm_translate_values()
 * will return for one name (vms-420). Matches VMS_LNM_MAX_EQUIV
 * (src/kernel/vms_lnm.h), the executive arena's per-entry cap for SYSTEM/
 * GROUP/JOB -- named independently here rather than including vms_kif.h so
 * this header stays usable without pulling in the /dev/vms client. A
 * LNM$PROCESS entry can locally hold more (up to LNM_MAX_INDEX, via
 * lnm_create_multi), but no OVMX search list in practice approaches either
 * cap, so capping display/enumeration at the executive's number keeps the
 * local and executive-resident tables' search-list shape consistent.
 */
#define LNM_MAX_SEARCHLIST 8

/* Attributes */
#define LNM_ATTR_CONCEALED  0x01
#define LNM_ATTR_TERMINAL   0x02
#define LNM_ATTR_CONFINE    0x04
#define LNM_ATTR_NO_ALIAS   0x08
#define LNM_ATTR_CRELOG     0x10
#define LNM_ATTR_TABLE      0x20

/* Access modes */
#define LNM_MODE_KERNEL     0
#define LNM_MODE_EXEC       1
#define LNM_MODE_SUPER      2
#define LNM_MODE_USER       3

/* A single logical name translation (can have multiple equivalence strings) */
typedef struct lnm_translation {
    char value[LNM_MAX_VALUE + 1];
    uint16_t length;
    uint8_t index;       /* equivalence name index (0-127) */
} lnm_translation_t;

/* A logical name entry */
typedef struct lnm_entry {
    char name[LNM_MAX_NAME + 1];
    uint16_t name_length;
    uint32_t attributes;
    uint8_t acmode;          /* access mode */
    uint8_t num_translations;
    lnm_translation_t translations[LNM_MAX_INDEX];
    struct lnm_entry *next;  /* hash chain */
} lnm_entry_t;

/* A logical name table */
typedef struct lnm_table {
    char name[32];
    lnm_entry_t **buckets;   /* hash table */
    uint32_t num_buckets;
    uint32_t count;
    struct lnm_table *parent; /* parent table for search */
} lnm_table_t;

/* Logical name manager (holds all tables) */
typedef struct lnm_manager {
    lnm_table_t *process_table;
    lnm_table_t *job_table;
    lnm_table_t *group_table;
    lnm_table_t *system_table;
} lnm_manager_t;

/* Manager lifecycle */
lnm_manager_t *lnm_init(void);
void lnm_shutdown(lnm_manager_t *mgr);

/* Get the global/default manager instance */
lnm_manager_t *lnm_get_manager(void);

/* Table operations */
lnm_table_t *lnm_find_table(lnm_manager_t *mgr, const char *table_name);

/* Core operations */
uint32_t lnm_create(lnm_manager_t *mgr, const char *table_name,
                     const char *logical_name, const char *equivalence,
                     uint32_t attributes, uint8_t acmode);
uint32_t lnm_create_multi(lnm_manager_t *mgr, const char *table_name,
                           const char *logical_name,
                           const char **equivalences, int num_equiv,
                           uint32_t attributes, uint8_t acmode);
uint32_t lnm_delete(lnm_manager_t *mgr, const char *table_name,
                     const char *logical_name, uint8_t acmode);
uint32_t lnm_translate(lnm_manager_t *mgr, const char *table_name,
                        const char *logical_name, char *result, size_t result_size,
                        uint16_t *result_length, uint32_t *attributes);
/*
 * lnm_translate_filespec - the ONE filespec-aware iterative translation driver
 * (vms-240). Translates the DEVICE field of `filespec` through `table_name`
 * (normally LNM$FILE_DEV), honoring LNM_ATTR_TERMINAL (stop at a terminal
 * translation) and LNM_ATTR_CONCEALED (keep the concealed device name), and
 * re-attaches the preserved directory/name/type tail. `attributes`, if
 * non-NULL, receives the chain-ending translation's attributes. Supersedes the
 * removed, filespec-blind iterative translator. See lnm_translate.c for the
 * full contract and the VSI OpenVMS doc citations.
 */
uint32_t lnm_translate_filespec(lnm_manager_t *mgr, const char *table_name,
                                const char *filespec, char *result,
                                size_t result_size, uint16_t *result_length,
                                uint32_t *attributes);

/*
 * lnm_translate_values - fetch ALL equivalence strings (search-list order)
 * for `logical_name` in a SPECIFIC table_name (LNM$PROCESS_TABLE, LNM$JOB,
 * LNM$GROUP or LNM$SYSTEM -- NOT the LNM$FILE_DEV search-list alias). Fills
 * up to `max_values` entries into `values[]` (each NUL-terminated) and sets
 * *num_values to the entry's true count, capped at `max_values` (see
 * LNM_MAX_SEARCHLIST). *attributes, if non-NULL, receives the entry's
 * attributes (vms-420: the multi-value counterpart of lnm_translate(), which
 * only ever returns equivalence index 0).
 *
 * Returns SS$_NORMAL on success, SS$_NOLOGNAM if the name is not present in
 * this table, SS$_NOLOGTAB if table_name names no table, or SS$_NOSUCHDEV
 * for SYSTEM/GROUP/JOB when the executive is unavailable (CLAUDE.md Rule 9 /
 * INV-6 -- no per-process fallback).
 */
uint32_t lnm_translate_values(lnm_manager_t *mgr, const char *table_name,
                               const char *logical_name,
                               char values[][LNM_MAX_VALUE + 1],
                               uint8_t max_values, uint8_t *num_values,
                               uint32_t *attributes);

/*
 * lnm_translate_searchlist - fetch ALL equivalence strings for `logical_name`
 * using the LNM$FILE_DEV search order (LNM$PROCESS_TABLE, then the
 * executive-resident LNM$JOB, LNM$GROUP, LNM$SYSTEM). The FIRST table that
 * contains the name supplies EVERY one of its equivalence strings, in order
 * -- this is the multi-value counterpart of lnm_translate()'s search-list
 * branch (which returns only that first table's index-0 value). It is what a
 * filespec's device-position search list resolves through: RMS $OPEN/$SEARCH
 * try each returned equivalence in turn until one names an existing file
 * (VMS I/O User's Reference Manual, "Logical Name Search Lists"; DCL
 * Dictionary, DEFINE).
 *
 * Fills up to `max_values` entries into `values[]` (each NUL-terminated) and
 * sets *num_values to the winning table's count, capped at `max_values`.
 * *attributes, if non-NULL, receives that entry's attributes.
 *
 * Returns SS$_NORMAL if the name resolves in some table, SS$_NOLOGNAM if it is
 * absent from all of them, or SS$_NOSUCHDEV if an executive table must be read
 * but /dev/vms is unavailable (CLAUDE.md Rule 9 / INV-6 -- no per-process
 * fallback). Unlike lnm_translate_values(), this accepts NO table argument:
 * the search order is the whole point.
 */
uint32_t lnm_translate_searchlist(lnm_manager_t *mgr, const char *logical_name,
                                  char values[][LNM_MAX_VALUE + 1],
                                  uint8_t max_values, uint8_t *num_values,
                                  uint32_t *attributes);

/* Enumeration callback */
typedef int (*lnm_enum_callback_t)(const char *name, const lnm_entry_t *entry, void *ctx);
uint32_t lnm_enumerate(lnm_manager_t *mgr, const char *table_name,
                        lnm_enum_callback_t callback, void *ctx);

/* Setup default system logicals */
void lnm_setup_defaults(lnm_manager_t *mgr, const char *vms_root);

#endif /* __VMS_LOGICAL_H */
