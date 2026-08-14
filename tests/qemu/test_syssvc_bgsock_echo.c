/*
 * test_syssvc_bgsock_echo.c - a program opens a TCP echo connection through the
 * OVMX BSD-SOCKETS RTL VENEER (src/vmstcpip/sockets/vms_bgsock.c) using STANDARD
 * socket calls -- ovmx_socket()/ovmx_connect() + read()/write() -- and the
 * veneer translates them into the $QIO-to-BGn: ops against a REAL /dev/vms.
 *
 * ============================================================
 * THE POINT. The veneer is the missing middle layer between an application and
 * the executive INET device: app -> socket()/connect() -> veneer -> $QIO to
 * BGn: -> vms.ko -> host kernel TCP/IP. The application speaks ONLY sockets --
 * it never mentions BGn:/$QIO/TCPIP$DEVICE: -- exactly as ssh will. This is the
 * OpenVMS TCP/IP Services sockets-library model (DECC$SOCKET). test_syssvc_bg_
 * echo.c proves the RAW $QIO path (vms-527); THIS suite proves the SOCKETS
 * VENEER over that path: the same 127.0.0.1 loopback echo, driven byte-exact
 * through ovmx_socket/ovmx_connect and an ordinary read()/write() on the
 * returned pollable fd (the pump bridges it to IO$_READVBLK/IO$_WRITEVBLK).
 * The peer is a plain userspace listener (the loopback other end); the CLIENT
 * side -- the whole $QIO path under the veneer -- is executive/in-kernel. It
 * needs no NIC and runs under `-nic none`: 127.0.0.1 is the kernel loopback,
 * which this test brings up itself.
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko): the
 * veneer's ovmx_socket() must fail SS$_NOSUCHDEV->ENODEV, never fabricate a
 * private per-process socket (CLAUDE.md Rule 9 / INV-6). A veneer that returned
 * a working raw Linux socket with no executive would be claiming a pass it never
 * earned; this suite returns EXIT_SKIP (77).
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the byte-exact echo assertion is anchored by the bgsock-recv-length-zeroed
 * defect, which zeroes the received byte count in the veneer's inbound pump
 * (vms_bgsock.c pump_in) -- so the echo comes back as EOF/short and only that
 * assertion reddens, while the socket/connect/write assertions stay green.
 *
 * WATCHDOG: the pump's blocking $QIO recv lives in the executive, so a wedged
 * peer would hang the QEMU boot rather than fail one line. alarm() bounds it.
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "vms/pcb.h"
#include "vms_kif.h"
#include "vms_bgsock.h"

#define EXIT_SKIP 77

static const char MSG[] = "OVMX BSD-sockets veneer over BGn: -- ssh rides this, vms-22a";

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bgsock_echo timed out (peer/echo wedge)\n";
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

/* Bring the kernel loopback up (init.sh does not, and `-nic none` boots lo
 * DOWN). Raw SIOCSIFFLAGS -- no busybox ip/ifconfig dependency. */
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

/* Loopback echo peer: accept one connection, read one buffer, write it back. */
static void *echo_peer(void *arg)
{
    int lsock = *(int *)arg;
    int c = accept(lsock, NULL, NULL);
    if (c >= 0) {
        char buf[256];
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n > 0)
            (void)!write(c, buf, (size_t)n);
        close(c);
    }
    close(lsock);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bgsock_echo (TCP echo through the BSD-sockets veneer over BGn:) ===\n");

    /* Pure-logic checks that need no executive: the numeric-IPv4 resolver. */
    {
        struct sockaddr_in sa;
        struct in_addr a;
        CHECK(ovmx_inet_pton(AF_INET, "127.0.0.1", &a) == 1 &&
              a.s_addr == htonl(INADDR_LOOPBACK),
              "ovmx_inet_pton parses 127.0.0.1 to network-order loopback");
        CHECK(ovmx_inet_pton(AF_INET, "not.an.ip", &a) == 0,
              "ovmx_inet_pton rejects a non-literal (no DNS yet)");
        CHECK(ovmx_getaddrinfo_numeric("127.0.0.1", 22, &sa) == 0 &&
              sa.sin_family == AF_INET && sa.sin_port == htons(22),
              "ovmx_getaddrinfo_numeric fills AF_INET sockaddr for a dotted-quad");
    }

    /* A per-process PCB is a prerequisite for every sys$ channel call. */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        /* NO EXECUTIVE: the veneer's socket() must fail honestly (Rule 9/INV-6),
         * never hand back a working raw Linux socket. */
        int fd = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd < 0 && errno == ENODEV,
              "no executive: ovmx_socket() fails ENODEV, never a local per-process socket");
        printf("=== test_syssvc_bgsock_echo: %d passed, %d failed (SKIPPED: no /dev/vms -- veneer echo not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(20);

    bring_lo_up();

    /* Loopback echo peer: bind 127.0.0.1:0, listen, learn the port. */
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

    pthread_t th;
    if (pthread_create(&th, NULL, echo_peer, &lsock) != 0) {
        printf("  FAIL: pthread_create(echo_peer) failed\n"); return 1;
    }

    /* ----- the application: STANDARD sockets through the veneer ----- */
    int fd = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "ovmx_socket() opens a BGn:-backed socket (executive-resident)");

    struct sockaddr_in peer;
    int r = ovmx_getaddrinfo_numeric("127.0.0.1", peer_port, &peer);
    CHECK(r == 0, "ovmx_getaddrinfo_numeric resolves 127.0.0.1:<peer>");

    int cst = ovmx_connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    CHECK(cst == 0, "ovmx_connect() connects to the 127.0.0.1 echo peer (IO$_ACCESS)");

    ssize_t w = write(fd, MSG, sizeof(MSG));
    CHECK(w == (ssize_t)sizeof(MSG),
          "write() on the veneer fd sends the message (pump -> IO$_WRITEVBLK)");

    /* Read the echo back byte-exact. TCP may split; loop until we have it all. */
    char rbuf[256];
    memset(rbuf, 0, sizeof(rbuf));
    size_t got = 0;
    while (got < sizeof(MSG)) {
        ssize_t n = read(fd, rbuf + got, sizeof(MSG) - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    /* negctl: bgsock-recv-length-zeroed */
    CHECK(got == sizeof(MSG) && memcmp(rbuf, MSG, sizeof(MSG)) == 0,
          "read() returns the echoed message BYTE-EXACT through the veneer (pump <- IO$_READVBLK)");

    ovmx_socket_close(fd);
    pthread_join(th, NULL);

    printf("=== test_syssvc_bgsock_echo: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
