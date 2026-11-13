/*
 * test_syssvc_decnetd_syscall_audit.c - the booted NETACP (NETACP.EXE, the
 * executive-datalink build the image ships) reaches its NIC, waits and runs
 * through VMS services, not Linux libc (rd vms-d3cd; Baron 2026-11-02: "don't
 * drop to linux LIBC, use all VMS-authentic API").
 *
 * WHAT IS AUDITED. The suite makes a VMS subprocess (ovmx_fork_subprocess: a
 * process the executive created, holding the suite's privileges, PHY_IO among
 * them), has it ptrace(PTRACE_TRACEME) and exec the SHIPPED static NETACP.EXE
 * staged at SYS$SYSTEM with NO command line (rd vms-0a7 S3: NETACP takes no
 * argv; its executor 1.42 comes from the executor database, here the host-test
 * file OVMX_DECNET_EXECUTOR names), run for a few seconds:
 *
 *     NETACP.EXE
 *
 * which opens the executive datalink on the LAN unit ETH0:, senses its
 * hardware address, starts the session pool and the request broker (its
 * request read an async $QIO, its sleep $SETIMR + $WFLOR), sends hellos and
 * serves the loop until the suite stops it with SIGTERM. Every system call of
 * the process -- and of any process or thread it makes (PTRACE_O_TRACEFORK /
 * TRACECLONE / TRACEVFORK) -- is seen at its entry stop, and the suite asserts
 * that NONE is
 *
 *   - socket(AF_INET / AF_INET6 / AF_PACKET / AF_NETLINK)  (the old
 *     socket(AF_INET)+SIOCGIFHWADDR MAC lookup; getifaddrs/if_nameindex,
 *     which libc implements over a netlink socket; a userspace raw datalink);
 *   - ioctl in the SIOCGIF* range 0x8910..0x89FF               (interface lookup);
 *   - poll / ppoll / select / pselect6 / epoll_wait / epoll_pwait (host
 *     descriptor waits -- the old terminal/datalink poll());
 *   - clone with CLONE_THREAD, or clone3                      (pthread_create);
 *   - pipe / pipe2                                            (a host pipe --
 *     the old SYS$OUTPUT reader's, sysoutput_rms.c).
 *
 * NOT VACUOUS. A trace that saw nothing would pass all of that, so the suite
 * also asserts what the run DID: the tracer saw the process's /dev/vms
 * executive ioctls (magic 'V'), DECNETD printed DECNETD-I-DATALINK naming
 * ETH0: and DECNETD-I-STARTED, and exited 0 after its counters. And the
 * classifier itself is proven on a control child that makes exactly the
 * forbidden calls (socket(AF_INET), poll, a SIOCGIFINDEX ioctl, a thread):
 * each must be flagged, or the audit is not measuring anything.
 *
 * NEGATIVE CONTROL (a product mutation that must redden this suite):
 * tests/qemu/defects.d/decnetd-lan-lookup-via-libc.defect.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#if defined(__alpha__)
#include <asm/ptrace.h>
#else
#include <sys/user.h>
#endif
#include <elf.h>

#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77

#ifndef DECNETD_PATH_DEFAULT
#define DECNETD_PATH_DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/NETACP.EXE"
#endif

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* ---- the per-arch syscall entry: number + first three arguments -------- */
struct sc { long nr; unsigned long a0, a1, a2; };

static int sc_read(pid_t pid, struct sc *s)
{
    struct iovec iov;
#if defined(__alpha__)
    /* Linux/Alpha has no PTRACE_GETREGSET (no user_regset view): read the
     * registers one at a time by number, as strace does there -- v0 ($0)
     * carries the syscall number at syscall-entry stop, a0..a2 are $16..$18. */
    static const int regno[4] = { 0, 16, 17, 18 };
    unsigned long val[4];
    (void)iov;
    for (int i = 0; i < 4; i++) {
        errno = 0;
        val[i] = (unsigned long)ptrace(PTRACE_PEEKUSER, pid, (void *)(long)regno[i], 0);
        if (errno != 0) return -1;
    }
    s->nr = (long)val[0]; s->a0 = val[1]; s->a1 = val[2]; s->a2 = val[3];
    return 0;
#else
    struct user_regs_struct r;
    memset(&r, 0, sizeof r);
    iov.iov_base = &r; iov.iov_len = sizeof r;
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
#if defined(__x86_64__)
    s->nr = (long)r.orig_rax; s->a0 = r.rdi; s->a1 = r.rsi; s->a2 = r.rdx;
    return 0;
#elif defined(__aarch64__)
    s->nr = (long)r.regs[8]; s->a0 = r.regs[0]; s->a1 = r.regs[1]; s->a2 = r.regs[2];
    return 0;
#else
    (void)s;
    return -1;
#endif
#endif
}

/* ---- the classifier ------------------------------------------------------ */
struct verdict {
    unsigned long total;      /* syscall entries seen */
    unsigned long vms_ioctls; /* ioctls whose request carries the 'V' magic */
    unsigned bad_socket, bad_ifioctl, bad_wait, bad_thread, bad_pipe;
    char first_bad[160];
};

static void flag(struct verdict *v, unsigned *ctr, const char *what, const struct sc *s)
{
    (*ctr)++;
    if (!v->first_bad[0])
        snprintf(v->first_bad, sizeof v->first_bad, "%s (nr %ld, a0 %#lx, a1 %#lx)",
                 what, s->nr, s->a0, s->a1);
}

static void classify(struct verdict *v, const struct sc *s)
{
    v->total++;
    long nr = s->nr;
#ifdef SYS_socket
    if (nr == SYS_socket) {
        int dom = (int)s->a0;
        if (dom == AF_INET || dom == AF_INET6 || dom == AF_PACKET || dom == AF_NETLINK)
            flag(v, &v->bad_socket, "socket(AF_INET/INET6/PACKET/NETLINK)", s);
        return;
    }
#endif
#ifdef SYS_socketcall
    if (nr == SYS_socketcall && s->a0 == 1 /* SYS_SOCKET */) {
        flag(v, &v->bad_socket, "socketcall(SOCKET)", s);
        return;
    }
#endif
    if (nr == SYS_ioctl) {
        unsigned long req = s->a1 & 0xFFFFFFFFul;
        if (((req >> 8) & 0xFF) == 'V')
            v->vms_ioctls++;
        if (req >= 0x8910ul && req <= 0x89FFul)
            flag(v, &v->bad_ifioctl, "ioctl(SIOCGIF*)", s);
        return;
    }
#ifdef SYS_poll
    if (nr == SYS_poll) { flag(v, &v->bad_wait, "poll", s); return; }
#endif
#ifdef SYS_ppoll
    if (nr == SYS_ppoll) { flag(v, &v->bad_wait, "ppoll", s); return; }
#endif
#ifdef SYS_select
    if (nr == SYS_select) { flag(v, &v->bad_wait, "select", s); return; }
#endif
#ifdef SYS_pselect6
    if (nr == SYS_pselect6) { flag(v, &v->bad_wait, "pselect6", s); return; }
#endif
#ifdef SYS_epoll_wait
    if (nr == SYS_epoll_wait) { flag(v, &v->bad_wait, "epoll_wait", s); return; }
#endif
#ifdef SYS_epoll_pwait
    if (nr == SYS_epoll_pwait) { flag(v, &v->bad_wait, "epoll_pwait", s); return; }
#endif
#ifdef SYS_pipe
    if (nr == SYS_pipe) { flag(v, &v->bad_pipe, "pipe", s); return; }
#endif
#ifdef SYS_pipe2
    if (nr == SYS_pipe2) { flag(v, &v->bad_pipe, "pipe2", s); return; }
#endif
    if (nr == SYS_clone) {
        /* x86_64/aarch64/alpha: flags are the first argument */
        if (s->a0 & CLONE_THREAD)
            flag(v, &v->bad_thread, "clone(CLONE_THREAD)", s);
        return;
    }
#ifdef SYS_clone3
    if (nr == SYS_clone3) { flag(v, &v->bad_thread, "clone3", s); return; }
#endif
}

static volatile pid_t g_root;
static volatile int g_stop_first;     /* the first alarm asks NETACP to stop */
static void on_bound(int sig)
{
    (void)sig;
    if (g_root <= 0) return;
    if (g_stop_first) {               /* SIGTERM: NETACP ends its loop, reports, exits 0 */
        g_stop_first = 0;
        kill(g_root, SIGTERM);
        alarm(20);                    /* ... and is killed if it does not */
        return;
    }
    kill(g_root, SIGKILL);
}

/*
 * trace_all - run the tracee (already stopped at its SIGSTOP after
 * PTRACE_TRACEME) and every process/thread it makes to completion, feeding
 * each syscall-ENTRY stop to classify(). Returns the first tracee's exit
 * status (waitpid form), or -1 if the trace broke.
 */
static int trace_all(pid_t root, struct verdict *v, int bound_s, int stop_s)
{
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    /* a tracee is "in syscall" between its entry and exit stops; a task the
     * trace has just met still owes its birth SIGSTOP */
    enum { MAXT = 64 };
    pid_t tids[MAXT]; int insys[MAXT]; int newborn[MAXT]; int nt = 0;
    int root_status = -1, live = 1;

    if (ptrace(PTRACE_SETOPTIONS, root, 0, (void *)opts) != 0)
        return -1;
    g_root = root;
    signal(SIGALRM, on_bound);
    g_stop_first = stop_s > 0;
    alarm((unsigned)(stop_s > 0 ? stop_s : bound_s)); /* a wedged tracee is killed, not waited on */
    tids[nt] = root; insys[nt] = 0; newborn[nt] = 0; nt++;
    if (ptrace(PTRACE_SYSCALL, root, 0, 0) != 0)
        return -1;

    while (live > 0) {
        int st = 0;
        pid_t p = waitpid(-1, &st, __WALL);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int k;
        for (k = 0; k < nt && tids[k] != p; k++)
            ;
        if (k == nt && nt < MAXT) { tids[nt] = p; insys[nt] = 0; newborn[nt] = 1; nt++; live++; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (p == root) root_status = st;
            live--;
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;
        int sig = WSTOPSIG(st);
        int deliver = 0;
        if (sig == (SIGTRAP | 0x80)) {
            if (k < nt) {
                if (!insys[k]) {
                    struct sc s;
                    if (sc_read(p, &s) == 0)
                        classify(v, &s);
                }
                insys[k] = !insys[k];
            }
        } else if (sig == SIGTRAP && (st >> 16)) {
            /* a fork/clone/vfork/exec event: the new task arrives on its own */
        } else if (sig == SIGSTOP && k < nt && newborn[k]) {
            newborn[k] = 0;            /* a new task's birth stop: swallow it */
        } else {
            deliver = sig;
        }
        (void)ptrace(PTRACE_SYSCALL, p, 0, (void *)(long)deliver);
    }
    alarm(0);
    return root_status;
}

/* The control child: it makes exactly the forbidden calls. */
static void *ctl_thread(void *a) { return a; }
static void control_child(void)
{
    struct pollfd pf = { -1, 0, 0 };
    (void)poll(&pf, 1, 0);
    int pp[2];
    if (pipe(pp) == 0) { close(pp[0]); close(pp[1]); }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        unsigned char ifr[40];
        memset(ifr, 0, sizeof ifr);
        memcpy(ifr, "lo", 2);
        (void)ioctl(s, 0x8933 /* SIOCGIFINDEX */, ifr);
        close(s);
    }
    pthread_t th;
    if (pthread_create(&th, NULL, ctl_thread, NULL) == 0)
        pthread_join(th, NULL);
    _exit(0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_decnetd_syscall_audit (booted NETACP uses VMS services, rd vms-d3cd) ===\n");

    const char *img = getenv("OVMX_DECNETD");
    if (!img) img = DECNETD_PATH_DEFAULT;

    /* ---- 0. the classifier works (anti-vacuity) ------------------------- */
    {
        struct verdict v;
        memset(&v, 0, sizeof v);
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            control_child();
        }
        int st = 0;
        waitpid(c, &st, 0);
        int rs = trace_all(c, &v, 30, 0);
        CHECK(rs >= 0 && v.bad_wait >= 1 && v.bad_socket >= 1 && v.bad_ifioctl >= 1 &&
              v.bad_thread >= 1 && v.bad_pipe >= 1,
              "control: the audit flags a poll, a pipe, a socket(AF_INET), a SIOCGIF ioctl and a thread");
    }

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_decnetd_syscall_audit: %d passed, %d failed (SKIPPED: no"
               " /dev/vms -- NETACP's datalink is executive-resident) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    vms_kif_close();
    if (access(img, X_OK) != 0) {
        /* The image build stages it (tests/qemu/Dockerfile); its absence is a
         * harness defect, not a skip. */
        printf("  FAIL: %s is not staged\n", img);
        printf("=== test_syssvc_decnetd_syscall_audit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* ---- 1. trace the shipped NETACP.EXE -------------------------------- */
    /* Its executor: the executor database, here the host-test file the store
     * reads when OVMX_DECNET_EXECUTOR names one (dnet_ncpstore.c). */
    static char exf[] = "/tmp/netacp-audit-executor.dat";
    {
        FILE *xf = fopen(exf, "w");
        if (!xf) { printf("  FAIL: cannot write %s\n", exf); return 1; }
        fprintf(xf, "EXECUTOR 1.42 NAME OVMX STATE ON\n");
        fclose(xf);
    }
    int out[2];
    if (pipe(out) != 0) { printf("  FAIL: pipe\n"); return 1; }
    uint32_t cst = 0;
    pid_t c = ovmx_fork_subprocess_st(&cst);
    if (c == 0) {
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(out[0]); close(out[1]);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        setenv("OVMX_DECNET_EXECUTOR", exf, 1);
        execl(img, "NETACP.EXE", (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    CHECK(c > 0, "the suite makes a VMS subprocess to run NETACP.EXE");
    if (c <= 0) {
        printf("=== test_syssvc_decnetd_syscall_audit: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    int st = 0;
    waitpid(c, &st, 0);
    /* drain DECNETD's output without blocking the trace */
    fcntl(out[0], F_SETFL, O_NONBLOCK);

    struct verdict v;
    memset(&v, 0, sizeof v);
    int rs = trace_all(c, &v, 60, 6);

    static char log[16384];
    size_t ln = 0;
    for (;;) {
        ssize_t n = read(out[0], log + ln, sizeof log - 1 - ln);
        if (n <= 0) break;
        ln += (size_t)n;
        if (ln >= sizeof log - 1) break;
    }
    log[ln] = '\0';
    close(out[0]);
    printf("  --- NETACP.EXE output ---\n%s  --- end ---\n", log);
    printf("  (syscalls seen %lu, executive ioctls %lu; flagged socket %u ifioctl %u wait %u"
           " thread %u pipe %u%s%s)\n", v.total, v.vms_ioctls, v.bad_socket, v.bad_ifioctl,
           v.bad_wait, v.bad_thread, v.bad_pipe, v.first_bad[0] ? "; first: " : "", v.first_bad);
    unlink(exf);

    CHECK(rs >= 0 && WIFEXITED(rs) && WEXITSTATUS(rs) == 0,
          "NETACP.EXE (no command line) ran under the trace, stopped on SIGTERM and exited 0");
    CHECK(strstr(log, "DECNETD-I-DATALINK, datalink open on ETH0: via the executive") != NULL,
          "it opened its datalink on the LAN unit ETH0: through the executive");
    CHECK(strstr(log, "DECNETD-I-STARTED") != NULL && strstr(log, "DECNETD-I-COUNTERS") != NULL,
          "it started, served its loop and reported its counters");
    CHECK(strstr(log, "DECNETD-I-BROKER") != NULL && strstr(log, "DECNETD-W-REQ") == NULL,
          "its request mailbox read is a queued $QIO (no $ASSIGN/$QIO refusal reported)");
    CHECK(v.total > 50 && v.vms_ioctls > 10,
          "the trace is not vacuous: it saw the process's executive (/dev/vms) calls");
    /* negctl: decnetd-lan-lookup-via-libc */
    CHECK(v.bad_socket == 0 && v.bad_ifioctl == 0,
          "NO host interface lookup: no socket(AF_INET/INET6/PACKET/NETLINK), no SIOCGIF* ioctl");
    /* negctl: netacp-wait-via-poll */
    CHECK(v.bad_wait == 0, "NO poll/ppoll/select/epoll wait on a host descriptor");
    CHECK(v.bad_thread == 0, "NO thread (clone CLONE_THREAD / clone3)");
    CHECK(v.bad_pipe == 0, "NO host pipe (pipe / pipe2)");

    printf("=== test_syssvc_decnetd_syscall_audit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
