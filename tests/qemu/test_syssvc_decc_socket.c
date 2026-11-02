/*
 * test_syssvc_decc_socket.c - a VMS-linked program's sockets are EXECUTIVE BGn:
 * sockets, and no Linux socket system call is reachable from it (rd vms-99f).
 *
 * THE SUBJECT is SOCKECHO.EXE (tests/qemu/sockecho/SOCKECHO.C): an ordinary
 * BSD-sockets C program, LINKED THE VMS WAY against DECC$SHR alone (LINK.EXE
 * --executable --use DECC$SHR, PT_INTERP = IMGACT.EXE). Its socket(), connect(),
 * send(), recv(), getaddrinfo() ... bind to DECC$SHR's universals at activation.
 *
 * WHAT IS ASSERTED.
 *   1. ROUND TRIP. This suite listens on 127.0.0.1 with an EXECUTIVE socket of
 *      its own (the vms_bgsock.c veneer over BGn:) and echoes what it receives.
 *      SOCKECHO connects, sends, and must get every byte back exactly, through
 *      send/recv, write/read, poll and select; and THIS process must have
 *      received the exact bytes SOCKECHO sent. SOCKECHO also runs a server leg of
 *      its own (bind/listen/accept/getsockname).
 *   2. IT IS A BG CHANNEL. SOCKECHO's socket descriptor holds no Linux socket
 *      (never a "socket:[...]"); it names the BG channel every call $QIOs on.
 *      O_NONBLOCK (EAGAIN), select()/poll() timeouts and wake-ups and dup() work.
 *   3. NO LINUX I/O ON A SOCKET. SOCKECHO runs under a ptrace system-call audit
 *      (PTRACE_GET_SYSCALL_INFO on every syscall entry, all of its threads).
 *      ZERO entries of socket, socketpair, connect, accept, accept4, bind, listen,
 *      sendto, recvfrom, sendmsg, recvmsg, sendmmsg, recvmmsg, shutdown,
 *      getsockname, getpeername, setsockopt and getsockopt; ZERO poll/ppoll/
 *      select/pselect6/epoll_*; and ZERO read/write/readv/writev/pread/pwrite/
 *      sendfile/splice whose descriptor is a socket or the executive device --
 *      while the round trip succeeds. Socket I/O and readiness reach the
 *      executive only as /dev/vms ioctls (the BGn: $QIO functions). (The audit is
 *      live: it must also have seen those ioctls.)
 *   4. HONEST FAILURE. SOCKECHO run with no /dev/vms (a private mount namespace
 *      with an empty /dev) must get socket() == -1, errno ENODEV, and still make
 *      no Linux socket syscall: no fallback to the host.
 *
 * NEGATIVE CONTROL decc-socket-bound-to-musl (tests/qemu/defects.d) binds
 * DECC$SHR's socket universals back to musl: SOCKECHO's socket is then a Linux
 * socket and assertions 2 and 3 go red.
 *
 * NO /dev/vms in this suite's own process -> honest SKIP (77).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms_bgsock.h"
#include "ovmx_test_fork.h"
#include "vmsfs/ods2.h"
#include "sysvol_stage.h"

#define EXIT_SKIP 77
#define SOCKECHO_IMAGE "/vms/SYS0/SYSCOMMON/SYSEXE/SOCKECHO.EXE"
#define CHILD_DEADLINE_S 90

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Bring the kernel loopback interface up: init.sh does not, and a guest boots
 * with lo DOWN, so 127.0.0.1 would be unreachable to the executive's sockets
 * (connect() fails). The test harness's own substrate step, as
 * test_syssvc_bg_echo.c does it -- not part of what is under test. */
static void bring_lo_up(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;
    if (s < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        (void)ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    close(s);
}

/* ---- the system-call audit -------------------------------------------- */

#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#define SYSCALL_INFO_ENTRY 1
struct syscall_info {               /* struct ptrace_syscall_info (uapi) */
    uint8_t  op;
    uint8_t  pad[3];
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t stack_pointer;
    uint64_t nr;                    /* entry.nr */
    uint64_t args[6];
    uint32_t ret_data;
};

static const struct { long nr; const char *name; } g_sock_sys[] = {
    { SYS_socket, "socket" },         { SYS_socketpair, "socketpair" },
    { SYS_connect, "connect" },       { SYS_accept, "accept" },
    { SYS_accept4, "accept4" },       { SYS_bind, "bind" },
    { SYS_listen, "listen" },         { SYS_sendto, "sendto" },
    { SYS_recvfrom, "recvfrom" },     { SYS_sendmsg, "sendmsg" },
    { SYS_recvmsg, "recvmsg" },       { SYS_sendmmsg, "sendmmsg" },
    { SYS_recvmmsg, "recvmmsg" },     { SYS_shutdown, "shutdown" },
    { SYS_getsockname, "getsockname" }, { SYS_getpeername, "getpeername" },
    { SYS_setsockopt, "setsockopt" }, { SYS_getsockopt, "getsockopt" },
};
#define N_SOCK_SYS (sizeof(g_sock_sys) / sizeof(g_sock_sys[0]))

/* The readiness waits: a VMS-linked image's select()/poll() on a socket is the
 * executive's IO$_SENSEMODE readiness wait (an ioctl), so SOCKECHO -- whose
 * every poll/select names only sockets -- must make none of these. */
static const long g_wait_sys[] = {
#ifdef SYS_poll
    SYS_poll,
#endif
#ifdef SYS_select
    SYS_select,
#endif
    SYS_ppoll, SYS_pselect6, SYS_epoll_pwait,
#ifdef SYS_epoll_wait
    SYS_epoll_wait,
#endif
};
#define N_WAIT_SYS (sizeof(g_wait_sys) / sizeof(g_wait_sys[0]))

/* The data calls whose first argument is a descriptor: on a socket descriptor
 * (a Linux socket, or the executive device that reserves a socket's number)
 * each would be Linux I/O standing in for IO$_READVBLK / IO$_WRITEVBLK. */
static const long g_fdio_sys[] = {
    SYS_read, SYS_write, SYS_readv, SYS_writev, SYS_pread64, SYS_pwrite64,
    SYS_preadv, SYS_pwritev, SYS_sendfile, SYS_splice,
};
#define N_FDIO_SYS (sizeof(g_fdio_sys) / sizeof(g_fdio_sys[0]))

struct audit {
    long total;                     /* syscall entries seen */
    long ioctls;
    long sock[N_SOCK_SYS];
    long sock_total;
    long waits;                     /* poll/select-family entries */
    long fdio_sock;                 /* read/write-family on a socket descriptor */
    int  status;                    /* main tracee's wait status */
    int  exited;
};

/* What tid's descriptor fd is, read while the tracee is stopped at the entry. */
static int fd_is_socketish(pid_t tid, long fd)
{
    char path[64], link[128];
    ssize_t k;

    if (fd < 0)
        return 0;
    snprintf(path, sizeof(path), "/proc/%d/fd/%ld", (int)tid, fd);
    k = readlink(path, link, sizeof(link) - 1);
    if (k <= 0)
        return 0;
    link[k] = '\0';
    return strncmp(link, "socket:", 7) == 0 || strcmp(link, "/dev/vms") == 0 ||
           strstr(link, "[bgconn]") != NULL;
}

static void audit_entry(struct audit *au, pid_t tid, uint64_t nr, const uint64_t *args)
{
    size_t i;
    au->total++;
    for (i = 0; i < N_WAIT_SYS; i++)
        if ((long)nr == g_wait_sys[i])
            au->waits++;
    for (i = 0; i < N_FDIO_SYS; i++)
        if ((long)nr == g_fdio_sys[i] && fd_is_socketish(tid, (long)args[0]))
            au->fdio_sock++;
    if ((long)nr == SYS_ioctl)
        au->ioctls++;
    for (i = 0; i < N_SOCK_SYS; i++)
        if ((long)nr == g_sock_sys[i].nr) {
            au->sock[i]++;
            au->sock_total++;
        }
}

static void audit_report(const char *who, const struct audit *au)
{
    size_t i;
    printf("  INFO: %s audit: %ld syscall entries, %ld ioctl, %ld Linux socket-family, "
           "%ld poll/select-family, %ld read/write-family on a socket descriptor\n",
           who, au->total, au->ioctls, au->sock_total, au->waits, au->fdio_sock);
    for (i = 0; i < N_SOCK_SYS; i++)
        if (au->sock[i])
            printf("  INFO:   %s x%ld\n", g_sock_sys[i].name, au->sock[i]);
}

static volatile pid_t g_watch_pid;
static void *watchdog(void *arg)
{
    (void)arg;
    sleep(CHILD_DEADLINE_S);
    if (g_watch_pid > 0) {
        printf("  INFO: watchdog: SOCKECHO still running after %ds -- killed\n",
               CHILD_DEADLINE_S);
        kill(g_watch_pid, SIGKILL);
    }
    return NULL;
}

/* Trace `pid` (stopped at its own SIGSTOP before execve) to its exit, counting
 * every syscall entry of it and of any thread or child it makes. */
static void trace(pid_t pid, struct audit *au)
{
    int st;
    pthread_t wd;

    memset(au, 0, sizeof(*au));
    if (waitpid(pid, &st, __WALL) != pid || !WIFSTOPPED(st)) {
        printf("  INFO: tracee did not reach its pre-exec stop (status 0x%x)\n", st);
        au->status = st;
        au->exited = 1;
        return;
    }
    ptrace(PTRACE_SETOPTIONS, pid, 0,
           PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE |
           PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEEXEC);
    g_watch_pid = pid;
    pthread_create(&wd, NULL, watchdog, NULL);
    pthread_detach(wd);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);

    for (;;) {
        pid_t w = waitpid(-1, &st, __WALL);
        int sig;

        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (w == pid) {
                au->status = st;
                au->exited = 1;
                break;
            }
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;
        sig = WSTOPSIG(st);
        if (sig == (SIGTRAP | 0x80)) {
            struct syscall_info si;
            memset(&si, 0, sizeof(si));
            if (ptrace(PTRACE_GET_SYSCALL_INFO, w, (void *)sizeof(si), &si) > 0 &&
                si.op == SYSCALL_INFO_ENTRY)
                audit_entry(au, w, si.nr, si.args);
            sig = 0;
        } else if (sig == SIGTRAP && (st >> 16) != 0) {
            sig = 0;                /* clone/fork/exec event stop */
        } else if (sig == SIGSTOP && w != pid) {
            sig = 0;                /* a new thread's/child's birth stop */
        }
        ptrace(PTRACE_SYSCALL, w, 0, (void *)(long)sig);
    }
    g_watch_pid = 0;
}

/* ---- the executive echo listener (this process's own BGn: socket) ------- */

struct echo {
    int  lsock;
    int  stop;                      /* read end: written when SOCKECHO is gone */
    char got[512];
    size_t ngot;
    int  accepted;
};

static void *echo_server(void *arg)
{
    struct echo *e = arg;
    char buf[512];
    struct pollfd p[2];
    int c;

    /* Accept only once SOCKECHO has connected; if it ends without connecting
     * (main signals `stop`), give up rather than block in accept for ever. */
    p[0].fd = ovmx_readyfd(e->lsock); p[0].events = POLLIN; p[0].revents = 0;
    p[1].fd = e->stop;                p[1].events = POLLIN; p[1].revents = 0;
    if (p[0].fd < 0 || poll(p, 2, CHILD_DEADLINE_S * 1000) < 1 || !(p[0].revents & POLLIN))
        return NULL;
    c = ovmx_accept(e->lsock, NULL, NULL);
    if (c < 0)
        return NULL;
    e->accepted = 1;
    for (;;) {
        ssize_t n = ovmx_recv(c, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        if (e->ngot + (size_t)n <= sizeof(e->got)) {
            memcpy(e->got + e->ngot, buf, (size_t)n);
            e->ngot += (size_t)n;
        }
        if (ovmx_send(c, buf, (size_t)n, 0) != n)
            break;
    }
    ovmx_socket_close(c);
    return NULL;
}

/* ---- run SOCKECHO ------------------------------------------------------ */

/* Fork SOCKECHO with stdout/stderr on a pipe, stopped for the tracer before it
 * execs. hide_exec: give it a private mount namespace with an EMPTY /dev, so it
 * runs with no /dev/vms at all. Returns the pid; *out_fd = the pipe's read end. */
static pid_t start_sockecho(int hide_exec, char *const argv[], int *out_fd)
{
    int p[2];
    pid_t pid;

    if (pipe(p) != 0)
        return -1;
    pid = hide_exec ? fork() : ovmx_fork_subprocess();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[1]);
        if (hide_exec) {
            if (unshare(CLONE_NEWNS) != 0 ||
                mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
                mount("none", "/dev", "tmpfs", 0, NULL) != 0) {
                printf("SOCKECHO: FAIL could not hide /dev/vms (errno %d)\n", errno);
                _exit(126);
            }
            if (access("/dev/vms", F_OK) == 0) {
                printf("SOCKECHO: FAIL /dev/vms still visible\n");
                _exit(126);
            }
        }
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execv(SOCKECHO_IMAGE, argv);
        printf("SOCKECHO: FAIL execv %s errno %d\n", SOCKECHO_IMAGE, errno);
        _exit(127);
    }
    close(p[1]);
    *out_fd = p[0];
    return pid;
}

static size_t drain(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        ssize_t k = read(fd, buf + n, cap - 1 - n);
        if (k <= 0 || n + (size_t)k >= cap - 1) {
            if (k > 0) n += (size_t)k;
            break;
        }
        n += (size_t)k;
    }
    buf[n] = '\0';
    close(fd);
    return n;
}

static void show(const char *out)
{
    const char *l = out;
    while (*l) {
        const char *e = strchr(l, '\n');
        int len = e ? (int)(e - l) : (int)strlen(l);
        printf("    | %.*s\n", len, l);
        l += len + (e ? 1 : 0);
    }
}

static int has(const char *out, const char *s)
{
    return strstr(out, s) != NULL;
}

int main(void)
{
    static char out[16384];
    struct audit au;
    struct echo e;
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    pthread_t th;
    char port[16];
    int fd, stop[2];
    pid_t pid;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_decc_socket (DECC$SHR sockets are $QIO on BGn: channels, vms-99f) ===\n");

    if (!executive_present()) {
        int s = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(s < 0 && errno == ENODEV,
              "with no executive the veneer's socket FAILS ENODEV (never a host socket)");
        printf("=== test_syssvc_decc_socket: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    CHECK(sysvol_stage_subject(SOCKECHO_IMAGE, "SOCKECHO.EXE") == 0,
          "SOCKECHO.EXE staged on " SYSVOL_UNIT " over the ACP");
    CHECK(sysvol_stage_shareables_from("/vms/SYS0/SYSCOMMON/SYSLIB") >= 0,
          "DECC$SHR.EXE staged on " SYSVOL_UNIT " SYS$LIBRARY");

    bring_lo_up();

    /* 1. the listener: an executive socket of this process. */
    memset(&e, 0, sizeof(e));
    e.lsock = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(e.lsock >= 0 &&
          ovmx_bind(e.lsock, (struct sockaddr *)&a, sizeof(a)) == 0 &&
          ovmx_listen(e.lsock, 4) == 0 &&
          ovmx_getsockname(e.lsock, (struct sockaddr *)&a, &al) == 0 &&
          a.sin_port != 0,
          "the suite's own executive listener is up on 127.0.0.1:<ephemeral>");
    snprintf(port, sizeof(port), "%u", (unsigned)ntohs(a.sin_port));
    if (pipe(stop) != 0) { printf("  FAIL: pipe\n"); return 1; }
    e.stop = stop[0];
    pthread_create(&th, NULL, echo_server, &e);

    /* 2. SOCKECHO client under the audit. */
    {
        char *argv[] = { (char *)SOCKECHO_IMAGE, (char *)"client", port, NULL };
        pid = start_sockecho(0, argv, &fd);
    }
    CHECK(pid > 0, "SOCKECHO forked as a VMS subprocess");
    if (pid <= 0) {
        printf("=== test_syssvc_decc_socket: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    trace(pid, &au);
    drain(fd, out, sizeof(out));
    show(out);
    audit_report("SOCKECHO client", &au);
    (void)!write(stop[1], "x", 1);
    pthread_join(th, NULL);
    ovmx_socket_close(e.lsock);

    CHECK(au.exited && WIFEXITED(au.status) && WEXITSTATUS(au.status) == 0 &&
          has(out, "SOCKECHO: DONE ok") && !has(out, "SOCKECHO: FAIL"),
          "SOCKECHO (linked against DECC$SHR) completed every socket step");
    CHECK(has(out, "SOCKECHO: PASS the socket descriptor holds no Linux socket"),
          "SOCKECHO's socket() returned a BG channel descriptor, not a Linux socket");
    CHECK(has(out, "SOCKECHO: PASS recv() returns the echoed message BYTE-EXACT") &&
          has(out, "SOCKECHO: PASS read() returns the second echo BYTE-EXACT"),
          "bytes round-tripped through the executive listener BYTE-EXACT");
    CHECK(e.accepted && e.ngot == 37 + 34 &&
          memcmp(e.got, "OVMX-DECC-SOCKET-ROUNDTRIP-0123456789"
                        "second-message-over-write-and-read", e.ngot) == 0,
          "this process's executive socket received exactly the bytes SOCKECHO sent");
    CHECK(has(out, "SOCKECHO: PASS accept() returns a BG channel descriptor") &&
          has(out, "SOCKECHO: PASS bytes cross from the connecting socket to the accepted one"),
          "SOCKECHO's own bind/listen/accept server leg ran on executive sockets");
    /* negctl: decc-socket-bound-to-musl */
    CHECK(au.exited && au.total > 0 && au.ioctls > 0 && au.sock_total == 0,
          "ZERO Linux socket-family syscalls from the VMS-linked image (the audit saw its executive ioctls)");
    /* negctl: decc-socket-read-via-musl */
    CHECK(au.exited && au.total > 0 && au.fdio_sock == 0 && au.waits == 0,
          "ZERO read/write-family syscalls on a socket descriptor and ZERO poll/select syscalls: "
          "socket I/O and readiness are $QIOs on the BG channel (executive ioctls only)");
    CHECK(has(out, "SOCKECHO: PASS a non-blocking recv() with nothing sent fails EAGAIN") &&
          has(out, "SOCKECHO: PASS the select() timeout was waited out") &&
          has(out, "SOCKECHO: PASS poll() wakes when the bytes arrive") &&
          has(out, "SOCKECHO: PASS write() through a dup() of the connecting socket"),
          "O_NONBLOCK, select()/poll() timeouts and wake-ups, and dup() work through the executive readiness wait");

    /* 3. SOCKECHO with no executive at all. */
    {
        char *argv[] = { (char *)SOCKECHO_IMAGE, (char *)"noexec", NULL };
        pid = start_sockecho(1, argv, &fd);
    }
    if (pid > 0) {
        trace(pid, &au);
        drain(fd, out, sizeof(out));
        show(out);
        audit_report("SOCKECHO noexec", &au);
    } else {
        out[0] = '\0';
    }
    CHECK(pid > 0 && au.exited && WIFEXITED(au.status) && WEXITSTATUS(au.status) == 0 &&
          has(out, "SOCKECHO: PASS with no executive socket() FAILS with ENODEV"),
          "with no /dev/vms SOCKECHO's socket() fails ENODEV (no host fallback)");
    CHECK(pid > 0 && au.exited && au.total > 0 && au.sock_total == 0 &&
          au.waits == 0 && au.fdio_sock == 0,
          "and still ZERO Linux socket-family, poll/select or socket read/write syscalls");

    printf("=== test_syssvc_decc_socket: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
