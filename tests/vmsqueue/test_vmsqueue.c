/*
 * test_vmsqueue.c - Unit tests for VMS Queue Manager Library
 *
 * Tests: create queue, submit jobs, show entries, delete entry,
 *        hold/release, persistence across close/reopen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ssdef.h>
#include "vmsqueue.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%d]: %s\n", tests_run, msg); \
        return 1; \
    } \
    tests_passed++; \
    printf("PASS [%d]: %s\n", tests_run, msg); \
} while (0)

static const char *test_db_path = "/tmp/test_qman_master.dat";

static int test_create_queue(void)
{
    int status;

    status = vmsq_init(test_db_path);
    ASSERT(status == SS$_NORMAL, "vmsq_init succeeds");

    status = vmsq_create_queue("SYS$BATCH", VMSQ_TYPE_BATCH);
    ASSERT(status == SS$_NORMAL, "create SYS$BATCH queue");

    /* Duplicate should fail */
    status = vmsq_create_queue("SYS$BATCH", VMSQ_TYPE_BATCH);
    ASSERT(status == SS$_DUPLNAM, "duplicate queue name rejected");

    /* Show the queue */
    struct vms_queue info;
    status = vmsq_show_queue("SYS$BATCH", &info);
    ASSERT(status == SS$_NORMAL, "show SYS$BATCH succeeds");
    ASSERT(info.type == VMSQ_TYPE_BATCH, "queue type is BATCH");
    ASSERT(info.status == VMSQ_STATUS_STARTED, "queue status is STARTED");
    ASSERT(info.entry_count == 0, "queue has 0 entries initially");

    return 0;
}

static int test_submit_jobs(void)
{
    int status;
    uint32_t eid1, eid2, eid3;

    status = vmsq_submit("SYS$BATCH", "LOGIN.COM", "SYSTEM", &eid1);
    ASSERT(status == SS$_NORMAL, "submit job 1 (LOGIN.COM)");

    status = vmsq_submit("SYS$BATCH", "BACKUP.COM", "SYSTEM", &eid2);
    ASSERT(status == SS$_NORMAL, "submit job 2 (BACKUP.COM)");

    status = vmsq_submit("SYS$BATCH", "CLEANUP.COM", "OPERATOR", &eid3);
    ASSERT(status == SS$_NORMAL, "submit job 3 (CLEANUP.COM)");

    ASSERT(eid1 < eid2 && eid2 < eid3, "entry IDs are sequential");

    /* Verify 3 entries */
    struct vms_queue_entry entries[10];
    int count = 0;
    status = vmsq_show_entries("SYS$BATCH", entries, 10, &count);
    ASSERT(status == SS$_NORMAL, "show_entries succeeds");
    ASSERT(count == 3, "3 entries in SYS$BATCH");

    /* Verify queue entry count */
    struct vms_queue qinfo;
    status = vmsq_show_queue("SYS$BATCH", &qinfo);
    ASSERT(status == SS$_NORMAL, "show_queue after submits");
    ASSERT(qinfo.entry_count == 3, "queue reports 3 entries");

    /* Verify individual entry */
    struct vms_queue_entry entry;
    status = vmsq_show_entry(eid1, &entry);
    ASSERT(status == SS$_NORMAL, "show_entry for job 1");
    ASSERT(strcmp(entry.job_name, "LOGIN.COM") == 0, "job 1 name is LOGIN.COM");
    ASSERT(strcmp(entry.username, "SYSTEM") == 0, "job 1 user is SYSTEM");
    ASSERT(entry.status == VMSQ_ENTRY_PENDING, "job 1 status is PENDING");
    ASSERT(entry.submit_time > 0, "job 1 has non-zero submit time");

    return 0;
}

static int test_delete_entry(void)
{
    int status;

    /* Get entries to find one to delete */
    struct vms_queue_entry entries[10];
    int count = 0;
    status = vmsq_show_entries("SYS$BATCH", entries, 10, &count);
    ASSERT(status == SS$_NORMAL, "show_entries before delete");
    ASSERT(count == 3, "still 3 entries before delete");

    /* Delete the second entry */
    uint32_t del_id = entries[1].entry_id;
    status = vmsq_delete_entry(del_id);
    ASSERT(status == SS$_NORMAL, "delete entry succeeds");

    /* Verify 2 entries remain */
    status = vmsq_show_entries("SYS$BATCH", entries, 10, &count);
    ASSERT(status == SS$_NORMAL, "show_entries after delete");
    ASSERT(count == 2, "2 entries remain after delete");

    /* Deleted entry should not be found */
    struct vms_queue_entry entry;
    status = vmsq_show_entry(del_id, &entry);
    ASSERT(status == SS$_ITEMNOTFOUND, "deleted entry not found");

    /* Queue count should reflect deletion */
    struct vms_queue qinfo;
    status = vmsq_show_queue("SYS$BATCH", &qinfo);
    ASSERT(status == SS$_NORMAL, "show_queue after delete");
    ASSERT(qinfo.entry_count == 2, "queue reports 2 entries after delete");

    return 0;
}

static int test_hold_release(void)
{
    int status;

    /* Get remaining entries */
    struct vms_queue_entry entries[10];
    int count = 0;
    status = vmsq_show_entries("SYS$BATCH", entries, 10, &count);
    ASSERT(status == SS$_NORMAL, "show_entries for hold test");
    ASSERT(count >= 1, "at least 1 entry for hold test");

    uint32_t hold_id = entries[0].entry_id;

    /* Hold the entry */
    status = vmsq_hold_entry(hold_id);
    ASSERT(status == SS$_NORMAL, "hold_entry succeeds");

    struct vms_queue_entry entry;
    status = vmsq_show_entry(hold_id, &entry);
    ASSERT(status == SS$_NORMAL, "show held entry");
    ASSERT(entry.status == VMSQ_ENTRY_HOLDING, "entry status is HOLDING");

    /* Release the entry */
    status = vmsq_release_entry(hold_id);
    ASSERT(status == SS$_NORMAL, "release_entry succeeds");

    status = vmsq_show_entry(hold_id, &entry);
    ASSERT(status == SS$_NORMAL, "show released entry");
    ASSERT(entry.status == VMSQ_ENTRY_PENDING, "entry status back to PENDING");

    return 0;
}

static int test_persistence(void)
{
    int status;

    /* Record current state */
    struct vms_queue_entry entries_before[10];
    int count_before = 0;
    status = vmsq_show_entries("SYS$BATCH", entries_before, 10, &count_before);
    ASSERT(status == SS$_NORMAL, "show_entries before close");

    /* Close and reopen */
    vmsq_close();

    status = vmsq_init(test_db_path);
    ASSERT(status == SS$_NORMAL, "vmsq_init after reopen");

    /* Verify queue still exists */
    struct vms_queue qinfo;
    status = vmsq_show_queue("SYS$BATCH", &qinfo);
    ASSERT(status == SS$_NORMAL, "queue survives close/reopen");
    ASSERT(qinfo.type == VMSQ_TYPE_BATCH, "queue type preserved");

    /* Verify entries survived */
    struct vms_queue_entry entries_after[10];
    int count_after = 0;
    status = vmsq_show_entries("SYS$BATCH", entries_after, 10, &count_after);
    ASSERT(status == SS$_NORMAL, "show_entries after reopen");
    ASSERT(count_after == count_before, "entry count preserved across reopen");

    /* Verify entry data integrity */
    for (int i = 0; i < count_after; i++) {
        ASSERT(entries_after[i].entry_id == entries_before[i].entry_id,
               "entry ID preserved");
        ASSERT(strcmp(entries_after[i].job_name, entries_before[i].job_name) == 0,
               "job name preserved");
    }

    return 0;
}

static int test_delete_queue(void)
{
    int status;

    /* Delete non-existent queue */
    status = vmsq_delete_queue("NOSUCHQUEUE");
    ASSERT(status == SS$_ITEMNOTFOUND, "delete non-existent queue fails");

    /* Create and delete a queue */
    status = vmsq_create_queue("SYS$PRINT", VMSQ_TYPE_PRINT);
    ASSERT(status == SS$_NORMAL, "create SYS$PRINT");

    status = vmsq_delete_queue("SYS$PRINT");
    ASSERT(status == SS$_NORMAL, "delete SYS$PRINT");

    struct vms_queue qinfo;
    status = vmsq_show_queue("SYS$PRINT", &qinfo);
    ASSERT(status == SS$_ITEMNOTFOUND, "deleted queue not found");

    return 0;
}

int main(void)
{
    int result = 0;

    /* Clean up any leftover test file */
    unlink(test_db_path);

    printf("=== VMS Queue Manager Unit Tests ===\n\n");

    if (test_create_queue() != 0) result = 1;
    if (test_submit_jobs() != 0) result = 1;
    if (test_delete_entry() != 0) result = 1;
    if (test_hold_release() != 0) result = 1;
    if (test_persistence() != 0) result = 1;
    if (test_delete_queue() != 0) result = 1;

    /* Cleanup */
    vmsq_close();
    unlink(test_db_path);

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return result;
}
