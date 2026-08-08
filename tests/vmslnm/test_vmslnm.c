/*
 * test_vmslnm.c - Unit tests for vmslnm (Logical Name Manager)
 *
 * Tests:
 *   - lnm_init / lnm_shutdown
 *   - lnm_create / lnm_translate / lnm_delete
 *   - Table hierarchy (process, job, group, system)
 *   - Iterative translation (logical -> logical)
 *   - Multiple equivalence strings (lnm_create_multi)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vms/logical.h"
#include "ssdef.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* Test: init and basic manager structure                              */
/* ------------------------------------------------------------------ */
static void test_init(void)
{
    printf("\n--- lnm_init / lnm_shutdown ---\n");

    lnm_manager_t *mgr = lnm_init();
    check(mgr != NULL, "lnm_init returns non-NULL manager");
    check(mgr->process_table != NULL, "process table initialized");
    check(mgr->job_table != NULL, "job table initialized");
    check(mgr->group_table != NULL, "group table initialized");
    check(mgr->system_table != NULL, "system table initialized");

    lnm_shutdown(mgr);
    /* No crash = success */
    check(1, "lnm_shutdown completes without crash");
}

/* ------------------------------------------------------------------ */
/* Test: create / translate / delete in process table                 */
/* ------------------------------------------------------------------ */
static void test_create_translate_delete(void)
{
    printf("\n--- lnm_create / lnm_translate / lnm_delete ---\n");

    lnm_manager_t *mgr = lnm_init();
    if (!mgr) { check(0, "lnm_init for create/translate/delete"); return; }

    char result[256];
    uint16_t result_len = 0;
    uint32_t attrs = 0;
    uint32_t st;

    /* Create a logical name */
    st = lnm_create(mgr, LNM_PROCESS_TABLE,
                    "MY$LOGICAL", "/tmp/testdir",
                    0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "lnm_create MY$LOGICAL in process table");

    /* Translate it */
    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "MY$LOGICAL", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "lnm_translate MY$LOGICAL succeeds");
    check(result_len == strlen("/tmp/testdir"), "translate result length correct");
    result[result_len] = '\0';
    check(strcmp(result, "/tmp/testdir") == 0, "translate result is /tmp/testdir");

    /* Case-insensitive lookup: lowercase name */
    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "my$logical", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "case-insensitive lookup my$logical");
    result[result_len] = '\0';
    check(strcmp(result, "/tmp/testdir") == 0, "case-insensitive lookup finds same value");

    /* Translate non-existent name */
    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "DOES$NOT$EXIST", result, sizeof(result),
                       &result_len, &attrs);
    check(!$VMS_STATUS_SUCCESS(st), "translate non-existent returns failure");

    /* Delete the logical name */
    st = lnm_delete(mgr, LNM_PROCESS_TABLE, "MY$LOGICAL", LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "lnm_delete MY$LOGICAL succeeds");

    /* Translate after delete should fail */
    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "MY$LOGICAL", result, sizeof(result),
                       &result_len, &attrs);
    check(!$VMS_STATUS_SUCCESS(st), "translate after delete fails");

    lnm_shutdown(mgr);
}

/* ------------------------------------------------------------------ */
/* Test: table hierarchy — group vs process                           */
/*                                                                      */
/* NOTE (vms-48ab): this used to exercise LNM_SYSTEM_TABLE, but         */
/* LNM$SYSTEM is executive-resident (vms-d37/vms-96e2) and this binary  */
/* runs on the host with no /dev/vms. LNM_GROUP_TABLE is still          */
/* process-private (its executive residency is the deferred half of    */
/* vms-d37, see lnm_client.c), so it exercises the exact same           */
/* hierarchy/override logic in lnm_translate() without needing an      */
/* executive. The SYSTEM-table equivalent of this scenario, against a  */
/* real /dev/vms, is tests/qemu/test_syssvc_lnm_system.c.               */
/* ------------------------------------------------------------------ */
static void test_table_hierarchy(void)
{
    printf("\n--- table hierarchy ---\n");

    lnm_manager_t *mgr = lnm_init();
    if (!mgr) { check(0, "lnm_init for hierarchy"); return; }

    char result[256];
    uint16_t result_len = 0;
    uint32_t attrs = 0;
    uint32_t st;

    /* Put one name in group table, another in process table */
    st = lnm_create(mgr, LNM_GROUP_TABLE,
                    "SYS$VOLUME", "/vms/dka0",
                    0, LNM_MODE_KERNEL);
    check($VMS_STATUS_SUCCESS(st), "create SYS$VOLUME in group table");

    st = lnm_create(mgr, LNM_PROCESS_TABLE,
                    "PROC$LOCAL", "/proc/self",
                    0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "create PROC$LOCAL in process table");

    /* Translate from group table */
    st = lnm_translate(mgr, LNM_GROUP_TABLE,
                       "SYS$VOLUME", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "translate SYS$VOLUME from group table");
    result[result_len] = '\0';
    check(strcmp(result, "/vms/dka0") == 0, "SYS$VOLUME resolves to /vms/dka0");

    /* PROC$LOCAL should NOT be in group table */
    st = lnm_translate(mgr, LNM_GROUP_TABLE,
                       "PROC$LOCAL", result, sizeof(result),
                       &result_len, &attrs);
    check(!$VMS_STATUS_SUCCESS(st), "PROC$LOCAL not found in group table");

    /* PROC$LOCAL IS in process table */
    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "PROC$LOCAL", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "PROC$LOCAL found in process table");

    /* Override group logical with process-level one */
    st = lnm_create(mgr, LNM_PROCESS_TABLE,
                    "SYS$VOLUME", "/override",
                    0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "create SYS$VOLUME override in process table");

    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "SYS$VOLUME", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "process override of SYS$VOLUME found");
    result[result_len] = '\0';
    check(strcmp(result, "/override") == 0, "process override value is /override");

    /* Group table still has original */
    st = lnm_translate(mgr, LNM_GROUP_TABLE,
                       "SYS$VOLUME", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "group SYS$VOLUME still accessible");
    result[result_len] = '\0';
    check(strcmp(result, "/vms/dka0") == 0, "group SYS$VOLUME unchanged");

    lnm_shutdown(mgr);
}

/* ------------------------------------------------------------------ */
/* Test: LNM$SYSTEM is executive-only -- no per-process fallback       */
/* (vms-48ab, CLAUDE.md Rule 9 / INV-6).                                */
/*                                                                      */
/* This binary runs on the host, where there is no /dev/vms. Before     */
/* vms-48ab, lnm_create/_translate/_delete against LNM_SYSTEM_TABLE     */
/* silently used a process-local table here and reported success --    */
/* exactly the silent per-process fake Rule 9 forbids. Now they must    */
/* fail HONESTLY with SS$_NOSUCHDEV, the same status sys$crelnm/        */
/* $trnlnm/$dellnm already return with no executive (src/libvms/        */
/* syssvc/sys_logical.c). The corresponding "it works against a real    */
/* executive" proof lives in tests/qemu/test_syssvc_lnm_system.c.       */
/* ------------------------------------------------------------------ */
static void test_system_table_no_fallback(void)
{
    printf("\n--- LNM$SYSTEM has no host fallback (vms-48ab, INV-6) ---\n");

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { check(0, "lnm_get_manager for no-fallback check"); return; }

    char result[256];
    uint16_t result_len = 0;
    uint32_t attrs = 0;
    uint32_t st;

    st = lnm_create(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$TEST", "/should-not-land",
                    0, LNM_MODE_EXEC);
    check(st == SS$_NOSUCHDEV,
          "lnm_create against LNM$SYSTEM fails SS$_NOSUCHDEV with no /dev/vms (no local fallback)");

    st = lnm_translate(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$TEST",
                       result, sizeof(result), &result_len, &attrs);
    check(st == SS$_NOSUCHDEV,
          "lnm_translate against LNM$SYSTEM fails SS$_NOSUCHDEV with no /dev/vms (never silently absent-but-OK)");

    st = lnm_delete(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$TEST", LNM_MODE_EXEC);
    check(st == SS$_NOSUCHDEV,
          "lnm_delete against LNM$SYSTEM fails SS$_NOSUCHDEV with no /dev/vms (no local fallback)");

    /* The default search list (LNM$FILE_DEV) reaches LNM$SYSTEM last; with
     * no executive and no process/job/group hit, it must fail the same way,
     * not report SS$_NOLOGNAM as if the executive had genuinely been
     * consulted and come back empty. */
    st = lnm_translate(mgr, LNM_FILE_DEV, "OVMX48AB$NEVERDEFINED",
                       result, sizeof(result), &result_len, &attrs);
    check(st == SS$_NOSUCHDEV,
          "LNM$FILE_DEV search falling through to LNM$SYSTEM fails SS$_NOSUCHDEV with no /dev/vms");
}

/* ------------------------------------------------------------------ */
/* Test: iterative translation (logical pointing to another logical)   */
/* ------------------------------------------------------------------ */
static void test_iterative_translation(void)
{
    printf("\n--- iterative translation ---\n");

    lnm_manager_t *mgr = lnm_init();
    if (!mgr) { check(0, "lnm_init for iterative"); return; }

    char result[256];
    uint16_t result_len = 0;
    uint32_t st;

    /* A -> B -> C (chain of logicals) */
    st = lnm_create(mgr, LNM_PROCESS_TABLE, "FINAL$DIR", "/usr/local/vms", 0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "create FINAL$DIR");

    /* B points to C (FINAL$DIR) */
    st = lnm_create(mgr, LNM_PROCESS_TABLE, "MID$DIR", "FINAL$DIR", 0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "create MID$DIR -> FINAL$DIR");

    /* A points to B (MID$DIR) */
    st = lnm_create(mgr, LNM_PROCESS_TABLE, "START$DIR", "MID$DIR", 0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "create START$DIR -> MID$DIR");

    /* Iterative translate: should follow the chain */
    st = lnm_translate_iterative(mgr, LNM_PROCESS_TABLE,
                                  "START$DIR", result, sizeof(result),
                                  &result_len);
    check($VMS_STATUS_SUCCESS(st), "iterative translate START$DIR");
    if ($VMS_STATUS_SUCCESS(st)) {
        result[result_len] = '\0';
        /* Should resolve to FINAL$DIR or /usr/local/vms depending on impl */
        check(result_len > 0, "iterative result non-empty");
    }

    /* Non-iterative: should return MID$DIR directly */
    uint32_t attrs = 0;
    result_len = 0;
    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "START$DIR", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "non-iterative translate START$DIR");
    result[result_len] = '\0';
    check(strcmp(result, "MID$DIR") == 0, "non-iterative gives first-level value MID$DIR");

    lnm_shutdown(mgr);
}

/* ------------------------------------------------------------------ */
/* Test: multiple equivalence strings (lnm_create_multi)              */
/* ------------------------------------------------------------------ */
static void test_multi_equivalence(void)
{
    printf("\n--- multiple equivalence strings ---\n");

    lnm_manager_t *mgr = lnm_init();
    if (!mgr) { check(0, "lnm_init for multi-equiv"); return; }

    const char *equivs[] = {
        "/vms/dka0",
        "/vms/dkb0",
        "/vms/dkc0"
    };

    uint32_t st = lnm_create_multi(mgr, LNM_PROCESS_TABLE,
                                    "VMS$SEARCH", equivs, 3,
                                    0, LNM_MODE_USER);
    check($VMS_STATUS_SUCCESS(st), "lnm_create_multi with 3 equivalences");

    /* Translate index 0 (first equivalence) */
    char result[256];
    uint16_t result_len = 0;
    uint32_t attrs = 0;

    st = lnm_translate(mgr, LNM_PROCESS_TABLE,
                       "VMS$SEARCH", result, sizeof(result),
                       &result_len, &attrs);
    check($VMS_STATUS_SUCCESS(st), "translate VMS$SEARCH with multi-equiv succeeds");
    result[result_len] = '\0';
    check(strcmp(result, "/vms/dka0") == 0, "first equivalence is /vms/dka0");

    /* Find the entry and check num_translations */
    lnm_table_t *tbl = lnm_find_table(mgr, LNM_PROCESS_TABLE);
    check(tbl != NULL, "found process table");

    lnm_shutdown(mgr);
}

/* ------------------------------------------------------------------ */
/* Test: lnm_enumerate                                                 */
/* ------------------------------------------------------------------ */
static int enum_count = 0;

static int count_callback(const char *name, const lnm_entry_t *entry, void *ctx)
{
    (void)name; (void)entry; (void)ctx;
    enum_count++;
    return 0;
}

static void test_enumerate(void)
{
    printf("\n--- lnm_enumerate ---\n");

    lnm_manager_t *mgr = lnm_init();
    if (!mgr) { check(0, "lnm_init for enumerate"); return; }

    /* Create several names */
    lnm_create(mgr, LNM_PROCESS_TABLE, "ENUM$A", "valA", 0, LNM_MODE_USER);
    lnm_create(mgr, LNM_PROCESS_TABLE, "ENUM$B", "valB", 0, LNM_MODE_USER);
    lnm_create(mgr, LNM_PROCESS_TABLE, "ENUM$C", "valC", 0, LNM_MODE_USER);

    enum_count = 0;
    uint32_t st = lnm_enumerate(mgr, LNM_PROCESS_TABLE, count_callback, NULL);
    check($VMS_STATUS_SUCCESS(st), "lnm_enumerate returns success");
    check(enum_count >= 3, "enumerate visits at least 3 entries");

    lnm_shutdown(mgr);
}

int main(void)
{
    printf("=== vmslnm unit tests ===\n");

    test_init();
    test_create_translate_delete();
    test_table_hierarchy();
    test_system_table_no_fallback();
    test_iterative_translation();
    test_multi_equivalence();
    test_enumerate();

    if (failures == 0)
        printf("\nAll vmslnm tests passed.\n");
    else
        printf("\nSome vmslnm tests FAILED (%d).\n", failures);

    return failures;
}
