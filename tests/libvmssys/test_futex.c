/*
 * test_futex.c - Test futex-based mutex and condvar
 *
 * Uses fork (clone with SIGCHLD) to create child processes sharing a
 * MAP_SHARED memory region, then exercises mutex lock contention and
 * condvar signaling.
 */

#include "vmssys.h"

/* Shared state for multi-process mutex test.
 * Allocated via MAP_SHARED so forked children share it.
 *
 * We use non-PRIVATE futex ops here because the standard vms_mutex
 * uses FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE which only work within
 * a single process.  For cross-process shared memory, we need the
 * non-PRIVATE variants (FUTEX_WAIT/FUTEX_WAKE).
 */
struct shared_state {
    volatile uint32_t lock;   /* 0=free, 1=held, 2=held+waiters */
    volatile int counter;
};

#define ITERATIONS 10000

/* Process-shared mutex using non-private futex ops */
static void shared_lock(volatile uint32_t *lock)
{
    uint32_t c = 0;
    __atomic_compare_exchange_n(lock, &c, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (c == 0) return;  /* fast path: was unlocked */
    if (c != 2) c = __atomic_exchange_n(lock, 2, __ATOMIC_SEQ_CST);
    while (c != 0) {
        vms_sys_futex((uint32_t *)lock, VMS_FUTEX_WAIT, 2, NULL, NULL, 0);
        c = __atomic_exchange_n(lock, 2, __ATOMIC_SEQ_CST);
    }
}

static void shared_unlock(volatile uint32_t *lock)
{
    if (__atomic_fetch_sub(lock, 1, __ATOMIC_SEQ_CST) != 1) {
        __atomic_store_n(lock, 0, __ATOMIC_SEQ_CST);
        vms_sys_futex((uint32_t *)lock, VMS_FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    int failures = 0;

    vms_printf("=== libvmssys futex test ===\n");

    /* Test 1: Mutex basic lock/unlock */
    {
        vms_mutex_t m = VMS_MUTEX_INIT;
        vms_mutex_lock(&m);
        vms_mutex_unlock(&m);
        vms_printf("  OK: mutex lock/unlock\n");
    }

    /* Test 2: Mutex trylock */
    {
        vms_mutex_t m = VMS_MUTEX_INIT;
        int ret = vms_mutex_trylock(&m);
        if (ret == 0) {
            vms_mutex_unlock(&m);
            vms_printf("  OK: mutex trylock\n");
        } else {
            vms_printf("  FAIL: mutex trylock\n");
            failures++;
        }
    }

    /* Test 3: Condvar basic signal */
    {
        vms_condvar_t cv = VMS_CONDVAR_INIT;
        vms_condvar_signal(&cv);
        vms_condvar_broadcast(&cv);
        vms_printf("  OK: condvar signal/broadcast (no waiters)\n");
    }

    /* Test 4: Multi-process mutex contention
     *
     * Raw clone() with a new stack is unsafe from C: after the syscall,
     * the child returns through frames that don't exist on the new stack.
     * Instead, we fork() child processes that share a MAP_SHARED mmap
     * region containing the mutex and counter.  Each child gets its own
     * stack (COW) so C call frames work correctly.
     */
    {
        int num_procs = 4;
        vms_pid_t pids[4];

        /* Allocate shared state via MAP_SHARED so fork'd children see it */
        struct shared_state *shared = (struct shared_state *)vms_sys_mmap(
            NULL, sizeof(struct shared_state),
            VMS_PROT_READ | VMS_PROT_WRITE,
            VMS_MAP_SHARED | VMS_MAP_ANONYMOUS,
            -1, 0);
        if (shared == VMS_MAP_FAILED) {
            vms_printf("  FAIL: mmap shared state\n");
            failures++;
            goto skip_mt;
        }
        shared->lock = 0;
        shared->counter = 0;

        for (int i = 0; i < num_procs; i++) {
            /* fork via clone(SIGCHLD, NULL, ...) — child gets COW stack */
            long ret = vms_sys_clone(VMS_SIGCHLD, NULL, NULL, NULL, 0);
            if (ret == 0) {
                /* Child: increment shared counter under mutex, then exit */
                for (int j = 0; j < ITERATIONS; j++) {
                    shared_lock(&shared->lock);
                    shared->counter++;
                    shared_unlock(&shared->lock);
                }
                vms_sys_exit(0);
            } else if (ret < 0) {
                vms_printf("  FAIL: clone returned %ld\n", ret);
                failures++;
                goto skip_mt;
            }
            pids[i] = (vms_pid_t)ret;
        }

        /* Wait for all children */
        for (int i = 0; i < num_procs; i++) {
            vms_sys_wait4(pids[i], NULL, 0, NULL);
        }

        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        int expected = num_procs * ITERATIONS;
        if (shared->counter == expected) {
            vms_printf("  OK: multi-process mutex (%d procs x %d iterations = %d)\n",
                       num_procs, ITERATIONS, shared->counter);
        } else {
            vms_printf("  FAIL: multi-process mutex (expected %d, got %d)\n",
                       expected, shared->counter);
            failures++;
        }

    skip_mt:
        if (shared != NULL && shared != (struct shared_state *)VMS_MAP_FAILED)
            vms_sys_munmap(shared, sizeof(struct shared_state));
    }

    if (failures == 0)
        vms_printf("All futex tests passed.\n");
    else
        vms_printf("Some futex tests FAILED (%d).\n", failures);

    return failures;
}
