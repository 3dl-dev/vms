/*
 * decnetd.c - the OVMX DECnet Phase IV routing ENGINE daemon (rd vms-449d,
 *             engine rung 1 of epic vms-30e).
 *
 * This is the DECnet analogue of src/vmsscs/scsd.c: a userspace daemon that
 * owns a raw-L2 datalink and speaks a DEC wire protocol over it, while
 * presenting a VMS-faithful surface upward. It is the ONLY place the AF_PACKET
 * socket is touched (scs_datalink_{open,send,recv} -- the SAME generic raw-L2
 * abstraction scsd.c uses, deliberately written engine-agnostic for exactly
 * this second consumer, see src/vmsscs/scs_datalink.h). Everything the wire
 * logic does lives in the pure, socketless engine core (dnet_engine.{c,h}),
 * which drives the three landed codecs.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The raw socket and the Linux/NetBSD
 * interface name are the HIDDEN mechanism. What this daemon puts on stdout is
 * the DECnet routing surface an NCP user sees -- an executor node, a circuit,
 * and a live adjacency table (SHOW ADJACENT NODES) -- plus DECNETD-I- facility
 * messages in the scsd house style. It never prints a raw socket or a bare
 * `ethN`, exactly as scsd never exposes its AF_PACKET fd behind the SCS face.
 *
 * OPERATOR RULING 2026-08-31 (rd vms-a1c): Option B -- userspace AF_PACKET, not
 * an in-kernel AF_DECnet forward-port. See docs/decnet-provenance-register.md
 * sec 6.
 *
 * SCOPE (rung 1, rd vms-449d): datalink open (fail-honest, INV-6) + endnode
 * HELLO transmit on the T3 cadence + receive/decode of peer HELLOs driving the
 * adjacency SM + the VMS presentation surface. NOT in this rung (filed as
 * children of vms-30e): the NSP logical-link connection service (vms-c23) and
 * the live-VAX oracle adjacency bracket (vms-aac0).
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): wire form from public DNA Phase IV + the
 * vms-3be lab capture + OVMX's own scsd datalink pattern. No VSI/HPE source.
 */
#include <errno.h>
#include <net/if.h>      /* if_nametoindex() */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dnet_engine.h"
#include "scs_datalink.h"   /* the shared raw-L2 datalink (src/vmsscs) */

/* Default datalink interface, matching scsd's br0 default (the lab-2 pod
 * bridge model that carries raw Phase IV multicast; SLIRP cannot, see
 * docs/decnet-provenance-register.md sec 4.2). */
#define DECNETD_DEFAULT_IFACE  "br0"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int signo) { (void)signo; g_stop = 1; }

/* A monotonic seconds tick -- the unit the engine's T3/listen timers use. */
static dnet_tick_t monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (dnet_tick_t)ts.tv_sec;
}

static void log_ts(FILE *out)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    time_t t = ts.tv_sec;
    gmtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof(b), "%d-%b-%Y %H:%M:%S", &tmv);
    fprintf(out, "%s", b);
}

/* Parse "area.node" (e.g. "1.42"). Returns 0 on success. */
static int parse_addr(const char *s, unsigned *area, unsigned *node)
{
    if (!s)
        return -1;
    char *end = NULL;
    long a = strtol(s, &end, 10);
    if (end == s || *end != '.')
        return -1;
    char *end2 = NULL;
    long n = strtol(end + 1, &end2, 10);
    if (end2 == end + 1 || *end2 != '\0')
        return -1;
    if (a < 1 || a > 63 || n < 1 || n > 1023)
        return -1;
    *area = (unsigned)a;
    *node = (unsigned)n;
    return 0;
}

/*
 * ============================ --self-test =============================
 * A no-privilege, no-netdev proof that the engine really MOVES a HELLO frame
 * and drives adjacency: it stands up TWO engines (a "left" and a "right"
 * node) and shuttles their built HELLO frames between them over a real
 * socketpair(2) -- genuine write(2)/read(2) of the actual encoded bytes, not
 * an in-memory handoff -- then asserts the adjacency SM advances. This is the
 * DECnet analogue of scsd's --dlm-selftest: it runs anywhere (Docker/CI, no
 * CAP_NET_RAW), and it exercises the exact build_hello_frame -> wire ->
 * rx_frame path the live datalink uses, so a green self-test is a real
 * tx/rx/decode/SM proof, not a facade (INV-6).
 *
 * Returns 0 on PASS, 1 on FAIL.
 */
static int run_self_test(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, socketpair failed: %s\n",
                strerror(errno));
        return 1;
    }

    const uint8_t hwL[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    const uint8_t hwR[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
    struct dnet_engine L, R;
    /* left = 1.10 (OVMXL), right = 1.11 (OVMXR); both endnodes. */
    if (dnet_engine_init(&L, 1, 10, "OVMXL", "EWA0", NULL, hwL, 0, 0, 0) != 0 ||
        dnet_engine_init(&R, 1, 11, "OVMXR", "EWA0", NULL, hwR, 0, 0, 0) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, engine init failed\n");
        close(sv[0]); close(sv[1]);
        return 1;
    }

    int fail = 0;
    uint8_t frame[DNET_FRAME_MAX];
    uint8_t rxbuf[DNET_FRAME_MAX];
    size_t flen = 0;
    ssize_t n;
    enum dnet_adj_state st = DNET_ADJ_DOWN;
    uint8_t from[6];
    dnet_tick_t now = 100;

    /* 1) LEFT emits a plain endnode HELLO -> RIGHT. RIGHT should move the
     *    neighbour to INITIALIZING (one-way: our HELLO names no router). */
    if (dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, build HELLO (L) failed\n");
        fail = 1; goto done;
    }
    if (write(sv[0], frame, flen) != (ssize_t)flen) {
        fprintf(stderr, "DECNETD-E-SELFTEST, write failed: %s\n", strerror(errno));
        fail = 1; goto done;
    }
    n = read(sv[1], rxbuf, sizeof(rxbuf));
    if (n <= 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, read failed: %s\n", strerror(errno));
        fail = 1; goto done;
    }
    if (dnet_engine_rx_frame(&R, now, rxbuf, (size_t)n, from, &st) != 1 ||
        st != DNET_ADJ_INITIALIZING) {
        fprintf(stderr, "DECNETD-E-SELFTEST, R did not reach INITIALIZING (st=%d)\n",
                (int)st);
        fail = 1; goto done;
    }

    /* 2) RIGHT emits a HELLO that NAMES LEFT as its neighbour (the two-way
     *    handshake). Feed it to a fresh view on LEFT: LEFT should reach UP. */
    {
        /* Build RIGHT's HELLO, then overwrite its routing neighbour field so it
         * names LEFT -- the DNA two-way reachability signal. We do this at the
         * decoded-struct layer via the codec to stay honest to the wire form. */
        struct dnet_endnode_hello h;
        memset(&h, 0, sizeof(h));
        h.rflags  = DNET_RFLAG_ENDNODE_HELLO;
        h.version = 2;
        memcpy(h.id, R.my_id, 6);
        h.iinfo   = DNET_NODETYPE_ENDNODE;
        h.blksize = 1498;
        h.timer   = 15;
        memcpy(h.neighbor, L.my_id, 6);   /* names LEFT => two-way */
        h.datalen = 0;
        uint8_t payload[DNET_FRAME_MAX];
        size_t plen = 0;
        if (dnet_hello_encode(&h, payload, sizeof(payload), &plen) != DNET_HELLO_OK) {
            fprintf(stderr, "DECNETD-E-SELFTEST, encode (R two-way) failed\n");
            fail = 1; goto done;
        }
        /* Prepend the Ethernet header (dst=mcast, src=R id, type 0x6003). */
        uint8_t f2[DNET_FRAME_MAX];
        memcpy(f2, DNET_HELLO_MCAST, 6);
        memcpy(f2 + 6, R.my_id, 6);
        f2[12] = 0x60; f2[13] = 0x03;
        memcpy(f2 + DNET_ETH_HDRLEN, payload, plen);
        size_t f2len = DNET_ETH_HDRLEN + plen;

        if (write(sv[1], f2, f2len) != (ssize_t)f2len) {
            fprintf(stderr, "DECNETD-E-SELFTEST, write2 failed\n");
            fail = 1; goto done;
        }
        n = read(sv[0], rxbuf, sizeof(rxbuf));
        if (n <= 0) {
            fprintf(stderr, "DECNETD-E-SELFTEST, read2 failed\n");
            fail = 1; goto done;
        }
        st = DNET_ADJ_DOWN;
        if (dnet_engine_rx_frame(&L, now + 1, rxbuf, (size_t)n, from, &st) != 1 ||
            st != DNET_ADJ_UP) {
            fprintf(stderr, "DECNETD-E-SELFTEST, L did not reach UP (st=%d)\n",
                    (int)st);
            fail = 1; goto done;
        }
    }

    /* 3) LEFT's neighbour (RIGHT) must age to DOWN once the listen timer lapses
     *    with no further HELLO (T4 = BCT3MULT * T3 = 30 s). */
    if (dnet_engine_tick(&L, now + 1 + 31) < 1 ||
        dnet_adj_state_of(&L.adj, R.my_id) != DNET_ADJ_DOWN) {
        fprintf(stderr, "DECNETD-E-SELFTEST, L neighbour did not age to DOWN\n");
        fail = 1; goto done;
    }

    /* 4) An echo of our OWN frame must be ignored (src == my_id). */
    if (dnet_engine_build_hello_frame(&L, frame, sizeof(frame), &flen) != 0 ||
        dnet_engine_rx_frame(&L, now + 40, frame, flen, NULL, NULL) != 0) {
        fprintf(stderr, "DECNETD-E-SELFTEST, own-echo was not ignored\n");
        fail = 1; goto done;
    }

done:
    close(sv[0]);
    close(sv[1]);
    if (fail) {
        printf("DECNETD-SELFTEST: FAIL\n");
        return 1;
    }
    printf("DECNETD-I-SELFTEST, engine tx/rx/adjacency proof PASSED"
           " (HELLO moved over a real socketpair; INITIALIZING->UP->DOWN + own-echo drop)\n");
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --address AREA.NODE [options]\n"
        "  --address A.N       DECnet Phase IV executor address (REQUIRED;\n"
        "                      1..63 . 1..1023). No identity is invented if\n"
        "                      omitted -- the daemon exits (INV-6).\n"
        "  --name NAME         NCP node name (1..6 chars; default OVMX)\n"
        "  --iface IFNAME      datalink interface (default %s)\n"
        "  --device DEV        VMS device label for the circuit (default EWA0)\n"
        "  --circuit CIRC      DECnet circuit name (default derived, e.g. EWA-0)\n"
        "  --hello-interval N  HELLO cadence T3 seconds (default %u, oracle vms-3be)\n"
        "  --duration N        run N seconds then exit (default: until SIGINT/TERM)\n"
        "  --show-executor     print the NCP executor summary and exit (no socket)\n"
        "  --self-test         run the in-process tx/rx/adjacency proof and exit\n"
        "                      (no CAP_NET_RAW, no netdev -- moves a real HELLO\n"
        "                      frame over a socketpair; DECnet analogue of\n"
        "                      scsd --dlm-selftest)\n",
        argv0, DECNETD_DEFAULT_IFACE, (unsigned)DNET_T3_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *ifname = DECNETD_DEFAULT_IFACE;
    const char *addr_s = NULL;
    const char *name = "OVMX";
    const char *device = "EWA0";
    const char *circuit = NULL;
    int hello_interval = (int)DNET_T3_DEFAULT;
    int duration = 0;
    int show_executor_only = 0;
    int self_test = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--address") && i + 1 < argc)      addr_s = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)    name = argv[++i];
        else if (!strcmp(argv[i], "--iface") && i + 1 < argc)   ifname = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)  device = argv[++i];
        else if (!strcmp(argv[i], "--circuit") && i + 1 < argc) circuit = argv[++i];
        else if (!strcmp(argv[i], "--hello-interval") && i + 1 < argc)
            hello_interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show-executor")) show_executor_only = 1;
        else if (!strcmp(argv[i], "--self-test"))     self_test = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "DECNETD-E-BADARG, unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (self_test)
        return run_self_test();

    /* Identity is required and never invented (INV-6; the scsd
     * resolve_node_identity discipline: a wrong identity must never be made up). */
    unsigned area = 0, node = 0;
    if (!addr_s || parse_addr(addr_s, &area, &node) != 0) {
        fprintf(stderr, "DECNETD-E-NOADDRESS, a valid --address AREA.NODE is"
                        " required (1..63 . 1..1023); refusing to invent an"
                        " executor address\n");
        return 1;
    }

    if (hello_interval < 1)
        hello_interval = (int)DNET_T3_DEFAULT;

    /* --show-executor: report the identity/circuit this endnode would adopt and
     * exit, opening NO socket (needs no privilege). Analogue of scsd
     * --show-identity. */
    if (show_executor_only) {
        struct dnet_engine e;
        uint8_t zmac[6] = {0};
        if (dnet_engine_init(&e, area, node, name, device, circuit, zmac,
                             (uint16_t)hello_interval, 0, monotonic_sec()) != 0) {
            fprintf(stderr, "DECNETD-E-INIT, engine init failed\n");
            return 1;
        }
        dnet_engine_show_executor(&e, stdout);
        dnet_engine_show_circuit(&e, stdout);
        return 0;
    }

    /* Open the raw-L2 datalink (INV-6 fail-honest: no per-process fake if the
     * netdev cannot be opened). Same abstraction scsd.c uses. */
    int sock = scs_datalink_open(ifname, DNET_ETHERTYPE);
    if (sock < 0) {
        fprintf(stderr,
                "DECNETD-E-NOSOCKET, scs_datalink_open('%s', 0x%04x) failed: %s\n"
                "  (Linux needs CAP_NET_RAW -- run as root or"
                " setcap cap_net_raw+ep on this binary; the interface must exist"
                " and share the Phase IV L2 segment)\n",
                ifname, (unsigned)DNET_ETHERTYPE, strerror(errno));
        return 1;
    }
    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "DECNETD-E-NOIFACE, unknown interface '%s': %s\n",
                ifname, strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }
    uint8_t hw_mac[6] = {0};
    if (scs_datalink_get_hwaddr(ifname, hw_mac) != 0) {
        fprintf(stderr, "DECNETD-E-NOHWADDR, cannot read HW address of '%s': %s\n",
                ifname, strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }
    /* Wake the receive loop about once a second so the T3 cadence and the
     * listen-timer sweep still fire on an idle wire (as scsd does). */
    if (scs_datalink_set_recv_timeout(sock, 1) < 0) {
        fprintf(stderr, "DECNETD-E-RCVTIMEO, set_recv_timeout failed: %s\n",
                strerror(errno));
        scs_datalink_close(sock);
        return 1;
    }

    struct dnet_engine eng;
    if (dnet_engine_init(&eng, area, node, name, device, circuit, hw_mac,
                         (uint16_t)hello_interval, 0, monotonic_sec()) != 0) {
        fprintf(stderr, "DECNETD-E-INIT, engine init failed\n");
        scs_datalink_close(sock);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (duration > 0) {
        signal(SIGALRM, on_signal);
        alarm((unsigned)duration);
    }

    /* Startup: the VMS-visible face (never the raw socket). */
    log_ts(stdout);
    printf(" DECNETD-I-STARTED, DECnet Phase IV endnode up on circuit %s"
           " (datalink hidden behind the VMS surface, Rule 1)\n", eng.circuit);
    dnet_engine_show_executor(&eng, stdout);
    dnet_engine_show_circuit(&eng, stdout);
    fflush(stdout);

    uint8_t frame[DNET_FRAME_MAX];
    uint8_t rxbuf[DNET_FRAME_MAX];

    while (!g_stop) {
        dnet_tick_t now = monotonic_sec();

        /* T3 emission cadence: build + transmit our endnode HELLO. */
        if (dnet_engine_hello_due(&eng, now)) {
            size_t flen = 0;
            if (dnet_engine_build_hello_frame(&eng, frame, sizeof(frame), &flen)
                    == DNET_ENGINE_OK) {
                ssize_t sent = scs_datalink_send(sock, (int)ifindex,
                                                 DNET_ETHERTYPE, DNET_HELLO_MCAST,
                                                 frame, flen);
                if (sent < 0) {
                    fprintf(stderr, "DECNETD-E-SENDFAIL, HELLO transmit failed: %s\n",
                            strerror(errno));
                } else {
                    dnet_engine_hello_emitted(&eng, now);
                    log_ts(stdout);
                    printf(" DECNETD-I-HELLOSENT, circuit %s seq=%lu bytes=%zd\n",
                           eng.circuit, eng.hello_sent, sent);
                    fflush(stdout);
                }
            }
        }

        /* Age out adjacencies whose listen timer lapsed. */
        int gone = dnet_engine_tick(&eng, now);
        if (gone > 0) {
            log_ts(stdout);
            printf(" DECNETD-I-ADJDOWN, %d adjacency(ies) timed out on circuit %s\n",
                   gone, eng.circuit);
            fflush(stdout);
        }

        ssize_t n = scs_datalink_recv(sock, rxbuf, sizeof(rxbuf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue; /* timer wakeup / signal -- re-check cadence */
            fprintf(stderr, "DECNETD-E-RECVFAIL, recv failed: %s\n", strerror(errno));
            break;
        }
        char afrom[8];
        uint8_t from[6];
        enum dnet_adj_state st = DNET_ADJ_DOWN;
        int rc = dnet_engine_rx_frame(&eng, now, rxbuf, (size_t)n, from, &st);
        if (rc == 1) {
            uint16_t na = dnet_addr_from_id(from);
            if (st == DNET_ADJ_UP) {
                log_ts(stdout);
                printf(" DECNETD-I-ADJUP, adjacency to %s is up on circuit %s\n",
                       dnet_addr_str(na, afrom, sizeof(afrom)), eng.circuit);
            } else {
                log_ts(stdout);
                printf(" DECNETD-I-ADJINIT, heard %s (%s) on circuit %s\n",
                       dnet_addr_str(na, afrom, sizeof(afrom)),
                       st == DNET_ADJ_INITIALIZING ? "initializing" : "?",
                       eng.circuit);
            }
            fflush(stdout);
        }
    }

    log_ts(stdout);
    printf(" DECNETD-I-STOPPING, shutting down circuit %s\n", eng.circuit);
    dnet_engine_show_adjacent(&eng, stdout);
    printf("DECNETD-I-COUNTERS, hello_sent=%lu hello_recv=%lu frames_recv=%lu"
           " frames_dropped=%lu adj_up=%lu adj_down=%lu\n",
           eng.hello_sent, eng.hello_recv, eng.frames_recv, eng.frames_dropped,
           eng.adj_up_events, eng.adj_down_events);
    fflush(stdout);

    scs_datalink_close(sock);
    return 0;
}
