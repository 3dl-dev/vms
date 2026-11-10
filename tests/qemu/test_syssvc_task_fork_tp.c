/*
 * test_syssvc_task_fork_tp.c - the task a process service forks starts with its
 * creator's LIVE thread pointer (rd vms-8960).
 *
 * $CREPRC and LIB$SPAWN create the new process's task with vms_kif_task_fork()
 * (libvmssys, kif_transport_linux.c) on the OpenVMS-calling-standard Alpha
 * build. Everything the task runs before it activates its image -- the C RTL's
 * errno, malloc, stdio -- reaches its per-thread state through the thread
 * pointer, so the task must start with the value its creator is running with.
 *
 * On Alpha the thread pointer is the PALcode UNIQUE value. qemu-palcode caches
 * it and writes it to the process's HWPCB only at a context switch, and
 * Linux/Alpha's plain fork copies the HWPCB: a creator that changed its thread
 * pointer and forked before it was switched out handed its task the OLD value.
 * In the driver gate the old value was the pre-DECC$SHR block, gone, and the
 * JOINT_E2E $CREPRC task died with SIGSEGV on its first errno store.
 *
 * This suite reproduces that window deterministically: it sets a new UNIQUE
 * (WRUNIQUE) and forks at once through vms_kif_task_fork(), with nothing that
 * could switch it out in between, and the task reports the UNIQUE it started
 * with. Without CLONE_SETTLS the task reports the value from the creator's last
 * context switch, never the new one. Every architecture also checks the plain
 * case: no change, the task's thread pointer is the creator's.
 *
 * Between WRUNIQUE and the restore nothing touches thread-local state: signals
 * are blocked first, the fork is a raw clone, and the task writes its report and
 * exits through raw system calls.
 *
 * negctl: drop VMS_CLONE_SETTLS from kif_xport_task_fork()'s Alpha branch -> the
 * "new thread pointer" checks FAIL on qemu-system-alpha (run-syssvc-tests-alpha.sh).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include "vms_kif.h"

/* libvmssys's raw system-call trampolines (vms_syscall.h, whose own sigaction
 * types collide with <signal.h>): they touch no thread-local state. */
extern unsigned long __vms_syscall1(unsigned long nr, unsigned long a1);
extern unsigned long __vms_syscall3(unsigned long nr, unsigned long a1,
                                    unsigned long a2, unsigned long a3);

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static inline unsigned long tp_read(void)
{
#if defined(__alpha__)
    register unsigned long v0 __asm__("$0");
    __asm__ __volatile__("call_pal 0x9e" : "=r"(v0));           /* RDUNIQUE */
    return v0;
#elif defined(__x86_64__)
    unsigned long v;
    __asm__ __volatile__("mov %%fs:0, %0" : "=r"(v));            /* TCB self pointer */
    return v;
#elif defined(__aarch64__)
    unsigned long v;
    __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(v));
    return v;
#else
#error "test_syssvc_task_fork_tp: no thread-pointer read for this architecture"
#endif
}

#if defined(__alpha__)
static inline void tp_write(unsigned long v)
{
    register unsigned long a0 __asm__("$16") = v;
    __asm__ __volatile__("call_pal 0x9f" : : "r"(a0) : "memory"); /* WRUNIQUE */
}
#endif

/* Fork a task through vms_kif_task_fork() with the thread pointer set to
 * `use` (Alpha; elsewhere `use` must be the current value) for the length of
 * the fork. Returns 1 and *got = the task's starting thread pointer, or 0. */
#define EXIT_SKIP 77

/* A process task is an executive object: with no /dev/vms this suite has
 * nothing to prove and skips honestly (77), never a pass -- the kernel
 * executive negative control runs every suite with vms.ko absent. */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

static int fork_and_report(unsigned long use, unsigned long *got)
{
    int pfd[2];
    if (pipe(pfd) != 0)
        return 0;
    sigset_t all, old;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &old);

    unsigned long live = tp_read();
#if defined(__alpha__)
    tp_write(use);
#else
    (void)use;
#endif
    int pid = vms_kif_task_fork();
    if (pid == 0) {
        unsigned long mine = tp_read();
        (void)__vms_syscall3(SYS_write, (unsigned long)pfd[1],
                             (unsigned long)&mine, sizeof mine);
        for (;;)
            (void)__vms_syscall1(SYS_exit_group, 0);
    }
#if defined(__alpha__)
    tp_write(live);
#endif
    (void)live;
    sigprocmask(SIG_SETMASK, &old, NULL);

    close(pfd[1]);
    int ok = 0;
    if (pid > 0) {
        ok = read(pfd[0], got, sizeof *got) == (ssize_t)sizeof *got;
        int st = 0;
        ok = waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0 && ok;
    } else {
        printf("  vms_kif_task_fork: %d\n", pid);
    }
    close(pfd[0]);
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_task_fork_tp: a forked process task starts with its creator's live thread pointer (rd vms-8960) ===\n");
    if (!executive_present()) {
        printf("=== test_syssvc_task_fork_tp: SKIPPED (no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    unsigned long tp = tp_read(), got = 0;
    int ok = fork_and_report(tp, &got);
    printf("  creator TP %#lx, task TP %#lx\n", tp, got);
    /* negctl: task-fork-wrong-thread-pointer */
    CHECK(ok, "vms_kif_task_fork() created a task that reported and exited 0");
    CHECK(ok && got == tp, "the task's thread pointer is the creator's");

#if defined(__alpha__)
    /* The stale-HWPCB window, eight times: a fresh UNIQUE, then the fork at
     * once. Any one task starting with another value is the defect. */
    int same = 0, made = 0;
    for (int i = 1; i <= 8; i++) {
        unsigned long want = tp + 0x1000UL * (unsigned long)i;
        got = 0;
        if (fork_and_report(want, &got))
            made++;
        if (got == want)
            same++;
        else
            printf("  round %d: set TP %#lx just before the fork, task started with %#lx\n",
                   i, want, got);
    }
    CHECK(made == 8, "all eight forks right after WRUNIQUE created a task that reported");
    /* negctl: vms-8960 (VMS_CLONE_SETTLS dropped from kif_xport_task_fork) */
    CHECK(same == 8,
          "a thread pointer set just before the fork is the one the task starts with (8 of 8)");
    CHECK(tp_read() == tp, "the creator's own thread pointer is restored");
#endif

    printf("=== test_syssvc_task_fork_tp: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
