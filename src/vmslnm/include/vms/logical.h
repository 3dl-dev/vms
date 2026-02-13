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
uint32_t lnm_translate_iterative(lnm_manager_t *mgr, const char *table_name,
                                  const char *logical_name, char *result,
                                  size_t result_size, uint16_t *result_length);

/* Enumeration callback */
typedef int (*lnm_enum_callback_t)(const char *name, const lnm_entry_t *entry, void *ctx);
uint32_t lnm_enumerate(lnm_manager_t *mgr, const char *table_name,
                        lnm_enum_callback_t callback, void *ctx);

/* Setup default system logicals */
void lnm_setup_defaults(lnm_manager_t *mgr, const char *vms_root);

#endif /* __VMS_LOGICAL_H */
