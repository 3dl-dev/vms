/*
 * vms_futex.c - Futex-based synchronization (replaces pthreads)
 *
 * Uses the Linux futex(2) syscall directly.  The mutex uses a
 * 3-state protocol:
 *   0 = unlocked
 *   1 = locked, no waiters
 *   2 = locked, has waiters in kernel
 *
 * The condition variable uses a sequence number.  Waiters snapshot
 * the sequence, release the mutex, futex-wait on the sequence, then
 * re-acquire the mutex.
 */

#include "vms_futex.h"
#include "vms_syscall.h"
#include "vms_errno.h"

/* ================================================================
 * Atomic helpers (GCC builtins)
 * ================================================================ */

static inline uint32_t atomic_cmpxchg(volatile uint32_t *ptr, uint32_t expected, uint32_t desired)
{
    __atomic_compare_exchange_n(ptr, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

static inline uint32_t atomic_xchg(volatile uint32_t *ptr, uint32_t val)
{
    return __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST);
}

static inline uint32_t atomic_load(volatile uint32_t *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_SEQ_CST);
}

static inline void atomic_store(volatile uint32_t *ptr, uint32_t val)
{
    __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST);
}

static inline uint32_t atomic_fetch_add(volatile uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST);
}

/* ================================================================
 * Mutex
 * ================================================================ */

void vms_mutex_init(vms_mutex_t *m)
{
    atomic_store(&m->state, 0);
}

void vms_mutex_lock(vms_mutex_t *m)
{
    /* Fast path: unlocked -> locked */
    uint32_t c = atomic_cmpxchg(&m->state, 0, 1);
    if (c == 0)
        return;

    /* Slow path: mark as contended and wait */
    if (c != 2)
        c = atomic_xchg(&m->state, 2);

    while (c != 0) {
        vms_sys_futex((uint32_t *)&m->state, VMS_FUTEX_WAIT_PRIVATE, 2, NULL, NULL, 0);
        c = atomic_xchg(&m->state, 2);
    }
}

int vms_mutex_trylock(vms_mutex_t *m)
{
    uint32_t c = atomic_cmpxchg(&m->state, 0, 1);
    return (c == 0) ? 0 : -1;
}

void vms_mutex_unlock(vms_mutex_t *m)
{
    if (atomic_fetch_add(&m->state, (uint32_t)-1) != 1) {
        /* There were waiters; reset to 0 and wake one */
        atomic_store(&m->state, 0);
        vms_sys_futex((uint32_t *)&m->state, VMS_FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    }
}

/* ================================================================
 * Condition variable
 * ================================================================ */

void vms_condvar_init(vms_condvar_t *cv)
{
    atomic_store(&cv->seq, 0);
}

void vms_condvar_wait(vms_condvar_t *cv, vms_mutex_t *m)
{
    uint32_t seq = atomic_load(&cv->seq);
    vms_mutex_unlock(m);
    vms_sys_futex((uint32_t *)&cv->seq, VMS_FUTEX_WAIT_PRIVATE, seq, NULL, NULL, 0);
    vms_mutex_lock(m);
}

int vms_condvar_timedwait(vms_condvar_t *cv, vms_mutex_t *m,
                          const struct vms_timespec *abstime)
{
    uint32_t seq = atomic_load(&cv->seq);
    vms_mutex_unlock(m);
    /* Use FUTEX_WAIT_BITSET_PRIVATE which interprets timeout as absolute
     * (CLOCK_REALTIME), matching the abstime parameter semantics. Plain
     * FUTEX_WAIT expects a *relative* timeout which would be incorrect. */
    long ret = vms_sys_futex((uint32_t *)&cv->seq,
                              VMS_FUTEX_WAIT_BITSET_PRIVATE, seq,
                              abstime, NULL, VMS_FUTEX_BITSET_MATCH_ANY);
    vms_mutex_lock(m);
    return (ret == -VMS_ETIMEDOUT) ? -1 : 0;
}

void vms_condvar_signal(vms_condvar_t *cv)
{
    atomic_fetch_add(&cv->seq, 1);
    vms_sys_futex((uint32_t *)&cv->seq, VMS_FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

void vms_condvar_broadcast(vms_condvar_t *cv)
{
    atomic_fetch_add(&cv->seq, 1);
    vms_sys_futex((uint32_t *)&cv->seq, VMS_FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0);
}
