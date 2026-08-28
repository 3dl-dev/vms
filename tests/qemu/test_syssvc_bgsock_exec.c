/*
 * test_syssvc_bgsock_exec.c - the BGn: VENEER handle survives fork()+exec()
 * (rd vms-0cd; the userspace half of #815's channel-by-number inheritance).
 *
 * WHY. OpenSSH sshd's MASTER accepts a connection (a BGn: veneer handle), then
 * fork()+exec()s sshd-session to service it. #815 makes the EXECUTIVE channel
 * inherit by number across fork+exec; test_syssvc_bg_fork_inherit proves THAT via
 * the RAW kif. But sshd-session reaches the connection through the VENEER
 * (ovmx_send/recv on the handle), and the veneer's userspace handle->channel map
 * (g_socks) is a FRESH, empty table after execve. This test proves the veneer
 * layer works across exec: a fork()+exec()'d child, given only the inherited
 * handle NUMBER, drives ovmx_send/ovmx_recv on it and gets a BYTE-EXACT echo.
 *
 * It RED-fails when the handle is BASE+g_socks_index (the exec'd child's fresh
 * g_socks can't resolve it -> EBADF); it GREENS with the self-describing handle
 * (handle = BASE + exec_chan): chan_of() derives the channel with no g_socks, and
 * the state slot is lazily re-adopted. The connection socket is kref-shared across
 * the fork (#815), so the parent's loopback echo peer services the child's I/O.
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
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "vms_kif.h"
#include "vms_bgsock.h"

#define EXIT_SKIP 77
#define MSG "OVMX_EXEC_OK"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bgsock_exec timed out\n";
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

/* init.sh does not bring loopback up under -nic none; do it ourselves. */
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

/* Loopback echo peer: accept one connection and echo bytes back until close. */
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

int main(int argc, char **argv)
{
    /* Child (post-exec): drive the INHERITED veneer handle by NUMBER. */
    if (argc == 2 && strncmp(argv[1], "--child-io=", 11) == 0) {
        int h = atoi(argv[1] + 11);
        char rb[64] = {0};
        ssize_t w = ovmx_send(h, MSG, sizeof(MSG), 0);
        ssize_t r = (w == (ssize_t)sizeof(MSG)) ? ovmx_recv(h, rb, sizeof(MSG), 0) : -1;
        /* exit 0 iff the veneer resolved the inherited handle AND round-tripped
         * the bytes byte-exact; anything else (EBADF, short, mismatch) is nonzero. */
        return (r == (ssize_t)sizeof(MSG) && memcmp(rb, MSG, sizeof(MSG)) == 0) ? 0 : 1;
    }

    signal(SIGALRM, watchdog);
    alarm(30);
    printf("test_syssvc_bgsock_exec: BGn: veneer handle survives fork()+exec()\n");

    if (!executive_present()) {
        int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(h < 0 && errno == ENODEV,
              "no executive: ovmx_socket() fails ENODEV, never a local socket");
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    bring_lo_up();

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

    /* Parent: connect over the veneer, get a handle, then hand it to a
     * fork()+exec()'d child. */
    int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(h >= 0, "ovmx_socket() returns an executive-resident veneer handle");
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer.sin_port = la.sin_port;
    CHECK(ovmx_connect(h, (struct sockaddr *)&peer, sizeof(peer)) == 0,
          "ovmx_connect() connects the veneer handle to the loopback echo peer");

    char harg[32];
    snprintf(harg, sizeof(harg), "--child-io=%d", h);
    pid_t c = fork();
    if (c == 0) {
        execl(argv[0], argv[0], harg, (char *)NULL);
        _exit(127);
    }
    int cst = 0;
    waitpid(c, &cst, 0);
    /* negctl: bgsock-recv-length-zeroed */
    CHECK(WIFEXITED(cst) && WEXITSTATUS(cst) == 0,
          "a fork()+exec()'d child drove ovmx_send/recv on the INHERITED veneer handle byte-exact (self-describing handle survives exec, vms-0cd)");

    ovmx_socket_close(h);
    printf("=== test_syssvc_bgsock_exec: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
