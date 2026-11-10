/*
 * test_setimr_timer_reuse.c -- $SETIMR requests that EXPIRE give their host
 * timer back (rd vms-0a7 S3).
 *
 * Each $SETIMR is a POSIX timer, and each POSIX timer holds one of the
 * process's queued-signal resources (RLIMIT_SIGPENDING). The expiry handler
 * used to mark the request done but never delete its timer, so a process that
 * waits on a short $SETIMR in a loop -- NETACP's $WFLOR on its timer flag and
 * its request mailbox, every 5 ms while a link is live -- ran out after a few
 * thousand requests: $SETIMR returned SS$_INSFMEM and NETACP stopped in the
 * middle of a 2000-record COPY with a real VAX (live evidence:
 * tests/lab/captures/vms-0a7-s3-20261103/, "DECNETD-F-SETIMR ... 00000124").
 *
 * The test lowers RLIMIT_SIGPENDING to a small value L (raised only if this
 * user's other processes already hold that many), then runs 4*L requests that
 * each expire (an AST counts them). With the leak, request ~L+1 fails.
 * It also checks that $CANTIM of a pending request still works after reuse.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sys/resource.h>

#include "ssdef.h"
#include "efndef.h"
#include "starlet.h"

static volatile sig_atomic_t g_fired;

static void on_ast(uint32_t prm)
{
    (void)prm;
    g_fired++;
}

static int wait_fired(int want)
{
    struct timespec ts = { 0, 200 * 1000 };       /* 0.2 ms */
    for (int i = 0; i < 20000 && g_fired < want; i++)  /* up to ~4 s */
        nanosleep(&ts, NULL);
    return g_fired >= want;
}

/* Can this process create one POSIX timer under the current limit? */
static int one_timer_ok(void)
{
    struct sigevent sev;
    timer_t t;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN + 2;
    if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0)
        return 0;
    timer_delete(t);
    return 1;
}

int main(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_SIGPENDING, &rl) != 0) {
        printf("FAIL: getrlimit(RLIMIT_SIGPENDING)\n");
        return 1;
    }
    rlim_t lim = 64;
    for (;;) {
        struct rlimit nl = { lim, rl.rlim_max };
        if (rl.rlim_max != RLIM_INFINITY && lim > rl.rlim_max)
            nl.rlim_cur = rl.rlim_max;
        if (setrlimit(RLIMIT_SIGPENDING, &nl) != 0) {
            printf("FAIL: setrlimit(RLIMIT_SIGPENDING, %lu)\n", (unsigned long)lim);
            return 1;
        }
        if (one_timer_ok())
            break;
        if (nl.rlim_cur != lim || lim >= 65536) {
            printf("FAIL: no POSIX timer can be created even at RLIMIT_SIGPENDING %lu\n",
                   (unsigned long)nl.rlim_cur);
            return 1;
        }
        lim *= 2;
    }
    const int n = (int)lim * 4;
    printf("RLIMIT_SIGPENDING %lu, %d expiring $SETIMR requests\n", (unsigned long)lim, n);

    int64_t delta = -2000;                 /* 0.2 ms, VMS delta time (100 ns units) */
    for (int i = 0; i < n; i++) {
        uint32_t st = sys$setimr(EFN$C_ENF, (const uint64_t *)&delta, on_ast, 7, 0);
        if (!(st & 1)) {
            printf("FAIL: $SETIMR #%d returned %08X (expired requests kept their host timers)\n",
                   i + 1, (unsigned)st);
            return 1;
        }
        if (!wait_fired(i + 1)) {
            printf("FAIL: $SETIMR #%d never expired\n", i + 1);
            return 1;
        }
    }
    printf("PASS: %d requests queued and expired\n", n);

    /* A pending request in a reused slot is still cancellable, and never fires. */
    int64_t later = -10 * 1000 * 1000;     /* 1 s */
    int before = g_fired;
    uint32_t st = sys$setimr(EFN$C_ENF, (const uint64_t *)&later, on_ast, 9, 0);
    if (!(st & 1)) {
        printf("FAIL: $SETIMR (1 s) returned %08X\n", (unsigned)st);
        return 1;
    }
    st = sys$cantim(9, 0);
    if (!(st & 1)) {
        printf("FAIL: $CANTIM returned %08X\n", (unsigned)st);
        return 1;
    }
    struct timespec ts = { 1, 300 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    if (g_fired != before) {
        printf("FAIL: a cancelled request fired\n");
        return 1;
    }
    printf("PASS: $CANTIM of a pending request\n");
    return 0;
}
