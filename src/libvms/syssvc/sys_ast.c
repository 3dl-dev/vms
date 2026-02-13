/*
 * sys_ast.c - AST (Asynchronous System Trap) System Services
 *
 * ASTs are VMS's mechanism for asynchronous event notification.
 * They are more structured than Unix signals: each AST carries
 * a function pointer and a parameter, and delivery can be
 * enabled/disabled without losing queued ASTs.
 *
 * AST state is stored in the Per-Process Control Block (PCB),
 * with one queue per access mode (kernel/exec/super/user).
 *
 * Implementation:
 *   - AST queues: linked lists in PCB, one per access mode
 *   - SIGUSR1 signal handler flags delivery request
 *   - sys$setast(0) disables delivery; sys$setast(1) re-enables
 *     and delivers all queued ASTs
 *   - sys$dclexh registers exit handlers called by sys$exit
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include "starlet.h"
#include "vms/pcb.h"

static struct sigaction ast_old_sa;
static int ast_initialized = 0;

/*
 * SIGUSR1 signal handler - flags delivery request.
 */
static void ast_signal_handler(int sig) {
    (void)sig;
    struct vms_pcb *pcb = vms_pcb_get();
    if (pcb)
        pcb->ast_delivery_requested = 1;
}

/* One-time initialization of AST signal handling */
static void init_ast(void) {
    if (ast_initialized) return;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ast_signal_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, &ast_old_sa);

    ast_initialized = 1;
}

/*
 * Deliver all pending ASTs from the PCB queues.
 * Scans from most privileged (kernel=0) to least (user=3).
 */
static void deliver_pending_asts(struct vms_pcb *pcb) {
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

/*
 * sys$dclast - Declare AST (Asynchronous System Trap).
 *
 * Queues the AST in the PCB for the specified access mode.
 * If AST delivery is enabled for that mode, signals delivery.
 *
 * Parameters:
 *   astadr - AST routine to call
 *   astprm - Parameter passed to the AST routine
 *   acmode - Access mode for the AST queue
 */
uint32_t sys$dclast(void (*astadr)(uint32_t), uint32_t astprm,
                    uint32_t acmode) {
    if (!astadr) return SS$_BADPARAM;

    init_ast();

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_BADPARAM;

    uint8_t mode = (uint8_t)(acmode > 3 ? 3 : acmode);

    struct pcb_ast_entry *entry = (struct pcb_ast_entry *)malloc(sizeof(*entry));
    if (!entry) return SS$_INSFMEM;

    entry->handler = (pcb_ast_handler_t)astadr;
    entry->param   = astprm;
    entry->acmode  = mode;
    entry->next    = NULL;

    pthread_mutex_lock(&pcb->ast_lock);

    struct pcb_ast_queue *q = &pcb->ast[mode];

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

/*
 * sys$setast - Enable or disable AST delivery.
 *
 * Operates on the current access mode's queue in the PCB.
 * When enabling, any queued ASTs are delivered immediately.
 *
 * Returns:
 *   SS$_WASSET - AST delivery was previously enabled
 *   SS$_WASCLR - AST delivery was previously disabled
 */
uint32_t sys$setast(uint32_t enbflg) {
    init_ast();

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_WASCLR;

    uint32_t prev;
    int pending = 0;

    pthread_mutex_lock(&pcb->ast_lock);

    uint8_t mode = pcb->current_mode;
    struct pcb_ast_queue *q = &pcb->ast[mode];

    prev = q->enabled ? SS$_WASSET : SS$_WASCLR;
    q->enabled = enbflg ? 1 : 0;

    if (q->enabled && q->count > 0)
        pending = 1;

    pthread_mutex_unlock(&pcb->ast_lock);

    if (pending)
        deliver_pending_asts(pcb);

    return prev;
}

/* sys$dclexh is implemented in sys_process.c */
