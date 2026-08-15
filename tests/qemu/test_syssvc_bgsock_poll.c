/*
 * test_syssvc_bgsock_poll.c - a program waits on a BGn: socket with poll() the
 * ordinary way, using a REAL Linux readiness fd the OVMX BSD-sockets veneer
 * hands back (ovmx_pollfd -> VMS_IOCTL_BG_POLLFD), against a REAL /dev/vms.
 *
 * ============================================================
 * THE POINT -- select()-ABLE SOCKETS. Every sockets event loop -- OpenSSH's
 * clientloop/serverloop above all -- does poll()/select() on the connection fd
 * to wait for readability/writability. A VMS TCP/IP Services socket is
 * select()-able; so the OVMX veneer must surface a REAL pollable fd. The
 * executive (vms_bg.c) exposes one per BG channel: a READINESS-ONLY anon-inode
 * fd whose .poll delegates to the executive socket's own poll, so poll() blocks
 * and wakes on the true socket state while data still moves only through
 * IO$_READVBLK / IO$_WRITEVBLK (the socket stays executive-resident; the fd has
 * no read/write ops). This suite proves it end to end: obtain the readiness fd,
 * confirm it is NOT readable before any data (poll times out), send, then poll()
 * BLOCKS until the loopback peer's echo arrives and reports the fd readable, and
 * the byte-exact data is then read via the veneer. The peer is a plain userspace
 * loopback listener; the client side is executive/in-kernel. `-nic none`: 127.0.0.1
 * is the kernel loopback, brought up here.
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch): ovmx_socket() must fail
 * SS$_NOSUCHDEV->ENODEV (Rule 9 / INV-6); returns EXIT_SKIP (77).
 *
 * NEGATIVE CONTROL (facility_defects.sh): the "not readable before data"
 * assertion is anchored by bgsock-poll-always-ready, which makes the executive
 * readiness fd's .poll report EPOLLIN|EPOLLOUT unconditionally (ignoring the real
 * socket state) -- so poll() returns readable before any data arrives and only
 * that assertion reddens, while the readable-after-send and byte-exact reads
 * stay green.
 *
 * WATCHDOG: alarm() bounds a wedge (a poll() that never wakes) to a named FAIL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "vms/pcb.h"
#include "vms_kif.h"
#include "vms_bgsock.h"

#define EXIT_SKIP 77

static const char MSG[] = "OVMX BGn: readiness fd -- poll() waits, then read, vms-22a";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bgsock_poll timed out (poll never woke)\n";
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

/* Loopback echo peer: accept, read the whole message (robust to segmentation),
 * echo it back after a short delay so the client's pre-send poll() genuinely
 * sees "not readable yet". */
static void *echo_peer(void *arg)
{
    int lsock = *(int *)arg;
    int c = accept(lsock, NULL, NULL);
    if (c >= 0) {
        char buf[256];
        size_t got = 0;
        while (got < sizeof(MSG)) {
            ssize_t n = recv(c, buf + got, sizeof(MSG) - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got > 0) {
            size_t off = 0;
            while (off < got) {
                ssize_t w = write(c, buf + off, got - off);
                if (w <= 0) break;
                off += (size_t)w;
            }
        }
        close(c);
    }
    close(lsock);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bgsock_poll (poll() on a BGn: readiness fd over the veneer) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(h < 0 && errno == ENODEV,
              "no executive: ovmx_socket() fails ENODEV, never a local per-process socket");
        printf("=== test_syssvc_bgsock_poll: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(20);

    bring_lo_up();

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { printf("  FAIL: peer socket() failed\n"); return 1; }
    int one = 1;
    (void)setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = 0;
    if (bind(lsock, (struct sockaddr *)&la, sizeof(la)) < 0) {
        printf("  FAIL: peer bind(127.0.0.1) failed\n"); return 1;
    }
    if (listen(lsock, 1) < 0) { printf("  FAIL: peer listen() failed\n"); return 1; }
    socklen_t sl = sizeof(la);
    if (getsockname(lsock, (struct sockaddr *)&la, &sl) < 0) {
        printf("  FAIL: peer getsockname() failed\n"); return 1;
    }
    uint16_t peer_port = ntohs(la.sin_port);

    pthread_t peer_th;
    if (pthread_create(&peer_th, NULL, echo_peer, &lsock) != 0) {
        printf("  FAIL: pthread_create(echo_peer) failed\n"); return 1;
    }

    /* ----- open + connect the veneer socket ----- */
    int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(h >= 0, "ovmx_socket() opens a BGn:-backed socket");

    struct sockaddr_in peer;
    CHECK(ovmx_getaddrinfo_numeric("127.0.0.1", peer_port, &peer) == 0,
          "ovmx_getaddrinfo_numeric resolves 127.0.0.1:<peer>");
    CHECK(ovmx_connect(h, (struct sockaddr *)&peer, sizeof(peer)) == 0,
          "ovmx_connect() connects to the 127.0.0.1 echo peer");

    /* ----- the readiness fd: a REAL Linux fd usable with poll() ----- */
    int pfd = ovmx_pollfd(h);
    CHECK(pfd >= 0, "ovmx_pollfd() returns a real pollable fd for the connection");

    /* Before we send anything, the connection has no inbound data, so poll() for
     * readability must TIME OUT (return 0). This is the readiness fd reflecting
     * the socket's true state -- the property the negctl breaks. */
    struct pollfd pfds;
    pfds.fd = pfd;
    pfds.events = POLLIN;
    pfds.revents = 0;
    int pr = poll(&pfds, 1, 200);
    /* negctl: bgsock-poll-always-ready */
    CHECK(pr == 0,
          "poll() reports NOT readable before any data arrives (readiness reflects the socket)");

    /* Send, then poll() must BLOCK until the peer's echo makes the fd readable. */
    ssize_t w = ovmx_send(h, MSG, sizeof(MSG), 0);
    CHECK(w == (ssize_t)sizeof(MSG), "ovmx_send() writes the message");

    pfds.revents = 0;
    pr = poll(&pfds, 1, 5000);
    CHECK(pr == 1 && (pfds.revents & POLLIN),
          "poll() blocks then reports the fd READABLE when the peer's echo arrives");

    /* The data poll() promised is really there: read it byte-exact. */
    char rbuf[256];
    memset(rbuf, 0, sizeof(rbuf));
    size_t got = 0;
    while (got < sizeof(MSG)) {
        ssize_t n = ovmx_recv(h, rbuf + got, sizeof(MSG) - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    CHECK(got == sizeof(MSG) && memcmp(rbuf, MSG, sizeof(MSG)) == 0,
          "ovmx_recv() reads the byte-exact data poll() reported ready");

    close(pfd);
    ovmx_socket_close(h);
    pthread_join(peer_th, NULL);

    printf("=== test_syssvc_bgsock_poll: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
