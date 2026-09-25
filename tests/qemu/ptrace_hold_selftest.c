/*
 * ptrace_hold_selftest.c - host test for tests/qemu/ptrace_hold.h
 * (rd vms-d90). Needs no /dev/vms and no QEMU: it tests the ARRANGEMENT that
 * test_syssvc_procnam P13 relies on, which is ordinary Linux ptrace.
 *
 * PART A -- every event ordering, deterministically. The three events are
 * the fork event naming the child (F), the child's birth stop (S) and the
 * release request (R). F always precedes R (the tracer signals the caller
 * only after the fork event), and S can land anywhere, so the orderings are
 * SFR, FSR and FRS. In each the child must be detached EXACTLY ONCE and
 * never before its stop was observed. The pre-vms-d90 tracer detached at R
 * unconditionally, which is wrong in FRS: that is the ordering that lost
 * the child.
 *
 * PART B -- the real thing. A tracer holds every fork child of a traced
 * "creator" that forks, reads its child's report over a pipe (retrying
 * EINTR, as $CREPRC does) and reaps it, and interrupts the creator with a
 * caught signal while it waits -- the P13 arrangement. Everything is pinned
 * to ONE CPU next to a busy loop, which is what makes the FRS ordering
 * common (the old logic lost ~1% of rounds this way, each one a creator
 * blocked for ever). Every round must finish; a round that does not report
 * within ROUND_BOUND_MS is the hang, and fails the test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ptrace_hold.h"

static int failures;

#define CHECK(c, msg) do { \
    if (c) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); failures++; } } while (0)

/* ------------------------------------------------------------ part A */

static int order_ok(const char *order)
{
    struct ptrace_hold h;
    const pid_t child = 4242;
    int stopped = 0, detaches = 0, early = 0;
    const char *e;

    ptrace_hold_init(&h);
    for (e = order; *e; e++) {
        pid_t d = -1;
        switch (*e) {
        case 'F': d = ptrace_hold_on_fork(&h, child); break;
        case 'S': stopped = 1; d = ptrace_hold_on_stop(&h, child); break;
        case 'R': d = ptrace_hold_release(&h); break;
        }
        if (d >= 0) {
            detaches++;
            if (d != child || !stopped)
                early++;            /* detached something not yet stopped */
        }
    }
    printf("  order %s: %d detach(es), %d before the stop\n",
           order, detaches, early);
    return detaches == 1 && early == 0 && h.held == -1;
}

/* ------------------------------------------------------------ part B */

#define ROUNDS          300
#define FORKS_PER_ROUND 6
#define ROUND_BOUND_MS  5000
#define SLEEP_POLLS     2000

static volatile sig_atomic_t caught;
static void on_alrm(int s) { (void)s; caught++; }

static int is_sleeping(pid_t pid)
{
    char path[64], buf[512], *p;
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (!(f = fopen(path, "r"))) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strrchr(buf, ')');
    return p && p[1] && p[2] == 'S';
}

/* The $CREPRC-shaped caller: fork, read the child's report (EINTR-proof),
 * reap. Non-restarting handler, exactly like P13's probe. */
static void creator(int repfd)
{
    struct sigaction sa;
    int i;

    if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(2);
    raise(SIGSTOP);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigaction(SIGALRM, &sa, NULL);

    for (i = 0; i < FORKS_PER_ROUND; i++) {
        int fd[2];
        unsigned v = 0;
        size_t done = 0;
        pid_t c;

        if (pipe(fd) != 0) _exit(2);
        c = fork();
        if (c < 0) _exit(2);
        if (c == 0) {
            unsigned one = 1;
            close(fd[0]);
            if (write(fd[1], &one, sizeof(one)) != sizeof(one)) _exit(1);
            _exit(0);
        }
        close(fd[1]);
        while (done < sizeof(v)) {
            ssize_t r = read(fd[0], (char *)&v + done, sizeof(v) - done);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            done += (size_t)r;
        }
        close(fd[0]);
        while (waitpid(c, NULL, 0) < 0 && errno == EINTR)
            ;
        if (done != sizeof(v)) _exit(3);
    }
    if (write(repfd, "k", 1) != 1) _exit(2);
    _exit(0);
}

static void detach(pid_t p, unsigned *bad)
{
    if (p > 0 && ptrace(PTRACE_DETACH, p, 0, 0) != 0)
        (*bad)++;
}

/* The P13 tracer, on ptrace_hold.h. Reports "deferred releases" and
 * "failed detaches" through repfd as two unsigneds. */
static void tracer(int repfd)
{
    struct ptrace_hold h;
    unsigned out[2] = { 0, 0 };
    int a[2], st, alive = 1;
    pid_t cr;

    setpgid(0, 0);
    ptrace_hold_init(&h);
    if (pipe(a) != 0) _exit(2);
    cr = fork();
    if (cr < 0) _exit(2);
    if (cr == 0) { close(a[0]); creator(a[1]); }
    close(a[1]);
    if (waitpid(cr, &st, 0) < 0 || !WIFSTOPPED(st) ||
        ptrace(PTRACE_SETOPTIONS, cr, 0, PTRACE_O_TRACEFORK) != 0 ||
        ptrace(PTRACE_CONT, cr, 0, 0) != 0)
        _exit(2);

    while (alive) {
        pid_t p = waitpid(-1, &st, __WALL);
        int sig, ev;

        if (p < 0) { if (errno == EINTR) continue; break; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (p == cr) alive = 0;
            continue;
        }
        if (!WIFSTOPPED(st)) continue;
        sig = WSTOPSIG(st);
        ev  = (st >> 16) & 0xff;

        if (p != cr) {
            detach(ptrace_hold_on_stop(&h, p), &out[1]);
            continue;
        }
        if (ev == PTRACE_EVENT_FORK) {
            unsigned long msg = 0;
            int i;
            ptrace(PTRACE_GETEVENTMSG, cr, 0, &msg);
            ptrace_hold_on_fork(&h, (pid_t)msg);
            ptrace(PTRACE_CONT, cr, 0, 0);
            for (i = 0; i < SLEEP_POLLS; i++) {
                if (is_sleeping(cr)) break;
                usleep(1000);
            }
            if (i < SLEEP_POLLS)
                kill(cr, SIGALRM);
            else
                detach(ptrace_hold_release(&h), &out[1]);
            continue;
        }
        if (ev) { ptrace(PTRACE_CONT, cr, 0, 0); continue; }
        if (sig == SIGALRM) {
            ptrace(PTRACE_CONT, cr, 0, SIGALRM);
            detach(ptrace_hold_release(&h), &out[1]);
            continue;
        }
        ptrace(PTRACE_CONT, cr, 0, sig);
    }

    {
        char c;
        if (read(a[0], &c, 1) != 1) _exit(3);   /* creator did not finish */
    }
    out[0] = h.deferred;
    if (write(repfd, out, sizeof(out)) != (ssize_t)sizeof(out)) _exit(2);
    _exit(0);
}

int main(void)
{
    cpu_set_t one;
    pid_t hog;
    int round, hung = 0, broken = 0;
    unsigned deferred = 0, bad_detach = 0;

    /* ---- A */
    CHECK(order_ok("SFR"), "birth stop BEFORE the fork event: released once, after the stop");
    CHECK(order_ok("FSR"), "birth stop before the release request: released once, at the request");
    CHECK(order_ok("FRS"), "release requested BEFORE the birth stop: deferred to the stop, never detached early");

    /* ---- B: one CPU, shared with a busy loop */
    CPU_ZERO(&one);
    CPU_SET(sched_getcpu() < 0 ? 0 : sched_getcpu(), &one);
    if (sched_setaffinity(0, sizeof(one), &one) != 0) {
        CHECK(0, "pin the stress to one CPU");
        return 1;
    }
    hog = fork();
    if (hog == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);   /* never outlive the test */
        for (;;) ;
    }

    for (round = 0; round < ROUNDS; round++) {
        int a[2];
        pid_t t;
        struct pollfd pf;
        unsigned out[2];

        if (pipe(a) != 0) { broken++; break; }
        t = fork();
        if (t == 0) { close(a[0]); tracer(a[1]); }
        close(a[1]);
        pf.fd = a[0]; pf.events = POLLIN; pf.revents = 0;
        if (poll(&pf, 1, ROUND_BOUND_MS) <= 0) {
            hung++;
            kill(-t, SIGKILL);
        } else if (read(a[0], out, sizeof(out)) == (ssize_t)sizeof(out)) {
            deferred += out[0];
            bad_detach += out[1];
        } else {
            broken++;
        }
        kill(t, SIGKILL);
        while (waitpid(t, NULL, 0) < 0 && errno == EINTR)
            ;
        close(a[0]);
    }
    kill(hog, SIGKILL);
    waitpid(hog, NULL, 0);

    printf("  stress: %d rounds x %d forks, %d hung, %d broken, "
           "%u releases deferred to the child's stop, %u failed detaches\n",
           ROUNDS, FORKS_PER_ROUND, hung, broken, deferred, bad_detach);
    CHECK(broken == 0, "every stress round ran its tracer and creator to completion");
    CHECK(hung == 0, "no held child was ever lost: every creator's handshake read returned");
    CHECK(bad_detach == 0, "every PTRACE_DETACH hit a stopped tracee");

    printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
