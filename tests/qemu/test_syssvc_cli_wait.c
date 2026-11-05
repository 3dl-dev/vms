/*
 * test_syssvc_cli_wait.c - DCL's wait for a fork-path image is one executive
 * mailbox read, not a host poll (rd vms-6d9; Baron 2026-11-02: "don't drop to
 * linux LIBC, use all VMS-authentic API").
 *
 * PART A -- THE EXECUTIVE. The suite plays DCL's side exactly as
 * src/vmsdcl/dcl_cmd_process.c does: it creates a REQUEST and a REPLY
 * temporary mailbox, forks a task through the executive's creation record
 * (ovmx_creprc_fork_mbx, as dcl_image_fork_mbx does), and makes it its image
 * task (VMS_CREPRC_PCB_CONTINUE) naming the request mailbox as the task's
 * TERMINATION MAILBOX (vms_kif_creprc_pcb_mbx, $CREPRC mbxunt). Asserted:
 *   - a unit the creator holds no channel to is refused SS$_NOSUCHDEV, and the
 *     held task is released;
 *   - the task's LIB$SET_SYMBOL (the real RTL, OVMX$CLI_MBX naming the
 *     mailboxes) arrives as one request message in the request mailbox, and
 *     the CLI's reply reaches the caller as its status;
 *   - when the task ends, the EXECUTIVE writes a termination message
 *     (VMS_EXEC_MBXMSG_TERM) into the same mailbox -- the task's own substrate
 *     id, the VMS PID it ran as, and the final condition it recorded -- so a
 *     single blocking read sees a CLI request or the task's end.
 *
 * PART B -- DCL ITSELF. A login subprocess has ptrace(PTRACE_TRACEME) and execs
 * the shipped /bin/DCL.EXE, which RUNs this very binary as a fork-path image
 * (image mode: it sees OVMX$CLI_MBX, sets a global symbol, exits) and then
 * shows the symbol. Every system call of DCL (and of its threads) is seen at
 * its entry stop; the image task (a fork, not traced) is not DCL. Asserted:
 * DCL made NO poll / ppoll / select / pselect6 / epoll_wait / epoll_pwait and
 * NO socketpair / AF_UNIX socket, and the image's symbol is in DCL's table.
 * NOT VACUOUS: the trace saw DCL's /dev/vms ioctls, and the classifier flags a
 * control child that calls poll().
 *
 * CTRL/Y (the executive stopping the image and telling DCL's mailbox) needs a
 * keystroke on the executive terminal: tests/qemu/test_run_status_e2e.sh
 * types it at an image that ignores SIGTSTP.
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/cli-wait-no-termination-message.defect.
 * No /dev/vms -> honest SKIP (77).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
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
#include "ssdef.h"
#include "descrip.h"
#include "libdef.h"
#include "lib$routines.h"
#include "ovmx_cli.h"
#include "ovmx_test_fork.h"
#include "ovmx_subprocess.h"  /* ovmx_creprc_fork_mbx: DCL's image-task creation */

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void on_alarm(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: a wait did not end within its bound (hang)\n";
    (void)!write(1, m, sizeof m - 1);
    _exit(1);
}

static struct dsc$descriptor_s dsc(char *s)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, s };
    return d;
}

/* ---- image mode: run by DCL (Part B) ---------------------------------- */
static int image_mode(void)
{
    char n[] = "CLIWAIT_MARK", v[] = "set by the image";
    struct dsc$descriptor_s dn = dsc(n), dv = dsc(v);
    uint32_t glob = LIB$K_CLI_GLOBAL_SYM;
    uint32_t st = lib$set_symbol(&dn, &dv, &glob);
    printf("CLIWAIT-IMAGE: set_symbol %%X%08X\n", st);
    fflush(stdout);
    return 0;
}

/* ---- Part A ------------------------------------------------------------- */
struct mbx2 { uint32_t qc, rc, qunit, runit; char qdev[32], rdev[32]; };

static int mbx2_open(struct mbx2 *m)
{
    memset(m, 0, sizeof *m);
    if (!(vms_kif_mbx_create(0, sizeof(struct ovmx_cli_req), 4 * sizeof(struct ovmx_cli_req),
                             &m->qc, &m->qunit, m->qdev, sizeof m->qdev) & 1))
        return 0;
    if (!(vms_kif_mbx_create(0, sizeof(struct ovmx_cli_rsp), 2 * sizeof(struct ovmx_cli_rsp),
                             &m->rc, &m->runit, m->rdev, sizeof m->rdev) & 1))
        return 0;
    return 1;
}

static void part_a(void)
{
    struct vms_procinfo me;
    struct mbx2 m;
    int st;

    memset(&me, 0, sizeof me);
    (void)vms_kif_getjpi_self(&me);
    CHECK(mbx2_open(&m), "the request and reply mailboxes are created");

    /* 1. a unit the creator holds no channel to */
    fflush(NULL);
    uint32_t s1 = SS$_ABORT;
    pid_t c1 = ovmx_creprc_fork_mbx(VMS_CREPRC_PCB_CONTINUE, m.qunit + 1000u, &s1, NULL);
    if (c1 == 0)
        _exit(3);
    st = 0;
    while (waitpid(c1, &st, 0) < 0 && errno == EINTR)
        ;
    CHECK(s1 == SS$_NOSUCHDEV,
          "a termination mailbox the creator holds no channel to is refused SS$_NOSUCHDEV");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3,
          "...and the held task is released (the creation record ends its wait)");

    /* 2. the image task with its termination mailbox */
    fflush(NULL);
    uint32_t s2 = SS$_ABORT;
    pid_t c2 = ovmx_creprc_fork_mbx(VMS_CREPRC_PCB_CONTINUE, m.qunit, &s2, NULL);
    if (c2 == 0) {
        char spec[80];
        snprintf(spec, sizeof spec, "%s,%s", m.qdev, m.rdev);
        setenv(OVMX_CLI_MBX_ENV, spec, 1);
        char n[] = "CLIWAIT_A", v[] = "via mailbox";
        struct dsc$descriptor_s dn = dsc(n), dv = dsc(v);
        uint32_t s = lib$set_symbol(&dn, &dv, NULL);
        uint32_t code = 0;
        (void)vms_kif_setexit(0x10000003u, &code);   /* the final condition */
        _exit(s == SS$_NORMAL ? 0 : 5);
    }
    CHECK(s2 == SS$_NORMAL, "the image task is DCL's process, with a termination mailbox");

    static union { struct ovmx_cli_req rq; struct vms_exec_mbxmsg em; } msg;
    uint32_t len = 0, sender = 0;
    alarm(30);
    uint32_t r = vms_kif_mbx_read_ex(m.qc, &msg, sizeof msg, &len, 0, &sender);
    int got_req = (r & 1) && len == sizeof msg.rq && msg.rq.op == OVMX_CLI_OP_SET &&
                  msg.rq.name_len == 9 && memcmp(msg.rq.name, "CLIWAIT_A", 9) == 0;
    CHECK(got_req, "the task's LIB$SET_SYMBOL arrives as one request in the request mailbox");
    if (got_req) {
        struct ovmx_cli_rsp rs;
        memset(&rs, 0, sizeof rs);
        rs.status = SS$_NORMAL;
        CHECK(vms_kif_mbx_write(m.rc, &rs, sizeof rs) & 1, "the CLI's reply is written");
    }
    alarm(0);
    /* The executive deletes the process -- and writes its termination message
     * -- before the substrate reports the task's end (rd vms-9f32), so once
     * waitpid returns the message is there: read it without waiting. */
    st = 0;
    while (waitpid(c2, &st, 0) < 0 && errno == EINTR)
        ;
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "the CLI's reply reached LIB$SET_SYMBOL in the task as SS$_NORMAL");
    memset(&msg, 0, sizeof msg);
    len = 0;
    r = vms_kif_mbx_read_ex(m.qc, &msg, sizeof msg, &len, 1, &sender);
    /* negctl: cli-wait-no-termination-message */
    CHECK((r & 1) && len == sizeof msg.em && msg.em.msgtyp == VMS_EXEC_MBXMSG_TERM,
          "the executive writes the task's termination message into the mailbox");
    CHECK(msg.em.task_pid == (uint32_t)c2, "...naming the task that ended");
    CHECK(msg.em.pid == me.vms_pid, "...as the VMS process it ran as (DCL's own PID)");
    CHECK(msg.em.has_status && msg.em.finalsts == 0x10000003u,
          "...with the final condition the task recorded");
    (void)vms_kif_dassgn(m.qc);
    (void)vms_kif_dassgn(m.rc);
}

/* ---- Part B: the syscall trace --------------------------------------- */
struct sc { long nr; unsigned long a0; };

static int sc_read(pid_t pid, struct sc *s)
{
#if defined(__alpha__)
    errno = 0;
    s->nr = ptrace(PTRACE_PEEKUSER, pid, (void *)0L, 0);
    if (errno) return -1;
    s->a0 = (unsigned long)ptrace(PTRACE_PEEKUSER, pid, (void *)16L, 0);
    return errno ? -1 : 0;
#else
    struct user_regs_struct r;
    struct iovec iov = { &r, sizeof r };
    memset(&r, 0, sizeof r);
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
#if defined(__x86_64__)
    s->nr = (long)r.orig_rax; s->a0 = r.rdi;
    return 0;
#elif defined(__aarch64__)
    s->nr = (long)r.regs[8]; s->a0 = r.regs[0];
    return 0;
#else
    return -1;
#endif
#endif
}

struct verdict { unsigned long total, vms_ioctls; unsigned waits, unixsock; char first[96]; };

static void classify(struct verdict *v, const struct sc *s)
{
    const char *bad = NULL;
    v->total++;
#ifdef SYS_ioctl
    if (s->nr == SYS_ioctl) { v->vms_ioctls++; return; }  /* refined below by the fd */
#endif
#ifdef SYS_poll
    if (s->nr == SYS_poll) bad = "poll";
#endif
#ifdef SYS_ppoll
    if (s->nr == SYS_ppoll) bad = "ppoll";
#endif
#ifdef SYS_select
    if (s->nr == SYS_select) bad = "select";
#endif
#ifdef SYS_pselect6
    if (s->nr == SYS_pselect6) bad = "pselect6";
#endif
#ifdef SYS_epoll_wait
    if (s->nr == SYS_epoll_wait) bad = "epoll_wait";
#endif
#ifdef SYS_epoll_pwait
    if (s->nr == SYS_epoll_pwait) bad = "epoll_pwait";
#endif
    if (bad) {
        v->waits++;
    } else {
#ifdef SYS_socketpair
        if (s->nr == SYS_socketpair) bad = "socketpair";
#endif
#ifdef SYS_socket
        if (s->nr == SYS_socket && s->a0 == AF_UNIX) bad = "socket(AF_UNIX)";
#endif
        if (bad) v->unixsock++;
    }
    if (bad && !v->first[0])
        snprintf(v->first, sizeof v->first, "%s (nr %ld)", bad, s->nr);
}

static int trace(pid_t root, struct verdict *v)
{
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC |
                PTRACE_O_EXITKILL;
    enum { MAXT = 32 };
    pid_t tids[MAXT]; int insys[MAXT], newborn[MAXT], nt = 0, live = 1, rs = -1;

    if (ptrace(PTRACE_SETOPTIONS, root, 0, (void *)opts) != 0)
        return -1;
    tids[nt] = root; insys[nt] = 0; newborn[nt] = 0; nt++;
    if (ptrace(PTRACE_SYSCALL, root, 0, 0) != 0)
        return -1;
    while (live > 0) {
        int st = 0, k;
        pid_t p = waitpid(-1, &st, __WALL);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (k = 0; k < nt && tids[k] != p; k++)
            ;
        if (k == nt) {
            if (nt == MAXT) { (void)ptrace(PTRACE_SYSCALL, p, 0, 0); continue; }
            tids[nt] = p; insys[nt] = 0; newborn[nt] = 1; nt++; live++;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (p == root) rs = st;
            live--;
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;
        int sig = WSTOPSIG(st), deliver = 0;
        if (sig == (SIGTRAP | 0x80)) {
            if (!insys[k]) {
                struct sc s;
                if (sc_read(p, &s) == 0)
                    classify(v, &s);
            }
            insys[k] = !insys[k];
        } else if (sig == SIGTRAP && (st >> 16)) {
            /* a clone/exec event */
        } else if (sig == SIGSTOP && newborn[k]) {
            newborn[k] = 0;
        } else {
            deliver = sig;
        }
        (void)ptrace(PTRACE_SYSCALL, p, 0, (void *)(long)deliver);
    }
    return rs;
}

static void part_b_control(void)
{
    struct verdict v;
    memset(&v, 0, sizeof v);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        struct pollfd pf = { -1, 0, 0 };
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        (void)poll(&pf, 1, 0);
        _exit(0);
    }
    int st = 0;
    while (waitpid(c, &st, 0) < 0 && errno == EINTR)
        ;
    (void)trace(c, &v);
    CHECK(v.waits >= 1, "control: the classifier flags a poll()");
}

static void part_b(const char *self)
{
    char script[512], out[8192];
    int in_pipe[2], out_pipe[2];
    struct verdict v;

    memset(&v, 0, sizeof v);
    snprintf(script, sizeof script,
             "$ RUN \"%s\"\n"
             "$ SHOW SYMBOL CLIWAIT_MARK\n"
             "$ WRITE SYS$OUTPUT \"CLIWAIT-STATUS \", $STATUS\n"
             "$ EXIT\n", self);
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) { CHECK(0, "pipes"); return; }
    if (write(in_pipe[1], script, strlen(script)) < 0) { CHECK(0, "script"); return; }
    close(in_pipe[1]);
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) { CHECK(0, "a login subprocess for DCL"); return; }
    if (pid == 0) {
        static char *const env[] = { (char *)"PATH=/bin", NULL };
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        close(in_pipe[0]); close(out_pipe[0]); close(out_pipe[1]);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    /* DCL's output goes to a pipe the trace would block on if it filled: the
     * script prints a few lines, far under a pipe's capacity. */
    alarm(60);
    int rs = trace(pid, &v);
    alarm(0);
    ssize_t n = read(out_pipe[0], out, sizeof out - 1);
    out[n > 0 ? n : 0] = '\0';
    close(out_pipe[0]);
    printf("----- DCL transcript -----\n%s\n--------------------------\n", out);
    printf("  (trace: %lu syscalls, %lu ioctls; first forbidden: %s)\n",
           v.total, v.vms_ioctls, v.first[0] ? v.first : "none");

    CHECK(rs >= 0 && WIFEXITED(rs), "DCL ran the procedure to its EXIT under the trace");
    CHECK(strstr(out, "CLIWAIT-IMAGE: set_symbol %X00000001") != NULL,
          "the fork-path image's LIB$SET_SYMBOL reached DCL (SS$_NORMAL)");
    CHECK(strstr(out, "CLIWAIT_MARK") && strstr(out, "set by the image"),
          "the image's symbol is in DCL's table after the image exited");
    CHECK(v.vms_ioctls > 0, "the trace saw DCL's executive ioctls (not vacuous)");
    CHECK(v.waits == 0, "DCL made NO poll/ppoll/select/epoll wait while the image ran");
    CHECK(v.unixsock == 0, "DCL made NO socketpair / AF_UNIX socket for the CLI channel");
}

int main(int argc, char **argv)
{
    (void)argc;
    if (getenv(OVMX_CLI_MBX_ENV))
        return image_mode();

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_cli_wait: DCL's image wait through the executive (vms-6d9) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_cli_wait: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    signal(SIGALRM, on_alarm);

    char self[512];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    self[sl > 0 ? sl : 0] = '\0';
    (void)argv;

    part_a();
    part_b_control();
    if (access(DCL_PATH, X_OK) == 0 && self[0])
        part_b(self);
    else
        CHECK(0, DCL_PATH " is present in the initramfs");

    printf("=== test_syssvc_cli_wait: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
