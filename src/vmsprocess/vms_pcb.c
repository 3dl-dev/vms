/*
 * vms_pcb.c - Per-Process Control Block implementation
 *
 * Manages the thread-local PCB structure that consolidates all
 * per-process VMS state. The PCB is allocated once per thread
 * and accessed via the __thread pointer.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "vms/pcb.h"
#include "ssdef.h"
#include "vms_exec.h"

/* Thread-local PCB pointer */
static __thread struct vms_pcb *current_pcb = NULL;

/* Static PCB for the main process (avoids needing malloc before
 * the zone allocator is up).  main_pcb_used is accessed with atomic
 * compare-and-swap to avoid a race when multiple threads call
 * vms_pcb_init concurrently. */
static struct vms_pcb main_pcb;
static volatile int main_pcb_used = 0;

struct vms_pcb *vms_pcb_get(void)
{
    return current_pcb;
}

struct vms_pcb *vms_pcb_init(uint64_t initial_privs)
{
    struct vms_pcb *pcb;

    /* First call gets the static main PCB (atomic CAS to avoid race) */
    int expected = 0;
    if (__atomic_compare_exchange_n(&main_pcb_used, &expected, 1, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        pcb = &main_pcb;
    } else {
        pcb = (struct vms_pcb *)calloc(1, sizeof(struct vms_pcb));
        if (!pcb)
            return NULL;
    }

    memset(pcb, 0, sizeof(*pcb));
    pcb->self = pcb;

    /* Identity defaults */
    pcb->vms_pid = 0;
    pcb->uic = 0;
    memset(pcb->username, 0, sizeof(pcb->username));
    memset(pcb->prcnam, 0, sizeof(pcb->prcnam));
    strncpy(pcb->default_dir, "SYS$LOGIN:", sizeof(pcb->default_dir) - 1);

    /* Start in user mode */
    pcb->current_mode = PCB_MODE_USER;

    /*
     * Privileges are NOT ours to assign. `initial_privs` is a REQUEST that
     * goes to the executive (vms.ko via /dev/vms); the PCB then caches what
     * the executive actually granted. Before this, whatever the caller
     * passed became the process's privileges by fiat -- a process could
     * simply assert about itself that it held SYSPRV.
     *
     * NO SILENT FALLBACK (CLAUDE.md rule 9): with no executive reachable
     * there is no authority to grant anything, so the process holds NO
     * privileges. Absence denies; it must never grant the requested set.
     */
    pthread_mutex_init(&pcb->priv_lock, NULL);
    {
        uint64_t granted = 0, perm = 0;

        if (vms_exec_attach((uint32_t)getpid(), initial_privs, &granted)
                == VMS_EXEC_SS_NORMAL) {
            (void)vms_exec_getprv(&granted, &perm, &pcb->current_mode);
        }
        pcb->cur_privs = granted;
        pcb->perm_privs = perm;
    }

    /* Event flags: all clear */
    memset(pcb->ef_clusters, 0, sizeof(pcb->ef_clusters));
    pthread_mutex_init(&pcb->ef_lock, NULL);
    pthread_cond_init(&pcb->ef_cond, NULL);

    /* AST queues: all empty, all enabled */
    for (int i = 0; i < 4; i++) {
        pcb->ast[i].head = NULL;
        pcb->ast[i].tail = NULL;
        pcb->ast[i].count = 0;
        pcb->ast[i].enabled = 1;
    }
    pthread_mutex_init(&pcb->ast_lock, NULL);
    pcb->ast_delivery_requested = 0;

    /* Channels: all unused */
    for (int i = 0; i < PCB_MAX_CHANNELS; i++) {
        pcb->channels[i].fd = -1;
        pcb->channels[i].in_use = 0;
        pcb->channels[i].mbx_peer_fd = -1;
    }
    pthread_mutex_init(&pcb->chan_lock, NULL);

    /* Exit handlers */
    pcb->exit_handler_count = 0;

    /* Default quotas */
    pcb->quotas[PCB_QUOTA_ASTLM] = 64;
    pcb->quotas[PCB_QUOTA_BIOLM] = 32;
    pcb->quotas[PCB_QUOTA_BYTLM] = 65536;
    pcb->quotas[PCB_QUOTA_DIOLM] = 32;
    pcb->quotas[PCB_QUOTA_ENQLM] = 256;
    pcb->quotas[PCB_QUOTA_FILLM] = 64;
    pcb->quotas[PCB_QUOTA_PGFLQUOTA] = 32768;
    pcb->quotas[PCB_QUOTA_PRCLM] = 8;
    pcb->quotas[PCB_QUOTA_TQELM] = 16;
    pcb->quotas[PCB_QUOTA_WSQUOTA] = 4096;

    /* Kernel interface */
    pcb->kif_fd = -1;

    /* io_uring (lazy init on first QIO) */
    pcb->uring_fd = -1;
    pcb->uring_sq = NULL;
    pcb->uring_cq = NULL;
    pcb->uring_sqes = NULL;
    pcb->uring_sq_size = 0;
    pcb->uring_cq_size = 0;
    pcb->uring_running = 0;

    /* Set as current thread's PCB */
    current_pcb = pcb;

    return pcb;
}

void vms_pcb_cleanup(void)
{
    struct vms_pcb *pcb = current_pcb;
    if (!pcb)
        return;

    /* Free all AST entries */
    for (int mode = 0; mode < 4; mode++) {
        struct pcb_ast_entry *entry = pcb->ast[mode].head;
        while (entry) {
            struct pcb_ast_entry *next = entry->next;
            free(entry);
            entry = next;
        }
        pcb->ast[mode].head = NULL;
        pcb->ast[mode].tail = NULL;
        pcb->ast[mode].count = 0;
    }

    /* Close all open channels (including mailbox peer fds) */
    for (int i = 0; i < PCB_MAX_CHANNELS; i++) {
        if (pcb->channels[i].in_use) {
            if (pcb->channels[i].fd >= 0)
                close(pcb->channels[i].fd);
            if ((pcb->channels[i].flags & PCB_CHAN_MAILBOX) &&
                pcb->channels[i].mbx_peer_fd >= 0)
                close(pcb->channels[i].mbx_peer_fd);
            pcb->channels[i].fd = -1;
            pcb->channels[i].mbx_peer_fd = -1;
            pcb->channels[i].in_use = 0;
        }
    }

    /* Destroy mutexes */
    pthread_mutex_destroy(&pcb->priv_lock);
    pthread_mutex_destroy(&pcb->ef_lock);
    pthread_cond_destroy(&pcb->ef_cond);
    pthread_mutex_destroy(&pcb->ast_lock);
    pthread_mutex_destroy(&pcb->chan_lock);

    /* Clean up io_uring (weak reference — may not be linked) */
    if (pcb->uring_fd >= 0) {
        extern void vms_uring_cleanup(void) __attribute__((weak));
        if (vms_uring_cleanup)
            vms_uring_cleanup();
    }

    /* Close kernel interface */
    if (pcb->kif_fd >= 0) {
        close(pcb->kif_fd);
        pcb->kif_fd = -1;
    }

    /* Free if not the static main PCB */
    if (pcb != &main_pcb)
        free(pcb);

    current_pcb = NULL;
}

void vms_pcb_set_identity(uint32_t vms_pid, uint32_t uic,
                          const char *username, const char *prcnam)
{
    struct vms_pcb *pcb = current_pcb;
    if (!pcb)
        return;

    pcb->vms_pid = vms_pid;
    pcb->uic = uic;
    if (username)
        strncpy(pcb->username, username, sizeof(pcb->username) - 1);
    if (prcnam)
        strncpy(pcb->prcnam, prcnam, sizeof(pcb->prcnam) - 1);
}

void vms_pcb_set_default_dir(const char *dir)
{
    struct vms_pcb *pcb = current_pcb;
    if (!pcb || !dir)
        return;

    strncpy(pcb->default_dir, dir, sizeof(pcb->default_dir) - 1);
    pcb->default_dir[sizeof(pcb->default_dir) - 1] = '\0';
}
