/*
 * test_syssvc_bg_materialize_fd.c - the executive materializes an accepted/connected
 * BGn: channel as a REAL Linux fd whose read()/write() route to the executive socket
 * (rd vms-0cd, RUNG-3b). This is the last structural piece for a wrapped Unix daemon
 * (sshd) to hand a BGn: connection to its exec'd child.
 *
 * WHY. A ported daemon hands a connection to its per-connection child by dup2()'ing
 * it onto stdin/stdout, then execv()'ing the child, which does ordinary read()/
 * write() on fd 0/1. A BGn: veneer HANDLE (>= OVMX_BGSOCK_BASE) is not a real fd, so
 * dup2(handle, 0) fails EBADF -- exactly where RUNG-3a's wrapped sshd stopped.
 * VMS_IOCTL_BG_MATERIALIZE_FD (ovmx_materialize_fd) returns a REAL fd for the
 * channel's executive socket: dup2-able, and with NO O_CLOEXEC so it survives
 * execve.
 *
 * WHAT IT PROVES, end to end:
 *   1. ovmx_materialize_fd returns a REAL small-integer fd (< OVMX_BGSOCK_BASE), not
 *      a veneer handle -- the thing dup2 accepts.
 *   2. dup2(realfd, target) SUCCEEDS -- the EBADF that killed 3a's sshd is gone.
 *   3. a fork()+exec()'d child, given ONLY the inherited real fd number, does raw
 *      write()/read() on it and gets a BYTE-EXACT echo -- so the fd's fops genuinely
 *      route to the executive socket, AND it survives execve. The child issues NO
 *      $QIO and opens NO /dev/vms (just like sshd-session): the kernel fops carry it.
 *   INV-6 (a): the process holds NO AF_UNIX socket fd -- data transits the executive
 *      socket, never a fabricated socketpair (the vms-9ac excision must stay dead).
 *   INV-6 (b): materialize on a bogus handle FAILS HONEST (-1), never a fabricated fd.
 *
 * Honest Rule-9 skip with no /dev/vms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <dirent.h>

#include "vms_kif.h"
#include "vms_bgsock.h"

#define EXIT_SKIP  77
#define MSG        "OVMX_MATERIALIZE_OK"
#define CHILD_FD   20              /* the fd we dup2 the materialized fd onto */

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bg_materialize_fd timed out\n";
    (void)!write(1, m, sizeof(m) - 1);
    _exit(1);
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

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

/* Loopback echo peer: accept one connection, echo bytes until close. */
static void *echo_peer(void *arg)
{
    int lsock = *(int *)arg;
    int c = accept(lsock, NULL, NULL);
    char buf[256];
    ssize_t n;
    if (c < 0) return NULL;
    while ((n = read(c, buf, sizeof(buf))) > 0)
        (void)!write(c, buf, (size_t)n);
    close(c);
    return NULL;
}

/* INV-6(a): does this process hold ANY AF_UNIX socket fd? The materialized fd is a
 * vms.ko [bgconn] anon_inode, not a socket at all, so a surviving AF_UNIX socket
 * would mean a fabricated socketpair is back. /proc/self/fd socket inodes vs
 * /proc/net/unix. */
static int proc_has_afunix_socket(void)
{
    unsigned long uinodes[4096];
    size_t nu = 0;
    char line[512], tgt[128], fp[280];
    struct dirent *de;
    DIR *d;
    int found = 0;
    FILE *u = fopen("/proc/net/unix", "r");
    if (!u) return 0;
    if (!fgets(line, sizeof(line), u)) { fclose(u); return 0; }
    while (fgets(line, sizeof(line), u) && nu < 4096) {
        char c[6][48];
        unsigned long ino = 0;
        if (sscanf(line, "%47s %47s %47s %47s %47s %47s %lu",
                   c[0], c[1], c[2], c[3], c[4], c[5], &ino) >= 7 && ino != 0)
            uinodes[nu++] = ino;
    }
    fclose(u);
    d = opendir("/proc/self/fd");
    if (!d) return 0;
    while (!found && (de = readdir(d)) != NULL) {
        ssize_t r; unsigned long ino = 0;
        if (de->d_name[0] == '.') continue;
        snprintf(fp, sizeof(fp), "/proc/self/fd/%s", de->d_name);
        r = readlink(fp, tgt, sizeof(tgt) - 1);
        if (r <= 0) continue;
        tgt[r] = '\0';
        if (sscanf(tgt, "socket:[%lu]", &ino) == 1 && ino != 0)
            for (size_t i = 0; i < nu; i++)
                if (uinodes[i] == ino) { found = 1; break; }
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    /* Child (post-exec): drive the INHERITED REAL fd by NUMBER with raw read/write.
     * No $QIO, no /dev/vms -- exactly what sshd-session does on its stdin/stdout. */
    if (argc == 2 && strncmp(argv[1], "--child-io=", 11) == 0) {
        int fd = atoi(argv[1] + 11);
        char rb[64] = {0};
        ssize_t w = write(fd, MSG, sizeof(MSG));
        ssize_t r = (w == (ssize_t)sizeof(MSG)) ? read(fd, rb, sizeof(MSG)) : -1;
        return (r == (ssize_t)sizeof(MSG) && memcmp(rb, MSG, sizeof(MSG)) == 0) ? 0 : 1;
    }

    signal(SIGALRM, watchdog);
    alarm(30);
    printf("test_syssvc_bg_materialize_fd: accepted BGn: channel -> real fd (dup2-able, survives exec)\n");

    if (!executive_present()) {
        int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(h < 0 && errno == ENODEV,
              "no executive: ovmx_socket() fails ENODEV, never a local socket");
        printf("=== test_syssvc_bg_materialize_fd: SKIPPED (no /dev/vms) ===\n");
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    bring_lo_up();

    /* Loopback echo peer. */
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la;
    socklen_t sl = sizeof(la);
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lsock < 0 || bind(lsock, (struct sockaddr *)&la, sizeof(la)) < 0 ||
        listen(lsock, 1) < 0 || getsockname(lsock, (struct sockaddr *)&la, &sl) < 0) {
        printf("  FAIL: loopback peer setup\n"); return 1;
    }
    pthread_t pt;
    pthread_create(&pt, NULL, echo_peer, &lsock);

    /* Connect over the veneer -> a connected executive-resident socket (handle). */
    int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(h >= 0, "ovmx_socket() returns an executive-resident veneer handle");
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = la.sin_port;
    CHECK(ovmx_connect(h, (struct sockaddr *)&peer, sizeof(peer)) == 0,
          "ovmx_connect() connects the veneer handle to the loopback echo peer");

    /* (2b) INV-6 fail-honest: materialize on a bogus handle must fail, never fabricate. */
    int bogus = ovmx_materialize_fd(OVMX_BGSOCK_BASE + 0x0EEEEEEE);
    CHECK(bogus < 0,
          "ovmx_materialize_fd on a bogus handle fails HONEST (-1), never a fabricated fd (INV-6)");
    if (bogus >= 0) close(bogus);

    /* (1) Materialize the connected channel as a REAL fd. */
    int realfd = ovmx_materialize_fd(h);
    CHECK(realfd >= 0,
          "ovmx_materialize_fd returns a real fd for the executive socket");
    CHECK(realfd >= 0 && realfd < OVMX_BGSOCK_BASE,
          "the materialized fd is a REAL small-integer fd (< OVMX_BGSOCK_BASE), not a veneer handle -- the thing dup2 accepts");

    /* (2c) the materialized fd answers getpeername() from the executive socket with
     * the TRUE peer -- the SAME endpoint this connection was made to. This is what
     * lets a wrapped daemon's exec'd child (sshd-session) getpeername() its stdin.
     * INV-6: a REAL peer read from the REAL executive socket, never synthesized. */
    if (realfd >= 0) {
        struct sockaddr_in pn, rn;
        socklen_t pl = sizeof(pn), rl = sizeof(rn);
        int gr = ovmx_fd_getname(realfd, 1, (struct sockaddr *)&pn, &pl);
        CHECK(gr == 0 && pn.sin_family == AF_INET &&
              pn.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
              pn.sin_port == peer.sin_port,
              "getpeername() on the materialized fd returns the TRUE peer (the real "
              "connection endpoint from the executive socket), not a synthesized value");
        /* regression teeth: a REAL host socket is NOT a [bgconn], so ovmx_fd_getname
         * reports 1 (not mine) and the --wrap falls through to the real syscall --
         * getpeername on real sockets/stdio is untouched. */
        CHECK(ovmx_fd_getname(lsock, 1, (struct sockaddr *)&rn, &rl) == 1,
              "ovmx_fd_getname on a REAL host socket returns 1 (not a [bgconn]) -- "
              "getpeername/getsockname/setsockopt on real fds are left to the kernel");
    }

    /* (2a) dup2 onto a target fd SUCCEEDS -- the EBADF that killed 3a's sshd is gone. */
    int dr = (realfd >= 0) ? dup2(realfd, CHILD_FD) : -1;
    CHECK(dr == CHILD_FD,
          "dup2(materialized_fd, target) succeeds -- a wrapped daemon can dup2 the connection onto stdio (the RUNG-3a EBADF is gone)");

    /* (3) fork()+exec() a child that does RAW read/write on the inherited real fd. */
    if (dr == CHILD_FD) {
        char harg[32];
        snprintf(harg, sizeof(harg), "--child-io=%d", CHILD_FD);
        pid_t c = fork();
        if (c == 0) {
            execl(argv[0], argv[0], harg, (char *)NULL);
            _exit(127);
        }
        int cst = 0;
        waitpid(c, &cst, 0);
        /* negctl: bg-materialize-fd-not-routed */
        CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0,
              "a fork()+exec()'d child (no $QIO, no /dev/vms) does raw write()/read() on the "
              "inherited real fd byte-exact -- the fd's fops route to the executive socket AND survive execve (vms-0cd)");
    }

    /* (INV-6 a) no AF_UNIX socket fd -- the data path is the executive socket, not a socketpair. */
    CHECK(!proc_has_afunix_socket(),
          "no AF_UNIX socket fd in this process -- the materialized fd is a vms.ko [bgconn], data transits the executive, no fabricated socketpair (INV-6)");

    if (realfd >= 0) { close(CHILD_FD); close(realfd); }
    ovmx_socket_close(h);
    printf("=== test_syssvc_bg_materialize_fd: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
