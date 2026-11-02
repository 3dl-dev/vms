/*
 * test_syssvc_sethost_syscall_audit.c - $ SET HOST reaches the remote node
 * through RTPAD.EXE, CLI$GET_VALUE, $QIO on _NET: and $QIO on the terminal --
 * never Linux libc, never DECNETD.EXE (rd vms-0a7 slice S2, vms-18c9; Baron
 * 2026-11-02: "don't drop to linux LIBC, use all VMS-authentic API").
 *
 * WHAT IS AUDITED. DCL's SET HOST checks the command against DCL$TABLES,
 * records it in the executive CLI context and activates SYS$SYSTEM:RTPAD.EXE,
 * which reads NODE with CLI$GET_VALUE and opens the CTERM link (object 42)
 * with $QIOW IO$_ACCESS on _NET:. This suite runs the shipped DCL.EXE with one
 * `-c "SET HOST ..."` per process, as a VMS subprocess the executive created,
 * under ptrace from its first instruction, following every process and thread
 * it makes, and asserts NONE of them does
 *
 *   - pipe / pipe2                                    (a host pipe);
 *   - poll / ppoll / select / pselect6 / epoll_wait / epoll_pwait
 *                                                     (a host descriptor wait);
 *   - clone with CLONE_THREAD, or clone3              (a thread);
 *   - socket(AF_INET / AF_INET6 / AF_PACKET / AF_NETLINK)
 *                                                     (host networking);
 *   - execve of anything but that DCL and SYS$SYSTEM:RTPAD.EXE (DECNETD.EXE
 *     above all: the image SET HOST used to run with --set-host);
 *   - ioctl(TCGETS) -- isatty()/tcgetattr() -- from RTPAD.EXE: whether
 *     SYS$INPUT is a terminal is the executive's answer ($GETDVI), not the
 *     substrate tty's.
 *
 * NOT VACUOUS. For each command the trace must see RTPAD.EXE activated, its
 * /dev/vms executive ioctls, and its real output; and the classifier itself is
 * proven on a control child that makes exactly the forbidden calls.
 *
 * TWO PLACES IT RUNS, ONE BINARY.
 *  - The kernel-executive harness (tests/qemu/init.sh, no argument): an
 *    executive and a NIC, so a _NET: device, but NO NETACP. SET HOST 0 must
 *    reach RTPAD, which must take NODE from the CLI (the link refusal, not a
 *    missing-parameter error, is what it reports) and fail honestly at the
 *    link -- under the same audit.
 *  - A booted node with a RUNNING NETACP (argument NETACP): SET HOST 0 goes
 *    over NETACP's local loopback to its own CTERM object, the node's LOGINOUT
 *    challenges, SYSTEM logs in, runs SHOW TIME, logs out, and control returns
 *    with %REM-S-END. SYS$INPUT is a file there (the tracer's script), so
 *    RTPAD reads one record per host solicit.
 *
 * NEGATIVE CONTROL (a product mutation that must redden this suite):
 * tests/qemu/defects.d/rtpad-terminal-via-isatty.defect.
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
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <termios.h>      /* TCGETS: what isatty() issues */
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

/* ---- the classifier ------------------------------------------------------ */
#define MAXRT 8
struct verdict {
    unsigned long total;      /* syscall entries seen */
    unsigned long vms_ioctls; /* ioctls whose request carries the 'V' magic */
    unsigned long rt_ioctls;  /* ... of them, from RTPAD.EXE */
    unsigned bad_pipe, bad_wait, bad_thread, bad_socket, bad_exec, bad_tty;
    unsigned execs;           /* execve/execveat entries, the allowed ones included */
    unsigned rtpad_execs;     /* activations of SYS$SYSTEM:RTPAD.EXE */
    const char *allow_exec;   /* the one image the root may exec (its own DCL) */
    pid_t rt[MAXRT]; int nrt; /* the tasks that became RTPAD.EXE */
    char first_bad[256];
};

static void flag(struct verdict *v, unsigned *ctr, const char *what, const struct sc *s)
{
    (*ctr)++;
    if (!v->first_bad[0])
        snprintf(v->first_bad, sizeof v->first_bad, "%s (nr %ld, a0 %#lx, a1 %#lx)",
                 what, s->nr, s->a0, s->a1);
}

static int is_rtpad(const struct verdict *v, pid_t pid)
{
    for (int i = 0; i < v->nrt; i++)
        if (v->rt[i] == pid) return 1;
    return 0;
}

static int ends_with(const char *s, const char *t)
{
    size_t a = strlen(s), b = strlen(t);
    return a >= b && strcasecmp(s + a - b, t) == 0;
}

static void classify(struct verdict *v, pid_t pid, pid_t root, const struct sc *s)
{
    v->total++;
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
#ifdef SYS_socketcall
    if (nr == SYS_socketcall && s->a0 == 1 /* SYS_SOCKET */) {
        flag(v, &v->bad_socket, "socketcall(SOCKET)", s);
        return;
    }
#endif
    if (nr == SYS_ioctl) {
        unsigned long req = s->a1 & 0xFFFFFFFFul;
        if (((req >> 8) & 0xFF) == 'V') {
            v->vms_ioctls++;
            if (is_rtpad(v, pid)) v->rt_ioctls++;
        }
        if (req == (unsigned long)TCGETS && is_rtpad(v, pid))
            flag(v, &v->bad_tty, "ioctl(TCGETS) from RTPAD.EXE (isatty)", s);
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
        char path[512];
        unsigned long pa = s->a0;
#ifdef SYS_execveat
        if (nr == SYS_execveat) pa = s->a1;
#endif
        peek_str(pid, pa, path, sizeof path);
        v->execs++;
        /* The root task entering the DCL under audit (its first call). */
        if (pid == root && v->execs == 1 && v->allow_exec &&
            strcmp(path, v->allow_exec) == 0)
            return;
        /* DCL activating SYS$SYSTEM:RTPAD.EXE: the one image SET HOST runs. */
        if (ends_with(path, "/SYSEXE/RTPAD.EXE") || ends_with(path, "/RTPAD.EXE")) {
            v->rtpad_execs++;
            if (!is_rtpad(v, pid) && v->nrt < MAXRT) v->rt[v->nrt++] = pid;
            return;
        }
        v->bad_exec++;
        if (!v->first_bad[0])
            snprintf(v->first_bad, sizeof v->first_bad, "execve(\"%.200s\")", path);
        return;
    }
}

static volatile pid_t g_root;
static void on_bound(int sig) { (void)sig; if (g_root > 0) kill(g_root, SIGKILL); }

/* The traced command's output, drained while it runs. */
struct sink { int fd; char *buf; size_t len, cap; };

static void sink_drain(struct sink *k)
{
    if (!k || k->fd < 0) return;
    for (;;) {
        if (k->len + 1 >= k->cap) {
            /* full: keep reading so the writer never blocks, but drop it */
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

/*
 * trace_all - run the tracee (already stopped at its SIGSTOP after
 * PTRACE_TRACEME) and every process/thread it makes to completion, feeding
 * each syscall-ENTRY stop to classify(). Returns the first tracee's exit
 * status (waitpid form), or -1 if the trace broke.
 */
static int trace_all(pid_t root, struct verdict *v, int bound_s, struct sink *out)
{
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    enum { MAXT = 64 };
    pid_t tids[MAXT]; int insys[MAXT]; int newborn[MAXT]; int nt = 0;
    int root_status = -1, live = 1;

    if (ptrace(PTRACE_SETOPTIONS, root, 0, (void *)opts) != 0)
        return -1;
    g_root = root;
    signal(SIGALRM, on_bound);
    alarm((unsigned)bound_s);         /* a wedged tracee is killed, not waited on */
    tids[nt] = root; insys[nt] = 0; newborn[nt] = 0; nt++;
    if (ptrace(PTRACE_SYSCALL, root, 0, 0) != 0)
        return -1;

    while (live > 0) {
        int st = 0;
        pid_t p = waitpid(-1, &st, __WALL);
        sink_drain(out);
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
                        classify(v, p, root, &s);
                }
                insys[k] = !insys[k];
            }
        } else if (sig == SIGTRAP && (st >> 16)) {
            /* a fork/clone/vfork/exec event: the new task arrives on its own.
             * After an exec the task resumes at the exec's EXIT stop. */
        } else if (sig == SIGSTOP && k < nt && newborn[k]) {
            newborn[k] = 0;            /* a new task's birth stop: swallow it */
        } else {
            deliver = sig;
        }
        (void)ptrace(PTRACE_SYSCALL, p, 0, (void *)(long)deliver);
    }
    alarm(0);
    sink_drain(out);
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
    (void)isatty(0);                     /* TCGETS: this task counts as RTPAD */
    char *const av[] = { (char *)"DECNETD.EXE", NULL };
    (void)execv("/nonexistent/DECNETD.EXE", av);
    _exit(0);
}

/* ---- one traced DCL command --------------------------------------------- */
struct run {
    const char *label;
    const char *cmd;
    const char *input;        /* SYS$INPUT: the lines RTPAD reads, or NULL */
    struct verdict v;
    int rs;                   /* waitpid status of DCL, -1 if the trace broke */
    int made;
    char out[32768];
};

static const char *g_dcl;

static void run_dcl(struct run *r, int bound_s)
{
    memset(&r->v, 0, sizeof r->v);
    r->v.allow_exec = g_dcl;
    r->rs = -1; r->made = 0; r->out[0] = '\0';

    int o[2], in[2] = { -1, -1 };
    if (pipe(o) != 0) return;          /* the TRACER's pipes, made before the trace */
    if (r->input && pipe(in) != 0) { close(o[0]); close(o[1]); return; }
    uint32_t cst = 0;
    pid_t c = ovmx_fork_subprocess_st(&cst);
    if (c == 0) {
        dup2(o[1], 1);
        dup2(o[1], 2);
        close(o[0]); close(o[1]);
        if (in[0] >= 0) { dup2(in[0], 0); close(in[0]); close(in[1]); }
        else {
            int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) { dup2(nul, 0); close(nul); }
        }
        unsetenv("OVMX$CLI_FD");
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execl(g_dcl, "DCL.EXE", "-c", r->cmd, (char *)NULL);
        _exit(127);
    }
    close(o[1]);
    if (in[0] >= 0) {
        close(in[0]);
        /* the whole script fits a pipe buffer; EOF follows it */
        if (write(in[1], r->input, strlen(r->input)) < 0) { /* reported below */ }
        close(in[1]);
    }
    if (c <= 0) { close(o[0]); return; }
    r->made = 1;
    int st = 0;
    waitpid(c, &st, 0);
    fcntl(o[0], F_SETFL, O_NONBLOCK);
    struct sink k = { o[0], r->out, 0, sizeof r->out };
    r->rs = trace_all(c, &r->v, bound_s, &k);
    close(o[0]);

    printf("  --- %s: $ %s ---\n%s", r->label, r->cmd, r->out);
    if (k.len && r->out[k.len - 1] != '\n') printf("\n");
    printf("  --- end; exit %s%d; syscalls %lu, executive ioctls %lu (RTPAD %lu);"
           " RTPAD activations %u; pipe %u wait %u thread %u host-socket %u"
           " foreign-exec %u rtpad-isatty %u%s%s ---\n",
           (r->rs >= 0 && WIFEXITED(r->rs)) ? "" : "signal/broken ",
           (r->rs >= 0 && WIFEXITED(r->rs)) ? WEXITSTATUS(r->rs) : -1,
           r->v.total, r->v.vms_ioctls, r->v.rt_ioctls, r->v.rtpad_execs,
           r->v.bad_pipe, r->v.bad_wait, r->v.bad_thread, r->v.bad_socket,
           r->v.bad_exec, r->v.bad_tty, r->v.first_bad[0] ? "; first: " : "",
           r->v.first_bad);
}

static int has(const struct run *r, const char *s) { return strstr(r->out, s) != NULL; }

static const char *pick_dcl(void)
{
    const char *e = getenv("OVMX_DCL");
    if (e && *e) return e;
    static const char *cand[] = {
        "/run/ovmx-boot/DCL.EXE",   /* a booted node: the boot-staged SYS$SYSTEM image */
        "/tests/DCL.EXE",           /* the kernel-executive harness (tests/qemu/Dockerfile) */
        "/bin/DCL.EXE",
    };
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++)
        if (access(cand[i], X_OK) == 0) return cand[i];
    return cand[0];
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int netacp = argc > 1 && strcasecmp(argv[1], "NETACP") == 0;
    printf("=== test_syssvc_sethost_syscall_audit (SET HOST is RTPAD.EXE over _NET: and"
           " the terminal's \$QIO, rd vms-0a7 S2) [%s] ===\n",
           netacp ? "booted node: NETACP" : "executive rail: no NETACP");

    /* ---- 0. the classifier works (anti-vacuity) ------------------------- */
    {
        struct verdict v;
        memset(&v, 0, sizeof v);
        v.allow_exec = "/never/DCL.EXE";
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            control_child();
        }
        v.rt[v.nrt++] = c;              /* the control child stands in for RTPAD */
        int st = 0;
        waitpid(c, &st, 0);
        int rs = trace_all(c, &v, 30, NULL);
        CHECK(rs >= 0 && v.bad_pipe >= 1 && v.bad_wait >= 1 && v.bad_socket >= 1 &&
              v.bad_thread >= 1 && v.bad_exec >= 1 && v.bad_tty >= 1,
              "control: the audit flags a pipe, a poll, a socket(AF_INET), a thread, an isatty and an exec of another image");
    }

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_sethost_syscall_audit: %d passed, %d failed (SKIPPED: no"
               " /dev/vms -- _NET: and the CLI context are executive-resident) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    vms_kif_close();

    g_dcl = pick_dcl();
    if (access(g_dcl, X_OK) != 0) {
        printf("  FAIL: %s is not staged\n", g_dcl);
        printf("=== test_syssvc_sethost_syscall_audit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    printf("  (DCL under audit: %s)\n", g_dcl);

    static struct run R[2];
    struct run *sh = &R[0], *bad = &R[1];
    sh->label = "SET HOST 0";
    sh->cmd = "SET HOST 0";
    /* SYSTEM's password on the booted node is the shipped MANAGER */
    sh->input = netacp ? "SYSTEM\rMANAGER\rSHOW TIME\rLOGOUT\r" : "SYSTEM\r";
    bad->label = "SET HOST to a node the database does not know";
    bad->cmd = "SET HOST NOSUCH";
    bad->input = NULL;
    run_dcl(sh, netacp ? 240 : 120);
    run_dcl(bad, 120);

    for (int i = 0; i < 2; i++) {
        char m[256];
        snprintf(m, sizeof m, "%s: DCL activated SYS$SYSTEM:RTPAD.EXE under the trace and"
                 " RTPAD reached the executive (/dev/vms ioctls from RTPAD seen)", R[i].label);
        CHECK(R[i].made && R[i].rs >= 0 && R[i].v.rtpad_execs == 1 && R[i].v.rt_ioctls > 0, m);
    }

    if (netacp) {
        CHECK(has(sh, "Username:") && has(sh, "Password:"),
              "SET HOST 0: the node's LOGINOUT challenged over the CTERM link");
        CHECK(!has(sh, "MANAGER"), "SET HOST 0: the password was not echoed");
        CHECK(has(sh, "logged out at"), "SET HOST 0: SYSTEM logged in, ran SHOW TIME, logged out");
        CHECK(has(sh, "%REM-S-END, control returned to node "),
              "SET HOST 0: control returned with %REM-S-END");
        CHECK(has(bad, "%SYSTEM-F-NOSUCHNODE, remote node is unknown") && !has(bad, "DCL-E-ABORT"),
              "SET HOST NOSUCH: %SYSTEM-F-NOSUCHNODE, once, with no image line under it");
    } else {
        /* No NETACP: RTPAD took NODE from the CLI (it is the link it reports,
         * not a missing parameter) and failed honestly at the link. */
        int honest = 1;
        for (int i = 0; i < 2; i++)
            if (!strchr(R[i].out, '%') || has(&R[i], "BADPARAM") || has(&R[i], "Username:") ||
                has(&R[i], "DECNETD") || has(&R[i], "REM-S-END"))
                honest = 0;
        CHECK(honest,
              "with no NETACP, SET HOST reaches RTPAD, which reads its node from the CLI and fails honestly at the link (no session, no DECNETD)");
    }

    unsigned pipes = 0, waits = 0, threads = 0, socks = 0, execs = 0, ttys = 0;
    for (int i = 0; i < 2; i++) {
        pipes   += R[i].v.bad_pipe;
        waits   += R[i].v.bad_wait;
        threads += R[i].v.bad_thread;
        socks   += R[i].v.bad_socket;
        execs   += R[i].v.bad_exec;
        ttys    += R[i].v.bad_tty;
    }
    printf("  (audited commands: 2; pipe %u, wait %u, thread %u, host socket %u,"
           " foreign exec %u, RTPAD isatty %u)\n", pipes, waits, threads, socks, execs, ttys);
    CHECK(pipes == 0, "NO pipe/pipe2 in SET HOST");
    CHECK(waits == 0, "NO poll/ppoll/select/pselect6/epoll wait in SET HOST");
    CHECK(threads == 0, "NO thread (clone CLONE_THREAD / clone3) in SET HOST");
    CHECK(socks == 0, "NO socket(AF_INET/INET6/PACKET/NETLINK) in SET HOST");
    CHECK(execs == 0, "NO exec of DECNETD.EXE or any image but DCL and RTPAD.EXE in SET HOST");
    /* negctl: rtpad-terminal-via-isatty */
    CHECK(ttys == 0, "NO isatty / ioctl(TCGETS) from RTPAD.EXE in SET HOST");

    printf("=== test_syssvc_sethost_syscall_audit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
