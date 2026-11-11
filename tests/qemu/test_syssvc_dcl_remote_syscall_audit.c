/*
 * test_syssvc_dcl_remote_syscall_audit.c - DCL DIRECTORY, TYPE and COPY of a
 * node:: spec reach the network through RMS and the executive, never through
 * Linux libc (rd vms-0a7 slice S1; Baron 2026-11-02: "don't drop to linux
 * LIBC, use all VMS-authentic API").
 *
 * WHAT IS AUDITED. Since S1, DCL's DIRECTORY / TYPE / COPY of a node:: spec are
 * plain RMS clients: RMS ($PARSE/$SEARCH/$OPEN/$GET/$CREATE/$PUT, src/vmsrms/
 * rms_remote.c) $ASSIGNs _NET: and talks DAP to the node's FAL with $QIOW
 * IO$_ACCESS / WRITEVBLK / READVBLK / DEACCESS; DCL no longer activates
 * SYS$SYSTEM:DECNETD.EXE for them. This suite runs the shipped DCL.EXE, one
 * command per process (`DCL.EXE -c "<command>"`), as a VMS subprocess the
 * executive created (ovmx_fork_subprocess_st), under ptrace from its first
 * instruction, and sees every system call of it -- and of any process or
 * thread it makes (PTRACE_O_TRACEFORK / TRACECLONE / TRACEVFORK) -- at its
 * entry stop. Across the commands it asserts NONE is
 *
 *   - pipe / pipe2                                    (a host pipe);
 *   - poll / ppoll / select / pselect6 / epoll_wait / epoll_pwait
 *                                                     (a host descriptor wait);
 *   - clone with CLONE_THREAD, or clone3              (a thread);
 *   - socket(AF_INET / AF_INET6 / AF_PACKET / AF_NETLINK)
 *                                                     (host networking);
 *   - execve / execveat of anything but the DCL.EXE under trace (DECNETD.EXE
 *     above all: the image DCL used to run for these verbs).
 *
 * NOT VACUOUS. A trace that saw nothing passes all of that, so for each
 * command the suite also asserts what the run DID: the tracer saw its /dev/vms
 * executive ioctls (magic 'V') and the command's own output -- and the
 * classifier itself is proven on a control child that makes exactly the
 * forbidden calls (a pipe, a poll, a socket(AF_INET), a thread, an exec of
 * another image): each must be flagged, or the audit measures nothing.
 *
 * TWO PLACES IT RUNS, ONE BINARY.
 *
 *  - The kernel-executive harness (tests/qemu/init.sh runs it with no
 *    argument). The rail has the executive and a NIC -- so a _NET: device --
 *    but NO NETACP and no FAL. Each command must still take the RMS remote
 *    path through the executive and fail HONESTLY there: DCL's own error, a
 *    failure status, executive ioctls in the trace -- and the same forbidden-
 *    call audit. This is where the facility negative control runs.
 *  - A booted node with a RUNNING NETACP and FAL (tests/qemu/
 *    test_decnet_startnet_boot_e2e.sh, boot 2), argument NETACP. The e2e
 *    masters this binary onto a scratch ODS-2 volume, MOUNTs it, and runs it as
 *    a foreign command from the logged-in SYSTEM session (the same way a user
 *    runs any image off a mounted volume -- the shipped system disk carries no
 *    audit tool). There the commands must SUCCEED over NETACP's local loopback
 *    to a FAL.EXE server process: DIRECTORY lists the remote login directory
 *    ("Total of"), TYPE prints the remote record, COPY brings a remote file
 *    here and sends a local file there, and each copy is read back.
 *
 * WHY PTRACE AROUND A WHOLE DCL PROCESS, ONE COMMAND EACH. The property is
 * about what the user-visible command does, so the subject is the shipped
 * DCL.EXE itself, not a library call in this process. One `-c` command per
 * traced process attributes every call to exactly one verb.
 *
 * NEGATIVE CONTROL (a product mutation that must redden this suite):
 * tests/qemu/defects.d/rms-remote-link-via-host-pipe.defect.
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

/* The node:: spec every command names: node 0 (this node), SYSTEM's login. */
#define NODE   "0\"SYSTEM MANAGER\"::"
#define SYSMGR "SYS$SYSROOT:[SYSMGR]"
/* The rail's generated system-disk ODS-2 fixture (tests/qemu/run_tests.sh). */
#define RAIL_UNIT "VDA300:"

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
    /* Linux/Alpha has no PTRACE_GETREGSET (no user_regset view: the call
     * fails, every stop would be dropped and the trace would see nothing).
     * Read the registers one at a time by number, as strace does there --
     * v0 ($0) carries the syscall number at the syscall-entry stop, a0..a2
     * are $16..$18. */
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
struct verdict {
    unsigned long total;      /* syscall entries seen */
    unsigned long vms_ioctls; /* ioctls whose request carries the 'V' magic */
    unsigned bad_pipe, bad_wait, bad_thread, bad_socket, bad_exec;
    unsigned execs;           /* execve/execveat entries, the allowed one included */
    const char *allow_exec;   /* the one image the root may exec (its own DCL) */
    char first_bad[256];
};

static void flag(struct verdict *v, unsigned *ctr, const char *what, const struct sc *s)
{
    (*ctr)++;
    if (!v->first_bad[0])
        snprintf(v->first_bad, sizeof v->first_bad, "%s (nr %ld, a0 %#lx, a1 %#lx)",
                 what, s->nr, s->a0, s->a1);
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
        if (((req >> 8) & 0xFF) == 'V')
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
        /* x86_64/aarch64/alpha: flags are the first argument */
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
        /* The ONE exec allowed: the root task entering the DCL under audit
         * (its very first call after the trace starts). */
        if (pid == root && v->execs == 1 && v->allow_exec &&
            strcmp(path, v->allow_exec) == 0)
            return;
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
        int werr = (p < 0) ? errno : 0;  /* before the drain's EAGAIN clobbers it */
        sink_drain(out);
        if (p < 0) {
            if (werr == EINTR) continue;
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
    /* an exec of an image that is not the allowed one (it need not exist:
     * the entry is what is classified) */
    char *const av[] = { (char *)"DECNETD.EXE", NULL };
    (void)execv("/nonexistent/DECNETD.EXE", av);
    _exit(0);
}

/* ---- one traced DCL command --------------------------------------------- */
struct run {
    const char *label;        /* DIRECTORY / TYPE / COPY remote->here ... */
    const char *cmd;
    struct verdict v;
    int rs;                   /* waitpid status of DCL, -1 if the trace broke */
    int made;                 /* the VMS subprocess was created */
    char out[16384];
};

static const char *g_dcl;

static void run_dcl(struct run *r, int bound_s)
{
    memset(&r->v, 0, sizeof r->v);
    r->v.allow_exec = g_dcl;
    r->rs = -1; r->made = 0; r->out[0] = '\0';

    int o[2];
    if (pipe(o) != 0) return;          /* the TRACER's pipe, made before the trace */
    uint32_t cst = 0;
    pid_t c = ovmx_fork_subprocess_st(&cst);
    if (c == 0) {
        dup2(o[1], 1);
        dup2(o[1], 2);
        close(o[0]); close(o[1]);
        unsetenv("OVMX$CLI_MBX");       /* the subject is its own DCL, not a callback */
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execl(g_dcl, "DCL.EXE", "-c", r->cmd, (char *)NULL);
        _exit(127);
    }
    close(o[1]);
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
    printf("  --- end; exit %s%d; syscalls %lu, executive ioctls %lu; pipe %u wait %u"
           " thread %u host-socket %u foreign-exec %u%s%s ---\n",
           (r->rs >= 0 && WIFEXITED(r->rs)) ? "" : "signal/broken ",
           (r->rs >= 0 && WIFEXITED(r->rs)) ? WEXITSTATUS(r->rs) : -1,
           r->v.total, r->v.vms_ioctls, r->v.bad_pipe, r->v.bad_wait, r->v.bad_thread,
           r->v.bad_socket, r->v.bad_exec, r->v.first_bad[0] ? "; first: " : "",
           r->v.first_bad);
}

static int exited_ok(const struct run *r)
{
    return r->rs >= 0 && WIFEXITED(r->rs) && WEXITSTATUS(r->rs) == 0;
}
static int exited_fail(const struct run *r)
{
    return r->rs >= 0 && WIFEXITED(r->rs) && WEXITSTATUS(r->rs) != 0;
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
    printf("=== test_syssvc_dcl_remote_syscall_audit (DIRECTORY/TYPE/COPY node:: go through"
           " RMS + the executive, rd vms-0a7) [%s] ===\n",
           netacp ? "booted node: NETACP + FAL" : "executive rail: no NETACP");

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
        int st = 0;
        waitpid(c, &st, 0);
        int rs = trace_all(c, &v, 30, NULL);
        CHECK(rs >= 0 && v.bad_pipe >= 1 && v.bad_wait >= 1 && v.bad_socket >= 1 &&
              v.bad_thread >= 1 && v.bad_exec >= 1,
              "control: the audit flags a pipe, a poll, a socket(AF_INET), a thread and an exec of another image");
    }

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_remote_syscall_audit: %d passed, %d failed (SKIPPED: no"
               " /dev/vms -- _NET: and the FAL link are executive-resident) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    vms_kif_close();

    g_dcl = pick_dcl();
    if (access(g_dcl, X_OK) != 0) {
        /* The harness stages it (tests/qemu/Dockerfile); its absence is a
         * harness defect, not a skip. */
        printf("  FAIL: %s is not staged\n", g_dcl);
        printf("=== test_syssvc_dcl_remote_syscall_audit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    printf("  (DCL under audit: %s)\n", g_dcl);

    static struct run R[6];
    struct run *dir = &R[0], *typ = &R[1], *get = &R[2], *put = &R[3];
    struct run *getback = &R[4], *putback = &R[5];
    dir->label = "DIRECTORY node::";
    dir->cmd   = "DIRECTORY " NODE;
    typ->label = "TYPE node::file";
    typ->cmd   = "TYPE " NODE SYSMGR "NETLB_SRC.TXT";
    get->label = "COPY node::file here";
    get->cmd   = "COPY " NODE SYSMGR "NETLB_SRC.TXT " SYSMGR "DCLAUDIT_GET.TXT";
    put->label = "COPY here node::file";
    /* The local source must exist, or COPY fails before it reaches the node:
     * the NETLB record on the booted node; on the rail, the shipped SYSUAF.DAT
     * on the generated system-disk ODS-2 fixture (run_tests.sh puts it on
     * VDA300:, which this suite $MOUNTs for the one command, as
     * test_syssvc_sysuaf_uic_base does). */
    put->cmd   = netacp ? "COPY " SYSMGR "NETLB_SRC.TXT " NODE SYSMGR "DCLAUDIT_PUT.TXT"
                        : "COPY " RAIL_UNIT "[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT " NODE SYSMGR
                          "DCLAUDIT_PUT.TXT";
    getback->label = "read-back: TYPE the file COPY brought here";
    getback->cmd   = "TYPE " SYSMGR "DCLAUDIT_GET.TXT";
    putback->label = "read-back: TYPE node:: the file COPY sent there";
    putback->cmd   = "TYPE " NODE SYSMGR "DCLAUDIT_PUT.TXT";

    struct run *audited[] = { dir, typ, get, put, putback };
    const int n_audited = netacp ? 5 : 4;   /* the remote read-back needs a FAL */

    for (int i = 0; i < 3; i++)
        run_dcl(&R[i], 120);
    if (netacp) {
        run_dcl(put, 120);
    } else {
        uint32_t mst = vms_kif_acp_mount(RAIL_UNIT);
        printf("  ($MOUNT %s for COPY's local source: %%X%08X)\n", RAIL_UNIT, (unsigned)mst);
        run_dcl(put, 120);
        if (mst & 1)
            (void)vms_kif_acp_dmount(RAIL_UNIT);
    }
    if (netacp) {
        run_dcl(getback, 120);
        run_dcl(putback, 120);
    }

    for (int i = 0; i < 4; i++) {
        char m[256];
        snprintf(m, sizeof m, "%s: the shipped DCL ran it as a VMS subprocess under the trace"
                 " and reached the executive (/dev/vms ioctls seen)", R[i].label);
        CHECK(R[i].made && R[i].rs >= 0 && R[i].v.total > 20 && R[i].v.vms_ioctls > 0, m);
    }

    if (netacp) {
        /* The commands' REAL output, over NETACP's loopback to FAL.EXE. The
         * NETLB files are what the e2e's brokered-link proof left in
         * SYSTEM's SYS$SYSROOT:[SYSMGR] (one record each). */
        CHECK(exited_ok(dir) && has(dir, "Directory 0\"SYSTEM password\"::") &&
              has(dir, "NETLB_SRC.TXT;") && has(dir, "Total of "),
              "DIRECTORY node:: listed the remote directory through the FAL (header, NETLB_SRC.TXT, 'Total of')");
        CHECK(exited_ok(typ) && has(typ, "NETACP-brokered _NET: COPY over the local loopback"),
              "TYPE node::file printed the remote file's record");
        CHECK(exited_ok(get) && exited_ok(getback) &&
              has(getback, "NETACP-brokered _NET: COPY over the local loopback"),
              "COPY node::file here: the copy exists here and holds the remote record (read back)");
        CHECK(exited_ok(put) && exited_ok(putback) &&
              has(putback, "NETACP-brokered _NET: COPY over the local loopback"),
              "COPY here node::file: the copy exists on the node and holds the local record (read back through the FAL)");
        {
            char m[256];
            snprintf(m, sizeof m, "%s: the shipped DCL ran it under the trace and reached the"
                     " executive (/dev/vms ioctls seen)", putback->label);
            CHECK(putback->made && putback->rs >= 0 && putback->v.total > 20 &&
                  putback->v.vms_ioctls > 0, m);
        }
    } else {
        /* No NETACP on this rail: each verb still goes down the RMS remote
         * path through the executive and fails honestly AT THE NODE:: SPEC --
         * a DCL error naming that spec (password masked) and a failure
         * status, never a listing or a record, never a DECNETD helper. */
        int honest = 1;
        for (int i = 0; i < 4; i++)
            if (!exited_fail(&R[i]) || !strchr(R[i].out, '%') ||
                !has(&R[i], "0\"SYSTEM password\"::") || has(&R[i], "MANAGER") ||
                has(&R[i], "Total of ") || has(&R[i], "DECNETD"))
                honest = 0;
        CHECK(honest,
              "with no NETACP, DIRECTORY / TYPE / COPY (both directions) of a node:: spec fail at the node:: spec with a DCL error and a failure status");
    }

    unsigned pipes = 0, waits = 0, threads = 0, socks = 0, execs = 0;
    for (int i = 0; i < n_audited; i++) {
        pipes   += audited[i]->v.bad_pipe;
        waits   += audited[i]->v.bad_wait;
        threads += audited[i]->v.bad_thread;
        socks   += audited[i]->v.bad_socket;
        execs   += audited[i]->v.bad_exec;
    }
    printf("  (audited commands: %d; pipe %u, wait %u, thread %u, host socket %u,"
           " foreign exec %u)\n", n_audited, pipes, waits, threads, socks, execs);

    /* negctl: rms-remote-link-via-host-pipe */
    CHECK(pipes == 0, "NO pipe/pipe2 in DIRECTORY, TYPE or COPY (either direction) of a node:: spec");
    CHECK(waits == 0, "NO poll/ppoll/select/pselect6/epoll wait in DIRECTORY, TYPE or COPY of a node:: spec");
    CHECK(threads == 0, "NO thread (clone CLONE_THREAD / clone3) in DIRECTORY, TYPE or COPY of a node:: spec");
    CHECK(socks == 0, "NO socket(AF_INET/INET6/PACKET/NETLINK) in DIRECTORY, TYPE or COPY of a node:: spec");
    CHECK(execs == 0, "NO exec of DECNETD.EXE or any image but the DCL under audit in DIRECTORY, TYPE or COPY of a node:: spec");

    printf("=== test_syssvc_dcl_remote_syscall_audit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
