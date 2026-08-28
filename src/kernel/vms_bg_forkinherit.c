// SPDX-License-Identifier: GPL-2.0
/*
 * vms_bg_forkinherit.c - eager FORK-TIME BG channel inheritance (vms-0cd).
 *
 * WHY. #815 inherits a parent's BG channels into a child at the child's
 * REGISTRATION (its first /dev/vms ioctl). That is too late for the classic
 * forking-server pattern -- accept -> fork -> CLOSE the listener's copy of the
 * accepted connection -- used by sshd, inetd, and most connection-handing-off
 * daemons: the parent $DASSGNs the accepted channel right after the fork, long
 * before the forked child ever calls the executive, so a snapshot taken at the
 * child's registration finds the channel already gone (SS$_IVCHAN). A real fd table
 * is copied AT FORK, so the child keeps the fd no matter what the parent does next;
 * the BG channel table must inherit the same way.
 *
 * HOW. On sched_process_fork, if the forking process has a registered PCB with BG
 * channels, capture a kref'd SNAPSHOT of them (sharing the one host socket) into a
 * PENDING record keyed by the child's tgid -- taken BEFORE the parent can close.
 * When the child later registers, vms_proc_register consumes the pending record
 * (vms_proc_inherit_channels calls vms_bg_forkinherit_consume first, falling back to
 * #815's real_parent snapshot when there is no record). A child that forks off such
 * a parent but never registers has its record freed on its exit (sched_process_exit)
 * or at module unload -- the snapshot's socket refs are always released.
 *
 * LINUX RIND. The tracepoints, get_pid/put_pid, and the pending hash are Linux
 * facilities with no NetBSD analogue, and NetBSD does not build the BGn: core, so
 * this file is Linux-only; on VAX the eager path is simply absent and the executive
 * falls back to #815 -- fail-honest, never fabricated.
 *
 * CONTEXT SAFETY. Tracepoint probes run in atomic context (preemption disabled), so
 * allocations are GFP_ATOMIC and -- crucially -- a snapshot's socket refs are
 * dropped via exec_socket_release, which can SLEEP (sock_release). Probe-path frees
 * are therefore DEFERRED to a workqueue; consume (process/ioctl context) MOVES the
 * refs to the child and never drops them, and module unload frees directly.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include "vms_internal.h"       /* struct vms_proc, the forkinherit + capture decls */
#include "vms_bg_core.h"        /* vms_bg_adopt_channels / vms_bg_drop_captured */

/* A parent's BG channels captured at the moment it forked `tgid`. */
struct vms_bg_pending {
    struct hlist_node   node;       /* in fork_pending_hash, keyed by tgid */
    struct llist_node   free_node;  /* deferred-free list (probe context) */
    pid_t               tgid;       /* the forked child's tgid (== its pid: a leader) */
    struct pid         *pid_ref;    /* pinned, recycle-safe */
    struct list_head    channels;   /* captured kref'd vms_bg_chan copies */
    uint32_t            next_chan;
};

#define FORK_PENDING_BITS 8
static DEFINE_HASHTABLE(fork_pending_hash, FORK_PENDING_BITS);
static DEFINE_SPINLOCK(fork_pending_lock);

/* --- Tracepoint-driven capture (CONFIG_TRACEPOINTS only) ---------------------
 * Everything from here through vms_on_exit(), plus the tracepoint hookup and the
 * real vms_bg_forkinherit_init/_exit() below, needs the kernel tracepoint
 * infrastructure (for_each_kernel_tracepoint / tracepoint_probe_register).  An
 * arch whose kernel cannot enable CONFIG_TRACEPOINTS compiles all of it out.
 *
 * OpenVMS/Linux-Alpha is exactly that: init/main.c's UNCONDITIONAL initcall
 * TRACE_EVENTs mean CONFIG_TRACEPOINTS drags in the full event-tracing runtime
 * (trace_event_buffer_reserve, trace_event_printf, ...) -> CONFIG_TRACING ->
 * CONFIG_STACKTRACE -> a stack unwinder Alpha does not have, so TRACEPOINTS
 * cannot even build there (verified on a clean tree).  So on Alpha fork-time BG
 * channel inheritance is HONESTLY ABSENT: the #else tail makes init() return an
 * error, the caller (vms_module.c) logs the honest "unavailable; fall back to
 * #815" warning, and vms_bg_forkinherit_consume() below stays compiled and
 * honestly finds nothing (the pending hash is never populated) -- BG-inheritance
 * is genuinely off, NEVER a fabricated success (INV-6).  A follow-up revisits a
 * portable fork-hook (kprobe/other) if Alpha grows a forking daemon that would
 * exercise this.  x86_64 keeps CONFIG_TRACEPOINTS and the full mechanism; VAX
 * does not build this file (its NetBSD fork path handles it).
 */
#ifdef CONFIG_TRACEPOINTS

/* Deferred free (records freed from atomic probe context land here). */
static LLIST_HEAD(fork_pending_tofree);
static void fork_pending_free_work(struct work_struct *w);
static DECLARE_WORK(fork_pending_free_wk, fork_pending_free_work);

static void pending_destroy(struct vms_bg_pending *pend)
{
    put_pid(pend->pid_ref);
    vms_bg_drop_captured(&pend->channels);   /* releases each shared socket ref */
    kfree(pend);
}

static void fork_pending_free_work(struct work_struct *w)
{
    struct llist_node *batch = llist_del_all(&fork_pending_tofree);
    struct vms_bg_pending *pend, *tmp;

    (void)w;
    llist_for_each_entry_safe(pend, tmp, batch, free_node)
        pending_destroy(pend);
}

/* Free from ATOMIC probe context: defer to the workqueue (sock_release may sleep). */
static void pending_free_deferred(struct vms_bg_pending *pend)
{
    llist_add(&pend->free_node, &fork_pending_tofree);
    schedule_work(&fork_pending_free_wk);
}

/* sched_process_fork: capture the forking parent's BG channels for the new child. */
static void vms_on_fork(void *data, struct task_struct *parent,
                        struct task_struct *child)
{
    LIST_HEAD(captured);
    uint32_t next_chan = 0;
    struct vms_bg_pending *pend, *old = NULL, *cur;

    (void)data;
    /* New PROCESSES only (a new thread group leader); threads share the parent PCB. */
    if (!thread_group_leader(child))
        return;

    /* Fast path: capture only if the parent is a registered OVMX process WITH BG
     * channels (one hash probe, taken under vms_proc_hash_lock so the parent PCB
     * cannot be freed mid-capture). Empty capture -> nothing to remember. */
    if (!vms_proc_capture_channels_for_task(parent, &captured, &next_chan))
        return;

    pend = kzalloc(sizeof(*pend), GFP_ATOMIC);
    if (!pend) {
        /* OOM: drop the snapshot; the child falls back to #815 at registration
         * (honest -- it may then IVCHAN, never a fabricated channel). */
        vms_bg_drop_captured(&captured);
        return;
    }
    INIT_LIST_HEAD(&pend->channels);
    list_splice_tail_init(&captured, &pend->channels);
    pend->next_chan = next_chan;
    pend->tgid = task_tgid_nr(child);
    pend->pid_ref = get_pid(task_tgid(child));

    spin_lock(&fork_pending_lock);
    hash_for_each_possible(fork_pending_hash, cur, node, pend->tgid) {
        if (cur->tgid == pend->tgid) { old = cur; break; }   /* stale, recycled tgid */
    }
    if (old)
        hash_del(&old->node);
    hash_add(fork_pending_hash, &pend->node, pend->tgid);
    spin_unlock(&fork_pending_lock);

    if (old)
        pending_free_deferred(old);
}

/* sched_process_exit: a process that forked off a channel-holding parent but never
 * registered would otherwise leak its snapshot's socket refs -- reclaim it. */
static void vms_on_exit(void *data, struct task_struct *p)
{
    struct vms_bg_pending *pend = NULL, *cur;
    pid_t tgid;

    (void)data;
    if (!thread_group_leader(p))        /* reclaim once, when the process is gone */
        return;
    tgid = task_tgid_nr(p);

    spin_lock(&fork_pending_lock);
    hash_for_each_possible(fork_pending_hash, cur, node, tgid) {
        if (cur->tgid == tgid) { pend = cur; break; }
    }
    if (pend)
        hash_del(&pend->node);
    spin_unlock(&fork_pending_lock);

    if (pend)
        pending_free_deferred(pend);
}

#endif /* CONFIG_TRACEPOINTS -- capture probes + deferred-free machinery */

/*
 * Consume this task's fork-inherit record, adopting its captured channels onto
 * `child`. Returns 1 if a record was consumed, 0 if none (caller then falls back to
 * the #815 real_parent snapshot). Runs in process/ioctl context (vms_proc_register),
 * where adopt just MOVES the refs to the child -- no sleeping release here.
 */
int vms_bg_forkinherit_consume(struct vms_proc *child)
{
    struct vms_bg_pending *pend = NULL, *cur;
    pid_t tgid = current->tgid;
    struct pid *pidref = task_tgid(current);

    spin_lock(&fork_pending_lock);
    hash_for_each_possible(fork_pending_hash, cur, node, tgid) {
        if (cur->tgid == tgid && cur->pid_ref == pidref) { pend = cur; break; }
    }
    if (pend)
        hash_del(&pend->node);
    spin_unlock(&fork_pending_lock);

    if (!pend)
        return 0;

    vms_bg_adopt_channels(child, &pend->channels, pend->next_chan);  /* moves refs */
    put_pid(pend->pid_ref);
    kfree(pend);                        /* channels list is now empty (moved) */
    return 1;
}

#ifdef CONFIG_TRACEPOINTS
/*
 * The sched_process_fork / _exit tracepoints are NOT exported to out-of-tree
 * modules (no __tracepoint_* symbol), so we cannot use register_trace_*(). Instead
 * find the struct tracepoint by NAME via for_each_kernel_tracepoint (exported) and
 * attach with tracepoint_probe_register -- the standard non-exported-tracepoint
 * path. Linux-only machinery; on VAX this file is not built (fall back to #815).
 */
static struct tracepoint *tp_fork;
static struct tracepoint *tp_exit;

static void find_tracepoint(struct tracepoint *tp, void *priv)
{
    (void)priv;
    if (!strcmp(tp->name, "sched_process_fork"))
        tp_fork = tp;
    else if (!strcmp(tp->name, "sched_process_exit"))
        tp_exit = tp;
}

int vms_bg_forkinherit_init(void)
{
    int ret;

    tp_fork = NULL;
    tp_exit = NULL;
    for_each_kernel_tracepoint(find_tracepoint, NULL);
    if (!tp_fork || !tp_exit)
        return -ENOENT;

    ret = tracepoint_probe_register(tp_fork, (void *)vms_on_fork, NULL);
    if (ret)
        return ret;
    ret = tracepoint_probe_register(tp_exit, (void *)vms_on_exit, NULL);
    if (ret) {
        tracepoint_probe_unregister(tp_fork, (void *)vms_on_fork, NULL);
        tracepoint_synchronize_unregister();
        tp_fork = NULL;
        return ret;
    }
    return 0;
}

void vms_bg_forkinherit_exit(void)
{
    struct vms_bg_pending *pend;
    struct hlist_node *tmp;
    int bkt;

    if (tp_fork)
        tracepoint_probe_unregister(tp_fork, (void *)vms_on_fork, NULL);
    if (tp_exit)
        tracepoint_probe_unregister(tp_exit, (void *)vms_on_exit, NULL);
    tracepoint_synchronize_unregister();    /* no probe can run after this */
    tp_fork = NULL;
    tp_exit = NULL;

    /* Process context, no concurrent probes: free directly (release may sleep). */
    hash_for_each_safe(fork_pending_hash, bkt, tmp, pend, node) {
        hash_del(&pend->node);
        pending_destroy(pend);
    }
    flush_work(&fork_pending_free_wk);      /* drain records the probes deferred */
}

#else  /* !CONFIG_TRACEPOINTS -- the kernel has no tracepoint infrastructure
        * (see the banner above; Alpha cannot build CONFIG_TRACEPOINTS). Fork-time
        * BG channel inheritance is honestly UNAVAILABLE here: report it so the
        * caller falls back to #815, and NEVER fabricate success (INV-6). */

int vms_bg_forkinherit_init(void)
{
    return -ENOSYS;     /* honest: no kernel tracepoints -> no sched fork/exit hook */
}

void vms_bg_forkinherit_exit(void)
{
    /* Nothing was registered and the pending hash is never populated. */
}

#endif /* CONFIG_TRACEPOINTS */
