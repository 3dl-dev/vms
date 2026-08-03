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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cluster_authorize.h"
#include "scs_classify.h"
#include "scs_config.h"
#include "scs_connect.h"
#include "scs_dir.h"
#include "scs_hello.h"
#include "scs_member.h"
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

/* vms-9f3: OVMX's own local 100ns-tick clock for the HELLO timer field (abs
 * 96-101). The reference-lab wire carries a 48-bit LE counter advancing at
 * ~1e7 ticks/s = 100ns/tick (measured across the af2/ci3 padded directed
 * HELLOs); it is per-sender and the ONLY field that varies across a
 * channel-verify retransmit (spec sec 4k). PEDRIVER rejects a channel whose
 * verify HELLO carries a dead/static timer, so every emitted HELLO stamps the
 * CURRENT tick sampled here. Clean-room: this is OVMX's OWN monotonic clock in
 * 100ns units, never a value copied from VSI source. */
static uint64_t hello_timer_tick100(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 10000000u + (uint64_t)(ts.tv_nsec / 100);
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

/*
 * ovmx_cluster_logical - vms-9f3: compute OVMX's cluster-LOGICAL LAVC address
 * from its SCSSYSTEMID. GROUNDED convention (spec sec 3 decoder ring + README-lab):
 * a cluster node's logical addr is aa:00:04:00:<LE16(SCSSYSTEMID)>, e.g. VAX2
 * sysid 1026=0x0402 -> aa:00:04:00:02:04. This is the value a real member writes
 * in the SCA src-logical field (abs 24); OVMX's raw HW MAC there was why VAX1's
 * PEDRIVER never verified the channel (the old bug). No DECnet is needed to emit
 * it -- it is a cluster field that happens to use the LAVC address format.
 */
static void ovmx_cluster_logical(uint16_t sysid, uint8_t out[6])
{
    out[0] = 0xaa;
    out[1] = 0x00;
    out[2] = 0x04;
    out[3] = 0x00;
    out[4] = (uint8_t)(sysid & 0xff);        /* LE16 low byte  */
    out[5] = (uint8_t)((sysid >> 8) & 0xff); /* LE16 high byte */
}

/* --- vms-5fe: directed-HELLO / SCS-connect responder state --- */

#define OVMX_MAX_PEERS 4

/* OVMX's own VMS$VAXcluster Con.ID for this run (OVMX design choice; opaque
 * to the peer -- see scs_connect.h). We use two distinct handles: one for the
 * connection the MEMBER opens to OVMX (answered with a CONNECT-RESPONSE), and a
 * separate one for the connection OVMX opens to the member as an ACTIVE JOINER
 * (the clean-ref grounded requirement, vms-d94: VAXB opens its OWN
 * VMS$VAXcluster connection and sends the add-member burst on IT --
 * formation-clean-2node.pcap idx52/59). A single handle for both directions
 * would collide the two SCS connections. */
#define OVMX_LOCAL_CONID  (SCS_CONNECT_OVMX_CONID_BASE | 0x0001u)
#define OVMX_JOINER_CONID (SCS_CONNECT_OVMX_CONID_BASE | 0x0002u)

/* Absolute frame offsets used by the responder (spec byte-offset convention:
 * 0 = first byte of Ethernet dst). */
#define OFF_ETH_DST      0
#define OFF_ETH_SRC      6
#define OFF_HELLO_SRCLOG 24  /* HELLO SCA src-logical addr (abs 24-29) */
#define OFF_HELLO_DIRFLG 92  /* directed-HELLO flag (abs 92-93) */

/*
 * struct peer_state - vms-7be: what used to be a MAC-keyed struct describing a
 * remote node is now a THIN INDEX over the architected SCA structures.
 *
 * The node and the virtual circuit are described by the Path Block and System
 * Block that `pb` points at (VAXcluster Principles ch. 2, pp. 2-11..2-21, see
 * scs_config.h): the peer's Ethernet port address lives in pb->remote_port_addr
 * (p. 2-12), its 48-bit SCS System Address in pb->sb->system_id (p. 2-16), and
 * the state of the virtual circuit in pb->vc_state (p. 2-11) -- all of which
 * previously lived here as eth_mac/logical/ad-hoc flags.
 *
 * What REMAINS here is deliberately not SB/PB material: it is SCS CONNECTION
 * and SYSAP state (Con.ID pairs, directory handles, the connection-manager
 * dialogue) plus NISCA channel bookkeeping. Connections ride ON a virtual
 * circuit and are a layer above it (p. 2-11 Figure 2-6); moving them into
 * connection descriptors is a separate item.
 */
struct peer_state {
    struct scs_pb *pb;        /* Path Block: this peer's port + virtual circuit; NULL = free slot */
    int      channel_up;      /* >=1 directed HELLO exchanged (NISCA channel, not an SCA VC state) */
    long     directed_replies;
    struct timespec last_directed; /* CLOCK_MONOTONIC of our last directed reply (rate limit) */
    int      connect_sent;    /* we sent a CONNECT-REQUEST */
    int      connected;       /* Con.ID pair bound */
    uint32_t remote_conid;    /* peer's Con.ID once learned */
    /* --- vms-af2/vms-691: node-incarnation echo (the established-join gate) ---
     * The member advertises the incarnation it attributes to OVMX in its
     * directed-HELLO flag at abs 92 ([78:80]); OVMX echoes it into its 0x41
     * START [22:24] and its own directed-HELLO abs 92 (spec sec 4i.B). Read off
     * the wire, never hard-coded. 0 = not yet observed (fall back to 1, the
     * fresh-contact value). */
    uint16_t incarnation;
    /* --- vms-21e / vms-691: SCS sequenced-message VC engine state --- */
    struct scs_vc vc;              /* seq/ack tracking + credit + retransmit (embeds scs_seq_state) */
    int      start_replied;        /* we answered the peer's 0x41 START (sent round 0/1) */
    int      start_acked;          /* we sent the round-2 46-byte ack -> START complete */
    long     start_replies;        /* count of 0x41 frames we sent to this peer */
    long     credit_sent;          /* vms-691: 0x48 credit-returns we sent this peer */
    /* --- vms-246: SCS$DIRECTORY / SCS$DIR_LOOKUP responder state --- */
    int      dir_seen;             /* the peer has begun the directory exchange with us */
    int      dir_connected;        /* SCS$DIRECTORY Con.ID pair bound (we sent CONNECT-RESPONSE) */
    uint32_t dir_remote_conid;     /* the peer's SCS$DIRECTORY handle (learned from its request) */
    long     dir_lookups_answered; /* SCS$DIR_LOOKUP responses we sent this peer */
    /* --- vms-224: connection-manager add-member SYSAP dialogue (spec sec 4j) ---
     * The 190-byte VMS$VAXcluster VC carries a SECOND, application-level
     * send/ack-msg# counter pair (body[0:4]) distinct from the SCS VC seq. */
    int      cm_config_sent;       /* we sent our op 0x14/0x01/0x02 config burst */
    uint16_t sysap_send;           /* OVMX's next SYSAP send-msg# (body[0:2]; from 1) */
    uint16_t sysap_recv;           /* high-water of the member's SYSAP send-msg# (ack target) */
    long     cm_responses;         /* 0x81 responses we sent to member 0x03/0x05 txns */
    /* --- vms-d94: ACTIVE-JOINER VMS$VAXcluster connection (the connection OVMX
     * OPENS to the member, distinct from the member-opened one above). The
     * clean-ref join sends the add-member burst on THIS joiner-initiated
     * connection (formation-clean-2node.pcap idx52 CONNECT-REQ -> idx59 CM). */
    int      joiner_connect_sent;  /* we sent our own VMS$VAXcluster CONNECT-REQUEST */
    int      joiner_connected;     /* the member accepted it (Con.ID pair bound) */
    uint32_t joiner_remote_conid;  /* the member's Con.ID on OUR connection (from its response) */
    int      joiner_cm_sent;       /* we sent the add-member burst on our own connection */
    uint16_t joiner_req_seq;       /* the send_seq of our CONNECT-REQUEST (retransmits REUSE it) */
    struct timespec last_joiner_req; /* CLOCK_MONOTONIC of our last joiner CONNECT-REQUEST (retx) */
    /* --- vms-9f3: NISCA channel packet-size verification (padded directed
     * HELLO, spec sec 4k). An ESTABLISHED VAX1 zero-pads a directed HELLO up to
     * NISCS_MAX_PKTSZ and retransmits it (1500->1069->853->745, ~6s) until OVMX
     * RECIPROCATES on the reverse channel; that reciprocal is what lets VAX1
     * open OVMX's CSB and drive the sec 4j add-member commit. */
    long     padded_replies;       /* padded HELLOs we sent this peer (reciprocations) */
    struct timespec last_padded;   /* CLOCK_MONOTONIC of our last padded send (rate limit) */
    int      padded_initiated;     /* we proactively sent one padded HELLO (golden joiner-first) */
    uint16_t peer_padded_sca;      /* largest padded-HELLO total SCA size seen from the peer */
};

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

/*
 * is_padded_hello - vms-9f3: is `buf` (n bytes on the wire) a padded directed
 * HELLO (spec sec 4k)? A HELLO-family frame -- message-class byte 0x05 at abs 36
 * and NOT the 0x13 SCS-envelope format constant at abs 31 (this is a HELLO, not
 * an 0x4b sequenced message) -- whose total SCA content EXCEEDS a normal
 * 120-byte HELLO, up to NISCS_MAX_PKTSZ (+2). Per sec 4k the distinguishing
 * feature of a padded HELLO is its SIZE plus the HELLO class byte; the pad tail
 * is pure zeros. The frame is already known to be unicast to our HW MAC (gated
 * by the caller).
 */
static int is_padded_hello(const uint8_t *buf, size_t n)
{
    return n > (size_t)SCS_HELLO_FRAME_LEN &&
           n <= (size_t)SCS_HELLO_PADDED_MAX_FRAME &&
           buf[31] == 0x00 &&   /* HELLO family, NOT the 0x13 SCS envelope (sec 4k) */
           buf[36] == 0x05;     /* message-class byte 0x05 == HELLO (spec sec 4a/4k) */
}

/* vms-7be: all-zero address returned by the accessors below when the structure
 * they read is not built yet -- byte-for-byte what the old zero-initialized
 * peer_state fields produced in the same situation. */
static const uint8_t zero_addr[6] = {0, 0, 0, 0, 0, 0};

/* The peer's Ethernet PORT address (PB, p. 2-12) -- where we send its frames. */
static const uint8_t *ps_port_addr(const struct peer_state *ps)
{
    return (ps != NULL && ps->pb != NULL) ? ps->pb->remote_port_addr : zero_addr;
}

/* The peer's 48-bit SCS System Address (SB, p. 2-16) -- the cluster-logical
 * address OVMX writes in the SCA dest-logical field. Zeros until discovered,
 * exactly as the old peer_state.logical was. */
static const uint8_t *ps_sys_addr(const struct peer_state *ps)
{
    return (ps != NULL && ps->pb != NULL && ps->pb->sb != NULL) ? ps->pb->sb->system_id
                                                                : zero_addr;
}

/*
 * peer_find_or_add - vms-7be: look the peer up by its remote PORT ADDRESS in
 * the configuration structures (PDT formative queue + open PBs, p. 2-21) and,
 * on first contact, build the Path Block that describes the newly discovered
 * port and the circuit forming with it (p. 2-11). The peer_state slot is now
 * just the index cell that hangs the connection-level state off that PB.
 */
static struct peer_state *peer_find_or_add(struct scs_config *cfg, struct scs_pdt *pdt,
                                           struct peer_state *tbl, const uint8_t mac[6])
{
    int free_slot = -1;
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (tbl[i].pb != NULL && mac_eq(tbl[i].pb->remote_port_addr, mac)) {
            return &tbl[i];
        }
        if (tbl[i].pb == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return NULL;
    }
    struct scs_pb *pb = scs_pb_create(cfg, pdt, mac, SCS_PORT_TYPE_ETHERNET);
    if (pb == NULL) {
        return NULL; /* PB pool exhausted -- same "no room" answer as before */
    }
    memset(&tbl[free_slot], 0, sizeof(tbl[free_slot]));
    tbl[free_slot].pb = pb;
    return &tbl[free_slot];
}

/*
 * ps_learn_sys_addr - record the peer's SCA src-logical address (abs 24) as the
 * 48-bit SCS System Address of the System Block for its node. Replaces the old
 * `memcpy(ps->logical, buf + OFF_HELLO_SRCLOG, 6)`; the bytes stored are
 * identical, the structure they are stored in is the architected one.
 *
 * ONE FAILURE MODE THE OLD memcpy DID NOT HAVE: the store now needs an SB, and
 * scs_pb_learn_system_addr returns NULL if the SB pool is exhausted. If that ever
 * happened, ps_sys_addr() would keep returning zeros and OVMX would put a zero
 * peer-logical on the wire. That must NEVER be silent (CLAUDE.md rule 9 / INV-6),
 * so it is logged as an error here. It is also unreachable at the shipped pool
 * sizes -- SCS_CONFIG_MAX_SB (10) >= OVMX_MAX_PEERS (4) formative SBs + the local
 * node's own SB -- which tests/vmsscs/test_scsd_wire.c asserts at compile time AND
 * by driving a full peer table through this function.
 */
static void ps_learn_sys_addr(struct scs_config *cfg, struct peer_state *ps,
                              const uint8_t *src_logical)
{
    if (ps == NULL || ps->pb == NULL) {
        return;
    }
    if (scs_pb_learn_system_addr(cfg, ps->pb, src_logical) == NULL) {
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-E-NOSB, no system block available for peer"
                " %02x:%02x:%02x:%02x:%02x:%02x -- its SCS system address is NOT"
                " recorded and outgoing frames will carry a zero peer-logical\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
        fflush(stderr);
    }
}

/*
 * ps_channel_up - the NISCA channel to this peer is verified. OVMX INFERENCE
 * (labeled): a node exchanging directed HELLOs with us is handling normal SCA
 * communication, which is what "port state = ENABLED" means for a VAX port
 * (p. 2-11..2-12). Nothing here reaches the wire.
 */
static void ps_channel_up(struct peer_state *ps)
{
    ps->channel_up = 1;
    if (ps->pb != NULL) {
        ps->pb->remote_port_state = SCS_PORT_STATE_ENABLED;
    }
}

#ifdef SCSD_UNIT_TEST
/*
 * vms-7be TRANSMIT CAPTURE SEAM -- compiled ONLY into tests/vmsscs/test_scsd_wire.c,
 * which is the single translation unit that defines SCSD_UNIT_TEST. The daemon
 * never defines it, so the production build of send_frame_to below is byte-for-byte
 * the code that shipped before this item.
 *
 * WHY: the frames OVMX puts on the wire are assembled INSIDE this file (dst MAC
 * from ps_port_addr(), SCA peer-logical from ps_sys_addr()) and then handed to an
 * AF_PACKET socket that needs CAP_NET_RAW, which CI does not have. Substituting the
 * sendto() with a capture buffer lets a test drive the REAL scsd.c senders and
 * inspect the REAL bytes, instead of re-deriving them in the test.
 */
uint8_t  scsd_test_last_frame[SCA_FRAME_MAX];
uint8_t  scsd_test_last_dst[6];
size_t   scsd_test_last_len;
unsigned scsd_test_frames;
#endif

/* Send a fully-built Ethernet frame to a specific unicast MAC on ifindex. */
static ssize_t send_frame_to(int sock, int ifindex, const uint8_t mac[6],
                             const uint8_t *frame, size_t len)
{
#ifdef SCSD_UNIT_TEST
    (void)sock;
    (void)ifindex;
    if (len > sizeof(scsd_test_last_frame)) {
        return -1;
    }
    memcpy(scsd_test_last_dst, mac, 6);
    memcpy(scsd_test_last_frame, frame, len);
    scsd_test_last_len = len;
    scsd_test_frames++;
    return (ssize_t)len;
#else
    struct sockaddr_ll da;
    memset(&da, 0, sizeof(da));
    da.sll_family = AF_PACKET;
    da.sll_protocol = htons(SCA_ETHERTYPE);
    da.sll_ifindex = ifindex;
    da.sll_halen = 6;
    memcpy(da.sll_addr, mac, 6);
    return sendto(sock, frame, len, 0, (struct sockaddr *)&da, sizeof(da));
#endif
}

/*
 * cm_send_config_burst - vms-224: drive OVMX's connection-manager add-member
 * config burst on the bound 190-byte VMS$VAXcluster VC (spec sec 4j): op 0x14
 * (node model advertisement), op 0x01 (cluster parameters, VOTES=0 non-voting),
 * op 0x02 (config/topology). Each is a sequenced SCS message -- it advances the
 * VC send_seq (SCS layer) AND carries its own SYSAP send-msg# (application
 * layer). This is the joiner's active contribution that lets the member drive
 * the op 0x03 commit + op 0x05 lock-rebuild transactions, promoting OVMX to a
 * full MEMBER. Returns the number of frames sent (0..3).
 */
static int cm_send_config_burst(int sock, int ifindex, struct peer_state *ps,
                                const uint8_t our_hw_mac[6],
                                const uint8_t our_src_logical[6],
                                uint32_t local_conid, uint32_t remote_conid)
{
    if (ps->sysap_send == 0) {
        ps->sysap_send = 1; /* SYSAP send-msg# starts at 1 (spec sec 4j) */
    }

    struct scs_member_params mp;
    uint8_t frame[SCS_MEMBER_FRAME_LEN];
    int sent = 0;

    /* Shared identity + connection fields for all three frames. The Con.ID pair
     * identifies WHICH VMS$VAXcluster connection the burst rides (vms-d94: the
     * add-member burst must ride the JOINER-initiated connection). */
    memset(&mp, 0, sizeof(mp));
    memcpy(mp.dst_mac, ps_port_addr(ps), 6);
    memcpy(mp.src_mac, our_hw_mac, 6);
    memcpy(mp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 cluster-logical addr */
    memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
    mp.remote_conid = remote_conid;
    mp.local_conid = local_conid;
    mp.incarnation = ps->incarnation;

    /* op 0x14 node CPU/model advertisement (OVMX's own model string). */
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.sysap_send_msg = ps->sysap_send++;
    mp.sysap_ack_msg = ps->sysap_recv;
    mp.model = NULL; /* OVMX_MODEL_STRING default */
    if (scs_member_build_model(&mp, frame) == 0 &&
        send_frame_to(sock, ifindex, ps_port_addr(ps), frame, sizeof(frame)) > 0) {
        sent++;
    }

    /* op 0x01 cluster parameters -- VOTES=0 (OVMX joins non-voting so it can
     * never break VAX1/VAX2 quorum, design sec 8). */
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.sysap_send_msg = ps->sysap_send++;
    mp.sysap_ack_msg = ps->sysap_recv;
    mp.votes = SCS_MEMBER_VOTES_NONVOTING; /* VOTES=1 tested (vms-d94), did NOT change NEW->MEMBER */
    if (scs_member_build_params(&mp, frame) == 0 &&
        send_frame_to(sock, ifindex, ps_port_addr(ps), frame, sizeof(frame)) > 0) {
        sent++;
    }

    /* op 0x02 config/topology. (vms-d94: holding 0x02 to a later step was tested
     * and did NOT change NEW->MEMBER, so the initial burst sends all three.) */
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.sysap_send_msg = ps->sysap_send++;
    mp.sysap_ack_msg = ps->sysap_recv;
    if (scs_member_build_config(&mp, frame) == 0 &&
        send_frame_to(sock, ifindex, ps_port_addr(ps), frame, sizeof(frame)) > 0) {
        sent++;
    }

    ps->cm_config_sent = 1;
    return sent;
}

/*
 * send_joiner_connect_request - vms-d94: send OVMX's ACTIVE-JOINER
 * VMS$VAXcluster CONNECT-REQUEST to the member (remote=0, offering
 * OVMX_JOINER_CONID). A real joiner opens its OWN connection to the member's
 * connection manager and drives the add-member on it (clean-ref idx52). Timing
 * matters: the member's CM times out ~1.4s after START and re-issues START, so
 * this must fire PROMPTLY (right after the post-START directory phase begins),
 * not on a lazy HELLO poll. The request is one sequenced message: the first
 * send allocates a send_seq; retransmits REUSE it (a retransmit is not a new
 * message). Returns 1 if a frame was sent.
 */
static int send_joiner_connect_request(int sock, int ifindex, struct peer_state *ps,
                                       const uint8_t our_hw_mac[6],
                                       const uint8_t our_src_logical[6])
{
    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, ps_port_addr(ps), 6);
    memcpy(cp.src_mac, our_hw_mac, 6);
    memcpy(cp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
    memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
    cp.local_conid = OVMX_JOINER_CONID;
    cp.remote_conid = 0; /* CONNECT-REQUEST: the member's Con.ID is not yet known */
    cp.recv_ack = ps->vc.seq.recv_seq; /* always ack the member's latest send_seq */
    if (ps->joiner_req_seq == 0) {
        ps->joiner_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    cp.send_seq = ps->joiner_req_seq; /* retransmits reuse the same seq */
    cp.incarnation = ps->incarnation;
    uint8_t cframe[SCS_CONNECT_FRAME_LEN];
    if (scs_connect_build_request(&cp, cframe) == 0 &&
        send_frame_to(sock, ifindex, ps_port_addr(ps), cframe, sizeof(cframe)) > 0) {
        ps->joiner_connect_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->last_joiner_req);
        scs_vc_record_sent(&ps->vc, cp.send_seq, monotonic_ms());
        return 1;
    }
    return 0;
}

#ifdef SCSD_UNIT_TEST
/* vms-7be: tests/vmsscs/test_scsd_wire.c #includes this file and supplies its own
 * main(). The daemon entry point is RENAMED, not compiled out, so every static
 * helper above is compiled exactly as the daemon compiles it. */
int scsd_daemon_main(int argc, char **argv);
int scsd_daemon_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
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

    /* vms-5fe responder state, now indexed onto the vms-7be SCA structures. */
    struct peer_state peers[OVMX_MAX_PEERS];
    memset(peers, 0, sizeof(peers));
    /* vms-7be: the VMS SCA Configuration Queue (p. 2-17) and the Port Descriptor
     * Table for OVMX's single local SCA port -- the Ethernet/NISCA port this
     * daemon has bound (p. 2-21: "VMS has one Port Descriptor Table for each
     * local SCA port"). Formative Path Blocks queue here until their circuit
     * opens; open ones move to the System Block of the node they reach. */
    static struct scs_config scs_cfg;
    struct scs_pdt scs_pdt;
    scs_config_init(&scs_cfg);
    scs_pdt_init(&scs_pdt, SCS_PORT_TYPE_ETHERNET, SCS_HELLO_PADDED_MAX_SCA);
    static const uint8_t lab_nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    uint8_t our_hw_mac[6];
    memset(our_hw_mac, 0, sizeof(our_hw_mac));
    uint8_t our_src_logical[6]; /* vms-9f3: OVMX's cluster-LOGICAL LAVC addr (abs 24) */
    memset(our_src_logical, 0, sizeof(our_src_logical));
    long directed_sent = 0;
    long connect_req_sent = 0;
    long connect_resp_sent = 0;
    long start_sent = 0;
    long start_ack_sent = 0;
    long credit_sent = 0;        /* vms-691: total 0x48 credit-returns sent */
    long retransmit_sent = 0;    /* vms-691: connect-request retransmits sent */
    long dir_conn_resp_sent = 0; /* vms-246: SCS$DIRECTORY CONNECT-RESPONSEs sent */
    long dir_lookup_sent = 0;    /* vms-246: SCS$DIR_LOOKUP responses sent */
    long cm_config_frames = 0;   /* vms-224: op 0x14/0x01/0x02 CM config frames sent */
    long cm_response_sent = 0;   /* vms-224: 0x81 responses to member 0x03/0x05 txns */
    long padded_sent = 0;        /* vms-9f3: padded directed HELLOs sent (sec 4k channel verify) */

    /* OVMX identity for the phase-2 START/config body (vms-21e). Resolved once;
     * shared by every peer's START responder. */
    char ovmx_node[SYSGEN_STRVAL_LEN];
    resolve_node_identity(ovmx_node, sizeof(ovmx_node));
    uint16_t ovmx_scssystemid = resolve_scssystemid();
    /* vms-9f3: OVMX's cluster-LOGICAL LAVC address, computed ONCE from its own
     * SCSSYSTEMID (aa:00:04:00:<LE16(sysid)>). Written at the SCA src-logical
     * field (abs 24) of every emitted frame; the raw HW MAC stays at eth-src
     * and the HELLO HW-MAC tail. This is the fix that lets VAX1's PEDRIVER
     * verify the channel and open an OVMX CSB. */
    ovmx_cluster_logical(ovmx_scssystemid, our_src_logical);

    /* vms-7be: OVMX's OWN System Block, in its own configuration queue.
     * "SCA requires that each node in a network maintain an SB for every node in
     * the network. Consequently, a node must maintain an SB that describes its
     * own CPU and operating system." (p. 2-16; Figure 2-10 on p. 2-18 shows
     * VAX_1's own SB queued alongside the remote ones.) Local bookkeeping only:
     * nothing here is transmitted. */
    {
        struct scs_sb_info self_info;
        memset(&self_info, 0, sizeof(self_info));
        memcpy(self_info.system_id, our_src_logical, SCS_SYSTEM_ID_LEN);
        self_info.node_name = ovmx_node;
        self_info.os_name = "OVMX";
        scs_config_insert_sb(&scs_cfg, &self_info);
    }

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
        memcpy(hello_params.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
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
    /* vms-9f3: reusable send buffer for padded directed HELLOs (up to 1514B on
     * the wire) -- static to keep it off the loop's stack frame. */
    static uint8_t pframe[SCS_HELLO_PADDED_MAX_FRAME];
    struct timespec last_hello = {0, 0};

    while (!g_stop) {
        if (emit_hello) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - last_hello.tv_sec >= hello_interval) {
                uint8_t frame[SCS_HELLO_FRAME_LEN];
                hello_params.timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
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
                if (ps->pb == NULL || ps->connected || !ps->connect_sent) {
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
                memcpy(cp.dst_mac, ps_port_addr(ps), 6);
                memcpy(cp.src_mac, our_hw_mac, 6);
                memcpy(cp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
                cp.local_conid = OVMX_LOCAL_CONID;
                cp.remote_conid = 0;
                /* vms-c6d: re-send the SAME outstanding sequenced message --
                 * reuse the recorded unacked send_seq (do NOT advance) and the
                 * current recv_ack. */
                cp.recv_ack = ps->vc.seq.recv_seq;
                cp.send_seq = ps->vc.unacked_seq;
                cp.incarnation = ps->incarnation;
                uint8_t rframe[SCS_CONNECT_FRAME_LEN];
                if (scs_connect_build_request(&cp, rframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, ps_port_addr(ps), rframe, sizeof(rframe)) > 0) {
                    scs_vc_mark_retransmitted(&ps->vc, now_ms);
                    retransmit_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-RETX, retransmit CONNECT-REQUEST to peer"
                           " %02x:%02x:%02x:%02x:%02x:%02x (attempt %u)\n",
                           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
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
            (buf[30] == SCS_MSGTYPE_DIRLOOKUP || buf[30] == SCS_DIR_OPCODE_RETX ||
             buf[30] == SCS_MSGTYPE_SEQAPP || cls == SCS_CLASS_SCS_FIXED)) {
            uint16_t peer_send_seq = (uint16_t)(buf[34] | ((uint16_t)buf[35] << 8)); /* [20:22] */
            uint16_t peer_recv_ack = (uint16_t)(buf[32] | ((uint16_t)buf[33] << 8)); /* [18:20] */
            struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
            if (ps != NULL) {
                if (!ps->vc.initialized) {
                    scs_vc_init(&ps->vc);
                }
                /* The peer's leading counter acks OVMX's own sequenced sends. */
                scs_vc_note_peer_ack(&ps->vc, peer_recv_ack);
                ps_learn_sys_addr(&scs_cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */

                if (scs_vc_owes_credit(peer_send_seq)) {
                    scs_vc_note_recv(&ps->vc, peer_send_seq);
                    uint8_t cframe[SCS_CREDIT_FRAME_LEN];
                    if (scs_vc_build_credit_for(&ps->vc, ps_port_addr(ps), our_hw_mac,
                                                our_src_logical, ps_sys_addr(ps), cframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps_port_addr(ps), cframe,
                                      sizeof(cframe)) > 0) {
                        ps->credit_sent++;
                        credit_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-CREDIT, 0x48 credit-return acked peer_seq=%u"
                               " to %02x:%02x:%02x:%02x:%02x:%02x (#%ld)\n",
                               peer_send_seq,
                               ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                               ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                               ps->credit_sent);
                        fflush(stdout);
                    }
                }
            }
        }

        /* --- vms-224: connection-manager add-member SYSAP dialogue (spec sec
         * 4j) on the bound 190-byte VMS$VAXcluster VC. THE MILESTONE. Once the
         * 0x4b connect binds the VC (ps->connected), OVMX (the joiner) drives
         * its config burst (op 0x14/0x01/0x02) and then answers the member's
         * op 0x03 commit + op 0x05 lock-rebuild transactions with a 0x81
         * response echoing the (txn,checksum) token -- which is what promotes
         * OVMX to a full cluster MEMBER (a CSB in SDA SHOW CLUSTER). The credit
         * block above already 0x48-acked this frame at the SCS layer. */
        if (do_connect && cls == SCS_CLASS_SCS_FIXED) {
            struct scs_member_view mv;
            if (scs_member_parse(buf, (size_t)n, &mv) == 0 &&
                mv.msgtype == SCS_MEMBER_MSGTYPE &&
                (mv.remote_conid == OVMX_LOCAL_CONID ||
                 mv.local_conid == OVMX_LOCAL_CONID)) {
                struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
                if (ps != NULL && ps->connected) {
                    /* Track the member's SYSAP send-msg# high-water (our ack
                     * target). Only category-0x01 config messages carry the
                     * membership dialogue; DLM (cat 0x02) rides here later. */
                    if (mv.sysap_send_msg > ps->sysap_recv) {
                        ps->sysap_recv = mv.sysap_send_msg;
                    }

                    /* vms-d94: the add-member burst is NO LONGER triggered on
                     * the member-opened connection -- it rides OUR joiner
                     * connection (see the OVMX_JOINER_CONID bind branch). We keep
                     * tracking the member's SYSAP send-msg# high-water above so
                     * our joiner-side acks (sysap_ack_msg) stay correct. */

                    /* Answer the member-driven op 0x03 commit / op 0x05
                     * lock-rebuild transactions (echo the token, spec sec 4j). */
                    if (mv.is_member_txn) {
                        if (ps->sysap_send == 0) {
                            ps->sysap_send = 1;
                        }
                        struct scs_member_params mp;
                        memset(&mp, 0, sizeof(mp));
                        memcpy(mp.dst_mac, ps_port_addr(ps), 6);
                        memcpy(mp.src_mac, our_hw_mac, 6);
                        memcpy(mp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                        memcpy(mp.peer_logical, ps_sys_addr(ps), 6);
                        mp.remote_conid = ps->remote_conid;
                        mp.local_conid = OVMX_LOCAL_CONID;
                        mp.incarnation = ps->incarnation;
                        mp.recv_ack = ps->vc.seq.recv_seq;
                        mp.send_seq = scs_seq_advance(&ps->vc.seq);
                        mp.sysap_send_msg = ps->sysap_send++;
                        mp.sysap_ack_msg = mv.sysap_send_msg; /* ack this request */
                        uint8_t rframe[SCS_MEMBER_FRAME_LEN];
                        if (scs_member_build_response(&mp, buf, (size_t)n, rframe) == 0 &&
                            send_frame_to(sock, (int)ifindex, ps_port_addr(ps), rframe,
                                          sizeof(rframe)) > 0) {
                            ps->cm_responses++;
                            cm_response_sent++;
                            log_ts(stdout);
                            printf(" SCSD-I-CMRESP, 0x81 response to member op 0x%02x"
                                   " txn=0x%04x csum=0x%04x (echoed) send_msg=%u ack_msg=%u\n",
                                   mv.opcode, mv.txn, mv.checksum,
                                   mp.sysap_send_msg, mp.sysap_ack_msg);
                            fflush(stdout);
                        }
                    }
                }
            }
        }

        /* (a0) vms-9f3: NISCA channel packet-size verification (spec sec 4k).
         * An ESTABLISHED VAX1 zero-pads a directed HELLO up to NISCS_MAX_PKTSZ
         * (1500 SCA / 1514 wire) and retransmits it (~6s: 1500->1069->853->745)
         * until OVMX RECIPROCATES with its OWN padded HELLO on the reverse
         * channel. That reciprocal is the "ack" VAX1 waits on before opening
         * OVMX's CSB and driving the sec 4j add-member commit -- an unpadded
         * 120-byte HELLO does NOT satisfy it (GROUNDED negative, sec 4k). We
         * reply to the SAME total size (capped at NISCS_MAX_PKTSZ), rate-limited
         * to ~5s so we never out-pace VAX1's ~6s probe cadence (reply-
         * amplification guard: do NOT flood 1500B frames). A padded HELLO does
         * not classify as SCS_CLASS_HELLO (length != 120), so it reaches here as
         * SCS_CLASS_OTHER and is detected by is_padded_hello(). */
        if (is_padded_hello(buf, (size_t)n)) {
            struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            ps_learn_sys_addr(&scs_cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */
            uint16_t rx_sca = (uint16_t)((size_t)n - 14u); /* total SCA content received */
            if (rx_sca > ps->peer_padded_sca) {
                ps->peer_padded_sca = rx_sca;
            }
            /* The member advertises the node-incarnation it attributes to OVMX
             * in the padded HELLO's abs-92 flag (sec 4i.B); capture it so the
             * START [22:24] responder can echo it (that echo is the sec 4i.B
             * gate and is driven elsewhere -- left unchanged). */
            uint16_t adv_inc =
                (uint16_t)(buf[OFF_HELLO_DIRFLG] | ((uint16_t)buf[OFF_HELLO_DIRFLG + 1] << 8));
            if (adv_inc != 0) {
                ps->incarnation = adv_inc;
            }
            /* OVMX's OWN padded HELLO carries the incarnation OVMX attributes to
             * the PEER, which for a freshly-booted joiner's first contact is 1 --
             * NOT the member's advertised value. GROUNDED (spec sec 4i.B: "the
             * joiner's own directed HELLO always carries [78:80]=0x0001"), and
             * byte-exact in the golden formation-ci1 padded pair (BOTH the
             * joiner VAX2->VAX1 and member VAX1->VAX2 1500B frames carry abs-92
             * = 1). Stamping the echoed member incarnation here instead makes
             * VAX1 reject the channel-size verification and re-probe forever. */
            /* vms-d94: a padded HELLO is a channel-verify REQUEST (its abs-30
             * word is b3, spec sec 4a offset-30 / sec 4k). The GROUNDED ack to a
             * padded b3 probe is a PLAIN b4 CONFIRM HELLO -- byte-exact in
             * formation-clean-2node.pcap: VAXA->VAXB padded(1514) b3 @+34.2159 is
             * answered +0.2ms later by VAXB->VAXA PLAIN b4 @+34.2161 (and
             * symmetrically). OVMX previously RECIPROCATED a padded probe with its
             * OWN padded b3 (grounded on the degraded ci3 flood captures); on the
             * clean reference that never satisfies VAX1's size-verify -- VAX1's
             * probe needs a b4 ack it never got, so it re-floods forever and no
             * CSB opens. We now ACK each padded probe with a plain b4. OVMX's OWN
             * outbound size-verify (proving OVMX can SEND a full 1500B frame) is
             * the one-shot proactive padded b3 sent from the plain-HELLO path
             * below (guarded by padded_initiated). */
            uint8_t pad_resp_pfw = scs_hello_response_pfw(buf[30]); /* b3 -> b4 */
            ps_channel_up(ps);
            struct timespec pnow;
            clock_gettime(CLOCK_MONOTONIC, &pnow);
            uint8_t ackframe[SCS_HELLO_FRAME_LEN];
            hello_params.timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
            if (scs_hello_build_directed_frame(&hello_params, src_mac, lab_nonce,
                                               SCS_HELLO_JOINER_INCARNATION,
                                               pad_resp_pfw, ackframe) == 0 &&
                send_frame_to(sock, (int)ifindex, src_mac, ackframe, sizeof(ackframe)) > 0) {
                ps->padded_replies++;
                ps->last_padded = pnow;
                directed_sent++;
                log_ts(stdout);
                printf(" SCSD-I-PADACK, acked padded HELLO probe (%u SCA rx, abs30 %02x->%02x)"
                       " with plain b4 CONFIRM to %02x:%02x:%02x:%02x:%02x:%02x (ack #%ld)\n",
                       rx_sca, buf[30], pad_resp_pfw,
                       src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                       ps->padded_replies);
                fflush(stdout);
            }
            continue;
        }

        /* (a) Directed HELLO -> reply with our directed HELLO (NISCA channel,
         * spec sec 4b). The frame is already known to be unicast to our HW MAC
         * (gated above), so any non-zero directed flag marks a directed HELLO
         * aimed at us. Observed on the wire: an established member uses 0x0001,
         * but a member SOLICITING a not-yet-joined node (us) uses 0x0002 --
         * accept both. */
        if (cls == SCS_CLASS_HELLO && n >= OFF_HELLO_DIRFLG + 2 &&
            (buf[OFF_HELLO_DIRFLG] != 0x00 || buf[OFF_HELLO_DIRFLG + 1] != 0x00)) {
            struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            ps_learn_sys_addr(&scs_cfg, ps, buf + OFF_HELLO_SRCLOG);

            /* vms-af2/vms-691/vms-9f3: the member advertises the node-incarnation
             * it attributes to OVMX in its directed-HELLO flag [78:80] (abs 92),
             * LE u16. Capture it -- OVMX echoes it into its 0x41 START [22:24]
             * (the established-join gate, spec sec 4i.B; driven by the START
             * responder from ps->incarnation). READ off the wire; never a
             * hard-coded constant. 1 for a fresh/first contact.
             *
             * CRITICAL (spec sec 4i.B, GROUNDED on the golden wire): the value
             * OVMX stamps in its OWN directed HELLO abs 92 is NOT this advertised
             * value -- "the joiner's own directed HELLO always carries
             * [78:80]=0x0001". abs 92 is the incarnation the SENDER attributes to
             * the PEER, which for a freshly-booted joiner's first contact is 1.
             * The member's advertised value belongs ONLY in the START [22:24].
             * Echoing it back into OVMX's own HELLO abs 92 (the earlier bug)
             * leaves VAX1's PEDRIVER channel unverifiable -- it re-probes the
             * padded-HELLO ladder and re-sends START round-0 forever, and no CSB
             * ever opens. */
            uint16_t adv_incarnation =
                (uint16_t)(buf[OFF_HELLO_DIRFLG] | ((uint16_t)buf[OFF_HELLO_DIRFLG + 1] << 8));
            if (adv_incarnation != 0 && adv_incarnation != ps->incarnation) {
                ps->incarnation = adv_incarnation;
                log_ts(stdout);
                printf(" SCSD-I-INCARN, member advertises node-incarnation N=%u"
                       " (abs 92 [78:80]) -- will echo into START [22:24]"
                       " (own HELLO abs 92 stays %u)\n",
                       adv_incarnation, (unsigned)SCS_HELLO_JOINER_INCARNATION);
                fflush(stdout);
            }

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
                hello_params.timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
                /* vms-d94: reply per the GROUNDED abs-30 channel-verify rule
                 * (spec sec 4a offset-30): the received per-frame word buf[30]
                 * (b2/b3/b4) drives our reply word -- b2->b3, b3->b4 (the fix
                 * that makes the member see OVMX CONFIRM the channel), b4->b3
                 * (re-initiate). OVMX previously held a fixed b3 and never sent
                 * b4, so VAX1's NISCA handshake never finalized (vms-5fe). */
                uint8_t resp_pfw = scs_hello_response_pfw(buf[30]);
                /* abs 92 = SCS_HELLO_JOINER_INCARNATION (1): OVMX's own directed
                 * HELLO carries the incarnation it attributes to the peer on a
                 * fresh first contact, NOT the member's advertised value (spec
                 * sec 4i.B). The member's value is echoed only into START [22:24]. */
                if (scs_hello_build_directed_frame(&hello_params, src_mac, lab_nonce,
                                                   SCS_HELLO_JOINER_INCARNATION,
                                                   resp_pfw, dframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, src_mac, dframe, sizeof(dframe)) > 0) {
                    directed_sent++;
                    ps->directed_replies++;
                    ps_channel_up(ps);
                    ps->last_directed = dnow;
                    log_ts(stdout);
                    printf(" SCSD-I-DIRHELLO, replied directed HELLO (abs30 %02x->%02x)"
                           " to peer %02x:%02x:%02x:%02x:%02x:%02x (reply #%ld)\n",
                           buf[30], resp_pfw,
                           src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                           ps->directed_replies);
                    fflush(stdout);
                }
            } else {
                ps_channel_up(ps);
            }

            /* vms-9f3: match the golden joiner (VAX2) -- once our directed
             * channel is up, proactively send ONE padded HELLO up to
             * NISCS_MAX_PKTSZ to advertise OVMX's own channel size, as golden
             * idx 5990 did (spec sec 4k step 2), so an established VAX1 that is
             * waiting for the joiner to initiate verifies both directions. ONE
             * frame, guarded by padded_initiated -- not a flood. */
            if (ps->channel_up && !ps->padded_initiated) {
                size_t plen = 0;
                hello_params.timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
                /* abs-92 = SCS_HELLO_JOINER_INCARNATION (1): OVMX's own padded
                 * HELLO carries the incarnation it attributes to the peer on a
                 * fresh first contact, NOT the member's advertised value (spec
                 * sec 4i.B; golden pair carries abs-92 = 1). */
                if (scs_hello_build_padded_directed_frame(&hello_params, src_mac, lab_nonce,
                                                          SCS_HELLO_JOINER_INCARNATION,
                                                          (uint16_t)SCS_HELLO_PADDED_MAX_SCA,
                                                          pframe, sizeof(pframe), &plen) == 0 &&
                    send_frame_to(sock, (int)ifindex, src_mac, pframe, plen) > 0) {
                    ps->padded_initiated = 1;
                    clock_gettime(CLOCK_MONOTONIC, &ps->last_padded);
                    padded_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-PADINIT, initiated padded HELLO (%u SCA / %zu wire)"
                           " to %02x:%02x:%02x:%02x:%02x:%02x (golden joiner-first)\n",
                           (unsigned)SCS_HELLO_PADDED_MAX_SCA, plen,
                           src_mac[0], src_mac[1], src_mac[2],
                           src_mac[3], src_mac[4], src_mac[5]);
                    fflush(stdout);
                }
            }

            /* vms-d94: OVMX is a cluster JOINER, and a real joiner ACTIVELY OPENS
             * its OWN VMS$VAXcluster connection to the member and sends its
             * add-member request on it -- it is NOT a pure passive responder.
             * GROUNDED on the clean 2-node reference (formation-clean-2node.pcap):
             * the joiner VAXB, after the START handshake + directory exchange,
             * sends its OWN VMS$VAXcluster CONNECT-REQUEST (idx52, remote=0,
             * offering its own Con.ID) and then streams the 190-byte add-member
             * config on THAT joiner-initiated connection (idx59). The prior
             * vms-246 "pure joiner-responder" derivation was wrong: it saw only
             * the member->joiner connect in formation-ci1 and missed that the
             * joiner ALSO opens its own connection (VMS$VAXcluster is symmetric).
             * OVMX answering the member's connect but sending its burst on the
             * MEMBER-opened connection is exactly why VAX1 ignored the burst and
             * re-issued START round-0 forever.
             *
             * The PROMPT send happens in the directory handler the moment the
             * post-START directory phase begins (see SCSD-I-DIRCONN). Here we
             * only RETRANSMIT it (reusing the same send_seq) every ~1s until the
             * member accepts (joiner_connected), in case the first was lost. */
            if (do_connect && ps->start_acked && ps->joiner_connect_sent &&
                !ps->joiner_connected) {
                long now_ms = monotonic_ms();
                long last_ms = ps->last_joiner_req.tv_sec * 1000L +
                               ps->last_joiner_req.tv_nsec / 1000000L;
                if ((now_ms - last_ms) >= 1000) {
                    if (send_joiner_connect_request(sock, (int)ifindex, ps,
                                                    our_hw_mac, our_src_logical)) {
                        connect_req_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-CONNREQ, retransmit OUR VMS$VAXcluster"
                               " CONNECT-REQUEST local_conid=0x%08X seq=%u\n",
                               OVMX_JOINER_CONID, ps->joiner_req_seq);
                        fflush(stdout);
                    }
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
            struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            /* vms-246: the 0x41 START handshake is driven by the config-round
             * (spec sec 4g/4i.A), NOT by the SCS VC sequence space. Do NOT fold
             * the member's START send_seq into the VC recv_seq -- the member's
             * round-0 START may carry a large residual send_seq (sec 4i.A) and
             * the post-START VC resets to send_seq=1/recv_seq=0 on BOTH sides.
             * Feeding START counters into the VC left OVMX acking a sequence the
             * VAX never sent post-reset, so the VAX rejected OVMX's directory
             * CONNECT-RESPONSE and retransmitted its 0x5b/0x7b request forever. */
            /* vms-7be: a START describes the sending node, and its description is
             * what the System Block is built from (p. 2-12). OVMX fills the SB
             * with the one item its START parser resolves today -- the 48-bit SCS
             * System Address -- and leaves the CPU type / hardware revision / OS
             * name+version / 64-bit software incarnation fields unset rather than
             * inventing them. (The 16-bit node-incarnation OVMX echoes on the wire
             * is a NISCA field, NOT the 64-bit SCA software incarnation number of
             * p. 2-16; conflating them would be a fabrication.) */
            ps_learn_sys_addr(&scs_cfg, ps, buf + OFF_HELLO_SRCLOG); /* START src-logical, abs 24 */

            log_ts(stdout);
            printf(" SCSD-I-STARTRX, %s round=%u peer_seq=%u peer_sysid=%u\n",
                   sv.is_ack ? "ack" : "START", sv.config_round, sv.send_seq,
                   sv.is_ack ? 0 : sv.scssystemid);
            fflush(stdout);

            struct scs_start_params sp;
            memset(&sp, 0, sizeof(sp));
            memcpy(sp.dst_mac, ps_port_addr(ps), 6);
            memcpy(sp.src_mac, our_hw_mac, 6);
            memcpy(sp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
            memcpy(sp.peer_logical, ps_sys_addr(ps), 6);
            sp.scssystemid = ovmx_scssystemid;
            strncpy(sp.node_name, ovmx_node, SCS_START_NODENAME_LEN);
            sp.node_name[SCS_START_NODENAME_LEN] = '\0';
            /* START is sequenced-message #1 for a fresh VC: send_seq=1 (from the
             * state machine), leading counter recv_ack=0 -- GROUNDED joiner
             * values (spec sec 4i.A: "Every joiner 0x41 frame carries
             * send_seq=1, recv_ack=0"). Do NOT ack the member's send_seq here:
             * the member's round-0 START may carry a large residual send_seq
             * (its prior VC's continuation, e.g. 11974) which the joiner is
             * receive-tolerant of but never echoes -- the handshake is driven
             * by the config-round + incarnation echo, not by recv_ack. */
            sp.send_seq = ps->vc.seq.send_seq;
            sp.recv_ack = 0;
            /* THE ESTABLISHED-JOIN GATE (spec sec 4i.B): stamp [22:24] with the
             * node-incarnation the member advertised in its directed-HELLO
             * [78:80]. Read off the wire above; 1 for a fresh contact. */
            sp.incarnation = ps->incarnation ? ps->incarnation : 1;

            if (!sv.is_ack) {
                if (!ps->start_replied) {
                    /* Send our round-0 then round-1 START (the joiner emits both
                     * back-to-back; spec sec 4g phase-2 ordering #24/#25). */
                    for (uint16_t rnd = 0; rnd <= 1; rnd++) {
                        sp.config_round = rnd;
                        uint8_t sframe[SCS_START_FRAME_LEN];
                        if (scs_start_build(&sp, sframe) == 0 &&
                            send_frame_to(sock, (int)ifindex, ps_port_addr(ps), sframe, sizeof(sframe)) > 0) {
                            start_sent++;
                            ps->start_replies++;
                        }
                    }
                    ps->start_replied = 1;
                    /* vms-7be: drive the PB's virtual-circuit state machine
                     * (pp. 2-12..2-14). We have now SENT a START (-> START SENT)
                     * and RECEIVED one, whose description of the node we stored
                     * in the System Block above (-> START RECEIVED, the state a
                     * port driver holds while it waits for the acknowledgment
                     * that opens the circuit). Bookkeeping only: no byte of any
                     * frame depends on it. */
                    scs_pb_set_vc_state(ps->pb, SCS_VC_START_SENT);
                    scs_pb_set_vc_state(ps->pb, SCS_VC_START_RECEIVED);
                    log_ts(stdout);
                    printf(" SCSD-I-STARTTX, sent round-0+round-1 START"
                           " (sysid=%u node='%s' send_seq=%u recv_ack=%u incarnation=%u)\n",
                           ovmx_scssystemid, ovmx_node, sp.send_seq, sp.recv_ack, sp.incarnation);
                    fflush(stdout);
                }
            } else {
                /* Peer's round-2 46-byte ack -> answer with ours; START done. */
                if (!ps->start_acked) {
                    uint8_t aframe[SCS_START_ACK_FRAME_LEN];
                    if (scs_start_build_ack(&sp, aframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps_port_addr(ps), aframe, sizeof(aframe)) > 0) {
                        start_ack_sent++;
                        ps->start_acked = 1;
                        /* vms-246 FIX: the START->VC transition. Per spec sec
                         * 4i.A the phase-2 0x41 config-round counters are
                         * SEPARATE from the SCS VC; both sides reset the VC to
                         * send_seq=1/recv_seq=0 when START completes. Do it
                         * ONCE here (guarded by !start_acked) -- NOT per frame.
                         * Without it, recv_seq accumulated across formation, so
                         * OVMX's 0x5b CONNECT-RESPONSE carried recv_ack too high
                         * (observed 4 vs the golden joiner's 1) and the VAX
                         * rejected the SCS$DIRECTORY connect and retransmitted. */
                        scs_vc_reset_seq(&ps->vc);
                        /* vms-7be: the circuit is OPEN -- run the p. 2-21
                         * transition. The formative Path Block leaves the PDT
                         * and joins the System Block for this node.
                         *
                         * ===== WHAT IS ACTUALLY LIVE HERE (read this before
                         * believing a green scs test run) =====
                         * SCS_OPEN_NEW_SB is the only transition SCSD reaches
                         * today. Specifically:
                         *
                         *  - SCS_OPEN_EXISTING_REFRESHED -- the p. 2-21 rejoin
                         *    REFRESH rule -- is STRUCTURALLY UNREACHABLE here.
                         *    It needs an SB whose PB queue has emptied, and
                         *    scs_pb_close() has no caller in this file; peer
                         *    slots are never released (peer_find_or_add matches
                         *    on MAC and only ever allocates); and this call is
                         *    gated on !ps->start_acked, which is never reset, so
                         *    a rejoining node re-enters the SAME slot on the
                         *    SAME already-open PB and never gets here at all.
                         *    OVMX therefore does NOT implement the rejoin
                         *    refresh rule. tests/vmsscs/test_scs_config.c proves
                         *    the RULE; nothing proves the DAEMON does it.
                         *
                         *  - SCS_OPEN_EXISTING_SB needs a second circuit to a
                         *    node already in the configuration queue, i.e. a
                         *    peer presenting TWO Ethernet ports (two MACs, one
                         *    SCS System ID). The reference lab has never
                         *    presented one, so this is unexercised rather than
                         *    impossible; test_scsd_wire.c pins what it does to
                         *    the peer-logical bytes if it ever fires.
                         *
                         * Wiring close/reopen is WIRE-VISIBLE and therefore out
                         * of scope for this pure-refactor item: it is filed as
                         * vms-17f (blocked by vms-7be). The log below says so
                         * out loud if either branch ever fires. */
                        enum scs_open_result open_res = scs_pb_open(&scs_cfg, ps->pb);
                        log_ts(stdout);
                        printf(" SCSD-I-STARTDONE, START/config complete with peer"
                               " %02x:%02x:%02x:%02x:%02x:%02x -- VC reset"
                               " (send_seq=%u recv_seq=%u), awaiting 0x4b connect\n",
                               ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                               ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                               ps->vc.seq.send_seq, ps->vc.seq.recv_seq);
                        printf(" SCSD-I-VCOPEN, path block OPEN, %s"
                               " (configuration queue holds %u system blocks,"
                               " %u path block(s) on this node's SB)\n",
                               open_res == SCS_OPEN_NEW_SB ? "node learned for the first time"
                               : open_res == SCS_OPEN_EXISTING_REFRESHED
                                   ? "old system block REFRESHED (rejoin, p. 2-21 Note)"
                                     " -- UNEXPECTED: believed unreachable until vms-17f"
                                   : "queued to the existing system block"
                                     " -- UNEXPECTED: believed unreachable until vms-17f",
                               scs_config_sb_count(&scs_cfg),
                               scs_sb_pb_count(ps->pb->sb));
                        fflush(stdout);
                    }
                }
            }
            continue;
        }

        /* (b1) vms-d94: the member ACCEPTS OUR active-joiner VMS$VAXcluster
         * CONNECT-REQUEST. GROUNDED on the live wire (d94-fix2.pcap idx34): once
         * OVMX sends its CONNECT-REQUEST promptly after START, the member stops
         * re-issuing START and replies with a frame (observed op 0x5b, the
         * directory/resolution class) whose remote Con.ID == OVMX_JOINER_CONID
         * (our handle, echoed) and whose local Con.ID is the member's own
         * freshly-supplied handle -- i.e. the CONNECT-RESPONSE that binds OUR
         * joiner connection. scs_dir_parse does not classify it (remote != 0, op
         * != 0x0a) and it is not a 0x4b SEQAPP, so catch it here by the Con.ID
         * signature and drive the add-member burst on the bound joiner VC. */
        if (do_connect && n >= 72 &&
            (buf[30] == SCS_DIR_OPCODE || buf[30] == SCS_DIR_OPCODE_RETX ||
             buf[30] == SCS_MSGTYPE_SEQAPP)) {
            uint32_t rconid = (uint32_t)buf[64] | ((uint32_t)buf[65] << 8) |
                              ((uint32_t)buf[66] << 16) | ((uint32_t)buf[67] << 24);
            uint32_t lconid = (uint32_t)buf[68] | ((uint32_t)buf[69] << 8) |
                              ((uint32_t)buf[70] << 16) | ((uint32_t)buf[71] << 24);
            if (rconid == OVMX_JOINER_CONID && lconid != 0) {
                struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
                if (ps != NULL && !ps->joiner_connected) {
                    ps->joiner_remote_conid = lconid;
                    ps->joiner_connected = 1;
                    log_ts(stdout);
                    printf(" SCSD-I-JOINBOUND, member accepted OUR VMS$VAXcluster"
                           " connect: local=0x%08X remote=0x%08X\n",
                           OVMX_JOINER_CONID, lconid);
                    fflush(stdout);
                    if (!ps->joiner_cm_sent) {
                        int c = cm_send_config_burst(sock, (int)ifindex, ps, our_hw_mac,
                                                     our_src_logical,
                                                     OVMX_JOINER_CONID, lconid);
                        cm_config_frames += c;
                        ps->joiner_cm_sent = 1;
                        log_ts(stdout);
                        printf(" SCSD-I-CMCONFIG, sent add-member config burst"
                               " (op 0x14/0x01/0x02, %d frames, VOTES=0 non-voting)"
                               " on OUR joiner VC\n", c);
                        fflush(stdout);
                    }
                }
                continue;
            }
        }

        /* (b2) vms-246: SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder. After
         * START, the ESTABLISHED node opens an SCS$DIRECTORY SCS connection to
         * the joiner (OVMX) and queries OVMX's directory for each SYSAP it wants
         * to reach. OVMX must (1) bind the SCS$DIRECTORY Con.ID pair by answering
         * the CONNECT-REQUEST with a CONNECT-RESPONSE, and (2) answer each lookup
         * (affirm VMS$VAXcluster -- the connection manager OVMX serves --, and
         * "NOT PRESENT HERE" for SYSAPs it does not) so the VAX resolves OVMX and
         * proceeds to send the VMS$VAXcluster 0x4b CONNECT-REQUEST (spec sec 4h;
         * next item vms-c6d). Only sequenced directory frames (0x5b/0x7b, and the
         * 0x4b form the lookups switch to once the connection is up) reach here;
         * the credit-ack block above already 0x48-acked them. */
        if (do_connect &&
            (buf[30] == SCS_DIR_OPCODE || buf[30] == SCS_DIR_OPCODE_RETX ||
             buf[30] == SCS_MSGTYPE_SEQAPP)) {
            struct scs_dir_view dv;
            if (scs_dir_parse(buf, (size_t)n, &dv) == 0 &&
                (dv.is_dir_connect_request || dv.is_lookup_request)) {
                struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
                if (ps == NULL) {
                    continue;
                }
                if (!ps->vc.initialized) {
                    scs_vc_init(&ps->vc);
                }
                ps->dir_seen = 1;
                ps_learn_sys_addr(&scs_cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */
                /* Ensure recv_ack is current even if the credit block did not run
                 * (e.g. a 0x7b retransmit); note_recv only advances the high-water. */
                scs_vc_note_recv(&ps->vc, dv.send_seq);

                if (dv.is_dir_connect_request && !ps->dir_connected) {
                    /* Learn the peer's SCS$DIRECTORY handle (its local Con.ID),
                     * then reply op=1 CONNECT-ECHO + op=2 CONNECT-RESPONSE. Each
                     * is a sequenced message: advance OVMX's send_seq per frame
                     * (spec sec 4h(4)). */
                    ps->dir_remote_conid = dv.local_conid;
                    struct scs_dir_params dp;
                    memset(&dp, 0, sizeof(dp));
                    memcpy(dp.dst_mac, ps_port_addr(ps), 6);
                    memcpy(dp.src_mac, our_hw_mac, 6);
                    memcpy(dp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                    memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
                    dp.remote_conid = ps->dir_remote_conid;
                    dp.local_conid = SCS_DIR_OVMX_CONID;
                    /* vms-246 (§4i established-join): echo the member's current
                     * node-incarnation into the directory [22:24], the same
                     * value it stamps in its own 0x5b connect-request and its
                     * 0x41 START (§4i.B). Read off the wire (ps->incarnation);
                     * 0 leaves the fresh template value 1. */
                    dp.incarnation = ps->incarnation;

                    dp.recv_ack = ps->vc.seq.recv_seq;
                    dp.send_seq = scs_seq_advance(&ps->vc.seq);
                    uint8_t eframe[SCS_DIR_ECHO_FRAME_LEN];
                    if (scs_dir_build_connect_echo(&dp, eframe) == 0) {
                        send_frame_to(sock, (int)ifindex, ps_port_addr(ps), eframe, sizeof(eframe));
                    }

                    dp.recv_ack = ps->vc.seq.recv_seq;
                    dp.send_seq = scs_seq_advance(&ps->vc.seq);
                    uint8_t rframe[SCS_DIR_RESP_FRAME_LEN];
                    if (scs_dir_build_connect_response(&dp, rframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps_port_addr(ps), rframe,
                                      sizeof(rframe)) > 0) {
                        ps->dir_connected = 1;
                        dir_conn_resp_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-DIRCONN, bound SCS$DIRECTORY: remote=0x%08X"
                               " local=0x%08X with peer %02x:%02x:%02x:%02x:%02x:%02x\n",
                               ps->dir_remote_conid, (unsigned)SCS_DIR_OVMX_CONID,
                               ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                               ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
                        fflush(stdout);
                        /* vms-d94: the post-START directory phase has begun --
                         * PROMPTLY open OUR VMS$VAXcluster connection to the
                         * member (clean-ref idx52) before its CM times out and
                         * re-issues START. */
                        if (ps->start_acked && !ps->joiner_connected &&
                            send_joiner_connect_request(sock, (int)ifindex, ps,
                                                        our_hw_mac, our_src_logical)) {
                            connect_req_sent++;
                            log_ts(stdout);
                            printf(" SCSD-I-CONNREQ, sent OUR VMS$VAXcluster CONNECT-REQUEST"
                                   " local_conid=0x%08X seq=%u (active joiner, prompt)\n",
                                   OVMX_JOINER_CONID, ps->joiner_req_seq);
                            fflush(stdout);
                        }
                    }
                } else if (dv.is_lookup_request) {
                    /* Answer the SYSAP-name lookup. OVMX serves ONLY the
                     * VMS$VAXcluster connection manager; affirm it, refuse the
                     * rest with the GROUNDED "NOT PRESENT HERE" marker. */
                    struct scs_dir_lookup_params lp;
                    memset(&lp, 0, sizeof(lp));
                    memcpy(lp.dst_mac, ps_port_addr(ps), 6);
                    memcpy(lp.src_mac, our_hw_mac, 6);
                    memcpy(lp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                    memcpy(lp.peer_logical, ps_sys_addr(ps), 6);
                    lp.remote_conid = ps->dir_remote_conid ? ps->dir_remote_conid
                                                           : dv.local_conid;
                    lp.local_conid = SCS_DIR_OVMX_CONID;
                    lp.recv_ack = ps->vc.seq.recv_seq;
                    lp.send_seq = scs_seq_advance(&ps->vc.seq);
                    lp.incarnation = ps->incarnation; /* §4i established-join echo (see connect branch) */
                    lp.opcode = dv.opcode; /* echo the request opcode (0x5b/0x4b) */
                    lp.op = dv.op;
                    memcpy(lp.name, dv.name, SCS_DIR_NAME_LEN);
                    lp.affirmative = (memcmp(dv.name, "VMS$VAXcluster", 14) == 0);
                    uint8_t lframe[SCS_DIR_LOOKUP_FRAME_LEN];
                    if (scs_dir_build_lookup_response(&lp, lframe) == 0 &&
                        send_frame_to(sock, (int)ifindex, ps_port_addr(ps), lframe,
                                      sizeof(lframe)) > 0) {
                        ps->dir_lookups_answered++;
                        dir_lookup_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-DIRLOOKUP, resolved '%s' -> %s (op=0x%02x)\n",
                               dv.name, lp.affirmative ? "AFFIRMATIVE" : "NOT PRESENT HERE",
                               dv.opcode);
                        fflush(stdout);
                    }
                }
                continue;
            }
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
            struct peer_state *ps = peer_find_or_add(&scs_cfg, &scs_pdt, peers, src_mac);
            if (ps == NULL) {
                continue;
            }
            if (v.remote_conid == 0) {
                /* vms-c6d: the ESTABLISHED VAX drives the VMS$VAXcluster 0x4b
                 * CONNECT-REQUEST (remote Con.ID still 0 = OVMX's not yet known)
                 * and OVMX answers with a CONNECT-RESPONSE echoing the VAX's
                 * Con.ID and supplying its own -- the admission act that binds
                 * the CDT (spec sec 4g phase 4). The response MUST carry OVMX's
                 * LIVE VC counters: recv_ack = recv_seq (acks the VAX's just-sent
                 * request send_seq, already note_recv'd by the credit block) and
                 * a freshly-advanced send_seq. Baking the golden 7/8 made the VAX
                 * reject the accept and retransmit forever. The VAX retransmits
                 * its request until it accepts our response, so RE-ANSWER each
                 * request (a new sequenced message with current counters) instead
                 * of going silent once bound -- a lost response then self-heals. */
                struct scs_connect_params cp;
                memset(&cp, 0, sizeof(cp));
                memcpy(cp.dst_mac, ps_port_addr(ps), 6);
                memcpy(cp.src_mac, our_hw_mac, 6);
                memcpy(cp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
                cp.local_conid = OVMX_LOCAL_CONID;
                cp.remote_conid = v.local_conid;
                cp.recv_ack = ps->vc.seq.recv_seq;
                cp.send_seq = scs_seq_advance(&ps->vc.seq);
                cp.incarnation = ps->incarnation; /* §4i.B established-join echo (0 => fresh 1) */
                uint8_t rframe[SCS_CONNECT_FRAME_LEN];
                if (scs_connect_build_response(&cp, rframe) == 0 &&
                    send_frame_to(sock, (int)ifindex, ps_port_addr(ps), rframe, sizeof(rframe)) > 0) {
                    ps->remote_conid = v.local_conid;
                    connect_resp_sent++;
                    int first = !ps->connected;
                    ps->connected = 1;
                    log_ts(stdout);
                    printf(" SCSD-I-CONNRESP, %s peer VMS$VAXcluster CONNECT-REQUEST:"
                           " remote=0x%08X local=0x%08X recv_ack=%u send_seq=%u incarnation=%u\n",
                           first ? "answered" : "re-answered",
                           v.local_conid, OVMX_LOCAL_CONID, cp.recv_ack, cp.send_seq,
                           cp.incarnation ? cp.incarnation : 1);
                    fflush(stdout);
                    /* vms-d94: do NOT send the add-member burst on this
                     * member-opened connection. The clean-ref grounding
                     * (formation-clean-2node.pcap) shows the joiner sends its
                     * add-member config on the connection IT opens (idx52/59),
                     * not on the member-opened one -- OVMX doing the latter is
                     * exactly why VAX1 ignored the burst and looped START. We
                     * still answer the member's CONNECT-REQUEST (above) to keep
                     * that connection alive; the burst rides our own joiner
                     * connection, bound in the OVMX_JOINER_CONID branch below. */
                }
            } else if (v.remote_conid == OVMX_JOINER_CONID && !ps->joiner_connected) {
                /* vms-d94: the member's CONNECT-RESPONSE to OUR active-joiner
                 * CONNECT-REQUEST -> the joiner-initiated VMS$VAXcluster pair is
                 * bound {local=OVMX_JOINER_CONID, remote=member's}. THIS is the
                 * connection the add-member burst must ride (clean-ref idx59). */
                ps->joiner_remote_conid = v.local_conid;
                ps->joiner_connected = 1;
                log_ts(stdout);
                printf(" SCSD-I-JOINBOUND, member accepted OUR VMS$VAXcluster connect:"
                       " local=0x%08X remote=0x%08X\n", OVMX_JOINER_CONID, v.local_conid);
                fflush(stdout);
                if (!ps->joiner_cm_sent) {
                    int c = cm_send_config_burst(sock, (int)ifindex, ps, our_hw_mac,
                                                 our_src_logical,
                                                 OVMX_JOINER_CONID, ps->joiner_remote_conid);
                    cm_config_frames += c;
                    ps->joiner_cm_sent = 1;
                    log_ts(stdout);
                    printf(" SCSD-I-CMCONFIG, sent add-member config burst"
                           " (op 0x14/0x01/0x02, %d frames, VOTES=0 non-voting)"
                           " on OUR joiner VC\n", c);
                    fflush(stdout);
                }
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
        fprintf(stderr, "  DIR-CONNECT-RESP-SENT=%ld DIR-LOOKUP-RESP-SENT=%ld\n",
                dir_conn_resp_sent, dir_lookup_sent);
        fprintf(stderr, "  CM-CONFIG-FRAMES=%ld CM-RESPONSES-SENT=%ld PADDED-HELLO-SENT=%ld\n",
                cm_config_frames, cm_response_sent, padded_sent);
        for (int i = 0; i < OVMX_MAX_PEERS; i++) {
            if (peers[i].pb == NULL) {
                continue;
            }
            const uint8_t *pa = peers[i].pb->remote_port_addr;
            fprintf(stderr,
                    "  PEER %02x:%02x:%02x:%02x:%02x:%02x vc=%s channel=%s directed_replies=%ld"
                    " incarnation=%u start_replied=%d start_acked=%d dir_connected=%s"
                    " dir_lookups=%ld connect_sent=%d connected=%s"
                    " credit_sent=%ld retx=%u remote_conid=0x%08X"
                    " cm_config=%s cm_responses=%ld sysap_send=%u sysap_recv=%u"
                    " padded_replies=%ld padded_init=%d peer_padded_sca=%u\n",
                    pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
                    scs_vc_state_name(peers[i].pb->vc_state),
                    peers[i].channel_up ? "UP" : "down", peers[i].directed_replies,
                    peers[i].incarnation,
                    peers[i].start_replied, peers[i].start_acked,
                    peers[i].dir_connected ? "YES" : "no", peers[i].dir_lookups_answered,
                    peers[i].connect_sent, peers[i].connected ? "YES" : "no",
                    peers[i].credit_sent, peers[i].vc.retransmit_count,
                    peers[i].remote_conid,
                    peers[i].cm_config_sent ? "YES" : "no", peers[i].cm_responses,
                    peers[i].sysap_send, peers[i].sysap_recv,
                    peers[i].padded_replies, peers[i].padded_initiated,
                    peers[i].peer_padded_sca);
        }
    }

    close(sock);
    return 0;
}
