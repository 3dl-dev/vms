/*
 * test_syssvc_bg_udp.c - a UDP datagram socket on the INET pseudo-device BGn:
 * (vms-527), its receive timeout, and the DNS resolver that rides it, through
 * the PUBLIC $ASSIGN / $QIO / $DASSGN services against a REAL /dev/vms
 * (vms-a9e7).
 *
 * ============================================================
 * WHAT THIS PROVES. vms-a9e7 adds a third socket KIND to the executive: IO$_SETMODE
 * with P2 = IO$K_SOCK_UDP has vms.ko mint an AF_INET/SOCK_DGRAM/IPPROTO_UDP host
 * socket, and the IO$_SETMODE P4 socket-option list (TCPIP$C_RCVTIMEO) arms a
 * receive timeout. Against the kernel loopback (brought up here; no NIC, runs
 * under `-nic none`) the suite asserts:
 *   1. a UDP datagram round-trip over BGn: is BYTE-EXACT: IO$_WRITEVBLK reaches
 *      an in-guest UDP echo peer, IO$_READVBLK returns exactly what it echoed;
 *   2. IO$_READVBLK on a UDP channel connected to a SILENT port, with RCVTIMEO
 *      armed, completes SS$_TIMEOUT within bounds instead of blocking forever;
 *   3. the resolver engine the DCL TELNET/FTP/PING verbs ship
 *      (src/vmstcpip/services/tcpip_resolve.h, #included here so the shipped
 *      code and the proven code are one) resolves a name, follows a CNAME, and
 *      reports NXDOMAIN and a silent server (TIMEOUT) distinctly -- against an
 *      in-guest DNS responder that also sends a wrong-ID decoy first, which the
 *      resolver must ignore.
 * The echo peer and the DNS responder are TEST FIXTURES (the far end of the
 * wire, plain loopback sockets); the side under test -- the client -- touches
 * the network only through BGn:. The host kernel's UDP carries every datagram.
 * ============================================================
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is
 * loaded): $ASSIGN TCPIP$DEVICE: fails SS$_NOSUCHDEV and the resolver reports
 * it, never a private per-process socket (CLAUDE.md Rule 9 / INV-6). Returns
 * EXIT_SKIP (77) with no /dev/vms, as every BGn: suite does.
 *
 * NEGATIVE CONTROLS (tests/qemu/defects.d): bg-udp-recv-timeout-not-reported
 * (vms_bg.c reports a receive timeout as SS$_ABORT) and
 * tcpip-resolve-cname-not-followed (the parser stops at a CNAME).
 *
 * WATCHDOG: a blocking $QIO recv lives in the executive; alarm() bounds the run
 * so a missing timeout cannot hang the QEMU boot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

/* The resolver engine under test -- the SAME header the DCL verbs ship. */
#include "../../src/vmstcpip/services/tcpip_resolve.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char UDP_PAYLOAD[] =
    "OVMX-UDP vms-a9e7: one datagram, BYTE-EXACT, through BGn: SOCK_DGRAM.";

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bg_udp timed out (a BGn: read never completed)\n";
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

/* Fixture: a loopback UDP socket on an ephemeral port; *port gets it (host order). */
static int fixture_udp(uint16_t *port)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        getsockname(s, (struct sockaddr *)&sa, &sl) < 0) {
        close(s);
        return -1;
    }
    *port = ntohs(sa.sin_port);
    return s;
}

/* $ASSIGN TCPIP$DEVICE: + IO$_SETMODE(UDP) + IO$_ACCESS 127.0.0.1:port. */
static uint32_t bg_udp_open(uint16_t port, unsigned short *chan)
{
    static const char devname[] = "TCPIP$DEVICE:";
    struct dsc$descriptor_s dev;
    struct tcpip_res_sockaddr sa;
    struct _iosb iosb;
    uint32_t st;

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;
    st = sys$assign(&dev, chan, 0, NULL);
    if (!(st & 1)) return st;
    st = tcpip_qiow(*chan, IO$_SETMODE, &iosb, NULL, IO$K_SOCK_UDP, 0);
    if (!(st & 1)) { sys$dassgn(*chan); return st; }
    sa.family = 2;
    sa.port   = tcpip_port_be(port);
    sa.addr   = htonl(INADDR_LOOPBACK);
    st = tcpip_qiow(*chan, IO$_ACCESS, &iosb, &sa, (uint32_t)sizeof(sa), 0);
    if (!(st & 1)) { sys$dassgn(*chan); return st; }
    return SS$_NORMAL;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- the DNS responder fixture (RFC 1035 replies built by the test) ---- */

struct dns_fixture { int sock; int answered; };

static size_t fx_name(unsigned char *b, size_t n, const char *name)
{
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        b[n++] = (unsigned char)l;
        memcpy(b + n, p, l);
        n += l;
        p += l;
        if (*p == '.') p++;
    }
    b[n++] = 0;
    return n;
}

/* Answer one query: vaxa.lab.test -> A 10.11.12.13; alias.lab.test -> CNAME
 * vaxa.lab.test + its A; anything else NXDOMAIN. A wrong-ID decoy precedes
 * every real reply. */
static void *dns_responder(void *arg)
{
    struct dns_fixture *fx = (struct dns_fixture *)arg;
    int k;

    for (k = 0; k < 3; k++) {
        unsigned char q[512], r[512];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        char qname[256];
        size_t qoff = 12, qend, n = 0;
        ssize_t ql = recvfrom(fx->sock, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (ql < 17) break;
        /* decode the (uncompressed) question name the client sent */
        {
            size_t o = 0;
            while (qoff < (size_t)ql && q[qoff] && o + q[qoff] + 2 < sizeof(qname)) {
                if (o) qname[o++] = '.';
                memcpy(qname + o, q + qoff + 1, q[qoff]);
                o += q[qoff];
                qoff += 1 + q[qoff];
            }
            qname[o] = '\0';
            qend = qoff + 1 + 4;               /* root + QTYPE + QCLASS */
            if (qend > (size_t)ql) break;
        }
        /* decoy: right question, wrong ID, claims an answer */
        memcpy(r, q, qend);
        r[0] ^= 0x5a;
        r[2] = 0x81; r[3] = 0x80; r[7] = 1;
        n = qend;
        r[n++] = 0xc0; r[n++] = 12; r[n++] = 0; r[n++] = 1; r[n++] = 0; r[n++] = 1;
        r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 60; r[n++] = 0; r[n++] = 4;
        r[n++] = 6; r[n++] = 6; r[n++] = 6; r[n++] = 6;
        (void)sendto(fx->sock, r, n, 0, (struct sockaddr *)&from, fl);

        /* the real reply */
        memcpy(r, q, qend);
        r[2] = 0x81; r[3] = 0x80; r[6] = 0; r[7] = 0;
        r[8] = r[9] = r[10] = r[11] = 0;
        n = qend;
        if (strcasecmp(qname, "vaxa.lab.test") == 0) {
            r[7] = 1;
            r[n++] = 0xc0; r[n++] = 12; r[n++] = 0; r[n++] = 1; r[n++] = 0; r[n++] = 1;
            r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 60; r[n++] = 0; r[n++] = 4;
            r[n++] = 10; r[n++] = 11; r[n++] = 12; r[n++] = 13;
        } else if (strcasecmp(qname, "alias.lab.test") == 0) {
            size_t rdlen_at, rdata, cname_at;
            r[7] = 2;
            r[n++] = 0xc0; r[n++] = 12; r[n++] = 0; r[n++] = 5; r[n++] = 0; r[n++] = 1;
            r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 60;
            rdlen_at = n; n += 2;
            rdata = n;
            cname_at = n;
            n = fx_name(r, n, "vaxa.lab.test");
            r[rdlen_at] = 0; r[rdlen_at + 1] = (unsigned char)(n - rdata);
            r[n++] = (unsigned char)(0xc0 | (cname_at >> 8)); r[n++] = (unsigned char)cname_at;
            r[n++] = 0; r[n++] = 1; r[n++] = 0; r[n++] = 1;
            r[n++] = 0; r[n++] = 0; r[n++] = 0; r[n++] = 60; r[n++] = 0; r[n++] = 4;
            r[n++] = 10; r[n++] = 11; r[n++] = 12; r[n++] = 13;
        } else {
            r[3] = 0x83;                       /* NXDOMAIN */
        }
        (void)sendto(fx->sock, r, n, 0, (struct sockaddr *)&from, fl);
        fx->answered++;
    }
    return NULL;
}

int main(void)
{
    const uint32_t lo_be = htonl(INADDR_LOOPBACK);
    unsigned short chan = 0;
    struct _iosb iosb;
    uint32_t st, addr = 0, vst = 0;
    enum tcpip_dns_rc rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_bg_udp (UDP over BGn: $QIO + RCVTIMEO + resolver, vms-a9e7) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        rc = tcpip_dns_query_server(lo_be, 53, "vaxa.lab.test", 500, &addr, &vst);
        CHECK(rc == TCPIP_DNS_NONET && vst == SS$_NOSUCHDEV,
              "no executive: the resolver fails SS$_NOSUCHDEV, never a local per-process socket");
        printf("=== test_syssvc_bg_udp: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    signal(SIGALRM, watchdog);
    alarm(60);
    bring_lo_up();

    /* 1. UDP datagram round-trip through BGn: to an in-guest echo fixture. */
    {
        uint16_t port = 0;
        int peer = fixture_udp(&port);
        unsigned char buf[256], rbuf[256];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t got = -1;
        uint32_t wst = 0, rst = 0, bcnt = 0;

        st = (peer >= 0) ? bg_udp_open(port, &chan) : SS$_ABORT;
        CHECK(st & 1, "BGn: IO$_SETMODE P2=IO$K_SOCK_UDP + IO$_ACCESS create and connect a UDP socket");
        if (st & 1) {
            wst = tcpip_qiow(chan, IO$_WRITEVBLK, &iosb, (void *)UDP_PAYLOAD,
                             (uint32_t)(sizeof(UDP_PAYLOAD) - 1), 0);
            if (wst & 1) {
                struct timeval tv = { 5, 0 };
                setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                got = recvfrom(peer, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                if (got > 0)
                    (void)sendto(peer, buf, (size_t)got, 0, (struct sockaddr *)&from, fl);
            }
            memset(rbuf, 0, sizeof(rbuf));
            if (got > 0) {
                rst = tcpip_qiow(chan, IO$_READVBLK, &iosb, rbuf, (uint32_t)sizeof(rbuf), 0);
                bcnt = iosb.iosb$w_bcnt;
            }
            sys$qiow(0, chan, IO$_DEACCESS, &iosb, NULL, 0, NULL, 0, 0, 0, 0, 0);
            sys$dassgn(chan);
        }
        CHECK((wst & 1) && got == (ssize_t)(sizeof(UDP_PAYLOAD) - 1) &&
              memcmp(buf, UDP_PAYLOAD, sizeof(UDP_PAYLOAD) - 1) == 0,
              "BGn: IO$_WRITEVBLK delivers one UDP datagram to the peer byte-exact");
        CHECK((rst & 1) && bcnt == sizeof(UDP_PAYLOAD) - 1 &&
              memcmp(rbuf, UDP_PAYLOAD, sizeof(UDP_PAYLOAD) - 1) == 0,
              "BGn: IO$_READVBLK returns the echoed UDP datagram byte-exact");
        if (peer >= 0) close(peer);
    }

    /* 2. RCVTIMEO: a read on a UDP channel connected to a silent port times out. */
    {
        uint16_t port = 0;
        int silent = fixture_udp(&port);
        unsigned char rbuf[64];
        double t0, dt = -1;

        st = (silent >= 0) ? bg_udp_open(port, &chan) : SS$_ABORT;
        if (st & 1)
            st = tcpip_bg_set_rcvtimeo(chan, 500);
        CHECK(st & 1, "BGn: IO$_SETMODE P4 TCPIP$C_RCVTIMEO arms a receive timeout");
        if (st & 1) {
            (void)tcpip_qiow(chan, IO$_WRITEVBLK, &iosb, "x", 1, 0);
            t0 = now_s();
            st = tcpip_qiow(chan, IO$_READVBLK, &iosb, rbuf, (uint32_t)sizeof(rbuf), 0);
            dt = now_s() - t0;
            sys$qiow(0, chan, IO$_DEACCESS, &iosb, NULL, 0, NULL, 0, 0, 0, 0, 0);
            sys$dassgn(chan);
        }
        printf("  info: silent-port read ended status %u after %.3f s\n", (unsigned)st, dt);
        /* negctl: bg-udp-recv-timeout-not-reported */
        CHECK(st == SS$_TIMEOUT && dt >= 0.3 && dt < 5.0,
              "BGn: IO$_READVBLK on a silent UDP peer completes SS$_TIMEOUT within bounds");
        if (silent >= 0) close(silent);
    }

    /* 3. The resolver over BGn: UDP against an in-guest DNS responder. */
    {
        uint16_t port = 0;
        struct dns_fixture fx;
        pthread_t th;
        int started;

        fx.sock = fixture_udp(&port);
        fx.answered = 0;
        started = (fx.sock >= 0) &&
                  pthread_create(&th, NULL, dns_responder, &fx) == 0;
        CHECK(started, "DNS responder fixture listening on loopback");
        if (started) {
            addr = 0;
            rc = tcpip_dns_query_server(lo_be, port, "vaxa.lab.test", 2000, &addr, &vst);
            CHECK(rc == TCPIP_DNS_OK && addr == htonl(0x0a0b0c0du),
                  "resolver: A record for vaxa.lab.test over BGn: UDP (wrong-ID decoy ignored)");
            addr = 0;
            rc = tcpip_dns_query_server(lo_be, port, "ALIAS.lab.test", 2000, &addr, &vst);
            /* negctl: tcpip-resolve-cname-not-followed */
            CHECK(rc == TCPIP_DNS_OK && addr == htonl(0x0a0b0c0du),
                  "resolver: follows the CNAME alias.lab.test -> vaxa.lab.test to its A record");
            rc = tcpip_dns_query_server(lo_be, port, "nope.lab.test", 2000, &addr, &vst);
            CHECK(rc == TCPIP_DNS_NXDOMAIN,
                  "resolver: an unknown name is reported NXDOMAIN, distinctly");
            pthread_join(th, NULL);
        }
        if (fx.sock >= 0) close(fx.sock);
    }

    /* 4. The resolver against a silent server reports a timeout, not a hang. */
    {
        uint16_t port = 0;
        int silent = fixture_udp(&port);
        double t0 = now_s(), dt;
        rc = (silent >= 0)
             ? tcpip_dns_query_server(lo_be, port, "vaxa.lab.test", 300, &addr, &vst)
             : TCPIP_DNS_NONET;
        dt = now_s() - t0;
        printf("  info: silent-server resolve ended rc %d after %.3f s\n", (int)rc, dt);
        CHECK(rc == TCPIP_DNS_TIMEOUT && dt < 10.0,
              "resolver: a silent name server is reported as a timeout (one retry), not a hang");
        if (silent >= 0) close(silent);
    }

    alarm(0);
    printf("=== test_syssvc_bg_udp: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
