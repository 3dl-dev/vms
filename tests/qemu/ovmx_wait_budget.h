/*
 * ovmx_wait_budget.h - reap one child and report its status the way the QEMU
 * harness's reply protocol carries it, optionally under a run budget
 * (rd vms-84e).
 *
 * The corpus runtime gives each program a 40 s budget. That budget used to be
 * enforced by init.sh polling the boot task's reply FIFO with busybox
 * `read -t 1`. busybox's read consumes the FIFO one byte at a time and re-checks
 * its deadline before every byte: when the deadline lapses part-way through a
 * line (the guest shell descheduled between two bytes of "RC 0\n"), it returns
 * "timed out" having already DISCARDED the bytes it read. The next read then
 * sees the tail of the line -- "0" as the tag, nothing as the rc -- and the
 * harness reported `rc=` (CI run 41588156228, sys_cef). No read with a timeout
 * can be made safe against that, so the budget now lives here, in the process
 * that owns the child: the boot task enforces it, and init.sh only ever does a
 * blocking read of a whole reply line.
 *
 * ovmx_wait_rc(pid, budget_s):
 *   budget_s == 0  wait for the child however long it takes;
 *   budget_s  > 0  SIGKILL it once budget_s seconds have passed and report 124
 *                  (the corpus scoreboard's "budget expired" code).
 * Returns the exit status, 128 + signal for a child killed by a signal, or 124.
 * Header-only and libc-only so the host test (tests/integration/
 * test_corpus_rt_reply_protocol.sh) builds the same code the guest runs.
 */
#ifndef OVMX_WAIT_BUDGET_H
#define OVMX_WAIT_BUDGET_H

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define OVMX_RC_BUDGET_EXPIRED 124

static inline int ovmx_status_rc(int ws)
{
    return WIFEXITED(ws) ? WEXITSTATUS(ws)
         : WIFSIGNALED(ws) ? 128 + WTERMSIG(ws) : 1;
}

static inline int ovmx_wait_rc(pid_t pid, unsigned budget_s)
{
    int ws = 0;
    if (budget_s == 0) {
        while (waitpid(pid, &ws, 0) < 0 && errno == EINTR)
            ;
        return ovmx_status_rc(ws);
    }
    struct timespec now, end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += (time_t)budget_s;
    for (;;) {
        pid_t r = waitpid(pid, &ws, WNOHANG);
        if (r == pid)
            return ovmx_status_rc(ws);
        if (r < 0 && errno != EINTR)
            return 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec ||
            (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &ws, 0) < 0 && errno == EINTR)
                ;
            return OVMX_RC_BUDGET_EXPIRED;
        }
        struct timespec tick = { 0, 10 * 1000 * 1000 };   /* 10 ms */
        (void)nanosleep(&tick, NULL);
    }
}

#endif /* OVMX_WAIT_BUDGET_H */
