/*
 * test_syssvc_bgsock_peername.c - the OpenSSH DE-VENEER proof (vms-4bf, parent
 * vms-843). Over a REAL BGn: TCP connection through the executive to the host
 * kernel stack, prove the socket surface an UNMODIFIED OpenSSH needs actually
 * TOUCHES VMS rather than a bypass:
 *
 *   getpeername() returns the ACTUAL remote IP:port of the peer          <== the
 *     (so OpenSSH's known_hosts records the real host, not an AF_UNIX     anti-
 *      socketpair peer) -- the answer comes from the executive-resident   veneer
 *      kernel socket via IO$_SENSEMODE (VMS_IOCTL_BG_GETNAME), NOT from a  bar
 *      userspace fd;
 *   getsockname() returns the local endpoint (also from the kernel socket);
 *   setsockopt()/getsockopt() are HONORED against the real kernel socket
 *     (TCP_NODELAY set to 1 then read back 1, set to 0 then read back 0;
 *      SO_KEEPALIVE likewise) -- NOT swallowed as ENOPROTOOPT the way a
 *      socketpair degrades them;
 *   fcntl(O_NONBLOCK) makes recv return EAGAIN when the connection would block
 *     (gated on the executive readiness fd -- no would-block $QIO), and an
 *     unsupported option fails ENOPROTOOPT (honest, not a fake success).
 *
 * The peer is a plain userspace loopback listener; the CLIENT side -- the whole
 * $QIO path under the veneer -- is executive/in-kernel. Runs under `-nic none`:
 * 127.0.0.1 is the kernel loopback, brought up here.
 *
 * NO EXECUTIVE (honest-failure branch): ovmx_socket() must fail
 * SS$_NOSUCHDEV->ENODEV, never a private per-process socket (CLAUDE.md Rule 9 /
 * INV-6). Returns EXIT_SKIP (77).
 *
 * NEGATIVE CONTROL (facility_defects.sh): the getpeername-returns-real-IP
 * assertion is anchored by bgsock-getname-addr-zeroed, which zeroes the
 * sin_addr the executive copies out of the kernel socket in vms_ioctl_bg_getname
 * (vms_bg.c) -- so getpeername/getsockname report 0.0.0.0 and the address
 * assertions redden while the port, sockopt and nonblock assertions stay green.
 *
 * WATCHDOG: the connect + probes run against a real executive; a wedge would
 * hang the QEMU boot, so alarm() bounds the run.
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "vms/pcb.h"
#include "vms_kif.h"
#include "vms_bgsock.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bgsock_peername timed out (connect/probe wedge)\n";
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

/* Loopback peer: accept one connection and HOLD it open (block on a recv that
 * never returns data) until the client closes, so the connection is live the
 * whole time the client probes getpeername/getsockopt on it. */
static void *hold_peer(void *arg)
{
    int lsock = *(int *)arg;
    int c = accept(lsock, NULL, NULL);
    if (c >= 0) {
        char b[16];
        (void)recv(c, b, sizeof(b), 0);     /* returns 0 when the client closes */
        close(c);
    }
    close(lsock);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bgsock_peername (OpenSSH de-veneer: getpeername/setsockopt over $QIO on BGn:) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(h < 0 && errno == ENODEV,
              "no executive: ovmx_socket() fails ENODEV, never a local per-process socket");
        printf("=== test_syssvc_bgsock_peername: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(20);

    bring_lo_up();

    /* Loopback peer: bind 127.0.0.1:0, listen, learn the port. */
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
    if (pthread_create(&peer_th, NULL, hold_peer, &lsock) != 0) {
        printf("  FAIL: pthread_create(hold_peer) failed\n"); return 1;
    }

    /* ----- the application: STANDARD sockets through the veneer ----- */
    int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(h >= 0, "ovmx_socket() opens a BGn:-backed socket (executive-resident)");

    struct sockaddr_in peer;
    CHECK(ovmx_getaddrinfo_numeric("127.0.0.1", peer_port, &peer) == 0,
          "ovmx_getaddrinfo_numeric resolves 127.0.0.1:<peer>");
    CHECK(ovmx_connect(h, (struct sockaddr *)&peer, sizeof(peer)) == 0,
          "ovmx_connect() connects to the 127.0.0.1 peer (IO$_ACCESS)");

    /* ===== A1: getpeername returns the REAL remote IP:port (the anti-veneer bar) ===== */
    {
        struct sockaddr_in pn;
        socklen_t pl = sizeof(pn);
        memset(&pn, 0, sizeof(pn));
        int rc = ovmx_getpeername(h, (struct sockaddr *)&pn, &pl);
        CHECK(rc == 0 && pn.sin_family == AF_INET,
              "ovmx_getpeername() succeeds with AF_INET (IO$_SENSEMODE on the kernel socket)");
        /* negctl: bgsock-getname-addr-zeroed */
        CHECK(rc == 0 && pn.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
              "getpeername() returns the REAL remote IP 127.0.0.1 (known_hosts sees the true host, not an AF_UNIX peer)");
        CHECK(rc == 0 && ntohs(pn.sin_port) == peer_port,
              "getpeername() returns the REAL remote port (the peer's listening port)");
    }

    /* ===== getsockname returns the local endpoint from the kernel socket ===== */
    {
        struct sockaddr_in sn;
        socklen_t snl = sizeof(sn);
        memset(&sn, 0, sizeof(sn));
        int rc = ovmx_getsockname(h, (struct sockaddr *)&sn, &snl);
        CHECK(rc == 0 && sn.sin_family == AF_INET &&
              sn.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && sn.sin_port != 0,
              "ovmx_getsockname() returns the local 127.0.0.1:<ephemeral> endpoint");
    }

    /* ===== A3: setsockopt/getsockopt HONORED against the real kernel socket ===== */
    {
        int v = 1, got = -1; socklen_t gl = sizeof(got);
        CHECK(ovmx_setsockopt(h, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0,
              "ovmx_setsockopt(TCP_NODELAY=1) applied to the real kernel socket (IO$_SETMODE)");
        CHECK(ovmx_getsockopt(h, IPPROTO_TCP, TCP_NODELAY, &got, &gl) == 0 && got == 1,
              "ovmx_getsockopt(TCP_NODELAY) reads back 1 -- honored, NOT swallowed");

        v = 0; got = -1; gl = sizeof(got);
        CHECK(ovmx_setsockopt(h, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0 &&
              ovmx_getsockopt(h, IPPROTO_TCP, TCP_NODELAY, &got, &gl) == 0 && got == 0,
              "ovmx_setsockopt(TCP_NODELAY=0) round-trips back to 0 (the value tracks the real socket)");

        v = 1; got = -1; gl = sizeof(got);
        CHECK(ovmx_setsockopt(h, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof(v)) == 0 &&
              ovmx_getsockopt(h, SOL_SOCKET, SO_KEEPALIVE, &got, &gl) == 0 && got == 1,
              "ovmx_setsockopt(SO_KEEPALIVE=1) round-trips back to 1 on the real socket");
    }

    /* ===== unsupported option fails ENOPROTOOPT (honest, not a fake success) ===== */
    {
        int v = 4096; socklen_t gl = sizeof(v);
        CHECK(ovmx_getsockopt(h, IPPROTO_TCP, 0x7fff /* no such option */, &v, &gl) == -1 &&
              errno == ENOPROTOOPT,
              "getsockopt() of an unsupported option fails ENOPROTOOPT (honest, never faked)");
    }

    /* ===== A5: fcntl(O_NONBLOCK) -> recv would-block returns EAGAIN ===== */
    {
        char b[8];
        ssize_t n;
        CHECK(ovmx_fcntl(h, F_SETFL, O_NONBLOCK) == 0 &&
              (ovmx_fcntl(h, F_GETFL, 0) & O_NONBLOCK),
              "ovmx_fcntl(F_SETFL,O_NONBLOCK) sets non-blocking (F_GETFL reads it back)");
        errno = 0;
        n = ovmx_recv(h, b, sizeof(b), 0);
        CHECK(n < 0 && errno == EAGAIN,
              "non-blocking ovmx_recv() with no data returns EAGAIN (gated on the executive readiness fd, no blocking $QIO)");
    }

    ovmx_socket_close(h);           /* unblocks the peer's recv (EOF) */
    pthread_join(peer_th, NULL);

    printf("=== test_syssvc_bgsock_peername: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
