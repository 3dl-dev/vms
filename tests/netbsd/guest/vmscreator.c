/*
 * vmscreator.c - the guest harness's creator of VMS processes (rd vms-6ed,
 * part 3 of vms-2a5).
 *
 * On OVMX a process is a VMS process only because its creator made it one:
 * $CREPRC (VMS_IOCTL_CREPRC_PCB) for every process, ESTABLISH_SYSTEM once for
 * the boot task. A tool started from the guest's root shell has no creator, so
 * the executive does not know it (SS$_NONEXPR) -- the find-or-create that used
 * to register any task on its first ioctl is gone. This program is the guest
 * harness's stand-in for the boot task, in two halves of one binary:
 *
 *   vmscreator -d [sock]
 *       The daemon. Makes the one ESTABLISH_SYSTEM call of this boot (it is
 *       then the SYSTEM process), listens on a UNIX socket (default
 *       /tmp/vmscreator.sock), and for each request creates the tool as its
 *       SUBPROCESS: fork, hold the child, VMS_IOCTL_CREPRC_PCB, release; the
 *       child takes the requester's stdin/stdout/stderr and execs the tool.
 *       When the tool exits the daemon sends the requester its wait status.
 *       Prints "VMSCREATOR READY" once listening. Runs until killed.
 *
 *   vmscreator [-s sock] <program> [args...]
 *       The client. Hands the daemon its argv and its stdio (SCM_RIGHTS),
 *       waits, and exits as the tool did (128+signal for a signal). SIGINT/
 *       SIGTERM/SIGHUP to the client are passed on to the tool, so a driver
 *       that backgrounds a tool ("vmscreator vmseflag wait 66 &") and kills
 *       the client ends the tool.
 *
 * Exit codes of the client besides the tool's own: 120 = daemon unreachable,
 * 121 = request refused (the reply carries the SS$_ status; printed on
 * stderr as "VMSCREATOR REFUSED status=0x...").
 *
 * Wire (stream socket): request = u32 total length, then argv strings each
 * NUL-terminated, with the three fds in the first message's SCM_RIGHTS.
 * Reply = 'S' + u32 wait status, or 'R' + u32 SS$_ status (refused). After
 * the request the client may send 'K' + u32 signal number.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kif_transport.h"
#include "vms_proctab_nb.h"

#define DEF_SOCK "/tmp/vmscreator.sock"
#define MAXREQ   4096
#define MAXCLI   64

static int send_all(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int recv_all(int fd, void *b, size_t n)
{
    char *p = b;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static void reply(int fd, char tag, uint32_t v)
{
    char b[5];
    b[0] = tag;
    memcpy(b + 1, &v, 4);
    (void)send_all(fd, b, 5);
}

/* ---- daemon ---------------------------------------------------------- */

struct cli { int fd; pid_t pid; };
static struct cli clis[MAXCLI];
static int sigpipe_w = -1;

static void on_chld(int s)
{
    int e = errno;
    (void)s;
    (void)!write(sigpipe_w, "c", 1);
    errno = e;
}

/* Read one request (argv + 3 fds) from connection `c`; create the tool. */
static void serve(int devfd, int c)
{
    char buf[MAXREQ + 1];
    uint32_t len = 0;
    int fds[3] = { -1, -1, -1 };
    union { struct cmsghdr h; char b[CMSG_SPACE(3 * sizeof(int))]; } cm;
    struct msghdr m;
    struct iovec iov = { &len, sizeof len };
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cm.b; m.msg_controllen = sizeof cm.b;
    if (recvmsg(c, &m, 0) != (ssize_t)sizeof len || len == 0 || len > MAXREQ) {
        close(c);
        return;
    }
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS &&
            h->cmsg_len == CMSG_LEN(3 * sizeof(int)))
            memcpy(fds, CMSG_DATA(h), 3 * sizeof(int));
    if (fds[0] < 0 || recv_all(c, buf, len) != 0) {
        for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
        close(c);
        return;
    }
    buf[len] = '\0';
    char *argv[256];
    int argc = 0;
    for (char *p = buf; p < buf + len && argc < 255; p += strlen(p) + 1)
        argv[argc++] = p;
    argv[argc] = NULL;

    int slot = -1;
    for (int i = 0; i < MAXCLI; i++) if (clis[i].fd < 0) { slot = i; break; }
    int hold[2];
    if (slot < 0 || argc == 0 || pipe(hold) != 0) {
        reply(c, 'R', 0x124);          /* SS$_INSFMEM */
        for (int i = 0; i < 3; i++) close(fds[i]);
        close(c);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        char b = 0;
        close(hold[1]);
        while (read(hold[0], &b, 1) < 0 && errno == EINTR)
            ;
        if (b != 'g') _exit(125);
        for (int i = 0; i < 3; i++) dup2(fds[i], i);
        closefrom(3);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execv(argv[0], argv);
        execvp(argv[0], argv);
        fprintf(stderr, "VMSCREATOR EXEC_FAILED %s errno=%d\n", argv[0], errno);
        _exit(127);
    }
    close(hold[0]);
    for (int i = 0; i < 3; i++) close(fds[i]);
    uint32_t st = 0x124;
    if (pid > 0) {
        struct vms_creprc_pcb_args a;
        memset(&a, 0, sizeof a);
        a.child_pid = (uint32_t)pid;
        a.flags = VMS_CREPRC_PCB_SUBPROCESS;
        st = kif_xport_ioctl(devfd, VMS_IOCTL_CREPRC_PCB, &a) == 0 ? a.status : 0x2C;
    }
    (void)!write(hold[1], (st & 1) ? "g" : "x", 1);
    close(hold[1]);
    if (!(st & 1)) {
        if (pid > 0) {
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
                ;
        }
        reply(c, 'R', st);
        close(c);
        return;
    }
    clis[slot].fd = c;
    clis[slot].pid = pid;
}

static int daemon_main(const char *path)
{
    int devfd = kif_xport_dev_open();
    if (devfd < 0) {
        printf("VMSCREATOR NODEV\n");
        return 3;
    }
    struct vms_establish_system_args es;
    memset(&es, 0, sizeof es);
    if (kif_xport_ioctl(devfd, VMS_IOCTL_ESTABLISH_SYSTEM, &es) != 0 || !(es.status & 1)) {
        printf("VMSCREATOR ESTABLISH_REFUSED status=0x%x\n", es.status);
        return 4;
    }
    int sp[2];
    if (pipe(sp) != 0) return 5;
    sigpipe_w = sp[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, path, sizeof un.sun_path - 1);
    unlink(path);
    if (ls < 0 || bind(ls, (struct sockaddr *)&un, sizeof un) != 0 || listen(ls, 16) != 0) {
        printf("VMSCREATOR LISTEN_FAILED errno=%d\n", errno);
        return 5;
    }
    for (int i = 0; i < MAXCLI; i++) clis[i].fd = -1;
    printf("VMSCREATOR READY sock=%s\n", path);
    fflush(stdout);

    for (;;) {
        struct pollfd pf[2 + MAXCLI];
        int map[2 + MAXCLI], n = 0;
        pf[n].fd = ls; pf[n].events = POLLIN; map[n++] = -1;
        pf[n].fd = sp[0]; pf[n].events = POLLIN; map[n++] = -2;
        for (int i = 0; i < MAXCLI; i++)
            if (clis[i].fd >= 0) { pf[n].fd = clis[i].fd; pf[n].events = POLLIN; map[n++] = i; }
        if (poll(pf, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            return 6;
        }
        for (int k = 0; k < n; k++) {
            if (!pf[k].revents) continue;
            if (map[k] == -1) {
                int c = accept(ls, NULL, NULL);
                if (c >= 0) serve(devfd, c);
            } else if (map[k] == -2) {
                char d[64];
                (void)!read(sp[0], d, sizeof d);
                int ws;
                pid_t p;
                while ((p = waitpid(-1, &ws, WNOHANG)) > 0) {
                    for (int i = 0; i < MAXCLI; i++)
                        if (clis[i].fd >= 0 && clis[i].pid == p) {
                            reply(clis[i].fd, 'S', (uint32_t)ws);
                            close(clis[i].fd);
                            clis[i].fd = -1;
                        }
                }
            } else {
                struct cli *cl = &clis[map[k]];
                char b[5];
                ssize_t r = read(cl->fd, b, sizeof b);
                if (r == 5 && b[0] == 'K') {
                    uint32_t sig;
                    memcpy(&sig, b + 1, 4);
                    kill(cl->pid, (int)sig);
                } else if (r <= 0) {
                    /* the client went away: so does its tool */
                    kill(cl->pid, SIGKILL);
                    close(cl->fd);
                    cl->fd = -1;
                }
            }
        }
    }
}

/* ---- client ---------------------------------------------------------- */

static int cli_fd = -1;

static void on_term(int s)
{
    char b[5];
    uint32_t v = (uint32_t)s;
    b[0] = 'K';
    memcpy(b + 1, &v, 4);
    (void)!write(cli_fd, b, 5);
}

static int client_main(const char *path, int argc, char **argv)
{
    char buf[MAXREQ];
    uint32_t len = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (len + l > MAXREQ) { fprintf(stderr, "VMSCREATOR TOOLONG\n"); return 2; }
        memcpy(buf + len, argv[i], l);
        len += (uint32_t)l;
    }
    cli_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    strncpy(un.sun_path, path, sizeof un.sun_path - 1);
    if (cli_fd < 0 || connect(cli_fd, (struct sockaddr *)&un, sizeof un) != 0) {
        fprintf(stderr, "VMSCREATOR NODAEMON %s errno=%d\n", path, errno);
        return 120;
    }
    int fds[3] = { 0, 1, 2 };
    union { struct cmsghdr h; char b[CMSG_SPACE(3 * sizeof(int))]; } cm;
    memset(&cm, 0, sizeof cm);
    struct iovec iov = { &len, sizeof len };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cm.b; m.msg_controllen = CMSG_SPACE(3 * sizeof(int));
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(3 * sizeof(int));
    memcpy(CMSG_DATA(h), fds, sizeof fds);
    if (sendmsg(cli_fd, &m, 0) != (ssize_t)sizeof len || send_all(cli_fd, buf, len) != 0) {
        fprintf(stderr, "VMSCREATOR SEND_FAILED errno=%d\n", errno);
        return 120;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    char r[5];
    if (recv_all(cli_fd, r, 5) != 0) {
        fprintf(stderr, "VMSCREATOR LOST_DAEMON\n");
        return 120;
    }
    uint32_t v;
    memcpy(&v, r + 1, 4);
    if (r[0] == 'R') {
        fprintf(stderr, "VMSCREATOR REFUSED status=0x%x\n", v);
        return 121;
    }
    int ws = (int)v;
    if (WIFEXITED(ws)) return WEXITSTATUS(ws);
    if (WIFSIGNALED(ws)) return 128 + WTERMSIG(ws);
    return 122;
}

int main(int argc, char **argv)
{
    const char *path = DEF_SOCK;
    if (argc >= 2 && strcmp(argv[1], "-d") == 0)
        return daemon_main(argc >= 3 ? argv[2] : DEF_SOCK);
    int i = 1;
    if (argc >= 3 && strcmp(argv[1], "-s") == 0) { path = argv[2]; i = 3; }
    if (i >= argc) {
        fprintf(stderr, "usage: vmscreator -d [sock] | vmscreator [-s sock] program [args...]\n");
        return 2;
    }
    return client_main(path, argc - i, argv + i);
}
