/*
 * test_futex.c - Test futex-based mutex and condvar
 *
 * Uses clone() to create threads (no pthreads), then exercises
 * mutex lock contention and condvar signaling.
 */

#include "vmssys.h"

#define STACK_SIZE 65536

static vms_mutex_t g_mutex = VMS_MUTEX_INIT;
static volatile int g_counter = 0;
static int g_iterations = 10000;

/* Thread function: increment shared counter under mutex */
static int thread_func(void *arg)
{
    (void)arg;
    for (int i = 0; i < g_iterations; i++) {
        vms_mutex_lock(&g_mutex);
        g_counter++;
        vms_mutex_unlock(&g_mutex);
    }
    vms_sys_exit(0);
    return 0;  /* unreachable */
}

/* Clone a new thread (lightweight process with shared VM) */
static vms_pid_t spawn_thread(int (*fn)(void *), void *stack_top)
{
    return (vms_pid_t)vms_sys_clone(
        VMS_CLONE_VM | VMS_CLONE_FS | VMS_CLONE_FILES |
        VMS_CLONE_SIGHAND | VMS_CLONE_THREAD | VMS_CLONE_SYSVSEM |
        VMS_CLONE_PARENT_SETTID | VMS_CLONE_CHILD_CLEARTID | VMS_SIGCHLD,
        stack_top,
        NULL, NULL,
        0
    );
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

    /* Test 4: Multi-threaded mutex contention */
    {
        g_counter = 0;
        int num_threads = 4;

        /* Allocate stacks via mmap */
        void *stacks[4];
        vms_pid_t tids[4];

        for (int i = 0; i < num_threads; i++) {
            stacks[i] = vms_sys_mmap(NULL, STACK_SIZE,
                                     VMS_PROT_READ | VMS_PROT_WRITE,
                                     VMS_MAP_PRIVATE | VMS_MAP_ANONYMOUS,
                                     -1, 0);
            if (stacks[i] == VMS_MAP_FAILED) {
                vms_printf("  FAIL: mmap for thread stack\n");
                failures++;
                goto skip_mt;
            }
        }

        /* Spawn threads (stack grows down, pass top of allocation) */
        for (int i = 0; i < num_threads; i++) {
            void *stack_top = (char *)stacks[i] + STACK_SIZE;
            /* Use raw clone with function pointer via assembly trampoline */
            long ret = vms_sys_clone(
                VMS_CLONE_VM | VMS_CLONE_FS | VMS_CLONE_FILES |
                VMS_CLONE_SIGHAND | VMS_CLONE_THREAD | VMS_CLONE_SYSVSEM,
                stack_top,
                NULL, NULL, 0
            );
            if (ret == 0) {
                /* Child: call thread function and exit */
                thread_func(NULL);
                vms_sys_exit(0);
            } else if (ret < 0) {
                vms_printf("  FAIL: clone returned %ld\n", ret);
                failures++;
                goto skip_mt;
            }
            tids[i] = (vms_pid_t)ret;
        }
        (void)tids;

        /* Wait for threads: since CLONE_THREAD is set, we can't wait4 them.
         * Instead just spin until counter reaches expected value or timeout. */
        int expected = num_threads * g_iterations;
        struct vms_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
        for (int i = 0; i < 5000; i++) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (g_counter >= expected)
                break;
            vms_sys_nanosleep(&ts, NULL);
        }

        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (g_counter == expected) {
            vms_printf("  OK: multi-threaded mutex (%d threads x %d iterations = %d)\n",
                       num_threads, g_iterations, g_counter);
        } else {
            vms_printf("  FAIL: multi-threaded mutex (expected %d, got %d)\n",
                       expected, g_counter);
            failures++;
        }

    skip_mt:
        for (int i = 0; i < num_threads; i++) {
            if (stacks[i] && stacks[i] != VMS_MAP_FAILED)
                vms_sys_munmap(stacks[i], STACK_SIZE);
        }
    }

    if (failures == 0)
        vms_printf("All futex tests passed.\n");
    else
        vms_printf("Some futex tests FAILED (%d).\n", failures);

    return failures;
}
