/*
 * pcb.h - Per-Process Control Block (PCB)
 *
 * Consolidates all per-process VMS state into a single structure:
 *   - Access mode (from access_modes.c)
 *   - Privileges (from sys_misc.c / access_modes.c)
 *   - Event flags (from sys_event.c / event_flags.c)
 *   - AST queues (from sys_ast.c / ast.c)
 *   - I/O channel table (from sys_assign.c)
 *   - Process identity (pid, uic, username)
 *   - Exit handlers (from sys_process.c)
 *   - Quotas
 *
 * The PCB is thread-local: each thread gets its own pointer,
 * initialized via vms_pcb_init(). The main thread's PCB is
 * the "process" PCB; subthreads share some fields (privileges,
 * channels) and have private copies of others (mode, AST state).
 *
 * This replaces ~8 separate static/thread-local declarations
 * scattered across the syssvc and vmsprocess source files.
 */

#ifndef __VMS_PCB_H
#define __VMS_PCB_H

#include <stdint.h>
#include <pthread.h>
#include "vms/process.h"   /* vms_process_t (cached_process, below) */

/* ================================================================
 * Forward declarations and constants
 * ================================================================ */

/* Access modes */
#define PCB_MODE_KERNEL     0
#define PCB_MODE_EXEC       1
#define PCB_MODE_SUPER      2
#define PCB_MODE_USER       3

/* Maximum values */
#define PCB_MAX_CHANNELS    256
#define PCB_MAX_AST_QUEUE   64
#define PCB_MAX_EXIT_HANDLERS 32

/*
 * NO EVENT FLAGS IN THE PCB. There is no PCB_EF_CLUSTERS, no ef_clusters[],
 * no ef_lock and no ef_cond, and none of them may come back (vms-2a8).
 *
 * Event flags are a VMS SYSTEM FACILITY, and under CLAUDE.md Rule 11 a
 * system facility is shared state owned by the executive -- all 128 flags
 * live in src/kernel/vms_eflag.c and are reached through /dev/vms by
 * src/libvms/syssvc/sys_event.c. This structure held a second, private copy
 * of the whole facility (four 32-bit clusters behind a pthread mutex and
 * condvar, with eflag_set/eflag_clear/eflag_wait in event_flags.c on top of
 * it). Its only remaining caller was its own single-process unit test, and
 * that is precisely the coverage shape Rule 11 says proves nothing: a
 * per-process fake passes every single-process test perfectly.
 *
 * It was deleted rather than left orphaned because a facade with green
 * coverage argues for itself -- the next author to need event flags would
 * have found a linked, tested, one-#include-away implementation of the
 * facility that shares nothing.
 */

/* Quota indices */
#define PCB_QUOTA_ASTLM     0   /* AST limit */
#define PCB_QUOTA_BIOLM     1   /* Buffered I/O limit */
#define PCB_QUOTA_BYTLM     2   /* Buffered I/O byte limit */
#define PCB_QUOTA_DIOLM     3   /* Direct I/O limit */
#define PCB_QUOTA_ENQLM     4   /* Enqueue (lock) limit */
#define PCB_QUOTA_FILLM     5   /* Open file limit */
#define PCB_QUOTA_PGFLQUOTA 6   /* Pagefile quota */
#define PCB_QUOTA_PRCLM     7   /* Subprocess limit */
#define PCB_QUOTA_TQELM     8   /* Timer queue entry limit */
#define PCB_QUOTA_WSQUOTA   9   /* Working set quota */
#define PCB_NUM_QUOTAS      10

/* ================================================================
 * Channel entry (moved from sys_assign.c)
 * ================================================================ */

/* Channel flags */
#define PCB_CHAN_MAILBOX     0x0001  /* Channel is a mailbox */
#define PCB_CHAN_MBX_READER  0x0002  /* Reader end of mailbox */

struct pcb_channel {
    int         fd;         /* Linux file descriptor (-1 = unused) */
    char        devnam[256];/* device/file name */
    int         ref_count;
    uint32_t    flags;
    int         in_use;
    int         mbx_peer_fd;/* other end of mailbox socketpair (-1 if not mbx) */
    /*
     * The $ASSIGN channel this PCB slot was bound to in the EXECUTIVE's
     * device table (src/kernel/vms_devtab.c, VMS_IOCTL_ASSIGN), or 0 if
     * this slot was never established with the executive (a plain file,
     * SYS$INPUT/OUTPUT/ERROR, NLA0:, or a mailbox -- none of those are
     * rows in the device table, so nothing to assign there). 0 is a safe
     * sentinel: the executive hands out channel numbers starting at 1
     * (src/kernel/vms_devtab.c's vms_ioctl_assign, ++proc->next_chan).
     * vms-1c57: sys$qio uses this to make the channel the identity -- it
     * revalidates against the executive before doing real I/O, rather
     * than trusting a local slot number the executive never saw.
     */
    uint32_t    exec_chan;
};

/* ================================================================
 * AST entry (moved from ast.c / sys_ast.c)
 * ================================================================ */

typedef void (*pcb_ast_handler_t)(uint32_t astprm);

struct pcb_ast_entry {
    struct pcb_ast_entry *next;
    pcb_ast_handler_t     handler;
    uint32_t              param;
    uint8_t               acmode;
};

/* Per-mode AST queue */
struct pcb_ast_queue {
    struct pcb_ast_entry *head;
    struct pcb_ast_entry *tail;
    int                   count;
    int                   enabled;
};

/* ================================================================
 * Exit handler block (moved from sys_process.c)
 * ================================================================ */

struct pcb_exit_handler {
    void                    *flink;
    void                   (*handler)(uint32_t *);
    uint32_t                arg_count;
    uint32_t               *status_addr;
};

/* ================================================================
 * The PCB itself
 * ================================================================ */

struct vms_pcb {
    /* Self-pointer for TLS validation */
    struct vms_pcb *self;

    /* Process identity */
    uint32_t        vms_pid;
    uint32_t        uic;
    char            username[32];
    char            prcnam[16];
    char            default_dir[256];

    /* Per-thread cache for vms_get_current_process(): the PCB is the
     * per-thread home (see header banner), so this snapshot lives here
     * rather than in a separate __thread object. Keeping it in the PCB
     * ensures vmsprocess has exactly ONE TLS-defining object (vms_pcb.c's
     * current_pcb) — required by the VMS-native linker, which supports one
     * TLS object per image (vms-b65.1; general multi-object TLS is vms-212). */
    vms_process_t   cached_process;

    /* Access mode (per-thread) */
    uint8_t         current_mode;       /* PCB_MODE_KERNEL..PCB_MODE_USER */

    /* Privileges (process-wide, mutex-protected) */
    uint64_t        cur_privs;          /* current (temporary) privileges */
    uint64_t        perm_privs;         /* permanent privileges */
    pthread_mutex_t priv_lock;

    /* NO EVENT FLAGS HERE -- see the note above PCB_QUOTA_ASTLM. */

    /* AST queues: one per access mode (0=kernel .. 3=user) */
    struct pcb_ast_queue ast[4];
    pthread_mutex_t      ast_lock;
    volatile int         ast_delivery_requested;

    /* I/O channel table */
    struct pcb_channel  channels[PCB_MAX_CHANNELS];
    pthread_mutex_t     chan_lock;

    /* Exit handlers */
    struct pcb_exit_handler *exit_handlers[PCB_MAX_EXIT_HANDLERS];
    int                      exit_handler_count;

    /* Quotas */
    uint32_t        quotas[PCB_NUM_QUOTAS];

    /* Kernel interface fd (for /dev/vms ioctl) */
    int             kif_fd;

    /* io_uring async I/O state */
    int             uring_fd;       /* io_uring fd (-1 if not initialized) */
    void           *uring_sq;      /* SQ ring mmap pointer */
    void           *uring_cq;      /* CQ ring mmap pointer */
    void           *uring_sqes;    /* SQE array mmap pointer */
    uint32_t        uring_sq_size; /* SQ ring size */
    uint32_t        uring_cq_size; /* CQ ring size */
    pthread_t       uring_thread;  /* completion processing thread */
    volatile int    uring_running; /* 1 while completion thread is active */
};

/* ================================================================
 * PCB access functions
 * ================================================================ */

/*
 * Get the current thread's PCB pointer.
 * Returns NULL if not yet initialized.
 */
struct vms_pcb *vms_pcb_get(void);

/*
 * Initialize the PCB for the current thread/process.
 * Called once at process startup (from runtime init or main).
 *
 * initial_privs: starting privilege mask
 * Returns: pointer to the new PCB, or NULL on failure.
 */
struct vms_pcb *vms_pcb_init(uint64_t initial_privs);

/*
 * Clean up the PCB for the current thread.
 * Frees AST queues, closes channels, runs exit handlers.
 */
void vms_pcb_cleanup(void);

/*
 * Set the process identity fields in the PCB.
 */
void vms_pcb_set_identity(uint32_t vms_pid, uint32_t uic,
                          const char *username, const char *prcnam);

/*
 * Set default directory.
 */
void vms_pcb_set_default_dir(const char *dir);

#endif /* __VMS_PCB_H */
