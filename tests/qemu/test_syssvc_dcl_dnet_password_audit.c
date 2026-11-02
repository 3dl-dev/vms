/*
 * test_syssvc_dcl_dnet_password_audit.c - DCL hands the DECnet access-control
 * password of a node"user password":: spec to DECNETD.EXE in a VMS mailbox,
 * never on a host descriptor or in argv (rd vms-ad71).
 *
 * Until vms-ad71, DCL COPY / DIRECTORY / TYPE node:: wrote the password into a
 * host pipe(2) and passed DECNETD the fd number (--password-fd). Now DCL
 * creates a temporary mailbox ($CREMBX, owner-only protection), writes the
 * password with $QIOW, and names the unit (--password-mbx DNET$PASSWORD_MBX).
 *
 * THE AUDIT. The suite runs the shipped DCL.EXE as a VMS subprocess under
 * ptrace (PTRACE_O_TRACEFORK/VFORK/CLONE/EXEC, so DECNETD.EXE, which DCL
 * activates, is traced too) on
 *
 *     COPY 0"SYSTEM <password>"::AD71_SRC.TXT AD71_DST.TXT
 *
 * and asserts, from the syscall entry stops of every traced process:
 *   - DCL activated DECNETD.EXE with "--password-mbx DNET$PASSWORD_MBX" and the password
 *     appears in NO execve argv (and no "--password-fd");
 *   - NO pipe()/pipe2() call is made between DCL reading the COPY and the
 *     image's exit (the old handover);
 *   - NO write/writev/pwrite64/sendto/sendmsg carries the password bytes to
 *     any host descriptor.
 * No NETACP runs in this guest, so DECNETD reports that and exits; the
 * password's delivery to FAL over a running NETACP (and a wrong password
 * refused SS$_INVLOGIN) is proven by tests/qemu/test_decnet_startnet_boot_e2e.sh.
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-dnet-password-on-pipe.defect.
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
#include <signal.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/socket.h>
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
#define DCL_PATH_DEFAULT     "/tests/DCL.EXE"
#define DECNETD_DIR          "/vms/SYS0/SYSCOMMON/SYSEXE/"
#define PW                   "OVMXPWAD71"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct sc { long nr; unsigned long a[4]; };

static int sc_read(pid_t pid, struct sc *s)
{
    struct iovec iov;
#if defined(__alpha__)
    struct pt_regs r;
    memset(&r, 0, sizeof r);
    iov.iov_base = &r; iov.iov_len = sizeof r;
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
    s->nr = (long)r.r0; s->a[0] = r.r16; s->a[1] = r.r17; s->a[2] = r.r18; s->a[3] = r.r19;
    return 0;
#else
    struct user_regs_struct r;
    memset(&r, 0, sizeof r);
    iov.iov_base = &r; iov.iov_len = sizeof r;
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
#if defined(__x86_64__)
    s->nr = (long)r.orig_rax; s->a[0] = r.rdi; s->a[1] = r.rsi; s->a[2] = r.rdx; s->a[3] = r.r10;
    return 0;
#elif defined(__aarch64__)
    s->nr = (long)r.regs[8];
    for (int i = 0; i < 4; i++) s->a[i] = r.regs[i];
    return 0;
#else
    (void)s;
    return -1;
#endif
#endif
}

/* Copy up to n bytes of the tracee's memory at addr (0 on failure). */
static size_t peek(pid_t pid, unsigned long addr, void *buf, size_t n)
{
    struct iovec l = { buf, n }, r = { (void *)addr, n };
    ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
    return got > 0 ? (size_t)got : 0;
}

static int has_pw(const unsigned char *b, size_t n)
{
    return memmem(b, n, PW, sizeof(PW) - 1) != NULL;
}

struct audit {
    unsigned long total;
    unsigned pipes, pw_writes, pw_argv, fd_argv;
    int saw_mbx_argv;
    char mbx[256];
};

static void on_entry(pid_t pid, struct audit *a, const struct sc *s)
{
    unsigned char buf[4096];
    a->total++;
#ifdef SYS_pipe
    if (s->nr == SYS_pipe) { a->pipes++; return; }
#endif
#ifdef SYS_pipe2
    if (s->nr == SYS_pipe2) { a->pipes++; return; }
#endif
    if (s->nr == SYS_execve) {
        unsigned long argvp = s->a[1];
        int saw_flag = 0;
        for (int i = 0; i < 16; i++) {
            unsigned long ap = 0;
            if (peek(pid, argvp + i * sizeof(unsigned long), &ap, sizeof ap) != sizeof ap || !ap)
                break;
            char arg[256];
            size_t n = peek(pid, ap, arg, sizeof arg - 1);
            arg[n] = '\0';
            arg[strnlen(arg, n)] = '\0';
            if (strstr(arg, PW)) a->pw_argv++;
            if (!strcmp(arg, "--password-fd")) a->fd_argv++;
            if (saw_flag && arg[0] && !strstr(arg, PW)) {
                a->saw_mbx_argv = 1;
                snprintf(a->mbx, sizeof a->mbx, "%s", arg);
            }
            saw_flag = !strcmp(arg, "--password-mbx");
        }
        return;
    }
    if (s->nr == SYS_write
#ifdef SYS_pwrite64
        || s->nr == SYS_pwrite64
#endif
#ifdef SYS_sendto
        || s->nr == SYS_sendto
#endif
        ) {
        size_t n = s->a[2] < sizeof buf ? s->a[2] : sizeof buf;
        n = peek(pid, s->a[1], buf, n);
        if (has_pw(buf, n)) a->pw_writes++;
        return;
    }
    if (s->nr == SYS_writev
#ifdef SYS_pwritev
        || s->nr == SYS_pwritev
#endif
        ) {
        struct iovec v[8];
        size_t cnt = s->a[2] < 8 ? s->a[2] : 8;
        if (peek(pid, s->a[1], v, cnt * sizeof v[0]) != cnt * sizeof v[0]) return;
        for (size_t i = 0; i < cnt; i++) {
            size_t n = v[i].iov_len < sizeof buf ? v[i].iov_len : sizeof buf;
            n = peek(pid, (unsigned long)v[i].iov_base, buf, n);
            if (has_pw(buf, n)) a->pw_writes++;
        }
        return;
    }
#ifdef SYS_sendmsg
    if (s->nr == SYS_sendmsg) {
        struct msghdr m;
        if (peek(pid, s->a[1], &m, sizeof m) != sizeof m) return;
        struct iovec v[8];
        size_t cnt = m.msg_iovlen < 8 ? m.msg_iovlen : 8;
        if (peek(pid, (unsigned long)m.msg_iov, v, cnt * sizeof v[0]) != cnt * sizeof v[0]) return;
        for (size_t i = 0; i < cnt; i++) {
            size_t n = v[i].iov_len < sizeof buf ? v[i].iov_len : sizeof buf;
            n = peek(pid, (unsigned long)v[i].iov_base, buf, n);
            if (has_pw(buf, n)) a->pw_writes++;
        }
    }
#endif
}

static volatile pid_t g_root;
static void on_bound(int sig) { (void)sig; if (g_root > 0) kill(g_root, SIGKILL); }

static int trace_all(pid_t root, struct audit *a, int bound_s)
{
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    enum { MAXT = 64 };
    pid_t tids[MAXT]; int insys[MAXT], newborn[MAXT], nt = 0, live = 1, root_st = -1;

    if (ptrace(PTRACE_SETOPTIONS, root, 0, (void *)opts) != 0) return -1;
    g_root = root;
    signal(SIGALRM, on_bound);
    alarm((unsigned)bound_s);
    tids[nt] = root; insys[nt] = 0; newborn[nt] = 0; nt++;
    if (ptrace(PTRACE_SYSCALL, root, 0, 0) != 0) return -1;
    while (live > 0) {
        int st = 0;
        pid_t p = waitpid(-1, &st, __WALL);
        if (p < 0) { if (errno == EINTR) continue; break; }
        int k;
        for (k = 0; k < nt && tids[k] != p; k++)
            ;
        if (k == nt && nt < MAXT) { tids[nt] = p; insys[nt] = 0; newborn[nt] = 1; nt++; live++; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) { if (p == root) root_st = st; live--; continue; }
        if (!WIFSTOPPED(st)) continue;
        int sig = WSTOPSIG(st), deliver = 0;
        if (sig == (SIGTRAP | 0x80)) {
            if (k < nt) {
                if (!insys[k]) { struct sc s; if (sc_read(p, &s) == 0) on_entry(p, a, &s); }
                insys[k] = !insys[k];
            }
        } else if (sig == SIGTRAP && (st >> 16)) {
            /* fork/clone/exec event */
        } else if (sig == SIGSTOP && k < nt && newborn[k]) {
            newborn[k] = 0;
        } else {
            deliver = sig;
        }
        (void)ptrace(PTRACE_SYSCALL, p, 0, (void *)(long)deliver);
    }
    alarm(0);
    return root_st;
}

/* Run DCL.EXE (a VMS subprocess, traced) on `cmd`; fill a and out. */
static int run_dcl(const char *dcl, const char *cmd, struct audit *a, char *out, size_t outsz)
{
    char script[] = "/tmp/ad71scrXXXXXX", capture[] = "/tmp/ad71capXXXXXX";
    int sfd = mkstemp(script), cfd = mkstemp(capture);
    memset(a, 0, sizeof *a);
    out[0] = '\0';
    if (sfd < 0 || cfd < 0) return -1;
    char text[512];
    int len = snprintf(text, sizeof text,
                       "DEFINE SYS$SYSTEM \"" DECNETD_DIR "\"\n%s\n"
                       "WRITE SYS$OUTPUT \"OVMX-AD71-ALIVE\"\n", cmd);
    if (len < 0 || write(sfd, text, (size_t)len) != len) return -1;
    lseek(sfd, 0, SEEK_SET);
    pid_t c = ovmx_fork_subprocess();
    if (c == 0) {
        dup2(sfd, 0);
        dup2(cfd, 1);
        dup2(cfd, 2);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execl(dcl, dcl, (char *)NULL);
        _exit(127);
    }
    if (c <= 0) return -1;
    int st = 0;
    waitpid(c, &st, 0);
    int rs = trace_all(c, a, 90);
    lseek(cfd, 0, SEEK_SET);
    ssize_t n = read(cfd, out, outsz - 1);
    out[n > 0 ? n : 0] = '\0';
    close(sfd); close(cfd);
    unlink(script); unlink(capture);
    return rs;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_dnet_password_audit (DCL passes the DECnet password in a mailbox, rd vms-ad71) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_dnet_password_audit: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    vms_kif_close();
    const char *dcl = getenv("OVMX_DCL");
    if (!dcl || !dcl[0]) dcl = DCL_PATH_DEFAULT;
    if (access(dcl, X_OK) != 0 || access(DECNETD_DIR "DECNETD.EXE", X_OK) != 0) {
        printf("  FAIL: DCL.EXE or DECNETD.EXE is not staged\n");
        printf("=== test_syssvc_dcl_dnet_password_audit: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    static char out[16384], out0[16384];
    struct audit a, a0;
    /* the same COPY WITHOUT a password: the baseline every DCL activation
     * makes on its own (the old handover added a pipe only with a password) */
    int rs0 = run_dcl(dcl, "COPY 0\"SYSTEM\"::AD71_SRC.TXT AD71_DST.TXT", &a0, out0, sizeof out0);
    int rs = run_dcl(dcl, "COPY 0\"SYSTEM " PW "\"::AD71_SRC.TXT AD71_DST.TXT", &a, out, sizeof out);
    printf("  --- DCL output ---\n%s  --- end ---\n", out);
    printf("  (syscalls %lu; pipes %u [baseline without a password %u]; password in a write %u,"
           " in argv %u; --password-fd %u; mailbox %s)\n", a.total, a.pipes, a0.pipes,
           a.pw_writes, a.pw_argv, a.fd_argv, a.saw_mbx_argv ? a.mbx : "(none)");
    CHECK(rs0 >= 0 && strstr(out0, "OVMX-AD71-ALIVE") != NULL && !a0.saw_mbx_argv,
          "baseline: a COPY with no password runs and names no password mailbox");

    CHECK(rs >= 0 && strstr(out, "OVMX-AD71-ALIVE") != NULL,
          "DCL ran the COPY under the trace and went on to the next command");
    CHECK(a.saw_mbx_argv,
          "DCL activated DECNETD.EXE naming the password MAILBOX (--password-mbx DNET$PASSWORD_MBX)");
    /* negctl: dcl-dnet-password-on-pipe */
    CHECK(a.pipes <= a0.pipes && a.fd_argv == 0,
          "NO pipe()/pipe2() beyond the no-password baseline, and no --password-fd: the password is not handed over a host pipe");
    CHECK(a.pw_writes == 0, "the password crosses NO host descriptor (no write/writev/send of it)");
    CHECK(a.pw_argv == 0, "the password appears in NO execve argv");
    CHECK(strstr(out, PW) == NULL, "the password is never displayed");

    printf("=== test_syssvc_dcl_dnet_password_audit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
