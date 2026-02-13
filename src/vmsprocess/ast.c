/*
 * ast.c - Asynchronous System Trap (AST) emulation
 *
 * VMS ASTs are asynchronous callbacks delivered to a process when
 * an event occurs (I/O completion, timer expiry, etc.).  We emulate
 * them using SIGUSR1 to trigger delivery and per-mode queues in the
 * Per-Process Control Block (PCB).
 *
 * State is stored in the PCB's ast[] array (one queue per access mode).
 */

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "vms/ast.h"
#include "vms/pcb.h"
#include "ssdef.h"

/* Previous SIGUSR1 handler (for cleanup) */
static struct sigaction ast_old_sa;
static int ast_initialized = 0;

/* ------------------------------------------------------------------ */
/* SIGUSR1 signal handler                                             */
/* ------------------------------------------------------------------ */
static void ast_signal_handler(int sig)
{
    (void)sig;
    struct vms_pcb *pcb = vms_pcb_get();
    if (pcb)
        pcb->ast_delivery_requested = 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void ast_init(void)
{
    if (ast_initialized)
        return;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ast_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGUSR1, &sa, &ast_old_sa);
    ast_initialized = 1;
}

void ast_cleanup(void)
{
    if (!ast_initialized)
        return;

    sigaction(SIGUSR1, &ast_old_sa, NULL);

    /* Free queued entries via PCB cleanup (handled by vms_pcb_cleanup) */
    ast_initialized = 0;
}

uint32_t ast_queue(ast_handler_t handler, uint32_t param, uint8_t acmode)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb || !handler)
        return SS$_BADPARAM;

    if (acmode > 3)
        acmode = 3;

    struct pcb_ast_entry *entry = (struct pcb_ast_entry *)malloc(sizeof(*entry));
    if (!entry)
        return SS$_INSFMEM;

    entry->handler = handler;
    entry->param   = param;
    entry->acmode  = acmode;
    entry->next    = NULL;

    pthread_mutex_lock(&pcb->ast_lock);

    struct pcb_ast_queue *q = &pcb->ast[acmode];

    /* Guard against runaway queues */
    if (q->count >= PCB_MAX_AST_QUEUE) {
        pthread_mutex_unlock(&pcb->ast_lock);
        free(entry);
        return SS$_EXASTLM;
    }

    /* Append to tail */
    if (q->tail) {
        q->tail->next = entry;
    } else {
        q->head = entry;
    }
    q->tail = entry;
    q->count++;

    int should_signal = q->enabled;
    pthread_mutex_unlock(&pcb->ast_lock);

    if (should_signal)
        raise(SIGUSR1);

    return SS$_NORMAL;
}

uint32_t ast_set_enable(int enable)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_WASCLR;

    uint32_t prev;
    int pending = 0;

    pthread_mutex_lock(&pcb->ast_lock);

    /* Enable/disable for current mode's queue */
    uint8_t mode = pcb->current_mode;
    struct pcb_ast_queue *q = &pcb->ast[mode];

    prev = q->enabled ? SS$_WASSET : SS$_WASCLR;
    q->enabled = enable ? 1 : 0;

    if (q->enabled && q->count > 0)
        pending = 1;

    pthread_mutex_unlock(&pcb->ast_lock);

    if (pending)
        raise(SIGUSR1);

    return prev;
}

/*
 * Deliver all pending ASTs, scanning from most privileged to least.
 */
void ast_deliver_pending(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return;

    pcb->ast_delivery_requested = 0;

    /* Scan from kernel (0) to user (3) for priority delivery */
    for (int mode = 0; mode <= 3; mode++) {
        for (;;) {
            struct pcb_ast_entry *entry = NULL;

            pthread_mutex_lock(&pcb->ast_lock);
            struct pcb_ast_queue *q = &pcb->ast[mode];

            if (!q->enabled || !q->head) {
                pthread_mutex_unlock(&pcb->ast_lock);
                break;
            }

            /* Dequeue from head */
            entry = q->head;
            q->head = entry->next;
            if (!q->head)
                q->tail = NULL;
            q->count--;
            pthread_mutex_unlock(&pcb->ast_lock);

            /* Deliver */
            if (entry->handler)
                entry->handler(entry->param);

            free(entry);
        }
    }
}

int ast_is_enabled(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;

    int val;
    pthread_mutex_lock(&pcb->ast_lock);
    val = pcb->ast[pcb->current_mode].enabled;
    pthread_mutex_unlock(&pcb->ast_lock);
    return val;
}

int ast_pending_count(void)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return 0;

    int total = 0;
    pthread_mutex_lock(&pcb->ast_lock);
    for (int i = 0; i < 4; i++)
        total += pcb->ast[i].count;
    pthread_mutex_unlock(&pcb->ast_lock);
    return total;
}
