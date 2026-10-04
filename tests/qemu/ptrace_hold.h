/*
 * ptrace_hold.h - hold a traced process's fork child still, and release it
 * only once it is PROVABLY STOPPED (rd vms-d90).
 *
 * Used by tests/qemu/test_syssvc_procnam.c P13, whose tracer runs a $CREPRC
 * caller under PTRACE_O_TRACEFORK so that every child $CREPRC forks is held
 * at birth, signals the caller while it waits for that child's creation
 * report, and then releases the child.
 *
 * THE DEFECT THIS REPLACES. The tracer used to release the child with
 * PTRACE_DETACH the moment the caller's signal had been injected. But a
 * fork child auto-attached by PTRACE_O_TRACEFORK is NOT stopped when its
 * parent's PTRACE_EVENT_FORK is reported: it is merely runnable, with a
 * SIGSTOP pending, and enters its ptrace-stop only when it is first
 * scheduled. PTRACE_DETACH on a tracee that is not in a ptrace-stop fails
 * with ESRCH. The old tracer ignored that return, forgot the child, and when
 * the child did reach its birth stop a moment later it was left stopped for
 * ever -- so it never wrote its creation report, the caller's handshake read
 * (correctly) never returned, and P13 printed "sys$creprc did not return".
 *
 * Whether the child had been scheduled by then is the guest scheduler's
 * choice. On the single-vCPU QEMU guest under TCG it effectively always had;
 * under KVM the tracer can get the CPU back first (measured: 1 hang in ~55
 * KVM runs of the suite, 0 in 374 TCG runs; with P13 raised to 60 calls per
 * run, 55 of 90 KVM runs hung). Captured in the KVM guest on every hang:
 * PTRACE_DETACH -> ESRCH, the $CREPRC caller asleep in pipe_read, and the
 * child in ptrace_stop straight out of ret_from_fork -- it had never run a
 * user instruction, let alone reached the executive. It reproduces natively too: the
 * same tracer pinned to one CPU next to a busy loop lost 4 children in 400
 * rounds, every one with PTRACE_DETACH -> ESRCH. It is a race in the TEST'S
 * ARRANGEMENT, not in $CREPRC: a caller whose child is stopped for ever is
 * supposed to wait for ever.
 *
 * THE RULE. A release is a REQUEST. It is carried out only when BOTH the
 * request has been made AND the child's birth stop has been observed through
 * waitpid() -- in whichever order those two arrive. The birth stop can even
 * be reported before the parent's fork event names the child (the kernel
 * does not order the two), so a stop from an unknown pid is remembered.
 *
 * Pure bookkeeping, no syscalls: every function returns the pid the caller
 * must PTRACE_DETACH now, or -1. That is what lets
 * tests/qemu/ptrace_hold_selftest.c drive every event ordering
 * deterministically, as well as the real ptrace stress.
 */
#ifndef OVMX_TESTS_PTRACE_HOLD_H
#define OVMX_TESTS_PTRACE_HOLD_H

#include <sys/types.h>

struct ptrace_hold {
    pid_t held;             /* fork child being held; -1 when none */
    int   held_stopped;     /* its birth stop has been observed */
    int   release_wanted;   /* the arrangement is done: let it run */
    pid_t early_stop;       /* a birth stop seen before its fork event */
    unsigned deferred;      /* releases that had to wait for the stop */
};

static inline void ptrace_hold_init(struct ptrace_hold *h)
{
    h->held = -1;
    h->held_stopped = 0;
    h->release_wanted = 0;
    h->early_stop = -1;
    h->deferred = 0;
}

static inline pid_t ptrace_hold_take_(struct ptrace_hold *h)
{
    pid_t p = h->held;
    h->held = -1;
    h->held_stopped = 0;
    h->release_wanted = 0;
    return p;
}

/* The traced parent's PTRACE_EVENT_FORK named `child`. */
static inline pid_t ptrace_hold_on_fork(struct ptrace_hold *h, pid_t child)
{
    h->held = child;
    h->held_stopped = (h->early_stop == child);
    h->release_wanted = 0;
    if (h->early_stop == child)
        h->early_stop = -1;
    return -1;              /* nothing is released at birth */
}

/* waitpid() reported a ptrace-stop of `pid`, a process other than the
 * traced parent -- i.e. a fork child's birth stop. */
static inline pid_t ptrace_hold_on_stop(struct ptrace_hold *h, pid_t pid)
{
    if (pid != h->held || h->held < 0) {
        h->early_stop = pid;
        return -1;
    }
    h->held_stopped = 1;
    return h->release_wanted ? ptrace_hold_take_(h) : -1;
}

/* The arrangement is complete (or abandoned): release the held child as
 * soon as that is possible. */
static inline pid_t ptrace_hold_release(struct ptrace_hold *h)
{
    if (h->held < 0)
        return -1;
    if (h->held_stopped)
        return ptrace_hold_take_(h);
    h->release_wanted = 1;
    h->deferred++;
    return -1;
}

#endif /* OVMX_TESTS_PTRACE_HOLD_H */
