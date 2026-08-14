/*
 * test_syssvc_bgsock_echo.c - a program opens a TCP echo connection through the
 * OVMX BSD-SOCKETS RTL VENEER (src/vmstcpip/sockets/vms_bgsock.c) using STANDARD
 * socket calls -- ovmx_socket()/ovmx_connect()/ovmx_send()/ovmx_recv() -- and
 * the veneer translates them into $QIO-to-BGn: ops against a REAL /dev/vms.
 *
 * ============================================================
 * THE POINT -- FULL-DUPLEX. The veneer is the missing middle layer between an
 * application and the executive INET device: app -> socket()/connect()/send()/
 * recv() -> veneer -> $QIO to BGn: -> vms.ko -> host kernel TCP/IP. The app
 * speaks ONLY sockets, exactly as ssh will. Every sockets app -- SSH above all
 * -- reads and writes the same connection CONCURRENTLY, so this suite proves
 * full-duplex: a READER THREAD blocks in ovmx_recv() (IO$_READVBLK) on the
 * connection while the main thread calls ovmx_send() (IO$_WRITEVBLK) on the SAME
 * handle. A wedge here would mean the executive serializes read vs write on a
 * channel; it does not (nothing holds a lock across the socket op, and the host
 * kernel socket is full-duplex), so the reader gets the echoed bytes back
 * byte-exact. test_syssvc_bg_echo.c proves the RAW $QIO path sequentially
 * (vms-527); THIS suite proves the SOCKETS VENEER with concurrent r/w.
 *
 * The peer is a plain userspace loopback listener (the other end); the CLIENT
 * side -- the whole $QIO path under the veneer -- is executive/in-kernel. Runs
 * under `-nic none`: 127.0.0.1 is the kernel loopback, brought up here.
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch): the veneer's ovmx_socket() must fail
 * SS$_NOSUCHDEV->ENODEV, never fabricate a private per-process socket (CLAUDE.md
 * Rule 9 / INV-6). Returns EXIT_SKIP (77).
 *
 * NEGATIVE CONTROL (facility_defects.sh): the byte-exact echo assertion is
 * anchored by bgsock-recv-length-zeroed, which zeroes the received byte count in
 * ovmx_recv() (vms_bgsock.c) -- so recv reports EOF and the reader gets nothing,
 * reddening only the byte-exact assertion while socket/connect/send stay green.
 *
 * WATCHDOG: the blocking $QIO recv lives in the executive; a wedge would hang the
 * QEMU boot, so alarm() bounds the run.
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

static const char MSG[] = "OVMX BSD-sockets veneer over BGn:, full-duplex -- ssh rides this, vms-22a";

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

/* Reader thread: blocks in ovmx_recv() on the veneer handle (concurrently with
 * the main thread's ovmx_send()) and accumulates the echoed reply byte-exact. */
struct reader_ctx {
    int      handle;
    char     buf[256];
    size_t   got;
    int      err;
};
static void *reader_thread(void *arg)
{
    struct reader_ctx *r = arg;
    r->got = 0;
    r->err = 0;
    while (r->got < sizeof(MSG)) {
        ssize_t n = ovmx_recv(r->handle, r->buf + r->got, sizeof(MSG) - r->got, 0);
        if (n < 0) { r->err = errno; break; }
        if (n == 0) break;              /* EOF */
        r->got += (size_t)n;
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bgsock_echo (full-duplex TCP echo through the BSD-sockets veneer over BGn:) ===\n");

    /* Pure-logic checks (no executive): the numeric-IPv4 resolver. */
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

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
        CHECK(h < 0 && errno == ENODEV,
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

    pthread_t peer_th;
    if (pthread_create(&peer_th, NULL, echo_peer, &lsock) != 0) {
        printf("  FAIL: pthread_create(echo_peer) failed\n"); return 1;
    }

    /* ----- the application: STANDARD sockets through the veneer ----- */
    int h = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    CHECK(h >= 0, "ovmx_socket() opens a BGn:-backed socket (executive-resident)");

    struct sockaddr_in peer;
    CHECK(ovmx_getaddrinfo_numeric("127.0.0.1", peer_port, &peer) == 0,
          "ovmx_getaddrinfo_numeric resolves 127.0.0.1:<peer>");

    CHECK(ovmx_connect(h, (struct sockaddr *)&peer, sizeof(peer)) == 0,
          "ovmx_connect() connects to the 127.0.0.1 echo peer (IO$_ACCESS)");

    /* Start the READER thread FIRST: it blocks in ovmx_recv() (IO$_READVBLK) on
     * the connection with nothing to read yet -- so when the main thread sends
     * below, a blocking recv and a send are OUTSTANDING AT THE SAME TIME on the
     * one channel. This is the full-duplex property the veneer must have. */
    struct reader_ctx rc;
    memset(&rc, 0, sizeof(rc));
    rc.handle = h;
    pthread_t rd_th;
    if (pthread_create(&rd_th, NULL, reader_thread, &rc) != 0) {
        printf("  FAIL: pthread_create(reader) failed\n"); return 1;
    }
    /* Give the reader a moment to enter its blocking recv before we send. */
    usleep(50 * 1000);

    ssize_t w = ovmx_send(h, MSG, sizeof(MSG), 0);
    CHECK(w == (ssize_t)sizeof(MSG),
          "ovmx_send() writes the message WHILE the reader blocks in recv (concurrent IO$_WRITEVBLK)");

    pthread_join(rd_th, NULL);

    /* negctl: bgsock-recv-length-zeroed */
    CHECK(rc.err == 0 && rc.got == sizeof(MSG) && memcmp(rc.buf, MSG, sizeof(MSG)) == 0,
          "the reader thread received the echoed message BYTE-EXACT concurrently with the send (full-duplex)");

    ovmx_socket_close(h);
    pthread_join(peer_th, NULL);

    printf("=== test_syssvc_bgsock_echo: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
