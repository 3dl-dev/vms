/*
 * scsd.c - SCSD skeleton: the SCS Datalink daemon.
 *
 * Per docs/design-cluster-node.md section 3.1: a new src/vmsscs/ subsystem,
 * a userspace daemon that opens an AF_PACKET raw socket bound to the
 * cluster interface, receives SCA frames (ethertype 0x6007, DEC LAVC/SCA),
 * and classifies them by the GROUNDED length rule (see scs_classify.h /
 * docs/cluster-protocol-spec.md section 2).
 *
 * SCOPE (vms-294): raw socket open + bind + receive loop + length
 * classification + logging + CMake wiring. RECEIVE-ONLY by default.
 *
 * vms-b62 adds the TRANSMIT path: --emit-hello enables a periodic
 * multicast HELLO beacon (scs_hello.c builds the frame; see
 * docs/cluster-protocol-spec.md sec 4a/4b for the wire layout). Still NO
 * NISCA virtual-circuit engine, NO SCS connection multiplexing, NO
 * directed HELLOs, NO connect/membership -- those are later items
 * (vms-5fe). Node identity (SCSNODE) comes from the SYSGEN store
 * (sysgen_read_string, honors OVMX_SYSGEN_PATH); cluster group comes from
 * the cluster-authorize store (cluster_authorize_read, honors
 * OVMX_CLUSTER_AUTH_PATH), defaulting to group 1 (the reference lab's
 * group) if unconfigured.
 *
 * Requires CAP_NET_RAW (run as root, or `setcap cap_net_raw+ep` on the
 * built binary) -- AF_PACKET SOCK_RAW sockets need it.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cluster_authorize.h"
#include "scs_classify.h"
#include "scs_connect.h"
#include "scs_hello.h"
#include "scs_start.h"
#include "scs_vc.h"
#include "sysgen_params.h"

/* DEC LAVC/SCA ethertype -- docs/cluster-protocol-spec.md sec 2. */
#define SCA_ETHERTYPE 0x6007

/* NISCS_MAX_PKTSZ=1498 (GROUNDED against the SYSGEN tunable, spec sec 2
 * Table 2) plus the 14-byte Ethernet header, rounded up. */
#define SCA_FRAME_MAX 1600

/* Real VAX HELLOs beacon every ~1-2s (observed inter-frame timing in
 * scs-idle-baseline.pcap); default our beacon to the same cadence. */
#define HELLO_DEFAULT_INTERVAL_SEC 2

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/* Monotonic milliseconds -- feeds the VC retransmit timer (scs_vc_*). */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* vms-691: VC keepalive/retransmit tunables. The VAX beacons directed HELLOs
 * on its poller sweep; OVMX re-sends its connect-request if the peer has not
 * acked it within RETRANSMIT_TIMEOUT_MS, capped at RETRANSMIT_MAX to respect
 * the reply-amplification guard (never out-pace real-node cadence). */
#define VC_RETRANSMIT_TIMEOUT_MS 2000u
#define VC_RETRANSMIT_MAX        5u

static void log_ts(FILE *out)
{
    struct timespec ts;
    struct tm tmv;
    char tbuf[16];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tmv);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
    fprintf(out, "[%s.%03ld]", tbuf, ts.tv_nsec / 1000000);
}

/*
 * get_iface_hwaddr - Resolve ifname's hardware (MAC) address via
 * SIOCGIFHWADDR on a throwaway AF_INET socket (works for both physical
 * NICs and bridge devices like br0).
 */
static int get_iface_hwaddr(const char *ifname, uint8_t mac_out[6])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
        close(s);
        return -1;
    }
    memcpy(mac_out, ifr.ifr_hwaddr.sa_data, 6);
    close(s);
    return 0;
}

/*
 * resolve_node_identity - Read SCSNODE from the SYSGEN store (vms-ci.8;
 * sysgen_read_string honors OVMX_SYSGEN_PATH). Falls back to "OVMX" if
 * unconfigured or the store is missing -- matches the pre-vms-ci.8
 * hardcoded default noted in docs/design-cluster-node.md sec 6.
 */
static void resolve_node_identity(char *node_out, size_t node_out_len)
{
    char configured[SYSGEN_STRVAL_LEN];
    if (sysgen_read_string("SCSNODE", configured, sizeof(configured)) == 0 && configured[0] != '\0') {
        strncpy(node_out, configured, node_out_len - 1);
        node_out[node_out_len - 1] = '\0';
    } else {
        strncpy(node_out, "OVMX", node_out_len - 1);
        node_out[node_out_len - 1] = '\0';
    }
}

/*
 * resolve_cluster_group - Read the cluster group number from the
 * cluster-authorize store (vms-ci.8). Falls back to group 1 -- the only
 * GROUNDED group (the reference lab) -- if unconfigured.
 */
static uint16_t resolve_cluster_group(void)
{
    struct cluster_authorize auth;
    if (cluster_authorize_read(&auth) == 0) {
        return auth.group;
    }
    return SCS_HELLO_MCAST_GROUP1;
}

/* OVMX's default SCSSYSTEMID when the SYSGEN store has none configured. The
 * reference lab uses 1025 (VAX1), 1026 (VAX2), 1027 (VAX3 satellite); 1030 is
 * a non-colliding default so OVMX presents a distinct cluster identity. The
 * live value should come from the vms-ci.8 SYSGEN store (SET SCSSYSTEMID). */
#define OVMX_DEFAULT_SCSSYSTEMID 1030u

/*
 * resolve_scssystemid - Read SCSSYSTEMID from the SYSGEN store (vms-ci.8),
 * falling back to OVMX_DEFAULT_SCSSYSTEMID. Truncated to the 16-bit field the
 * phase-2 START body carries (spec sec 4g [46:48]).
 */
static uint16_t resolve_scssystemid(void)
{
    uint32_t v = 0;
    if (sysgen_read_param("SCSSYSTEMID", &v) == 0 && v != 0) {
        return (uint16_t)(v & 0xffffu);
    }
    return OVMX_DEFAULT_SCSSYSTEMID;
}

/* --- vms-5fe: directed-HELLO / SCS-connect responder state --- */

#define OVMX_MAX_PEERS 4

/* OVMX's own VMS$VAXcluster Con.ID for this run (OVMX design choice; opaque
 * to the peer -- see scs_connect.h). */
#define OVMX_LOCAL_CONID (SCS_CONNECT_OVMX_CONID_BASE | 0x0001u)

/* Absolute frame offsets used by the responder (spec byte-offset convention:
 * 0 = first byte of Ethernet dst). */
#define OFF_ETH_DST      0
#define OFF_ETH_SRC      6
#define OFF_HELLO_SRCLOG 24  /* HELLO SCA src-logical addr (abs 24-29) */
#define OFF_HELLO_DIRFLG 92  /* directed-HELLO flag (abs 92-93) */

struct peer_state {
    int      in_use;
    uint8_t  eth_mac[6];      /* peer's Ethernet source MAC (we reply here) */
    uint8_t  logical[6];      /* peer's advertised SCA src-logical addr (HELLO abs 24) */
    int      channel_up;      /* >=1 directed HELLO exchanged */
    long     directed_replies;
    struct timespec last_directed; /* CLOCK_MONOTONIC of our last directed reply (rate limit) */
    int      connect_sent;    /* we sent a CONNECT-REQUEST */
    int      connected;       /* Con.ID pair bound */
    uint32_t remote_conid;    /* peer's Con.ID once learned */
    /* --- vms-21e / vms-691: SCS sequenced-message VC engine state --- */
    struct scs_vc vc;              /* seq/ack tracking + credit + retransmit (embeds scs_seq_state) */
    int      start_replied;        /* we answered the peer's 0x41 START (sent round 0/1) */
    int      start_acked;          /* we sent the round-2 46-byte ack -> START complete */
    long     start_replies;        /* count of 0x41 frames we sent to this peer */
    long     credit_sent;          /* vms-691: 0x48 credit-returns we sent this peer */
};

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static struct peer_state *peer_find_or_add(struct peer_state *tbl, const uint8_t mac[6])
{
    int free_slot = -1;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (tbl[i].in_use && mac_eq(tbl[i].eth_mac, mac)) {
            return &tbl[i];
        }
        if (!tbl[i].in_use && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return NULL;
    }
    memset(&tbl[free_slot], 0, sizeof(tbl[free_slot]));
    tbl[free_slot].in_use = 1;
    memcpy(tbl[free_slot].eth_mac, mac, 6);
    return &tbl[free_slot];
}

/* Send a fully-built Ethernet frame to a specific unicast MAC on ifindex. */
static ssize_t send_frame_to(int sock, int ifindex, const uint8_t mac[6],
                             const uint8_t *frame, size_t len)
{
    struct sockaddr_ll da;
    memset(&da, 0, sizeof(da));
    da.sll_family = AF_PACKET;
    da.sll_protocol = htons(SCA_ETHERTYPE);
    da.sll_ifindex = ifindex;
    da.sll_halen = 6;
    memcpy(da.sll_addr, mac, 6);
    return sendto(sock, frame, len, 0, (struct sockaddr *)&da, sizeof(da));
}

int main(int argc, char **argv)
{
    const char *ifname = "br0";
    int duration = 0; /* 0 = run until SIGINT/SIGTERM */
    int emit_hello = 0;
    int respond = 0;      /* vms-5fe: reply to directed HELLOs (channel formation) */
    int do_connect = 0;   /* vms-5fe: also drive the VMS$VAXcluster SCS connect */
    int hello_interval = HELLO_DEFAULT_INTERVAL_SEC;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            ifname = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--emit-hello") == 0) {
            emit_hello = 1;
        } else if (strcmp(argv[i], "--respond") == 0) {
            respond = 1;
        } else if (strcmp(argv[i], "--connect") == 0) {
            do_connect = 1;
        } else if (strcmp(argv[i], "--hello-interval") == 0 && i + 1 < argc) {
            hello_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                    "usage: %s [--iface IFACE] [--duration SECONDS] [--emit-hello]\n"
                    "          [--respond] [--connect] [--hello-interval SECONDS]\n"
                    "  SCS datalink listener: opens an AF_PACKET raw socket on\n"
                    "  ethertype 0x%04x (DEC SCA/LAVC), classifies received frames\n"
                    "  by the GROUNDED length rule, and logs them.\n"
                    "  --emit-hello        also multicast a spec-valid HELLO beacon\n"
                    "                      (identity from SCSNODE/cluster-group config;\n"
                    "                      see docs/cluster-protocol-spec.md sec 4a/4b)\n"
                    "  --respond           reply to a peer's DIRECTED HELLO with our own\n"
                    "                      directed HELLO to form the NISCA channel\n"
                    "                      (implies --emit-hello; spec sec 4b)\n"
                    "  --connect           additionally drive the VMS$VAXcluster SCS\n"
                    "                      connect: send CONNECT-REQUEST after the\n"
                    "                      channel forms and answer a peer CONNECT-\n"
                    "                      REQUEST with CONNECT-RESPONSE (spec sec 4g).\n"
                    "                      Implies --respond.\n"
                    "  --hello-interval N  seconds between HELLO beacons (default %d)\n",
                    argv[0], SCA_ETHERTYPE, HELLO_DEFAULT_INTERVAL_SEC);
            return 0;
        }
    }

    /* --connect implies --respond implies --emit-hello (a beacon is what makes
     * the peer VAX send us the directed HELLO we reply to). */
    if (do_connect) {
        respond = 1;
    }
    if (respond) {
        emit_hello = 1;
    }

    int sock = socket(AF_PACKET, SOCK_RAW, htons(SCA_ETHERTYPE));
    if (sock < 0) {
        fprintf(stderr,
                "SCSD-E-NOSOCKET, socket(AF_PACKET, SOCK_RAW) failed: %s\n"
                "  (needs CAP_NET_RAW -- run as root or setcap cap_net_raw+ep on this binary)\n",
                strerror(errno));
        return 1;
    }

    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        fprintf(stderr, "SCSD-E-NOIFACE, unknown interface '%s': %s\n", ifname, strerror(errno));
        close(sock);
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(SCA_ETHERTYPE);
    sll.sll_ifindex = (int)ifindex;

    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "SCSD-E-BINDFAIL, bind to '%s' failed: %s\n", ifname, strerror(errno));
        close(sock);
        return 1;
    }

    /* --- vms-b62: resolve HELLO identity + build the send-side sockaddr --- */
    struct scs_hello_params hello_params;
    struct sockaddr_ll hello_dst;
    long hello_sent = 0;
    memset(&hello_params, 0, sizeof(hello_params));
    memset(&hello_dst, 0, sizeof(hello_dst));

    /* vms-5fe responder state. */
    struct peer_state peers[OVMX_MAX_PEERS];
    memset(peers, 0, sizeof(peers));
    static const uint8_t lab_nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    uint8_t our_hw_mac[6];
    memset(our_hw_mac, 0, sizeof(our_hw_mac));
    long directed_sent = 0;
    long connect_req_sent = 0;
    long connect_resp_sent = 0;
    long start_sent = 0;
    long start_ack_sent = 0;
    long credit_sent = 0;        /* vms-691: total 0x48 credit-returns sent */
    long retransmit_sent = 0;    /* vms-691: connect-request retransmits sent */

    /* OVMX identity for the phase-2 START/config body (vms-21e). Resolved once;
     * shared by every peer's START responder. */
    char ovmx_node[SYSGEN_STRVAL_LEN];
    resolve_node_identity(ovmx_node, sizeof(ovmx_node));
    uint16_t ovmx_scssystemid = resolve_scssystemid();

    if (emit_hello) {
        uint8_t hw_mac[6];
        if (get_iface_hwaddr(ifname, hw_mac) != 0) {
            fprintf(stderr, "SCSD-E-NOHWADDR, cannot resolve HW address of '%s': %s\n",
                    ifname, strerror(errno));
            close(sock);
            return 1;
        }

        char node_name[SYSGEN_STRVAL_LEN];
        resolve_node_identity(node_name, sizeof(node_name));
        uint16_t group = resolve_cluster_group();

        memcpy(our_hw_mac, hw_mac, 6);
        scs_hello_multicast_addr(group, hello_params.dst_mac);
        memcpy(hello_params.src_mac, hw_mac, 6);
        strncpy(hello_params.node_name, node_name, SCS_HELLO_NODENAME_LEN);
        hello_params.node_name[SCS_HELLO_NODENAME_LEN] = '\0';

        hello_dst.sll_family = AF_PACKET;
        hello_dst.sll_protocol = htons(SCA_ETHERTYPE);
        hello_dst.sll_ifindex = (int)ifindex;
        hello_dst.sll_halen = 6;
        memcpy(hello_dst.sll_addr, hello_params.dst_mac, 6);

        log_ts(stderr);
        fprintf(stderr,
                " SCSD-I-HELLOCFG, node='%s' group=%u mcast=%02x:%02x:%02x:%02x:%02x:%02x"
                " hwmac=%02x:%02x:%02x:%02x:%02x:%02x interval=%ds\n",
                node_name, group,
                hello_params.dst_mac[0], hello_params.dst_mac[1], hello_params.dst_mac[2],
                hello_params.dst_mac[3], hello_params.dst_mac[4], hello_params.dst_mac[5],
                hw_mac[0], hw_mac[1], hw_mac[2], hw_mac[3], hw_mac[4], hw_mac[5],
                hello_interval);

        /* Bound recv() so the loop wakes periodically to check the HELLO
         * timer even when the wire is idle. */
        struct timeval rcvtimeo;
        rcvtimeo.tv_sec = 1;
        rcvtimeo.tv_usec = 0;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo)) < 0) {
            fprintf(stderr, "SCSD-E-RCVTIMEO, setsockopt(SO_RCVTIMEO) failed: %s\n", strerror(errno));
            close(sock);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (duration > 0) {
        signal(SIGALRM, on_signal);
        alarm((unsigned)duration);
    }

    log_ts(stderr);
    fprintf(stderr,
            " SCSD-I-LISTEN, raw socket bound to '%s' ethertype 0x%04x (%s)%s\n",
            ifname, SCA_ETHERTYPE,
            duration > 0 ? "bounded run" : "run until SIGINT/SIGTERM",
            emit_hello ? ", HELLO beacon enabled" : "");

    long counts[5] = {0, 0, 0, 0, 0};
    long total_frames = 0;
    static uint8_t buf[SCA_FRAME_MAX];
    struct timespec last_hello = {0, 0};

    while (!g_stop) {
        if (emit_hello) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - last_hello.tv_sec >= hello_interval) {
                uint8_t frame[SCS_HELLO_FRAME_LEN];
                hello_params.timer_tick = (uint32_t)hello_sent;
                if (scs_hello_build_frame(&hello_params, frame) == 0) {
                    ssize_t sent = sendto(sock, frame, sizeof(frame), 0,
                                           (struct sockaddr *)&hello_dst, sizeof(hello_dst));
                    if (sent < 0) {
                        fprintf(stderr, "SCSD-E-SENDFAIL, sendto failed: %s\n", strerror(errno));
                    } else {
                        hello_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-HELLOSENT, node='%s' seq=%ld bytes=%zd\n",
                               hello_params.node_name, hello_sent, sent);
                        fflush(stdout);
                    }
                }
                last_hello = now;
            }
        }

        /* --- vms-691: retransmit OVMX's own unacked sequenced message (the
         * connect-request) on timeout, so a dropped CONNECT-REQUEST does not
         * stall the handshake. Rate-capped (VC_RETRANSMIT_MAX) to respect the
         * reply-amplification guard. Only while the connect is still unbound. */
        if (do_connect) {
            uint64_t now_ms = monotonic_ms();
            for (int i = 0; i < OVMX_MAX_PEERS; i++) {
                struct peer_state *ps = &peers[i];
                if (!ps->in_use || ps->connected || !ps->connect_sent) {
                    continue;
                }
                if (ps->vc.retransmit_count >= VC_RETRANSMIT_MAX) {
                    continue;
                }
                if (!scs_vc_retransmit_due(&ps->vc, now_ms, VC_RETRANSMIT_TIMEOUT_MS)) {
                    continue;
                }
                struct scs_connect_params cp;
                memset(&cp, 0, sizeof(cp));
                memcpy(cp.dst_mac, ps->eth_mac, 6);
                memcpy(cp.src_mac, our_hw_mac, 6);
                memcpy(cp.peer_logical, ps->logical, 6);
                cp.local_conid = OVMX_LOCAL_CONID;
                cp.remote_conid = 0;
                uint8_t rframe[SCS_CONNECT_FRAME_LEN];
                if (scs_connect_build_request(&cp, rframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, ps->eth_mac, rframe, sizeof(rframe)) > 0) {
                    scs_vc_mark_retransmitted(&ps->vc, now_ms);
                    retransmit_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-RETX, retransmit CONNECT-REQUEST to peer"
                           " %02x:%02x:%02x:%02x:%02x:%02x (attempt %u)\n",
                           ps->eth_mac[0], ps->eth_mac[1], ps->eth_mac[2],
                           ps->eth_mac[3], ps->eth_mac[4], ps->eth_mac[5],
                           ps->vc.retransmit_count);
                    fflush(stdout);
                }
            }
        }

        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue; /* SIGALRM/SIGINT -- g_stop checked at loop top */
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; /* SO_RCVTIMEO wakeup with nothing pending -- re-check HELLO timer */
            }
            fprintf(stderr, "SCSD-E-RECVFAIL, recv failed: %s\n", strerror(errno));
            break;
        }
        if (n < 14) {
            continue; /* shorter than a bare Ethernet header -- ignore */
        }

        /* The socket protocol filter already restricts delivery to
         * ethertype 0x6007, but re-check explicitly: it documents intent
         * and matches the absolute frame-offset convention (spec sec 2 /
         * dissect_sca.py) where the SCA payload begins at offset 14. */
        uint16_t ethertype = (uint16_t)(((unsigned)buf[12] << 8) | buf[13]);
        if (ethertype != SCA_ETHERTYPE) {
            continue;
        }

        const uint8_t *sca_payload = buf + 14;
        size_t payload_len = (size_t)n - 14;

        uint16_t total_sca_len = 0;
        scs_class_t cls = scs_classify_sca_payload(sca_payload, payload_len, &total_sca_len);

        counts[cls]++;
        total_frames++;

        log_ts(stdout);
        printf(" SCSD-I-FRAME, class=%-9s total_sca_len=%u eth_len=%zd"
               " src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
               scs_class_name(cls), total_sca_len, n,
               buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
               buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        fflush(stdout);

        /* --- vms-5fe responder --- only act on frames unicast to our HW MAC
         * (our own multicast beacon prompts the peer's directed HELLO). */
        if (!respond || !mac_eq(buf + OFF_ETH_DST, our_hw_mac)) {
            continue;
        }
        const uint8_t *src_mac = buf + OFF_ETH_SRC;

        /* --- vms-691: VC engine -- credit-ack every sequenced message the peer
         * sends us. Each 0x5b directory / 0x4b connect / 190-byte VC message
         * (send_seq != 0 at [20:22], abs 34) is answered by EXACTLY ONE 0x48
         * credit-return (strict 1-for-1, spec sec 4h(3)). This is what stops
         * the VAX's "%PEA0 Excessive packet losses / Closed Virtual Circuit"
         * teardown. The 0x41 START phase uses its own config-round ack
         * mechanism (branch (b) below), so 0x41 is excluded here. We do NOT
         * `continue`: 0x4b/190 frames still fall through to branch (c) for
         * Con.ID binding. */
        if (do_connect && n >= 36 && buf[31] == SCS_FORMAT_CONST &&
            (buf[30] == SCS_MSGTYPE_DIRLOOKUP || buf[30] == SCS_MSGTYPE_SEQAPP ||
             cls == SCS_CLASS_SCS_FIXED)) {
            uint16_t peer_send_seq = (uint16_t)(buf[34] | ((uint16_t)buf[35] << 8)); /* [20:22] */
            uint16_t peer_recv_ack = (uint16_t)(buf[32] | ((uint16_t)buf[33] << 8)); /* [18:20] */
            struct peer_state *ps = peer_find_or_add(peers, src_mac);
            if (ps != NULL) {
                if (!ps->vc.initialized) {
                    scs_vc_init(&ps->vc);
                }
                /* The peer's leading counter acks OVMX's own sequenced sends. */
                scs_vc_note_peer_ack(&ps->vc, peer_recv_ack);
                memcpy(ps->logical, buf + OFF_HELLO_SRCLOG, 6); /* src-logical, abs 24 */

                if (scs_vc_owes_credit(peer_send_seq)) {
                    scs_vc_note_recv(&ps->vc, peer_send_seq);
                    uint8_t cframe[SCS_CREDIT_FRAME_LEN];
                    if (scs_vc_build_credit_for(&ps->vc, ps->eth_mac, our_hw_mac,
                                                ps->logical, cframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps->eth_mac, cframe,
                                      sizeof(cframe)) > 0) {
                        ps->credit_sent++;
                        credit_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-CREDIT, 0x48 credit-return acked peer_seq=%u"
                               " to %02x:%02x:%02x:%02x:%02x:%02x (#%ld)\n",
                               peer_send_seq,
                               ps->eth_mac[0], ps->eth_mac[1], ps->eth_mac[2],
                               ps->eth_mac[3], ps->eth_mac[4], ps->eth_mac[5],
                               ps->credit_sent);
                        fflush(stdout);
                    }
                }
            }
        }

        /* (a) Directed HELLO -> reply with our directed HELLO (NISCA channel,
         * spec sec 4b). The frame is already known to be unicast to our HW MAC
         * (gated above), so any non-zero directed flag marks a directed HELLO
         * aimed at us. Observed on the wire: an established member uses 0x0001,
         * but a member SOLICITING a not-yet-joined node (us) uses 0x0002 --
         * accept both. */
        if (cls == SCS_CLASS_HELLO && n >= OFF_HELLO_DIRFLG + 2 &&
            (buf[OFF_HELLO_DIRFLG] != 0x00 || buf[OFF_HELLO_DIRFLG + 1] != 0x00)) {
            struct peer_state *ps = peer_find_or_add(peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            memcpy(ps->logical, buf + OFF_HELLO_SRCLOG, 6);

            /* Rate-limit our directed replies to a real-node cadence (~1/s per
             * peer). Replying 1:1 to every received directed HELLO amplifies
             * into a tight ping-pong storm with the peer; real nodes beacon on
             * a timer instead. */
            struct timespec dnow;
            clock_gettime(CLOCK_MONOTONIC, &dnow);
            int due = (ps->directed_replies == 0) ||
                      (dnow.tv_sec - ps->last_directed.tv_sec >= 1);
            if (due) {
                uint8_t dframe[SCS_HELLO_FRAME_LEN];
                hello_params.timer_tick = (uint32_t)directed_sent;
                if (scs_hello_build_directed_frame(&hello_params, src_mac, lab_nonce, dframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, src_mac, dframe, sizeof(dframe)) > 0) {
                    directed_sent++;
                    ps->directed_replies++;
                    ps->channel_up = 1;
                    ps->last_directed = dnow;
                    log_ts(stdout);
                    printf(" SCSD-I-DIRHELLO, replied directed HELLO to peer"
                           " %02x:%02x:%02x:%02x:%02x:%02x (reply #%ld)\n",
                           src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                           ps->directed_replies);
                    fflush(stdout);
                }
            } else {
                ps->channel_up = 1;
            }

            /* vms-21e: OVMX is the JOINER, so the established node drives the
             * 0x4b CONNECT-REQUEST and OVMX answers it (branch (c) below). We
             * only open the connection from our side as a FALLBACK, and only
             * AFTER the phase-2 START/config handshake has completed
             * (ps->start_acked) and the peer has not already bound it. Firing a
             * CONNECT-REQUEST before START (the old vms-5fe behavior) put a
             * premature 0x4b on the wire that the VAX ignored. */
            if (do_connect && ps->start_acked && !ps->connect_sent && !ps->connected) {
                struct scs_connect_params cp;
                memset(&cp, 0, sizeof(cp));
                memcpy(cp.dst_mac, ps->eth_mac, 6);
                memcpy(cp.src_mac, our_hw_mac, 6);
                memcpy(cp.peer_logical, ps->logical, 6);
                cp.local_conid = OVMX_LOCAL_CONID;
                cp.remote_conid = 0;
                uint8_t cframe[SCS_CONNECT_FRAME_LEN];
                if (scs_connect_build_request(&cp, cframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, ps->eth_mac, cframe, sizeof(cframe)) > 0) {
                    ps->connect_sent = 1;
                    connect_req_sent++;
                    /* vms-691: the connect-request is OVMX's own outstanding
                     * sequenced message -- track it for retransmit until the
                     * peer's CONNECT-RESPONSE binds the Con.ID pair. */
                    scs_vc_record_sent(&ps->vc, ps->vc.seq.send_seq, monotonic_ms());
                    log_ts(stdout);
                    printf(" SCSD-I-CONNREQ, sent CONNECT-REQUEST local_conid=0x%08X to peer\n",
                           OVMX_LOCAL_CONID);
                    fflush(stdout);
                }
            }
            continue;
        }

        /* (b) vms-21e: phase-2 START/config (opcode 0x41). After the channel
         * forms, the established node streams 0x41 START frames and WAITS for
         * OVMX's own 0x41 START before proceeding to the 0x4b connect. Answer
         * as the joiner does (spec sec 4g phase 2): reply round-0 + round-1
         * START, then the round-2 46-byte ack when the peer acks. */
        if (do_connect && n >= 32 && buf[30] == SCS_START_OPCODE) {
            struct scs_start_view sv;
            if (scs_start_parse(buf, (size_t)n, &sv) != 0) {
                continue;
            }
            struct peer_state *ps = peer_find_or_add(peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            scs_vc_note_recv(&ps->vc, sv.send_seq);
            memcpy(ps->logical, buf + OFF_HELLO_SRCLOG, 6); /* START src-logical, abs 24 */

            log_ts(stdout);
            printf(" SCSD-I-STARTRX, %s round=%u peer_seq=%u peer_sysid=%u\n",
                   sv.is_ack ? "ack" : "START", sv.config_round, sv.send_seq,
                   sv.is_ack ? 0 : sv.scssystemid);
            fflush(stdout);

            struct scs_start_params sp;
            memset(&sp, 0, sizeof(sp));
            memcpy(sp.dst_mac, ps->eth_mac, 6);
            memcpy(sp.src_mac, our_hw_mac, 6);
            memcpy(sp.peer_logical, ps->logical, 6);
            sp.scssystemid = ovmx_scssystemid;
            strncpy(sp.node_name, ovmx_node, SCS_START_NODENAME_LEN);
            sp.node_name[SCS_START_NODENAME_LEN] = '\0';
            /* START is sequenced-message #1 for OVMX's side of the VC:
             * send_seq=1 (from the state machine). The leading counter
             * [18:20] is OVMX's ACK of the peer -- it MUST carry recv_seq (the
             * highest peer send_seq seen), NOT a hardcoded 0. vms-691 live
             * finding: the established VAX streams round-0 START with
             * send_seq=N and retransmits it until OVMX's reply acknowledges it
             * (recv_ack >= N). OVMX previously sent recv_ack=0, so the VAX saw
             * its START as unacked and looped round-0 forever, and the VC never
             * formed. Acking recv_seq is what advances the handshake. */
            sp.send_seq = ps->vc.seq.send_seq;
            sp.recv_ack = ps->vc.seq.recv_seq;

            if (!sv.is_ack) {
                /* (Re)send round-0 + round-1 START, acking the peer's send_seq.
                 * We reply on EVERY received round START (not just once) so each
                 * of the VAX's round-0 retransmits is answered with the current
                 * recv_ack -- the keepalive that drives the handshake forward.
                 * The VAX beacons round-0 on its poller sweep (~5s), so this is
                 * well under the reply-amplification threshold. */
                for (uint16_t rnd = 0; rnd <= 1; rnd++) {
                    sp.config_round = rnd;
                    uint8_t sframe[SCS_START_FRAME_LEN];
                    if (scs_start_build(&sp, sframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps->eth_mac, sframe, sizeof(sframe)) > 0) {
                        start_sent++;
                        ps->start_replies++;
                    }
                }
                if (!ps->start_replied) {
                    ps->start_replied = 1;
                }
                log_ts(stdout);
                printf(" SCSD-I-STARTTX, sent round-0+round-1 START"
                       " (sysid=%u node='%s' send_seq=%u recv_ack=%u)\n",
                       ovmx_scssystemid, ovmx_node, sp.send_seq, sp.recv_ack);
                fflush(stdout);
            } else {
                /* Peer's round-2 46-byte ack -> answer with ours; START done. */
                if (!ps->start_acked) {
                    uint8_t aframe[SCS_START_ACK_FRAME_LEN];
                    if (scs_start_build_ack(&sp, aframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps->eth_mac, aframe, sizeof(aframe)) > 0) {
                        start_ack_sent++;
                        ps->start_acked = 1;
                        log_ts(stdout);
                        printf(" SCSD-I-STARTDONE, START/config complete with peer"
                               " %02x:%02x:%02x:%02x:%02x:%02x -- awaiting 0x4b connect\n",
                               ps->eth_mac[0], ps->eth_mac[1], ps->eth_mac[2],
                               ps->eth_mac[3], ps->eth_mac[4], ps->eth_mac[5]);
                        fflush(stdout);
                    }
                }
            }
            continue;
        }

        /* (c) SCS envelope directed to us -> inspect; complete the connect. */
        if (do_connect && (cls == SCS_CLASS_OTHER || cls == SCS_CLASS_SCS_FIXED)) {
            struct scs_connect_view v;
            if (scs_connect_parse(buf, (size_t)n, &v) != 0) {
                continue;
            }
            log_ts(stdout);
            printf(" SCSD-I-SCSENV, msgtype=0x%02x fmt=0x%02x len=%u"
                   " remote_conid=0x%08X local_conid=0x%08X\n",
                   v.msgtype, v.format, v.total_sca_len, v.remote_conid, v.local_conid);
            fflush(stdout);

            if (v.msgtype != SCS_MSGTYPE_SEQAPP || !v.has_conid) {
                continue;
            }
            struct peer_state *ps = peer_find_or_add(peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            if (v.remote_conid == 0 && !ps->connected) {
                /* Peer's CONNECT-REQUEST to us -> answer, echoing its Con.ID. */
                struct scs_connect_params cp;
                memset(&cp, 0, sizeof(cp));
                memcpy(cp.dst_mac, ps->eth_mac, 6);
                memcpy(cp.src_mac, our_hw_mac, 6);
                memcpy(cp.peer_logical, ps->logical, 6);
                cp.local_conid = OVMX_LOCAL_CONID;
                cp.remote_conid = v.local_conid;
                uint8_t rframe[SCS_CONNECT_FRAME_LEN];
                if (scs_connect_build_response(&cp, rframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, ps->eth_mac, rframe, sizeof(rframe)) > 0) {
                    ps->remote_conid = v.local_conid;
                    ps->connected = 1;
                    connect_resp_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-CONNRESP, answered peer CONNECT-REQUEST:"
                           " remote=0x%08X local=0x%08X\n", v.local_conid, OVMX_LOCAL_CONID);
                    fflush(stdout);
                }
            } else if (v.remote_conid == OVMX_LOCAL_CONID && !ps->connected) {
                /* Peer's CONNECT-RESPONSE to our request -> the pair is bound. */
                ps->remote_conid = v.local_conid;
                ps->connected = 1;
                log_ts(stdout);
                printf(" SCSD-I-CONNBOUND, peer accepted our connect:"
                       " local=0x%08X remote=0x%08X\n", OVMX_LOCAL_CONID, v.local_conid);
                fflush(stdout);
            }
            continue;
        }
    }

    log_ts(stderr);
    fprintf(stderr, " SCSD-I-SUMMARY, %ld total 0x6007 frames received on '%s'%s\n",
            total_frames, ifname,
            emit_hello ? " (and HELLO beacon was active)" : "");
    fprintf(stderr, "  HELLO=%ld SCS-FIXED=%ld SOLICIT=%ld OTHER=%ld RUNT=%ld\n",
            counts[SCS_CLASS_HELLO], counts[SCS_CLASS_SCS_FIXED],
            counts[SCS_CLASS_SOLICIT], counts[SCS_CLASS_OTHER], counts[SCS_CLASS_RUNT]);
    if (emit_hello) {
        fprintf(stderr, "  HELLO-SENT=%ld\n", hello_sent);
    }
    if (respond) {
        fprintf(stderr, "  DIRECTED-HELLO-SENT=%ld START-SENT=%ld START-ACK-SENT=%ld"
                " CONNECT-REQ-SENT=%ld CONNECT-RESP-SENT=%ld CREDIT-SENT=%ld RETX-SENT=%ld\n",
                directed_sent, start_sent, start_ack_sent, connect_req_sent, connect_resp_sent,
                credit_sent, retransmit_sent);
        for (int i = 0; i < OVMX_MAX_PEERS; i++) {
            if (!peers[i].in_use) {
                continue;
            }
            fprintf(stderr,
                    "  PEER %02x:%02x:%02x:%02x:%02x:%02x channel=%s directed_replies=%ld"
                    " start_replied=%d start_acked=%d connect_sent=%d connected=%s"
                    " credit_sent=%ld retx=%u remote_conid=0x%08X\n",
                    peers[i].eth_mac[0], peers[i].eth_mac[1], peers[i].eth_mac[2],
                    peers[i].eth_mac[3], peers[i].eth_mac[4], peers[i].eth_mac[5],
                    peers[i].channel_up ? "UP" : "down", peers[i].directed_replies,
                    peers[i].start_replied, peers[i].start_acked,
                    peers[i].connect_sent, peers[i].connected ? "YES" : "no",
                    peers[i].credit_sent, peers[i].vc.retransmit_count,
                    peers[i].remote_conid);
        }
    }

    close(sock);
    return 0;
}
