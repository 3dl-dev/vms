/*
 * test_syssvc_setimr_ast_rearm.c - a $SETIMR AST that re-arms its own timer
 * never deadlocks, and a process's expired timer requests never pile up
 * (rd vms-2a84).
 *
 * WHY. test_syssvc_rms_dir_writers (rd vms-b6e) runs two subprocesses whose
 * 1 ms $SETIMR is re-armed from its own AST. Once in the merge queue (run
 * 41917160473) its logger never finished. Two defects in sys$setimr's host
 * timer table explain it, and this suite pins each one:
 *
 *   1. sys$setimr armed the timer while it held the table's mutex. A timer
 *      that expired before the mutex was released ran its AST at once, in the
 *      same thread; the AST called sys$setimr, which waited for the mutex its
 *      own thread held, for ever. On VMS an AST is not delivered inside the
 *      system service that queued its timer; it runs once the service returns.
 *   2. A timer that fired was never deleted. Every expired request kept its
 *      host timer -- and the signal-queue entry the substrate charges for it --
 *      until the process exited, so after a few hundred re-arms per process
 *      (measured: 762 and 716 in the two dir_writers children) $SETIMR failed
 *      SS$_INSFMEM and the AST chain stopped. On VMS an expired timer request
 *      is gone: it holds no quota.
 *
 * WHAT IS ASSERTED, each in a subprocess of the executive under a deadline:
 *   - REARM: REARM_ROUNDS times, the mainline queues a 100 ns timer whose AST
 *     re-arms it REARM_CHAIN more times. The timer expires around the moment
 *     sys$setimr arms it, so its AST lands inside the service whenever the
 *     service lets it. Every chain completes; the subprocess exits.
 *   - NO PILE-UP: SEQ_TIMERS one-shot timers, one after the other, each waited
 *     for: every $SETIMR succeeds and every AST runs.
 *
 * NO /dev/vms -> honest SKIP (77).
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP     77
#define REARM_ROUNDS  2000
#define REARM_CHAIN   3
#define SEQ_TIMERS    10000
#define DEADLINE_S    60
#define TIMER_ID      0x2A84

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
    fflush(stdout);
}

/* What a child reached, readable by the parent after the deadline. */
struct progress {
    volatile uint32_t rounds;        /* rounds (or timers) completed */
    volatile uint32_t asts;          /* ASTs run */
    volatile uint32_t bad_st;        /* first failing $SETIMR status, 0 = none */
    volatile uint32_t bad_at;        /* the round it failed in */
};
static struct progress *prog;        /* [0] REARM, [1] NO PILE-UP */
static struct progress *me;

static const uint64_t tick = (uint64_t)-1LL;          /* 100 ns delta */
static volatile uint32_t chain_left;

static void note_bad(uint32_t st)
{
    if (!(st & 1) && me->bad_st == 0) {
        me->bad_st = st;
        me->bad_at = me->rounds;
    }
}

static void rearm_ast(uint32_t reqidt)
{
    (void)reqidt;
    me->asts++;
    if (chain_left > 0) {
        chain_left--;
        note_bad(sys$setimr(0, &tick, rearm_ast, TIMER_ID, 0));
    }
}

/* Wait (bounded) for the AST count to reach `want`; 0 when it did. */
static int wait_asts(uint32_t want)
{
    time_t give_up = time(NULL) + 5;

    while (me->asts < want) {
        if (time(NULL) > give_up)
            return -1;
    }
    return 0;
}

static int run_rearm(void)
{
    uint32_t r, want = 0;

    me = &prog[0];
    for (r = 0; r < REARM_ROUNDS; r++) {
        uint32_t st;

        chain_left = REARM_CHAIN;
        st = sys$setimr(0, &tick, rearm_ast, TIMER_ID, 0);
        note_bad(st);
        if (!(st & 1))
            return 11;
        want += 1 + REARM_CHAIN;
        if (wait_asts(want) != 0)
            return 12;
        me->rounds = r + 1;
    }
    return me->bad_st ? 13 : 0;
}

static void one_ast(uint32_t reqidt)
{
    (void)reqidt;
    me->asts++;
}

static int run_sequence(void)
{
    uint32_t i;

    me = &prog[1];
    for (i = 0; i < SEQ_TIMERS; i++) {
        uint32_t st = sys$setimr(0, &tick, one_ast, TIMER_ID, 0);

        note_bad(st);
        if (!(st & 1))
            return 21;
        if (wait_asts(i + 1) != 0)
            return 22;
        me->rounds = i + 1;
    }
    return 0;
}

/* Wait for `pid` until `deadline`; -1 = still running at it. */
static int wait_until(pid_t pid, time_t deadline)
{
    int st;

    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        if (r < 0 && errno != EINTR)
            return 129;
        if (time(NULL) >= deadline)
            return -1;
        usleep(100000);
    }
}

static int run_child(int (*body)(void), const char *what, int total,
                     const struct progress *p)
{
    pid_t pid;
    int rc;

    pid = ovmx_fork_subprocess();
    if (pid == 0)
        _exit(body());
    if (pid < 0) {
        printf("  %s: no subprocess\n", what);
        return -2;
    }
    rc = wait_until(pid, time(NULL) + DEADLINE_S);
    printf("  %s: rc=%d (-1 = still running at the deadline), %u of %d done, "
           "%u ASTs, first failing $SETIMR %08X at %u\n",
           what, rc, (unsigned)p->rounds, total, (unsigned)p->asts,
           (unsigned)p->bad_st, (unsigned)p->bad_at);
    fflush(stdout);
    if (rc == -1) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
    }
    return rc;
}

int main(void)
{
    int rc;

    printf("=== test_syssvc_setimr_ast_rearm (rd vms-2a84) ===\n");
    if (vms_kif_open() < 0) {
        printf("SKIP: no /dev/vms -- no executive\n");
        return EXIT_SKIP;
    }
    vms_kif_close();

    prog = mmap(NULL, 2 * sizeof(*prog), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (prog == MAP_FAILED) {
        printf("  FAIL: no shared progress page\n");
        return 1;
    }
    memset(prog, 0, 2 * sizeof(*prog));

    rc = run_child(run_rearm, "rearm", REARM_ROUNDS, &prog[0]);
    /* negctl: setimr-arms-under-table-lock */
    check(rc != -1,
          "*** an AST that re-arms its own $SETIMR never deadlocks inside $SETIMR ***");
    check(rc == 0, "  and every re-armed chain ran to its end");

    rc = run_child(run_sequence, "sequence", SEQ_TIMERS, &prog[1]);
    /* negctl: setimr-expired-timer-kept */
    check(rc == 0 && prog[1].bad_st == 0,
          "*** expired $SETIMR requests hold nothing: 10000 timers in a row all succeed ***");
    check(prog[1].asts == SEQ_TIMERS, "  and every one of their ASTs ran");

    printf("RESULTS: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
