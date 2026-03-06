/*
 * vmsqueue.h - VMS Queue Manager Library
 *
 * Provides VMS-compatible batch and print queue management.
 * Data is persisted in QMAN$MASTER.DAT using fixed-size binary records
 * with file locking (flock) for concurrent access.
 *
 * All functions return VMS status codes (odd = success, even = error).
 */

#ifndef VMSQUEUE_H
#define VMSQUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queue types */
#define VMSQ_TYPE_BATCH     1
#define VMSQ_TYPE_PRINT     2
#define VMSQ_TYPE_GENERIC   3

/* Queue status */
#define VMSQ_STATUS_STARTED 1
#define VMSQ_STATUS_STOPPED 2
#define VMSQ_STATUS_PAUSED  3

/* Entry status */
#define VMSQ_ENTRY_PENDING      1
#define VMSQ_ENTRY_EXECUTING    2
#define VMSQ_ENTRY_HOLDING      3
#define VMSQ_ENTRY_COMPLETED    4

/* Maximum limits */
#define VMSQ_MAX_QUEUES     64
#define VMSQ_MAX_ENTRIES    1024

/* File format magic and version */
#define VMSQ_MAGIC          0x4E414D51  /* "QMAN" */
#define VMSQ_VERSION        1

/*
 * Queue descriptor — stored in QMAN$MASTER.DAT
 */
struct vms_queue {
    char        name[32];           /* SYS$BATCH, SYS$PRINT, user-defined */
    uint8_t     type;               /* VMSQ_TYPE_BATCH, PRINT, GENERIC */
    uint8_t     status;             /* VMSQ_STATUS_STARTED, STOPPED, PAUSED */
    uint8_t     _pad[2];
    uint32_t    entry_count;        /* Number of active entries */
    uint32_t    next_entry_id;      /* Next entry ID to assign */
};

/*
 * Queue entry — a submitted job
 */
struct vms_queue_entry {
    uint32_t    entry_id;           /* Sequential entry number */
    char        queue_name[32];     /* Queue this entry belongs to */
    char        job_name[256];      /* Job/file name */
    char        username[64];       /* Submitting user */
    uint8_t     status;             /* VMSQ_ENTRY_PENDING, etc. */
    uint8_t     priority;           /* Job priority (0-255) */
    uint8_t     _pad[2];
    uint64_t    submit_time;        /* VMS binary time (100ns since Nov 17 1858) */
    uint64_t    start_time;         /* VMS binary time */
};

/*
 * Initialize the queue manager, opening or creating the database file.
 * @param db_path  Path to QMAN$MASTER.DAT (NULL for default)
 * @return SS$_NORMAL on success
 */
int vmsq_init(const char *db_path);

/*
 * Create a new queue.
 * @param name  Queue name (e.g. "SYS$BATCH")
 * @param type  VMSQ_TYPE_BATCH, VMSQ_TYPE_PRINT, or VMSQ_TYPE_GENERIC
 * @return SS$_NORMAL on success, SS$_DUPLNAM if exists
 */
int vmsq_create_queue(const char *name, int type);

/*
 * Delete a queue (must have no active entries).
 * @param name  Queue name
 * @return SS$_NORMAL on success, SS$_ITEMNOTFOUND if not found
 */
int vmsq_delete_queue(const char *name);

/*
 * Submit a job to a queue.
 * @param queue     Queue name
 * @param job       Job/file name
 * @param user      Submitting username
 * @param entry_id  [out] Assigned entry ID
 * @return SS$_NORMAL on success
 */
int vmsq_submit(const char *queue, const char *job, const char *user,
                uint32_t *entry_id);

/*
 * Get queue information.
 * @param queue  Queue name
 * @param info   [out] Queue descriptor
 * @return SS$_NORMAL on success, SS$_ITEMNOTFOUND if not found
 */
int vmsq_show_queue(const char *queue, struct vms_queue *info);

/*
 * List entries in a queue.
 * @param queue    Queue name
 * @param entries  [out] Array of entries
 * @param max      Maximum entries to return
 * @param count    [out] Actual count returned
 * @return SS$_NORMAL on success
 */
int vmsq_show_entries(const char *queue, struct vms_queue_entry *entries,
                      int max, int *count);

/*
 * Get a single entry by ID.
 * @param entry_id  Entry ID
 * @param entry     [out] Entry descriptor
 * @return SS$_NORMAL on success, SS$_ITEMNOTFOUND if not found
 */
int vmsq_show_entry(uint32_t entry_id, struct vms_queue_entry *entry);

/*
 * Delete (cancel) an entry.
 * @param entry_id  Entry ID
 * @return SS$_NORMAL on success, SS$_ITEMNOTFOUND if not found
 */
int vmsq_delete_entry(uint32_t entry_id);

/*
 * Place an entry on hold.
 * @param entry_id  Entry ID
 * @return SS$_NORMAL on success
 */
int vmsq_hold_entry(uint32_t entry_id);

/*
 * Release a held entry (set back to PENDING).
 * @param entry_id  Entry ID
 * @return SS$_NORMAL on success
 */
int vmsq_release_entry(uint32_t entry_id);

/*
 * Update entry status directly.
 * @param entry_id    Entry ID
 * @param new_status  New status value
 * @return SS$_NORMAL on success
 */
int vmsq_update_status(uint32_t entry_id, uint8_t new_status);

/*
 * Set queue status (start/stop/pause).
 * @param name        Queue name
 * @param new_status  VMSQ_STATUS_STARTED, STOPPED, or PAUSED
 * @return SS$_NORMAL on success, SS$_ITEMNOTFOUND if not found
 */
int vmsq_set_queue_status(const char *name, uint8_t new_status);

/*
 * Close the queue manager, flushing and closing the database file.
 */
void vmsq_close(void);

#ifdef __cplusplus
}
#endif

#endif /* VMSQUEUE_H */
