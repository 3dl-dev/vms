/*
 * vmsqueue.c - VMS Queue Manager Library Implementation
 *
 * Binary file format (QMAN$MASTER.DAT):
 *   [Header]                        - 16 bytes
 *   [Queue records × MAX_QUEUES]    - fixed-size array
 *   [Entry records × MAX_ENTRIES]   - fixed-size array
 *
 * File locking via flock() for concurrent access.
 * All timestamps use VMS binary time (100ns intervals since Nov 17 1858).
 */

#include "vmsqueue.h"
#include <ssdef.h>
#include <ovmx_layout.h>
#include <vmsfs/filespec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>

/* ---------- File format ---------- */

struct qman_header {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    queue_count;
    uint32_t    entry_count;
};

/* On-disk queue record: active flag + queue struct */
struct qman_queue_record {
    uint32_t            active;     /* 0 = free slot, 1 = in use */
    struct vms_queue    queue;
};

/* On-disk entry record: active flag + entry struct */
struct qman_entry_record {
    uint32_t                active; /* 0 = free slot, 1 = in use */
    struct vms_queue_entry  entry;
};

/* ---------- Module state ---------- */

static FILE *g_db_file = NULL;
static char  g_db_path[512] = {0};

/* ---------- VMS time ---------- */

/*
 * VMS epoch: November 17, 1858 00:00:00
 * Unix epoch: January 1, 1970 00:00:00
 * Difference: 3506716800 seconds
 * VMS time unit: 100 nanoseconds
 */
#define VMS_UNIX_EPOCH_DIFF  3506716800ULL
#define VMS_TICKS_PER_SEC    10000000ULL

static uint64_t vmsq_current_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t unix_secs = (uint64_t)ts.tv_sec;
    uint64_t unix_nsec = (uint64_t)ts.tv_nsec;
    uint64_t vms_secs = unix_secs + VMS_UNIX_EPOCH_DIFF;
    return vms_secs * VMS_TICKS_PER_SEC + unix_nsec / 100;
}

/* ---------- File I/O helpers ---------- */

static size_t header_size(void)
{
    return sizeof(struct qman_header);
}

static size_t queue_section_offset(void)
{
    return header_size();
}

static size_t entry_section_offset(void)
{
    return queue_section_offset() +
           VMSQ_MAX_QUEUES * sizeof(struct qman_queue_record);
}

static size_t total_file_size(void)
{
    return entry_section_offset() +
           VMSQ_MAX_ENTRIES * sizeof(struct qman_entry_record);
}

static int db_lock(void)
{
    int fd = fileno(g_db_file);
    if (flock(fd, LOCK_EX) != 0)
        return SS$_ABORT;
    return SS$_NORMAL;
}

static void db_unlock(void)
{
    int fd = fileno(g_db_file);
    flock(fd, LOCK_UN);
}

static int read_header(struct qman_header *hdr)
{
    fseek(g_db_file, 0, SEEK_SET);
    if (fread(hdr, sizeof(*hdr), 1, g_db_file) != 1)
        return SS$_ABORT;
    return SS$_NORMAL;
}

static int write_header(const struct qman_header *hdr)
{
    fseek(g_db_file, 0, SEEK_SET);
    if (fwrite(hdr, sizeof(*hdr), 1, g_db_file) != 1)
        return SS$_ABORT;
    fflush(g_db_file);
    return SS$_NORMAL;
}

static int read_queue_record(int index, struct qman_queue_record *rec)
{
    size_t offset = queue_section_offset() + (size_t)index * sizeof(*rec);
    fseek(g_db_file, (long)offset, SEEK_SET);
    if (fread(rec, sizeof(*rec), 1, g_db_file) != 1)
        return SS$_ABORT;
    return SS$_NORMAL;
}

static int write_queue_record(int index, const struct qman_queue_record *rec)
{
    size_t offset = queue_section_offset() + (size_t)index * sizeof(*rec);
    fseek(g_db_file, (long)offset, SEEK_SET);
    if (fwrite(rec, sizeof(*rec), 1, g_db_file) != 1)
        return SS$_ABORT;
    fflush(g_db_file);
    return SS$_NORMAL;
}

static int read_entry_record(int index, struct qman_entry_record *rec)
{
    size_t offset = entry_section_offset() + (size_t)index * sizeof(*rec);
    fseek(g_db_file, (long)offset, SEEK_SET);
    if (fread(rec, sizeof(*rec), 1, g_db_file) != 1)
        return SS$_ABORT;
    return SS$_NORMAL;
}

static int write_entry_record(int index, const struct qman_entry_record *rec)
{
    size_t offset = entry_section_offset() + (size_t)index * sizeof(*rec);
    fseek(g_db_file, (long)offset, SEEK_SET);
    if (fwrite(rec, sizeof(*rec), 1, g_db_file) != 1)
        return SS$_ABORT;
    fflush(g_db_file);
    return SS$_NORMAL;
}

/* ---------- Queue lookup helpers ---------- */

/* Find queue slot by name. Returns slot index or -1 if not found. */
static int find_queue_slot(const char *name)
{
    struct qman_queue_record rec;
    for (int i = 0; i < VMSQ_MAX_QUEUES; i++) {
        if (read_queue_record(i, &rec) != SS$_NORMAL)
            continue;
        if (rec.active && strncasecmp(rec.queue.name, name, 32) == 0)
            return i;
    }
    return -1;
}

/* Find a free queue slot. Returns index or -1. */
static int find_free_queue_slot(void)
{
    struct qman_queue_record rec;
    for (int i = 0; i < VMSQ_MAX_QUEUES; i++) {
        if (read_queue_record(i, &rec) != SS$_NORMAL)
            continue;
        if (!rec.active)
            return i;
    }
    return -1;
}

/* Find entry slot by ID. Returns slot index or -1. */
static int find_entry_slot(uint32_t entry_id)
{
    struct qman_entry_record rec;
    for (int i = 0; i < VMSQ_MAX_ENTRIES; i++) {
        if (read_entry_record(i, &rec) != SS$_NORMAL)
            continue;
        if (rec.active && rec.entry.entry_id == entry_id)
            return i;
    }
    return -1;
}

/* Find a free entry slot. Returns index or -1. */
static int find_free_entry_slot(void)
{
    struct qman_entry_record rec;
    for (int i = 0; i < VMSQ_MAX_ENTRIES; i++) {
        if (read_entry_record(i, &rec) != SS$_NORMAL)
            continue;
        if (!rec.active)
            return i;
    }
    return -1;
}

/* ---------- Public API ---------- */

int vmsq_init(const char *db_path)
{
    if (g_db_file)
        return SS$_NORMAL;  /* Already initialized */

    if (db_path) {
        strncpy(g_db_path, db_path, sizeof(g_db_path) - 1);
    } else {
        /* Resolve VMS filespec to Linux path at runtime */
        if (vmsfs_to_linux_path(VMS_QMAN_DB, g_db_path, sizeof(g_db_path)) != 0) {
            return SS$_NOSUCHFILE;
        }
    }

    /* Try to open existing file */
    g_db_file = fopen(g_db_path, "r+b");
    if (g_db_file) {
        /* Validate header */
        struct qman_header hdr;
        if (read_header(&hdr) == SS$_NORMAL &&
            hdr.magic == VMSQ_MAGIC && hdr.version == VMSQ_VERSION) {
            return SS$_NORMAL;
        }
        /* Invalid file — recreate */
        fclose(g_db_file);
        g_db_file = NULL;
    }

    /* Create new file */
    g_db_file = fopen(g_db_path, "w+b");
    if (!g_db_file)
        return SS$_ABORT;

    /* Write empty database */
    struct qman_header hdr = {
        .magic = VMSQ_MAGIC,
        .version = VMSQ_VERSION,
        .queue_count = 0,
        .entry_count = 0
    };
    if (write_header(&hdr) != SS$_NORMAL) {
        fclose(g_db_file);
        g_db_file = NULL;
        return SS$_ABORT;
    }

    /* Zero-fill queue and entry slots */
    size_t data_size = total_file_size() - header_size();
    void *zeros = calloc(1, data_size);
    if (!zeros) {
        fclose(g_db_file);
        g_db_file = NULL;
        return SS$_INSFMEM;
    }
    fwrite(zeros, 1, data_size, g_db_file);
    fflush(g_db_file);
    free(zeros);

    return SS$_NORMAL;
}

int vmsq_create_queue(const char *name, int type)
{
    if (!g_db_file || !name)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    /* Check for duplicate */
    if (find_queue_slot(name) >= 0) {
        db_unlock();
        return SS$_DUPLNAM;
    }

    /* Find free slot */
    int slot = find_free_queue_slot();
    if (slot < 0) {
        db_unlock();
        return SS$_EXQUOTA;
    }

    /* Initialize queue record */
    struct qman_queue_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.active = 1;
    strncpy(rec.queue.name, name, sizeof(rec.queue.name) - 1);
    rec.queue.type = (uint8_t)type;
    rec.queue.status = VMSQ_STATUS_STARTED;
    rec.queue.entry_count = 0;
    rec.queue.next_entry_id = 1;

    if ((status = write_queue_record(slot, &rec)) != SS$_NORMAL) {
        db_unlock();
        return status;
    }

    /* Update header */
    struct qman_header hdr;
    read_header(&hdr);
    hdr.queue_count++;
    write_header(&hdr);

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_delete_queue(const char *name)
{
    if (!g_db_file || !name)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    int slot = find_queue_slot(name);
    if (slot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    /* Mark slot as free */
    struct qman_queue_record rec;
    memset(&rec, 0, sizeof(rec));
    write_queue_record(slot, &rec);

    /* Update header */
    struct qman_header hdr;
    read_header(&hdr);
    if (hdr.queue_count > 0)
        hdr.queue_count--;
    write_header(&hdr);

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_submit(const char *queue, const char *job, const char *user,
                uint32_t *entry_id)
{
    if (!g_db_file || !queue || !job || !user || !entry_id)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    /* Find the queue */
    int qslot = find_queue_slot(queue);
    if (qslot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    struct qman_queue_record qrec;
    read_queue_record(qslot, &qrec);

    /* Find free entry slot */
    int eslot = find_free_entry_slot();
    if (eslot < 0) {
        db_unlock();
        return SS$_EXQUOTA;
    }

    /* Assign entry ID from queue's counter */
    uint32_t eid = qrec.queue.next_entry_id;

    /* Create entry */
    struct qman_entry_record erec;
    memset(&erec, 0, sizeof(erec));
    erec.active = 1;
    erec.entry.entry_id = eid;
    strncpy(erec.entry.queue_name, queue, sizeof(erec.entry.queue_name) - 1);
    strncpy(erec.entry.job_name, job, sizeof(erec.entry.job_name) - 1);
    strncpy(erec.entry.username, user, sizeof(erec.entry.username) - 1);
    erec.entry.status = VMSQ_ENTRY_PENDING;
    erec.entry.priority = 100;  /* Default priority */
    erec.entry.submit_time = vmsq_current_time();
    erec.entry.start_time = 0;

    write_entry_record(eslot, &erec);

    /* Update queue */
    qrec.queue.entry_count++;
    qrec.queue.next_entry_id++;
    write_queue_record(qslot, &qrec);

    /* Update header */
    struct qman_header hdr;
    read_header(&hdr);
    hdr.entry_count++;
    write_header(&hdr);

    *entry_id = eid;

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_show_queue(const char *queue, struct vms_queue *info)
{
    if (!g_db_file || !queue || !info)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    int slot = find_queue_slot(queue);
    if (slot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    struct qman_queue_record rec;
    read_queue_record(slot, &rec);
    *info = rec.queue;

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_show_entries(const char *queue, struct vms_queue_entry *entries,
                      int max, int *count)
{
    if (!g_db_file || !queue || !entries || !count)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    *count = 0;
    struct qman_entry_record rec;
    for (int i = 0; i < VMSQ_MAX_ENTRIES && *count < max; i++) {
        if (read_entry_record(i, &rec) != SS$_NORMAL)
            continue;
        if (rec.active && strncasecmp(rec.entry.queue_name, queue, 32) == 0) {
            entries[*count] = rec.entry;
            (*count)++;
        }
    }

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_show_entry(uint32_t entry_id, struct vms_queue_entry *entry)
{
    if (!g_db_file || !entry)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    int slot = find_entry_slot(entry_id);
    if (slot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    struct qman_entry_record rec;
    read_entry_record(slot, &rec);
    *entry = rec.entry;

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_delete_entry(uint32_t entry_id)
{
    if (!g_db_file)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    int slot = find_entry_slot(entry_id);
    if (slot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    /* Get the entry to find its queue */
    struct qman_entry_record erec;
    read_entry_record(slot, &erec);

    /* Decrement queue entry count */
    int qslot = find_queue_slot(erec.entry.queue_name);
    if (qslot >= 0) {
        struct qman_queue_record qrec;
        read_queue_record(qslot, &qrec);
        if (qrec.queue.entry_count > 0)
            qrec.queue.entry_count--;
        write_queue_record(qslot, &qrec);
    }

    /* Clear entry slot */
    memset(&erec, 0, sizeof(erec));
    write_entry_record(slot, &erec);

    /* Update header */
    struct qman_header hdr;
    read_header(&hdr);
    if (hdr.entry_count > 0)
        hdr.entry_count--;
    write_header(&hdr);

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_hold_entry(uint32_t entry_id)
{
    return vmsq_update_status(entry_id, VMSQ_ENTRY_HOLDING);
}

int vmsq_release_entry(uint32_t entry_id)
{
    return vmsq_update_status(entry_id, VMSQ_ENTRY_PENDING);
}

int vmsq_update_status(uint32_t entry_id, uint8_t new_status)
{
    if (!g_db_file)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    int slot = find_entry_slot(entry_id);
    if (slot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    struct qman_entry_record rec;
    read_entry_record(slot, &rec);
    rec.entry.status = new_status;

    /* Set start_time when transitioning to EXECUTING */
    if (new_status == VMSQ_ENTRY_EXECUTING && rec.entry.start_time == 0)
        rec.entry.start_time = vmsq_current_time();

    write_entry_record(slot, &rec);

    db_unlock();
    return SS$_NORMAL;
}

int vmsq_set_queue_status(const char *name, uint8_t new_status)
{
    if (!g_db_file || !name)
        return SS$_BADPARAM;

    int status;
    if ((status = db_lock()) != SS$_NORMAL)
        return status;

    int slot = find_queue_slot(name);
    if (slot < 0) {
        db_unlock();
        return SS$_ITEMNOTFOUND;
    }

    struct qman_queue_record rec;
    read_queue_record(slot, &rec);
    rec.queue.status = new_status;
    write_queue_record(slot, &rec);

    db_unlock();
    return SS$_NORMAL;
}

void vmsq_close(void)
{
    if (g_db_file) {
        fflush(g_db_file);
        fclose(g_db_file);
        g_db_file = NULL;
    }
    g_db_path[0] = '\0';
}
