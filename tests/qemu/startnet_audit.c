/*
 * startnet_audit.c - STARTNETAUDIT.EXE (test tooling, rd vms-0a7 S3 gate (c)):
 * a booted node STARTS ITS NETWORK and serves an inbound FAL with no Linux
 * thread, poll, pipe or host socket -- in DCL running @SYS$MANAGER:STARTNET,
 * in NCP, in the NETACP NCP creates, and in the FAL.EXE NETACP creates for the
 * inbound connect.
 *
 * WHERE IT RUNS. Boot 1 of tests/qemu/test_decnet_startnet_boot_e2e.sh, after
 * the executor is DEFINEd but before the network is on: the e2e masters this
 * image and NETACP_TEST.EXE onto a scratch ODS-2 volume, MOUNTs it, and runs
 *     $ STARTNETAUDIT <dev>:[000000]NETACP_TEST.EXE
 * from the SYSTEM session (the shipped disk carries no test tool).
 *
 * WHAT IS TRACED. A VMS subprocess (ovmx_fork_subprocess) runs the shipped
 * DCL.EXE -c "@SYS$MANAGER:STARTNET" under ptrace from its first instruction,
 * following every process and thread it makes (PTRACE_O_TRACEFORK/VFORK/
 * CLONE/EXEC): DCL runs NCP SET EXECUTOR STATE ON, NCP $CREPRCs NETACP, and
 * NETACP keeps running after DCL and NCP exit. Once NETACP has published its
 * request mailbox (DNET$NETACP_REQ), an UNTRACED client -- NETACP_TEST.EXE
 * --net-loopback-accept-test -- COPYs a file to 0"SYSTEM MANAGER":: and back
 * through it: NETACP's inbound object-17 dispatch $CREPRCs FAL.EXE, which is
 * therefore traced too. Then NETACP is stopped (SIGTERM) and the trace ends.
 *
 * ASSERTED, over every traced task: no pipe/pipe2; no poll/ppoll/select/
 * pselect6/epoll wait; no clone(CLONE_THREAD)/clone3; no socket(AF_INET/
 * INET6/PACKET/NETLINK); no exec of anything but DCL, NCP, NETACP and FAL
 * (never DECNETD.EXE). NOT VACUOUS: NCP, NETACP and FAL.EXE were each exec'd
 * under the trace, the tasks made executive (/dev/vms) calls, the client's
 * byte-verified COPY passed, STARTNET exited success, NETACP exited 0 on the
 * stop. The classifier is proven first on a control child that makes each
 * forbidden call.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
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
    struct pt_regs r;
    memset(&r, 0, sizeof r);
    iov.iov_base = &r; iov.iov_len = sizeof r;
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
    s->nr = (long)r.r0; s->a0 = r.r16; s->a1 = r.r17; s->a2 = r.r18;
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

/* A NUL-terminated string out of the tracee (an execve path). */
static void peek_str(pid_t pid, unsigned long addr, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    while (n + 1 < cap) {
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + n), 0);
        if (errno) break;
        for (size_t i = 0; i < sizeof w && n + 1 < cap; i++, n++) {
            char c = (char)((unsigned long)w >> (8 * i));
            out[n] = c;
            if (!c) return;
        }
    }
    out[n] = '\0';
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* ---- who made the call: each task is labelled by the image it runs ------- */
#define MAXL 16
static struct { char name[24]; unsigned pipe, wait, thread, socket; } g_lab[MAXL];
static int g_nlab;
static int lab_of(const char *name)
{
    for (int i = 0; i < g_nlab; i++)
        if (!strcmp(g_lab[i].name, name)) return i;
    if (g_nlab == MAXL) return MAXL - 1;
    snprintf(g_lab[g_nlab].name, sizeof g_lab[0].name, "%s", name);
    return g_nlab++;
}
enum { MAXTASK = 128 };
static pid_t g_tpid[MAXTASK]; static int g_tlab[MAXTASK]; static int g_ntask;
static int task_lab(pid_t p)
{
    for (int i = 0; i < g_ntask; i++) if (g_tpid[i] == p) return g_tlab[i];
    return lab_of("?");
}
static void task_set(pid_t p, int lab)
{
    for (int i = 0; i < g_ntask; i++) if (g_tpid[i] == p) { g_tlab[i] = lab; return; }
    if (g_ntask < MAXTASK) { g_tpid[g_ntask] = p; g_tlab[g_ntask] = lab; g_ntask++; }
}

/* ---- the classifier ------------------------------------------------------ */
struct verdict {
    unsigned long total, vms_ioctls;
    unsigned bad_pipe, bad_wait, bad_thread, bad_socket, bad_exec;
    unsigned exec_dcl, exec_ncp, exec_netacp, exec_fal;
    pid_t netacp_pid;
    char first_bad[256];
};

static void flag(struct verdict *v, unsigned *ctr, const char *what, const struct sc *s)
{
    (*ctr)++;
    if (!v->first_bad[0])
        snprintf(v->first_bad, sizeof v->first_bad, "%s (nr %ld, a0 %#lx, a1 %#lx)",
                 what, s->nr, s->a0, s->a1);
}

static void classify1(struct verdict *v, pid_t pid, const struct sc *s)
{
    long nr = s->nr;
#ifdef SYS_pipe
    if (nr == SYS_pipe) { flag(v, &v->bad_pipe, "pipe", s); return; }
#endif
#ifdef SYS_pipe2
    if (nr == SYS_pipe2) { flag(v, &v->bad_pipe, "pipe2", s); return; }
#endif
#ifdef SYS_socket
    if (nr == SYS_socket) {
        int dom = (int)s->a0;
        if (dom == AF_INET || dom == AF_INET6 || dom == AF_PACKET || dom == AF_NETLINK)
            flag(v, &v->bad_socket, "socket(AF_INET/INET6/PACKET/NETLINK)", s);
        return;
    }
#endif
    if (nr == SYS_ioctl) {
        if ((((s->a1 & 0xFFFFFFFFul) >> 8) & 0xFF) == 'V')
            v->vms_ioctls++;
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
    if (nr == SYS_clone) {
        if (s->a0 & CLONE_THREAD)
            flag(v, &v->bad_thread, "clone(CLONE_THREAD)", s);
        return;
    }
#ifdef SYS_clone3
    if (nr == SYS_clone3) { flag(v, &v->bad_thread, "clone3", s); return; }
#endif
    if (nr == SYS_execve
#ifdef SYS_execveat
        || nr == SYS_execveat
#endif
        ) {
        unsigned long pa = s->a0;
#ifdef SYS_execveat
        if (nr == SYS_execveat) pa = s->a1;
#endif
        char path[512];
        peek_str(pid, pa, path, sizeof path);
        const char *b = strrchr(path, '/');
        char nm[24];
        snprintf(nm, sizeof nm, "%.23s", b ? b + 1 : path);
        task_set(pid, lab_of(nm));
        if (ends_with(path, "/DCL.EXE")) v->exec_dcl++;
        else if (ends_with(path, "/NCP.EXE")) v->exec_ncp++;
        else if (ends_with(path, "/NETACP.EXE")) { v->exec_netacp++; v->netacp_pid = pid; }
        else if (ends_with(path, "/FAL.EXE")) v->exec_fal++;
        else {
            v->bad_exec++;
            if (!v->first_bad[0])
                snprintf(v->first_bad, sizeof v->first_bad, "execve(\"%.200s\")", path);
        }
    }
}

static void classify(struct verdict *v, pid_t pid, const struct sc *s)
{
    v->total++;
    int L = task_lab(pid);
    unsigned p0 = v->bad_pipe, w0 = v->bad_wait, t0 = v->bad_thread, k0 = v->bad_socket;
    classify1(v, pid, s);
    g_lab[L].pipe += v->bad_pipe - p0; g_lab[L].wait += v->bad_wait - w0;
    g_lab[L].thread += v->bad_thread - t0; g_lab[L].socket += v->bad_socket - k0;
}

/* ---- the trace, with the untraced client driven from inside the loop ----- */
struct sink { int fd; char *buf; size_t len, cap; };
static void sink_drain(struct sink *k)
{
    if (!k || k->fd < 0) return;
    for (;;) {
        if (k->len + 1 >= k->cap) {
            char junk[512];
            if (read(k->fd, junk, sizeof junk) <= 0) break;
            continue;
        }
        ssize_t n = read(k->fd, k->buf + k->len, k->cap - 1 - k->len);
        if (n <= 0) break;
        k->len += (size_t)n;
    }
    k->buf[k->len] = '\0';
}

static volatile sig_atomic_t g_tick;
static void on_tick(int sig) { (void)sig; g_tick = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int netacp_published(void)
{
    char dev[64]; uint16_t len = 0;
    return vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, "DNET$NETACP_REQ", 0, dev,
                                 sizeof dev - 1, &len, NULL, NULL) == 1 && len > 0;
}

/* The audit's two DCL procedures on the mastered test volume (set in main). */
static char g_proc_sn[512], g_proc_cl[512];

struct client { const char *image; const char *dcl; pid_t pid; int st; int done; struct sink out; };

static pid_t client_start(struct client *c, int *rfd)
{
    int o[2];
    if (pipe(o) != 0) return -1;        /* the AUDIT's own pipe: never traced */
    uint32_t cst = 0;
    pid_t p = ovmx_fork_subprocess_st(&cst);
    if (p == 0) {
        dup2(o[1], 1); dup2(o[1], 2);
        close(o[0]); close(o[1]);
        /* a foreign command off the mounted test volume, as a user runs it */
        execl(c->dcl, "DCL.EXE", g_proc_cl, (char *)NULL);
        _exit(127);
    }
    close(o[1]);
    if (p <= 0) { close(o[0]); return -1; }
    fcntl(o[0], F_SETFL, O_NONBLOCK);
    *rfd = o[0];
    return p;
}

/* Runs until every traced task is gone. Returns the root's (DCL's) status. */
static int trace_all(pid_t root, struct verdict *v, struct sink *out, struct client *cl,
                     int *netacp_status)
{
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    enum { MAXT = 128 };
    pid_t tids[MAXT]; int insys[MAXT]; int newborn[MAXT]; int nt = 0;
    int root_status = -1, live = 1, stopped = 0;
    uint64_t t0 = now_ms(), t_stop = 0;

    if (ptrace(PTRACE_SETOPTIONS, root, 0, (void *)opts) != 0)
        return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_tick;            /* no SA_RESTART: waitpid returns EINTR */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 0, 250000 }, { 0, 250000 } };
    setitimer(ITIMER_REAL, &it, NULL);
    tids[nt] = root; insys[nt] = 0; newborn[nt] = 0; nt++;
    task_set(root, lab_of("root"));
    if (ptrace(PTRACE_SYSCALL, root, 0, 0) != 0)
        return -1;

    while (live > 0) {
        int st = 0;
        pid_t p = waitpid(-1, &st, __WALL);
        /* waitpid's errno, taken NOW: the drains below read non-blocking pipes
         * (EAGAIN) and the driver makes executive calls, and a clobbered EINTR
         * ended this loop on the first idle tick -- leaving NETACP parked in a
         * ptrace stop nobody resumed (rd vms-1f69: no hellos, a dead COPY 0::,
         * no exit on SIGTERM). */
        int werr = (p < 0) ? errno : 0;
        sink_drain(out);
        sink_drain(&cl->out);
        uint64_t t = now_ms();
        /* the driver: client once NETACP serves; stop NETACP once it is done */
        if (!cl->pid && root_status >= 0 && v->netacp_pid > 0 && netacp_published()) {
            int rfd = -1;
            cl->pid = client_start(cl, &rfd);
            cl->out.fd = rfd;
            if (cl->pid <= 0) { cl->done = 1; cl->pid = -1; }
        }
        if (!stopped && (cl->done || t - t0 > 420000u) && v->netacp_pid > 0) {
            /* Diagnostics: where every traced task is when the client ends. */
            for (int i = 0; i < nt; i++) {
                char pth[64], buf[256] = "", wch[64] = "";
                snprintf(pth, sizeof pth, "/proc/%d/stat", (int)tids[i]);
                FILE *f = fopen(pth, "r");
                if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
                snprintf(pth, sizeof pth, "/proc/%d/wchan", (int)tids[i]);
                f = fopen(pth, "r");
                if (f) { if (!fgets(wch, sizeof wch, f)) wch[0] = 0; fclose(f); }
                buf[strcspn(buf, "\n")] = 0;
                printf("  (task %d [%s] insys=%d: %.80s | wchan %s)\n", (int)tids[i],
                       g_lab[task_lab(tids[i])].name, insys[i], buf, wch);
            }
            kill(v->netacp_pid, SIGTERM);
            stopped = 1; t_stop = t;
        }
        if (stopped && t - t_stop > 30000u) {           /* it did not stop: kill */
            for (int i = 0; i < nt; i++) kill(tids[i], SIGKILL);
        }
        if (!stopped && v->netacp_pid <= 0 && root_status >= 0 && t - t0 > 120000u)
            break;                                       /* no NETACP ever came */
        if (p < 0) {
            if (werr == EINTR) continue;
            break;
        }
        if (cl->pid > 0 && p == cl->pid) {               /* the untraced client */
            if (WIFEXITED(st) || WIFSIGNALED(st)) { cl->st = st; cl->done = 1; }
            continue;
        }
        int k;
        for (k = 0; k < nt && tids[k] != p; k++)
            ;
        if (k == nt && nt < MAXT) { tids[nt] = p; insys[nt] = 0; newborn[nt] = 1; nt++; live++; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (p == root) root_status = st;
            if (p == v->netacp_pid) *netacp_status = st;
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
                        classify(v, p, &s);
                }
                insys[k] = !insys[k];
            }
        } else if (sig == SIGTRAP && (st >> 16)) {
            /* fork/clone/vfork/exec event: the new task arrives on its own;
             * until it execs it runs its creator's code */
            int ev = st >> 16;
            if (ev == PTRACE_EVENT_FORK || ev == PTRACE_EVENT_VFORK || ev == PTRACE_EVENT_CLONE) {
                unsigned long np = 0;
                if (ptrace(PTRACE_GETEVENTMSG, p, 0, &np) == 0) {
                    char nm[24];
                    snprintf(nm, sizeof nm, "%.15s>fork", g_lab[task_lab(p)].name);
                    task_set((pid_t)np, lab_of(nm));
                }
            }
        } else if (sig == SIGSTOP && k < nt && newborn[k]) {
            newborn[k] = 0;
        } else {
            deliver = sig;
        }
        (void)ptrace(PTRACE_SYSCALL, p, 0, (void *)(long)deliver);
    }
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    if (cl->pid > 0 && !cl->done) { waitpid(cl->pid, &cl->st, 0); cl->done = 1; }
    sink_drain(out);
    sink_drain(&cl->out);
    return root_status;
}

/* ---- the control child: exactly the forbidden calls --------------------- */
static void *ctl_thread(void *a) { return a; }
static void control_child(void)
{
    int pp[2];
    if (pipe(pp) == 0) { close(pp[0]); close(pp[1]); }
    struct pollfd pf = { -1, 0, 0 };
    (void)poll(&pf, 1, 0);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) close(s);
    pthread_t th;
    if (pthread_create(&th, NULL, ctl_thread, NULL) == 0)
        pthread_join(th, NULL);
    char *const av[] = { (char *)"DECNETD.EXE", NULL };
    (void)execv("/nonexistent/DECNETD.EXE", av);
    _exit(0);
}

static const char *pick(const char *env, const char *name)
{
    const char *e = getenv(env);
    if (e && *e) return e;
    static char p[256];
    snprintf(p, sizeof p, "/run/ovmx-boot/%s", name);
    return p;
}

static char g_out[32768], g_clout[32768];

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== startnet_audit (STARTNET + an inbound FAL stay off Linux libc, rd vms-0a7 S3) ===\n");

    /* ---- 0. the classifier works (anti-vacuity) ------------------------- */
    {
        struct verdict v; memset(&v, 0, sizeof v);
        struct client none; memset(&none, 0, sizeof none); none.pid = -1; none.done = 1;
        none.out.fd = -1;
        int ns = -1;
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            control_child();
        }
        int st = 0;
        waitpid(c, &st, 0);
        int rs = trace_all(c, &v, NULL, &none, &ns);
        CHECK(rs >= 0 && v.bad_pipe >= 1 && v.bad_wait >= 1 && v.bad_socket >= 1 &&
              v.bad_thread >= 1 && v.bad_exec >= 1,
              "control: the audit flags a pipe, a poll, a socket(AF_INET), a thread and an exec of another image");
    }

    if (argc < 2) {
        printf("  FAIL: usage: STARTNETAUDIT <spec of NETACP_TEST.EXE>\n");
        printf("=== startnet_audit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("  FAIL: no /dev/vms -- this audit runs on a booted node\n");
        printf("=== startnet_audit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    CHECK(!netacp_published(), "before STARTNET no NETACP serves this node (DNET$NETACP_REQ undefined)");

    const char *dcl = pick("OVMX_DCL", "DCL.EXE");
    static char client_img[512];
    snprintf(client_img, sizeof client_img, "%s", argv[1]);
    /* Two DCL procedures (DCL script mode), MASTERED on the test volume beside
     * this image (test_decnet_startnet_boot_e2e.sh): DCL opens a procedure
     * the VMS way only -- RMS through the Files-11 ACP (dcl_script.c, libc-drop
     * vms-20c94) -- so a host /tmp path is %DCL-E-OPENIN. The traced one starts
     * the network exactly as the system does; the untraced one copies through
     * 0:: as a user does. Both are named off argv[1]'s device:[directory]. */
    {
        const char *rb = strchr(client_img, ']');
        size_t dl = rb ? (size_t)(rb - client_img) + 1 : 0;
        if (!dl || dl + sizeof "SNAUDIT_CL.COM" > sizeof g_proc_cl) {
            printf("  FAIL: argv[1] '%s' names no device:[directory] for the audit's procedures\n",
                   client_img);
            return 1;
        }
        snprintf(g_proc_sn, sizeof g_proc_sn, "%.*sSNAUDIT_SN.COM", (int)dl, client_img);
        snprintf(g_proc_cl, sizeof g_proc_cl, "%.*sSNAUDIT_CL.COM", (int)dl, client_img);
    }

    /* ---- 1. trace DCL @SYS$MANAGER:STARTNET and what it makes ----------- */
    g_nlab = 0; g_ntask = 0;              /* the control's tallies are not the run's */
    int o[2];
    if (pipe(o) != 0) { printf("  FAIL: pipe\n"); return 1; }
    uint32_t cst = 0;
    pid_t c = ovmx_fork_subprocess_st(&cst);
    if (c == 0) {
        dup2(o[1], 1); dup2(o[1], 2);
        close(o[0]); close(o[1]);
        unsetenv("OVMX$CLI_FD");
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execl(dcl, "DCL.EXE", g_proc_sn, (char *)NULL);
        _exit(127);
    }
    close(o[1]);
    CHECK(c > 0, "the audit makes a VMS subprocess to run DCL");
    if (c <= 0) return 1;
    int st = 0;
    waitpid(c, &st, 0);
    fcntl(o[0], F_SETFL, O_NONBLOCK);
    struct sink out = { o[0], g_out, 0, sizeof g_out };
    struct client cl;
    memset(&cl, 0, sizeof cl);
    cl.image = client_img;
    cl.dcl = dcl;
    cl.out.fd = -1; cl.out.buf = g_clout; cl.out.cap = sizeof g_clout;
    struct verdict v;
    memset(&v, 0, sizeof v);
    int netacp_st = -1;
    int rs = trace_all(c, &v, &out, &cl, &netacp_st);
    if (cl.out.fd >= 0) close(cl.out.fd);
    close(o[0]);

    printf("  --- traced: DCL @SYS$MANAGER:STARTNET (and NCP, NETACP, FAL.EXE) ---\n%s\n"
           "  --- end ---\n", g_out);
    printf("  --- untraced client: DCL COPY to and from 0\"SYSTEM MANAGER\":: ---\n%s\n  --- end ---\n",
           g_clout);
    printf("  (STARTNET's DCL: %s %d)\n",
           rs >= 0 && WIFEXITED(rs) ? "exit" : "signal/broken",
           rs >= 0 && WIFEXITED(rs) ? WEXITSTATUS(rs) : (rs >= 0 && WIFSIGNALED(rs) ? WTERMSIG(rs) : -1));
    printf("  (syscalls %lu, executive ioctls %lu; exec DCL %u NCP %u NETACP %u FAL %u;"
           " flagged pipe %u wait %u thread %u host-socket %u foreign-exec %u%s%s)\n",
           v.total, v.vms_ioctls, v.exec_dcl, v.exec_ncp, v.exec_netacp, v.exec_fal,
           v.bad_pipe, v.bad_wait, v.bad_thread, v.bad_socket, v.bad_exec,
           v.first_bad[0] ? "; first: " : "", v.first_bad);

    for (int i = 0; i < g_nlab; i++)
        if (g_lab[i].pipe || g_lab[i].wait || g_lab[i].thread || g_lab[i].socket)
            printf("  (flagged in %s: pipe %u wait %u thread %u host-socket %u)\n", g_lab[i].name,
                   g_lab[i].pipe, g_lab[i].wait, g_lab[i].thread, g_lab[i].socket);
    printf("  (NETACP's exit: %s %d)\n",
           netacp_st >= 0 && WIFEXITED(netacp_st) ? "exit" : "signal/none",
           netacp_st >= 0 && WIFEXITED(netacp_st) ? WEXITSTATUS(netacp_st)
           : (netacp_st >= 0 && WIFSIGNALED(netacp_st) ? WTERMSIG(netacp_st) : -1));
    CHECK(rs >= 0 && WIFEXITED(rs) && WEXITSTATUS(rs) == 0, "@SYS$MANAGER:STARTNET completed with success");
    CHECK(v.exec_ncp >= 1, "STARTNET ran NCP (NCP SET EXECUTOR STATE ON) under the trace");
    CHECK(v.exec_netacp == 1, "NCP created NETACP (NETACP.EXE exec'd under the trace, once)");
    CHECK(strstr(g_clout, "SNAUDIT-CLIENT-END") != NULL && strstr(g_clout, "SNAUDIT-FAIL") == NULL &&
          strstr(g_clout, "$ NCP SET EXECUTOR STATE ON") != NULL,
          "the client COPYed STARTNET.COM to 0\"SYSTEM MANAGER\":: (FAL.EXE) and back through NETACP, and the copy reads back");
    CHECK(v.exec_fal >= 1, "NETACP created FAL.EXE for the inbound object-17 connect, under the trace");
    CHECK(netacp_st >= 0 && WIFEXITED(netacp_st) && WEXITSTATUS(netacp_st) == 0,
          "NETACP stopped on SIGTERM and exited 0");
    CHECK(v.total > 200 && v.vms_ioctls > 50, "the trace is not vacuous: it saw the tasks' executive (/dev/vms) calls");
    CHECK(v.bad_pipe == 0, "NO host pipe (pipe/pipe2) in DCL, NCP, NETACP or FAL");
    CHECK(v.bad_wait == 0, "NO poll/ppoll/select/epoll wait");
    CHECK(v.bad_thread == 0, "NO thread (clone CLONE_THREAD / clone3)");
    CHECK(v.bad_socket == 0, "NO host socket (AF_INET/INET6/PACKET/NETLINK)");
    CHECK(v.bad_exec == 0, "NO exec of anything but DCL, NCP, NETACP and FAL (never DECNETD.EXE)");

    printf("=== startnet_audit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
