/*
 * vms_futex.h - Futex-based synchronization primitives
 */

#ifndef _VMS_FUTEX_H
#define _VMS_FUTEX_H

#include "vms_types.h"

/* ================================================================
 * Mutex (futex-based, similar semantics to pthread_mutex)
 * ================================================================ */

typedef struct {
    volatile uint32_t state;  /* 0=unlocked, 1=locked-no-waiters, 2=locked-has-waiters */
} vms_mutex_t;

#define VMS_MUTEX_INIT { 0 }

void vms_mutex_init(vms_mutex_t *m);
void vms_mutex_lock(vms_mutex_t *m);
int  vms_mutex_trylock(vms_mutex_t *m);
void vms_mutex_unlock(vms_mutex_t *m);

/* ================================================================
 * Condition variable (futex-based)
 * ================================================================ */

typedef struct {
    volatile uint32_t seq;   /* sequence counter */
} vms_condvar_t;

#define VMS_CONDVAR_INIT { 0 }

void vms_condvar_init(vms_condvar_t *cv);
void vms_condvar_wait(vms_condvar_t *cv, vms_mutex_t *m);
int  vms_condvar_timedwait(vms_condvar_t *cv, vms_mutex_t *m,
                           const struct vms_timespec *abstime);
void vms_condvar_signal(vms_condvar_t *cv);
void vms_condvar_broadcast(vms_condvar_t *cv);

#endif /* _VMS_FUTEX_H */
