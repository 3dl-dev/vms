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
#include "scs_cdt.h"
#include "scs_classify.h"
#include "scs_config.h"
#include "scs_conn.h"
#include "scs_dgram.h"
#include "scs_connect.h"
#include "scs_depart.h"
#include "scs_dir.h"
#include "scs_disc.h"
#include "scs_hello.h"
#include "scs_sdir.h"
#include "scs_member.h"
#include "scs_poll.h"
#include "scs_reason.h"
#include "scs_start.h"
#include "scs_svc.h"
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
/* vms-abc: reaching this cap IS the p. 2-31 delivery-guarantee failure, and
 * scs_vc_delivery_failed() is what decides that. The two numbers must be the
 * same one or OVMX would either give up before declaring failure (a silent
 * limp-on, the bug this item removes) or declare failure it never reached. */
_Static_assert(SCS_VC_DELIVERY_RETRY_LIMIT == VC_RETRANSMIT_MAX,
               "scsd's retransmit cap and the p. 2-31 delivery-failure limit disagree");

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
    /* vms-17f: CLOCK_MONOTONIC ms at which this peer was last heard from AT ALL
     * -- any 0x6007 frame carrying its source MAC, multicast beacons included,
     * not only the ones addressed to us. That breadth is deliberate: a node that
     * is still beaconing has not departed, and stamping only unicast-to-us
     * frames would declare a live-but-quiet member gone. 0 = never heard (a slot
     * built by a test, or by a lookup that allocated one). */
    uint64_t last_rx_ms;
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
    uint32_t poll_remote_conid;    /* vms-66f: the peer's handle on the connection OUR
                                    * SCS$DIR_LOOKUP opened to ITS SCS$DIRECTORY.
                                    * Distinct from dir_remote_conid, which is the
                                    * peer-opened direction (p. 2-49). */
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
    /* --- vms-dd5: the three CONNECTION DESCRIPTORS this peer's three SCS
     * connections live in, and with them the documented connection state
     * machine (scs_conn.h). Each is a CDT in the node-wide Connection
     * Descriptor List (scsd_cdl), claimed at the EXACT Con.ID OVMX has been
     * putting on the lab wire since vms-5fe so that wiring the CDL changes no
     * byte. NULL = not yet formed, or the machine is off (OVMX_NO_CONN_FSM), or
     * the CDL slot was already claimed by another peer -- see conn_bind(). The
     * booleans above (dir_connected/connected/joiner_connected) are NOT
     * replaced: they still gate every send, so the machine is a recorder, not a
     * gate. */
    struct scs_cdt *cdt_dir;    /* SCS$DIRECTORY,   local Con.ID = SCS_DIR_OVMX_CONID */
    struct scs_cdt *cdt_member; /* VMS$VAXcluster the MEMBER opened, local = OVMX_LOCAL_CONID */
    struct scs_cdt *cdt_joiner; /* VMS$VAXcluster OVMX opened,       local = OVMX_JOINER_CONID */
    /* vms-abc: has send_frame_vc() already announced that THIS circuit carries
     * no traffic? The refusal is a per-circuit fact, not a per-frame one, and
     * the daemon re-enters the refusing paths at the peer's HELLO cadence
     * (~1 Hz), so logging every refusal buries the run in identical lines.
     * Cleared by scsd_vc_on_open() so a circuit that breaks AGAIN says so
     * again. Every refusal is still COUNTED in vc_sends_refused and reported
     * by scsd_exit_summary(). */
    int      novc_logged;
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
    /* vms-17f: first contact IS contact. Without this stamp a slot allocated by
     * a frame that never reaches peer_touch() would sit at last_rx_ms 0 forever
     * and the departure sweep would have nothing to age it from. */
    tbl[free_slot].last_rx_ms = monotonic_ms();
    return &tbl[free_slot];
}

/*
 * peer_touch - vms-17f: record that `mac` was heard from at `now_ms`. Stamps an
 * EXISTING peer slot only; it never allocates one, so a stray frame from an
 * unknown node cannot consume a slot. Called for every 0x6007 frame the daemon
 * receives, BEFORE the "unicast to our HW MAC" gate -- see the last_rx_ms field
 * comment for why the multicast beacons have to count.
 */
static void peer_touch(struct peer_state *tbl, const uint8_t mac[6], uint64_t now_ms)
{
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        if (tbl[i].pb != NULL && mac_eq(tbl[i].pb->remote_port_addr, mac)) {
            tbl[i].last_rx_ms = now_ms;
            return;
        }
    }
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

/* =====================================================================
 * vms-dd5 -- THE CONNECTION DESCRIPTOR LIST AND THE CONNECTION STATE MACHINE
 * =====================================================================
 *
 * WHAT CHANGED AND WHAT DID NOT. vms-e1a built scs_cdt.c (the CDT/CDL/CONID
 * model of pp. 2-28..2-30) but nothing linked it and scsd.c called none of it;
 * scs_cdt.h said so plainly. This block is that wiring, plus the state machine
 * that vms-e1a explicitly left out. It is a RECORDER, not a gate: every
 * `if (!ps->dir_connected)` / `if (!ps->joiner_connected)` guard that decided
 * what OVMX sends is untouched and still decides it, the builders are called
 * with the same arguments in the same order, and the three Con.IDs are claimed
 * at their existing fixed values. No byte moves. See scs_conn.h's WIRE VERDICT.
 *
 * THE MAPPING FROM OBSERVED FRAME TO SCA MESSAGE is the weak claim here and it
 * is stated at each call site below, with its grounding, so it can be checked
 * against the frame that triggers it.
 *
 * KNOWN LIMIT, stated rather than hidden: OVMX's three Con.IDs are NODE-GLOBAL
 * (OVMX_LOCAL_CONID / OVMX_JOINER_CONID / SCS_DIR_OVMX_CONID are macros, not
 * per-peer allocations), so exactly one peer's connections can occupy those
 * three CDL slots. This is the same blocker scs_cdt.h recorded. conn_bind()
 * therefore LOGS and returns NULL for a second peer rather than allocating a
 * different Con.ID than the one on the wire, which would make the CDL lie. In
 * the lab (one VAX) this never fires; on a 3-node cluster the second peer's
 * connections are simply not tracked, and the log says which.
 */

/* The node-wide Connection Descriptor List (p. 2-29). File scope so the send
 * helpers can reach it; initialized once in daemon main. */
static struct scs_cdl scsd_cdl;
static int scsd_cdl_ready = 0;

/*
 * vms-561: the node's SCS SERVICE PORT -- the object the five architected
 * services (LISTEN, CONNECT, ACCEPT, REJECT, DISCONNECT) run against. One per
 * node, next to the one CDL it drives. See src/vmsscs/include/scs_svc.h.
 *
 * scsd.c is the PORT DRIVER half of p. 2-56's split: the services own the
 * descriptor lifecycle and the connection state, and call back into the
 * scsd_svc_emit_* functions below to build and transmit the SCS control message
 * each transition names. Those emitters are ordinary senders and are named in
 * the SEND SITE TABLE like any other.
 */
static struct scs_svc_port scsd_svc_port;

/*
 * scsd_svc - the port, with its CDL bound on first use.
 *
 * The binding is LAZY rather than done once in main() because main() is not the
 * only thing that starts a node: SCSD_UNIT_TEST renames main() away, and
 * tests/vmsscs/test_scsd_wire.c builds a world by initializing scsd_cdl
 * directly and then calling the production dispatch. A port whose CDL was only
 * ever bound from main() would be descriptorless in every one of those tests --
 * i.e. the entire connection model would silently stop being exercised, which
 * is the failure mode this epic keeps catching. Binding on first use makes the
 * daemon and the tests take the same path.
 *
 * scs_svc_port_init() zeroes the counters, so this must run at most once per
 * CDL; the scsd_svc_port.cdl == NULL guard is what guarantees that.
 */
/* vms-66f: the SCS Process Poller (p. 2-50). Declared up here with the other
 * node-wide state because scsd_sysap_vc_loss(), far above its definition, has
 * to re-enable polling on a lost connection. See the POLLER block further down
 * for what it does and what it deliberately does not do. */
static struct scs_poller scsd_poller;
static int scsd_poller_ready = 0;

static struct scs_svc_port *scsd_svc(void)
{
    if (scsd_svc_port.cdl == NULL && scsd_cdl_ready) {
        scs_svc_port_init(&scsd_svc_port, &scsd_cdl);
        /*
         * p. 2-22's "list of listening SYSAPs", which as of vms-7fe IS the
         * p. 2-48 SDIR queue: each of these allocates an SDIR carrying the name
         * plus the CONID of a listening CDT (scs_sdir.h).
         *
         * OVMX LISTENS FOR EXACTLY WHAT IT SERVES, AND NOT MSCP$DISK.
         * vms-7fe was dispatched naming three SYSAPs -- VMS$VAXcluster,
         * MSCP$DISK, SCS$DIRECTORY. Two are registered here and the third is
         * DELIBERATELY NOT, which is a stated deviation, not an oversight:
         *
         *   VMS$VAXcluster  SERVED. scsd.c answers its CONNECT-REQUEST, binds
         *                   the Con.ID pair and drives the add-member burst.
         *   SCS$DIRECTORY   SERVED. scsd.c answers its CONNECT-REQUEST and its
         *                   SCS$DIR_LOOKUP messages. Note that registering it
         *                   is itself the fix for a small lie: before this item
         *                   a lookup for SCS$DIRECTORY got "NOT PRESENT HERE"
         *                   from a hardcoded compare that affirmed only
         *                   VMS$VAXcluster, while the daemon was serving the
         *                   connection at the same moment.
         *   MSCP$DISK       NOT SERVED. OVMX has no disk server: there is no
         *                   MSCP responder anywhere in this tree. p. 2-48 says
         *                   LISTEN means "ready and willing to handle connect
         *                   requests from SYSAPs on other nodes", and the
         *                   Directory Service's whole job is to answer whether
         *                   that is true. Registering MSCP$DISK would make OVMX
         *                   answer AFFIRMATIVE to the MSCP$DISK lookup the
         *                   reference VAX actually sends (spec sec 1, golden
         *                   directory phase) -- changing a byte on the wire in
         *                   order to advertise a service that does not exist,
         *                   and inviting a CONNECT_REQ nothing here can honour.
         *                   That is exactly the INV-6 failure class. When OVMX
         *                   grows an MSCP server, that item adds the one line.
         */
        (void)scs_listen(&scsd_svc_port, "VMS$VAXcluster", NULL, NULL);
        (void)scs_listen(&scsd_svc_port, "SCS$DIRECTORY", NULL, NULL);
    }
    return &scsd_svc_port;
}

/*
 * vms-7fe: the p. 2-48 scan's outcomes, counted for the exit report. These are
 * the numbers that say what the SDIR queue actually did on a run -- in
 * particular sdir_busy_replies, which the OVMX design-choice-3 note in
 * scs_sdir.h predicts is 0 because the daemon's receive loop cannot reach it.
 * tests/vmsscs/test_scsd_wire.c turns that prediction into a check: it sums
 * this counter across every case in the file and asserts the total is 0.
 */
static unsigned long sdir_connect_scans = 0;   /* inbound CONNECT_REQs scanned */
static unsigned long sdir_no_such_sysap = 0;   /* p. 2-48 refusals emitted */
static unsigned long sdir_busy_replies = 0;    /* p. 2-50 refusals emitted */
static unsigned long sdir_refusals_unsent = 0; /* refusal the circuit would not carry */

/* Counters for the end-of-run summary. */
static unsigned long conn_transitions = 0;
static unsigned long conn_illegal_events = 0;
static unsigned long conn_unemitted_actions = 0;

/* vms-6b3: p. 2-26's 16-bit reason code, as RECEIVED. `seen` counts every
 * REJECT_REQ/DISCONNECT_REQ addressed to one of our Con.IDs whose field we
 * decoded; `nonzero` counts those where the peer actually supplied a reason.
 * Both are printed in the exit summary because "0 nonzero" is the answer our
 * whole lab capture set gives (scs_reason.h) and it must be VISIBLE rather than
 * inferred from the absence of a log line. */
static unsigned long conn_reason_seen = 0;
static unsigned long conn_reason_nonzero = 0;

/* vms-591: the DISCONNECT dialogue (Figure 2-16), counted for the exit summary.
 * These are the numbers that say whether the teardown was symmetric or whether
 * OVMX only ever answered: `req_recv` counts peer DISCONNECT_REQs delivered to
 * the machine, `rsp_sent` OVMX's answers, `req_sent` OVMX's OWN disconnect
 * calls (which is the half that did not exist before this item), `rsp_recv` the
 * peer's answers to those, `closed` connections that actually reached CLOSED,
 * and `simul` the p. 2-27 simultaneous case. `shutdown_pending` is how many
 * connections were still not CLOSED when the shutdown wait timed out. */
static unsigned long disc_req_recv = 0;
static unsigned long disc_rsp_sent = 0;
static unsigned long disc_req_sent = 0;
static unsigned long disc_rsp_recv = 0;
static unsigned long disc_closed = 0;
static unsigned long disc_simultaneous = 0;
static unsigned long disc_shutdown_pending = 0;

/* vms-abc: p. 2-31 message-guarantee enforcement, counted for the exit report. */
static unsigned long vc_seq_gaps = 0;    /* sequentiality failures detected */
static unsigned long vc_breaks = 0;      /* circuits explicitly broken */
static unsigned long vc_conns_broken = 0;/* connections broken with them */
static unsigned long sysap_vc_loss_notifications = 0; /* SYSAP handlers actually invoked */
static unsigned long vc_sends_refused = 0; /* frames send_frame_vc() would not transmit */

/*
 * vms-abc: THE SYSAP's VC-LOSS ERROR HANDLER, and the first one OVMX has ever
 * installed.
 *
 * p. 2-28: "the VMS implementation of SCA stores the address of the interested
 * SYSAP's error handler for virtual circuit loss in the CDT itself. This address
 * is supplied by a SYSAP as an argument to the VMS implementations of both the
 * CONNECT and ACCEPT services." p. 2-31: when a message guarantee fails, "every
 * connection supported by this virtual circuit is also broken, and the SYSAPs
 * participating in these connections are notified of the event."
 *
 * BEFORE THIS ITEM no CDT carried a VC-loss handler at all, so the notification
 * half of p. 2-31 was implemented and DEAD: scs_cdl_vc_loss() walked the Path
 * Block's connection queue, counted what it found and notified nobody, because
 * every vc_loss_handler it tested was NULL. conn_bind() below now installs THIS
 * function on every CDT it creates -- the first SYSAP error handler OVMX has
 * ever had.
 *
 * WHO CALLS THE SCAN. An earlier draft of this comment said scs_cdl_vc_loss()
 * "had no production caller". That was true when it was written and is FALSE on
 * this branch: vms-17f landed underneath this item and brought one. There are
 * TWO production callers, and this handler is now reached from BOTH:
 *
 *   1. scs_vc_break()  (src/vmsscs/scs_vc.c) -- THIS item's caller. A p. 2-31
 *      message-guarantee failure: a receive-side sequence gap detected in
 *      scsd_handle_frame(), or retransmit exhaustion in scsd_retransmit_tick().
 *      Gated by OVMX_NO_VC_BREAK.
 *   2. scs_pb_depart() (src/vmsscs/scs_depart.c) -- vms-17f's caller, the
 *      p. 2-28 ordered teardown, reached from scsd_peer_departure_sweep() below
 *      when a peer goes silent past the listen timeout.
 *      Gated by OVMX_NO_PEER_DEPART.
 *
 * THE CONSEQUENCE ON CALLER (2), which this item introduced and therefore owns:
 * because installation happens in conn_bind() rather than in the break path,
 * EVERY peer departure carrying bound connections now invokes this handler,
 * moves sysap_vc_loss_notifications and emits SCSD-W-SYSAPVCLOSS. vms-17f's
 * departures used to be silent here. That is asserted through the production
 * sweep by test_departure_notifies_the_sysaps() in tests/vmsscs/test_scsd_wire.c.
 *
 * WHY OVMX_NO_VC_BREAK DOES NOT GATE INSTALLATION -- decided, not defaulted.
 * The obvious alternative is to skip the install when this item's kill switch is
 * set, so the switch covers everything the item changes. Rejected, for three
 * reasons:
 *   - The switch's contract is "do not BREAK a circuit on a message-guarantee
 *     failure"; it announces itself as SCSD-W-VCBREAKOFF. A departure is not a
 *     message-guarantee failure.
 *   - Notifying the SYSAP on a departure is p. 2-28 behaviour that vms-17f
 *     explicitly wanted and could not have, only because no handler existed.
 *     Gating installation here would make THIS item's kill switch silently
 *     disable a SIBLING item's intended behaviour -- a switch that does more
 *     than revert its own item is worse than no switch.
 *   - It would change WHAT IS BOUND rather than what happens on failure: a CDT
 *     with a NULL vc_loss_handler is a structurally different object, so the
 *     switch would perturb steady state, not just the failure path.
 * The property that actually matters is that no path reaching this handler is
 * UNGATED, and that holds: caller (1) is gated by OVMX_NO_VC_BREAK, caller (2)
 * by OVMX_NO_PEER_DEPART. Both gates are RUN with the counter confirmed to stay
 * at 0, including the deliberately-not-gated combination (departure sweep with
 * OVMX_NO_VC_BREAK=1 set, where the notification MUST still fire).
 *
 * WHAT IT DOES: clears the bound-connection flag for exactly the connection
 * that was broken, so the daemon's view of what is bound matches the CDT it
 * just closed.
 *
 * WHAT IT DOES **NOT** DO, and an earlier revision of this comment claimed it
 * did -- the claim was FALSE and was measured false. It said these three
 * booleans "are what actually decide whether scsd.c sends anything on a
 * connection, so leaving them set ... would keep OVMX transmitting". Two of the
 * three are read NEGATED: the directory CONNECT-REQUEST branch gates on
 * `!ps->dir_connected`, and the prompt-joiner branch and
 * scsd_joiner_retransmit_pending() gate on `!ps->joiner_connected`. Clearing
 * them RE-ARMS those sends. MEASURED: with no other guard, replaying the peer's
 * captured SCS$DIRECTORY CONNECT-REQUEST after a seq-gap break made OVMX emit a
 * SECOND directory CONNECT-RESPONSE (dir_conn_resp_sent 1 -> 2) on a Path Block
 * whose vc_state was CLOSED.
 *
 * AND `ps->connected` DOES NOT SUPPRESS ANYTHING EITHER, which a previous
 * revision of this comment also got wrong: it claimed `ps->connected` (read
 * POSITIVE at the 0x4b branch) was "suppressed by the clear". It is not. The
 * branch that ANSWERS the member's 0x4b CONNECT-REQUEST is gated on
 * `v.remote_conid == 0` and never reads ps->connected at all -- it SETS it. So
 * replaying the peer's captured VMS$VAXcluster connect after a break re-armed
 * ps->connected 0 -> 1, which in turn re-opened the 0x81 CM-response path that
 * IS gated on it. MEASURED, same broken circuit: connect_resp_sent 1 -> 2,
 * credit_sent 2 -> 3, two frames on the wire, member CDT back to ACCEPT SENT.
 *
 * WHAT ACTUALLY ENFORCES p. 2-31 ("Any attempt to send a message from one port
 * to another in the absence of a virtual circuit will fail") is the CLOSED Path
 * Block, consulted at ONE choke point that every SCS-layer send now goes
 * through: send_frame_vc(). See its SEND SITE TABLE for the full census of
 * senders and the two justified exemptions (VC formation and NISCA HELLO).
 * These three booleans decide only which SYSAP dialogue the daemon believes is
 * bound; they gate no send that the choke point does not already refuse.
 *
 * WHAT IT DELIBERATELY DOES NOT DO, so nobody reads more into it: it does not
 * free the peer slot, does not release the CDT, and does not re-drive a join.
 * Reopening after a loss is vms-17f's teardown/rejoin path, not this handler's.
 */
static void scsd_sysap_vc_loss(struct scs_cdt *cdt, void *ctx)
{
    struct peer_state *ps = (struct peer_state *)ctx;
    const char *which = "?";

    sysap_vc_loss_notifications++;
    if (ps != NULL) {
        if (cdt == ps->cdt_dir) {
            ps->dir_connected = 0;
            which = "SCS$DIRECTORY";
        } else if (cdt == ps->cdt_member) {
            ps->connected = 0;
            which = "VMS$VAXcluster (member-opened)";
        } else if (cdt == ps->cdt_joiner) {
            ps->joiner_connected = 0;
            which = "VMS$VAXcluster (joiner-opened)";
            /* p. 2-50: "If that connection is lost, SYSAP_A has the option of
             * once again requesting its Process Poller to look for SYSAP_X on
             * NODE_X." OVMX exercises that option. */
            if (scsd_poller_ready) {
                scs_poll_connection_lost(&scsd_poller, "VMS$VAXcluster",
                                         ps_sys_addr(ps));
            }
        }
    }
    log_ts(stdout);
    printf(" SCSD-W-SYSAPVCLOSS, SYSAP notified: connection %s (conid=0x%08X)"
           " is broken; it is no longer bound\n",
           which, cdt != NULL ? (unsigned)cdt->local_conid : 0u);
    fflush(stdout);
}

/*
 * vms-17f: the p. 2-20/2-21 open transitions this run actually took, indexed by
 * enum scs_open_result, plus the departure sweep's tallies. File scope for the
 * same reason as the three above: scsd_vc_on_open() is handed a const context
 * and the sweep runs from main()'s timer block, so there is no one `rx` both can
 * reach. tests/vmsscs/test_scsd_wire.c resets and reads them.
 *
 * These exist because "the REFRESH branch is reachable now" is precisely the
 * kind of claim this epic has repeatedly sent back for being unmeasured. The
 * number is the claim.
 */
static unsigned long pb_open_results[3] = {0, 0, 0}; /* NEW_SB, EXISTING_SB, REFRESHED */
static unsigned long pb_open_errors = 0;
/* vms-22e: opens ABANDONED by the p. 2-21 footnote masquerade tests. Separate
 * from pb_open_errors because an abandonment is a RULE firing, not a fault, and
 * because "the wire has never produced one" is a claim this counter measures
 * rather than asserts -- see the reachability block on scsd_vc_on_open(). */
static unsigned long pb_open_masquerades = 0;
static unsigned long peer_departures = 0;
static unsigned long depart_connections_lost = 0;
static unsigned long depart_refusals = 0;

/*
 * ===== vms-561: SCSD AS THE PORT-DRIVER HALF OF THE FIVE SERVICES =====
 *
 * p. 2-56 splits the work and this file is on the far side of the split:
 *
 *   "The SCS CONNECT and ACCEPT services each result in the allocation of a CDT
 *    for new connections. The SCS DISCONNECT service results in releasing CDTs.
 *    Thus, the SYSAP calling interface for each of these services is in
 *    SYS$SCS. However, the actual dialogue for connection formation and
 *    disconnect is managed by SCS code in the port driver."
 *
 * So conn_bind() is GONE. Nothing in this file allocates, binds or releases a
 * CDT any more, and nothing here decides a connection's state: scs_connect()
 * and scs_accept() (src/vmsscs/scs_svc.c) do both, and call back into the three
 * scsd_svc_emit_* functions below for the frames. What is left here is frame
 * assembly, transmission and the peer bookkeeping that is genuinely the
 * daemon's -- ps->dir_connected, ps->joiner_req_seq, the run counters.
 *
 * The p. 2-28 VC-loss error handler ("supplied by a SYSAP as an argument to ...
 * both the CONNECT and ACCEPT services") is now passed the way the book says,
 * as args.vc_loss. OVMX still has no message/datagram input routines to install
 * (received frames are dispatched by Con.ID comparison, not through the CDL --
 * see scs_cdt.h), so args.msg_input/args.dgram_input stay NULL and
 * scs_cdl_deliver_* still has no production caller.
 *
 * scsd_svc_mark / scsd_svc_settle - the exit summary's three counters
 * (conn_transitions / conn_illegal_events / conn_unemitted_actions) used to be
 * incremented by conn_step(). The services keep the same tallies on the port,
 * so these two take the delta across one service call and fold it into the
 * daemon's counters. Sampling the delta rather than reading the port directly
 * keeps the counters resettable by a test that resets only scsd.c's.
 */
struct scsd_svc_mark {
    unsigned long transitions;
    unsigned long illegal;
};

static struct scsd_svc_mark scsd_svc_mark(void)
{
    struct scsd_svc_mark m;
    m.transitions = scsd_svc_port.transitions;
    m.illegal = scsd_svc_port.illegal;
    return m;
}

static void scsd_svc_settle(struct scsd_svc_mark m)
{
    conn_transitions += scsd_svc_port.transitions - m.transitions;
    conn_illegal_events += scsd_svc_port.illegal - m.illegal;
}

/*
 * scsd_svc_no_builder - the emitters' answer for an SCS control message OVMX
 * cannot build. Returns SCS_SVC_EMIT_NOBUILDER, so the connection still takes
 * the transition (it really is in the new state) while the run log records that
 * no frame went out. This is conn_step()'s old SCSD-W-CONNNOACT line, moved to
 * the one place that actually knows whether a builder exists.
 */
static int scsd_svc_no_builder(struct scs_cdt *cdt, enum scs_conn_action act)
{
    conn_unemitted_actions++;
    log_ts(stdout);
    printf(" SCSD-W-CONNNOACT, conid=0x%08X: the state machine requires"
           " '%s' here and OVMX has no builder for it -- nothing was sent\n",
           cdt != NULL ? (unsigned)cdt->local_conid : 0u,
           scs_conn_action_name(act));
    fflush(stdout);
    return SCS_SVC_EMIT_NOBUILDER;
}

/*
 * scsd_svc_slot_refused - the SCS_SVC_NOCDT log, which conn_bind() used to
 * carry. OVMX's three Con.IDs are NODE-GLOBAL (see the KNOWN LIMIT above), so a
 * second peer's connection cannot have its own CDT at the same Con.ID. The
 * frame still went out (the state machine is a recorder, never a gate); what is
 * lost is the tracking, and that is what this says.
 */
static void scsd_svc_slot_refused(uint32_t local_conid, const char *local_sysap)
{
    log_ts(stderr);
    fprintf(stderr,
            " SCSD-W-CONNSLOT, CDL slot for Con.ID 0x%08X (%s) is already"
            " claimed -- OVMX's Con.IDs are node-global, so this peer's"
            " connection is NOT tracked by the connection state machine\n",
            (unsigned)local_conid, local_sysap);
    fflush(stderr);
}

/*
 * The wire->event mapping lives in scs_conn.c (scs_conn_event_for_msgtype), NOT
 * here: it is a pure function of one wire field, it carries the grounding for
 * every value in docs/cluster-protocol-spec.md sec 4(h)(1a), and keeping it in
 * the library is what lets tests/vmsscs/test_scs_dir.c and test_scs_connect.c
 * assert that the frames OVMX BUILDS classify as the SCA messages the state
 * machine thinks they are -- without a socket.
 *
 * conn_step - drive the machine and account for the result. `emitted` names the
 * frame the daemon actually put on the wire for this step, or NULL if it sent
 * nothing; when the machine's action names a frame OVMX has no builder for,
 * that is LOGGED as an unemitted action instead of being passed over in
 * silence. That log line is the honest record of the OVMX/SCA divergence, and
 * on the 0x4b connections it fires on every run.
 */
static void conn_step(struct scs_cdt *cdt, enum scs_conn_event ev, const char *emitted)
{
    if (cdt == NULL) {
        return;
    }
    struct scs_conn_transition t = scs_conn_fsm_step(cdt, ev);
    if (t.suppressed) {
        return;
    }
    conn_transitions++;
    if (t.illegal) {
        conn_illegal_events++;
        return;
    }
    if (t.action != SCS_CONN_ACT_NONE && emitted == NULL) {
        conn_unemitted_actions++;
        log_ts(stdout);
        printf(" SCSD-W-CONNNOACT, conid=0x%08X: the state machine requires"
               " '%s' here and OVMX has no builder for it -- nothing was sent\n",
               (unsigned)cdt->local_conid, scs_conn_action_name(t.action));
        fflush(stdout);
    }
}

#ifdef SCSD_UNIT_TEST
/*
 * vms-7be TRANSMIT CAPTURE SEAM -- compiled ONLY into tests/vmsscs/test_scsd_wire.c,
 * which is the single translation unit that defines SCSD_UNIT_TEST. The daemon
 * never defines it, so the production build of send_frame_raw below is byte-for-byte
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

/*
 * send_frame_raw - THE TRANSPORT, and nothing else: hand a fully-built Ethernet
 * frame to a specific unicast MAC on ifindex. It applies NO policy.
 *
 * vms-abc RENAMED IT from send_frame_to, deliberately and mechanically. Every
 * SCS-layer send in this file now goes through send_frame_vc() below, which
 * asks the peer's Path Block whether a virtual circuit exists before any byte
 * leaves (p. 2-31). The rename is what makes the remaining direct callers
 * COUNTABLE: tests/vmsscs/test_scsd_send_sites.py attributes every
 * send_frame_raw() call to its enclosing function and reds unless that set is
 * exactly the EXEMPT list in the SEND SITE TABLE below. Under the old name a
 * new send site was indistinguishable from an old one.
 *
 * THE DIRECT CALLERS, and how many calls each makes: send_frame_vc() 1,
 * send_frame_channel() 1, scsd_vc_emit() 2. Those three counts are PINNED in
 * that script (EXEMPT_CALLS), so an extra raw send added inside an already
 * exempt function -- the one way to get an unguarded frame past a structural
 * check that only looks at function names -- reds too. The number of sends
 * routed through the choke point is reported by the script but deliberately
 * NOT pinned: adding one is safe by construction.
 */
static ssize_t send_frame_raw(int sock, int ifindex, const uint8_t mac[6],
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
 * ===== vms-abc: THE ONE PLACE p. 2-31 IS ENFORCED ON THE TRANSMIT PATH =====
 *
 * p. 2-31: "Any attempt to send a message from one port to another in the
 * absence of a virtual circuit will fail." NOT "any attempt to send a directory
 * reply". The rule is about the circuit, so the enforcement has to be about the
 * circuit too -- one predicate, consulted by every SCS-layer sender, rather
 * than a guard bolted onto whichever branch was last caught misbehaving.
 *
 * WHY THIS SHAPE, stated as the history that produced it. This item first
 * guarded ONE path (the SCS$DIRECTORY CONNECT-REQUEST reply). Re-measuring the
 * SAME broken circuit then found the identical defect on three more: the 0x48
 * credit-return, the 0x4b VMS$VAXcluster CONNECT-RESPONSE (which additionally
 * re-armed ps->connected and re-opened the CM 0x81 path), and the
 * SCS$DIR_LOOKUP reply. The defect was never per-path: NO SEND SITE CONSULTED
 * THE PATH BLOCK. Guarding sites one at a time cannot terminate, because the
 * next site added is unguarded by default. Routing them through one function
 * inverts that: a new site is guarded by default and an UNguarded one has to be
 * written deliberately, named in the table below, and justified.
 *
 * WHY THE GATE BOOLEANS CANNOT DO THIS JOB. scsd_sysap_vc_loss() clears
 * ps->dir_connected / ps->connected / ps->joiner_connected on a break, but two
 * of the three are read NEGATED by the dispatch (`!ps->dir_connected`,
 * `!ps->joiner_connected`), so clearing them RE-ARMS those sends. The durable
 * fact is the Path Block: breaking a circuit does not deallocate it (scs_vc.h
 * note 4), it leaves it CLOSED, and CLOSED is what this predicate reads.
 *
 * ============================ SEND SITE TABLE ============================
 * EVERY sender in this file and its verdict. ALL THREE parts of this table are
 * MECHANICALLY CHECKED by tests/vmsscs/test_scsd_send_sites.py, which parses
 * scsd.c and attributes every call to its enclosing function:
 *
 *   - it reds unless the ONLY function NAMING a transmit primitive is
 *     send_frame_raw(), and it names exactly one. The list is
 *     TRANSMIT_PRIMITIVES in that script (sendto, sendmsg, sendmmsg, send,
 *     writev, pwritev, pwrite, write, syscall) and it is matched as a BARE
 *     IDENTIFIER, so taking one as a value rather than calling it is caught
 *     too;
 *   - it reds unless the direct send_frame_raw() callers are exactly the EXEMPT
 *     list, with exactly the pinned per-function call counts;
 *   - it reds unless the send_frame_vc() callers are exactly the CHOKED list;
 *   - it reds unless the send_frame_channel() callers, and how many calls each
 *     makes, are exactly what the EXEMPT entry for it below says. The exemption
 *     is per FUNCTION, so its call sites are the one thing a name-only check
 *     could not see growing.
 *
 * Adding OR renaming a sender without editing this table reds the test run.
 *
 * THE PRIMITIVE CHECK IS WHY 'EVERY' IS SUPPORTABLE. Before vms-abc's second
 * round the census keyed only on the two wrapper NAMES, so a raw sendto() was
 * invisible to it by construction -- and MEASURED, one existed: main()'s HELLO
 * beacon loop called sendto() on the AF_PACKET socket directly, incremented
 * rx.hello_sent and appeared in the exit summary, while sitting in neither half
 * of a table that claimed to list every sender. It is now routed through
 * send_frame_channel() (see the EXEMPT entry), and the census keys on the
 * primitive so the next one cannot hide the same way.
 *
 *   CHOKED (go through send_frame_vc, refused on a non-OPEN circuit) -- these
 *   are SCS sequenced messages and connection-control messages, i.e. exactly
 *   the traffic p. 2-31 says a circuit carries:
 *     cm_send_config_burst()       op 0x14 / 0x01 / 0x02 add-member config
 *     scsd_handle_frame()          0x48 credit-return
 *                                  0x81 CM transaction response
 *                                  SCS$DIR_LOOKUP response
 *
 *   CHOKED, and new in vms-561: the three SERVICE EMITTERS. These are the
 *   port-driver half of the five SCS services (p. 2-56) -- scs_connect() and
 *   scs_accept() call them once per transition that names a packet. The frames
 *   are the same frames, from the same builders, in the same order; what
 *   changed is that the CDT and the state transition now belong to
 *   src/vmsscs/scs_svc.c instead of being open-coded around each send:
 *     scsd_svc_emit_connect_req()    0x4b joiner CONNECT-REQUEST (CONNECT), and
 *                                    the DEAD scsd_retransmit_tick() branch's
 *                                    0x4b retransmit, which reuses it
 *     scsd_svc_emit_dir_accept()     op=1 SCS$DIRECTORY CONNECT-ECHO and
 *                                    op=2 SCS$DIRECTORY CONNECT-RESPONSE (ACCEPT)
 *     scsd_svc_emit_member_accept()  0x4b VMS$VAXcluster CONNECT-RESPONSE (ACCEPT)
 *
 *   CHOKED, and new in vms-591: the DISCONNECT emitter. One function, two
 *   frames, both of the Figure 2-16 teardown arrows:
 *     scsd_svc_emit_disconnect()   the 62-byte DISCONNECT_REQ (message type 6)
 *                                  and the 58-byte DISCONNECT_RSP (type 7),
 *                                  built by src/vmsscs/scs_disc.c from
 *                                  byte-exact captured templates. Reached from
 *                                  scs_svc_deliver() on a received
 *                                  DISCONNECT_REQ/RSP, from scs_disconnect()
 *                                  when the SYSAP invokes its own symmetric
 *                                  disconnect, and from
 *                                  scs_svc_disconnect_all() at shutdown --
 *                                  three call paths, ONE send site, which is
 *                                  the shape this table exists to keep.
 *                                  CHOKED because a teardown is ordinary
 *                                  sequenced traffic: p. 2-31's rule applies
 *                                  and, if the circuit is already gone, every
 *                                  connection on it has been driven to CLOSED
 *                                  by scs_vc_break() and there is nothing left
 *                                  to disconnect.
 *
 *   CHOKED, and new in vms-7fe:
 *     scsd_send_sdir_refusal()     the 66-byte CONNECT_RSP that DECLINES a
 *                                  connect request -- p. 2-48 "no such SYSAP",
 *                                  and p. 2-50 "busy ... try again later" as a
 *                                  status value it will take but that THIS FILE
 *                                  NEVER PASSES IT (see the function). Not
 *                                  a service emitter, because the p. 2-48 scan
 *                                  failed before any SYSAP saw the request, so
 *                                  there is no CDT and no Figure 2-14
 *                                  transition; see the function for the full
 *                                  argument and for why the census's check 7
 *                                  lists it beside the three. It is never
 *                                  emitted in the configuration OVMX runs.
 *
 *   CHOKED, and new in vms-66f: the SCS PROCESS POLLER'S TWO SENDERS. These are
 *   the first frames OVMX has ever sent to a REMOTE SCS$DIRECTORY (everything
 *   above answers one). Both are sequenced messages on an established circuit,
 *   so both are choked like any other; neither takes an exemption:
 *     scsd_poll_emit()      the 110-byte 0x5b SCS$DIRECTORY CONNECT-REQUEST
 *                           the poller opens its cycle with (p. 2-50, template
 *                           SCA#21). This is the CONNECT service's emitter,
 *                           called by scs_connect() exactly as the three above.
 *                           Its DISCONNECT_REQ arm builds nothing and says so.
 *     scsd_poll_inquire()   the 94-byte lookup REQUEST ("is <SYSAP> in your list
 *                           of listening SYSAPs?", template SCA#29). Not a
 *                           service emitter -- an inquiry is a SYSAP message on
 *                           an open connection, not a Figure 2-14 action -- so
 *                           it is listed separately, like scsd_send_sdir_refusal
 *                           above and for the same kind of reason.
 *
 *   EXEMPT (call send_frame_raw directly). Two functions, and the reason is the
 *   same in both: THESE FRAMES ARE HOW A CIRCUIT COMES TO EXIST. Refusing them
 *   on a non-OPEN circuit is not conservative, it is a deadlock -- no circuit
 *   could ever open, and a broken one could never re-form.
 *     scsd_vc_emit()        The 0x41 VC-FORMATION frames (START / STACK / ACK,
 *                           p. 2-14). The formation machine's states are
 *                           CLOSED, START SENT, START RECEIVED, OPEN; three of
 *                           the four are not OPEN, and the machine's whole job
 *                           is to transmit from them. The circuit state IS the
 *                           guard here -- scs_vc_fsm_*() decides what may be
 *                           sent from each state, which is a stricter check
 *                           than "is it OPEN", not a weaker one.
 *     send_frame_channel()  ALL NISCA HELLO traffic, called from 4 sites in 2
 *                           functions -- a set and a count BOTH PINNED by the
 *                           census (CHANNEL_CALLERS), so this paragraph
 *                           re-derives:
 *                             scsd_handle_frame()       3 -- the padded-probe
 *                               b4 ack, the rate-limited directed reply, and
 *                               the one-shot proactive padded HELLO;
 *                             scsd_hello_beacon_emit()  1 -- the periodic
 *                               MULTICAST beacon off main()'s timer, the one
 *                               that increments rx.hello_sent and is reported
 *                               in the exit summary.
 *                           See that function for the reason; in short, HELLO
 *                           is CHANNEL maintenance BELOW the virtual circuit
 *                           and is the only thing that re-establishes a
 *                           channel after a break. The beacon is the strongest
 *                           case in the class: it is addressed to the cluster
 *                           MULTICAST group, not to any port address, and it is
 *                           sent when NO peer and therefore no Path Block and
 *                           no circuit is known at all -- it is how peers are
 *                           discovered. Gating it on an OPEN circuit would mean
 *                           a node could never announce itself.
 *
 *   THE EXEMPTION IS PER FUNCTION, WHICH IS WHY send_frame_channel() EXISTS.
 *   The three HELLO sends used to be inline in scsd_handle_frame(), so the
 *   census had to exempt that whole 1000-line function -- and MEASURED, four
 *   unguarded SCS senders inside it passed the census while transmitting on a
 *   CLOSED circuit. Never exempt a function that contains anything else.
 * =========================================================================
 *
 * scsd_refuse_without_open_vc - the predicate. Returns 1 (caller must send
 * nothing) when `vc` -- the circuit the caller intends to send on -- is not
 * OPEN; returns 0 when it is, filling *out (when non-NULL) with the CONFIG_PATH
 * view the caller then addresses the frame from.
 *
 * THE CIRCUIT IS NAMED, NOT ASSUMED. Every caller but one passes ps->pb, the
 * peer's own Path Block. send_joiner_connect_request() passes the circuit
 * CONNECT selected, which p. 2-47 permits to be a NAMED path or one found by
 * the CONFIG_SYS + OPEN scan -- not necessarily ps->pb. Reading ps->pb there
 * would check a different circuit from the one the frame rides.
 *
 * The state is read through scs_config_path() -- CONFIG_PATH (p. 2-47) -- not
 * by reaching into the Path Block, so the daemon sees the same view of a path
 * that any other CONFIG_PATH caller would. A NULL circuit, or one CONFIG_PATH
 * refuses to describe, is also "no circuit" and is refused the same way.
 *
 * NEVER SILENT, but never repetitive either: a dropped reply that says nothing
 * reads as a healthy run (CLAUDE.md rule 9 / INV-6), while one line per refused
 * frame buries the run -- the daemon re-enters these paths at the peer's HELLO
 * cadence, ~1 Hz, for the rest of the run. So the FIRST refusal on a circuit
 * logs SCSD-E-NOVC naming the circuit, its state and what was suppressed; later
 * refusals on the same circuit are counted, not printed, and the total is in
 * the exit summary. scsd_vc_on_open() clears the latch, so a circuit that
 * breaks a second time announces it a second time.
 */
static int scsd_refuse_without_open_vc(struct peer_state *ps, const struct scs_pb *vc,
                                       const char *what,
                                       struct scs_config_path_info *out)
{
    struct scs_config_path_info scratch;
    struct scs_config_path_info *path = (out != NULL) ? out : &scratch;
    if (vc != NULL && scs_config_path(vc, path) && path->vc_state == SCS_VC_OPEN) {
        return 0;
    }
    vc_sends_refused++;
    if (ps != NULL && ps->novc_logged) {
        return 1; /* already announced for this circuit -- counted, not printed */
    }
    if (ps != NULL) {
        ps->novc_logged = 1;
    }
    const uint8_t *pa = ps_port_addr(ps);
    log_ts(stderr);
    fprintf(stderr,
            " SCSD-E-NOVC, no OPEN virtual circuit to peer"
            " %02x:%02x:%02x:%02x:%02x:%02x (circuit is %s) -- %s NOT sent"
            " (p. 2-31: a send with no virtual circuit fails). Further refusals"
            " on this circuit are counted, not logged.\n",
            pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
            vc != NULL ? scs_vc_state_name(vc->vc_state) : "absent", what);
    fflush(stderr);
    return 1;
}

/*
 * send_frame_vc - THE CHOKE POINT. Transmit `frame` on circuit `vc`, or refuse.
 * Returns the byte count send_frame_raw() returned, or -1 if the circuit
 * refused it -- so every existing `... > 0` call-site idiom keeps working
 * unchanged and a refused send takes the same "nothing was sent" branch a
 * builder failure would.
 *
 * THE DESTINATION COMES FROM THE CIRCUIT, not from the caller. p. 2-47: the
 * remote port address a frame is addressed to is a property of the Path Block
 * CONNECT selected, so it is read back out of the CONFIG_PATH view that just
 * proved the circuit OPEN. For every caller passing ps->pb this is byte-for-
 * byte ps_port_addr(ps), the value those sites passed before; for
 * send_joiner_connect_request() it is path.remote_port_addr, the value that
 * site already used.
 */
static ssize_t send_frame_vc(int sock, int ifindex, struct peer_state *ps,
                             const struct scs_pb *vc, const char *what,
                             const uint8_t *frame, size_t len)
{
    struct scs_config_path_info path;
    if (scsd_refuse_without_open_vc(ps, vc, what, &path)) {
        return -1;
    }
    return send_frame_raw(sock, ifindex, path.remote_port_addr, frame, len);
}

/*
 * send_frame_channel - THE OTHER EXEMPTION, given a name so it can be counted.
 *
 * NISCA channel traffic: the directed and padded HELLO replies, and (since
 * vms-abc's second round) the periodic multicast HELLO beacon in main().
 * Deliberately NOT routed through send_frame_vc(), for the reason in the SEND
 * SITE TABLE -- a HELLO is not a sequenced message, carries no Con.ID, is
 * addressed to the frame's source MAC (or, for the beacon, to the cluster
 * multicast group) rather than to a Path Block's remote port address, is
 * exchanged with peers that have no Path Block at all, and is the only thing
 * that re-establishes a channel after a break. Gating it on an OPEN circuit
 * would make a broken circuit permanently unrecoverable, and would stop a
 * node announcing itself at all.
 *
 * IT IS ALSO WHERE THE TRANSMIT PRIMITIVE STOPS BEING SPELLED OUT TWICE. The
 * beacon used to build its own `struct sockaddr_ll` and call sendto() on the
 * socket directly, so the "a HELLO has no circuit" exemption was argued in two
 * places and enumerated in neither. It is argued once now, here.
 *
 * WHY IT IS A FUNCTION RATHER THAN THREE INLINE send_frame_raw() CALLS, which
 * is what it was: the census in tests/vmsscs/test_scsd_send_sites.py works by
 * attributing each call to its enclosing function. With the HELLO sends inline
 * in scsd_handle_frame(), exempting the HELLOs meant exempting that entire
 * 1000-line function -- and MEASURED, four unguarded SCS senders inside it
 * (the SCS$DIR_LOOKUP reply, the two SCS$DIRECTORY replies and the 0x81 CM
 * response) passed the census while transmitting on a CLOSED circuit. Hoisting
 * the three HELLO sends out is what shrinks the exemption to the frames it is
 * actually about.
 */
static ssize_t send_frame_channel(int sock, int ifindex, const uint8_t dst[6],
                                  const uint8_t *frame, size_t len)
{
    return send_frame_raw(sock, ifindex, dst, frame, len);
}

/*
 * ===== vms-4071: the VC FORMATION state machine, driven onto the NISCA wire =====
 *
 * The machine itself is in src/vmsscs/scs_vc.c (CLOSED / START SENT / START
 * RECEIVED / OPEN, the p. 2-14 acceptable-response table, the reissue timer,
 * the OS-dependent retry limit, the p. 2-16 implied ACK). What lives HERE is
 * only the translation between the machine's abstract packet classes and the
 * concrete 0x41 config-round frames the existing vms-21e builders emit:
 *
 *     SCS_VC_ACT_SEND_START -> scs_start_build,     config-round 0 (106 bytes)
 *     SCS_VC_ACT_SEND_STACK -> scs_start_build,     config-round 1 (106 bytes)
 *     SCS_VC_ACT_SEND_ACK   -> scs_start_build_ack, config-round 2 ( 46 bytes)
 *
 * No frame layout is re-derived: these are the same two builders, called with
 * the same parameters, that shipped before this item.
 *
 * ===== WHAT THE DEFAULT BUILD PUTS ON A FRESH-JOIN WIRE =====
 * Unchanged from pre-vms-4071, in BYTES and in ORDER RELATIVE TO THE PEER's
 * frames. The fresh-join interleaving is still
 *     OVMX r0, OVMX r1, peer r1, peer r2, OVMX r2
 * because scsd_vc_settle() holds OVMX's round-2 ack until the PEER's round-2
 * frame arrives, exactly as the pre-vms-4071 `else if (sv.is_ack)` branch did.
 * The state machine reaches OPEN one frame earlier than the ack goes out; that
 * is a state change, not a wire change. See scsd_vc_settle().
 *
 * MEASURED, not asserted (lab-2 pod vaxlab-3, 2026-08-03). Head-to-head fresh
 * joins, this branch (tag W9A) vs the same commit's parent main (tag WAA), same
 * pod, same runner, captures at
 * /data/training/vax/k8s-labs/vaxlab-3/logs/d94-{W9A,WAA}.pcap:
 *   - Both emitted 3 OVMX 0x41 frames in the order round-0, round-1, round-2,
 *     with the peer's frames interleaved identically:
 *       peer r0, OVMX r0, OVMX r1, peer r1, peer r2, OVMX r2.
 *   - Byte-diffing the three OVMX frames pairwise, the ONLY differing absolute
 *     offsets are 28, 60 and 109 -- the SCSSYSTEMID low byte (1409 vs 1410) and
 *     one SCSNODE character (OVMXW9 vs OVMXWA). Zero protocol bytes differ.
 *   - Emission counters identical: START-SENT=2 START-ACK-SENT=1
 *     CONNECT-REQ-SENT=1 CREDIT-SENT=8 DIR-CONNECT-RESP-SENT=1
 *     DIR-LOOKUP-RESP-SENT=4 CM-CONFIG-FRAMES=3.
 * NEITHER run reached CLUSTER_NODES=3 on that pod, and that is PRE-EXISTING:
 * the main control failed identically. This item does not claim to fix it.
 *
 * ===== WHAT IS NEW ON THE WIRE, AND WHAT GATES IT =====
 *   1. RETRY / ABANDON. A START/STACK unanswered for
 *      SCS_VC_FORMATION_TIMEOUT_MS is reissued (p. 2-14) up to
 *      scs_vc_retry_limit() times, after which formation is ABANDONED. Fires
 *      only under loss; never on a clean join. Kill-switch:
 *      OVMX_VC_NO_RETRY_LIMIT=1 restores unbounded retry with no abandon.
 *   2. THE EARLY ACK -- OPT-IN, DEFAULT OFF. OVMX_VC_EARLY_ACK=1 emits the
 *      round-2 ack on the OPEN transition instead (the literal p. 2-14 rule:
 *      "A response of either ACK or STACK will advance the circuit to the OPEN
 *      state; and if the response is STACK, the port driver will issue an
 *      ACK"). Same 46 bytes, one peer-frame earlier. NO lab capture has shown
 *      the reference member accepts that ordering, so it does not ship on by
 *      default. See SCS_VC_EARLY_ACK_ENV in scs_vc.h.
 *
 *      KILL-SWITCH PROVEN TO MOVE (guardrail 23), lab-2 tag W5A vs W1A: with
 *      OVMX_VC_EARLY_ACK=1 the daemon logs SCSD-I-STARTDONE immediately after
 *      "STACK received -> send ACK, VC OPEN" and BEFORE the peer's round-2
 *      arrives; with the switch off it logs it after "ACK received", i.e. after
 *      the peer's round-2 -- which is where the pre-vms-4071 control (W4A)
 *      logs it too. NOTE HONESTLY: on THIS lab the two orderings are NOT
 *      distinguishable in the capture, because the peer emits its round-1 and
 *      round-2 back to back inside one millisecond, so both are already on the
 *      wire before OVMX can turn its ack around. The switch changes the daemon's
 *      emission point; on this peer's timing it does not change the pcap.
 *   3. THE IMPLIED ACK (p. 2-16) can open a circuit with no peer round-2 ever
 *      arriving, in which case OVMX owes its round-2 immediately. Reachable
 *      only from START RECEIVED on an SCS sequenced-message class, which a
 *      clean join never produces mid-dialogue.
 */
struct scsd_vc_ctx {
    int             sock;
    int             ifindex;
    const uint8_t  *hw_mac;      /* OVMX HW MAC (Ethernet src) */
    const uint8_t  *src_logical; /* OVMX cluster-logical addr (SCA abs 24) */
    uint16_t        scssystemid;
    const char     *node_name;
    struct scs_config *cfg;
};

/*
 * scsd_vc_emit - transmit the formation packet the state machine asked for.
 * Returns 1 if a frame went out, 0 otherwise. `act` must be one of the three
 * SEND_* actions; anything else is a no-op.
 */
static int scsd_vc_emit(const struct scsd_vc_ctx *ctx, struct peer_state *ps,
                        enum scs_vc_action act)
{
    if (ctx == NULL || ps == NULL || ps->pb == NULL) {
        return 0;
    }
    struct scs_start_params sp;
    memset(&sp, 0, sizeof(sp));
    memcpy(sp.dst_mac, ps_port_addr(ps), 6);
    memcpy(sp.src_mac, ctx->hw_mac, 6);
    memcpy(sp.src_logical, ctx->src_logical, 6);
    memcpy(sp.peer_logical, ps_sys_addr(ps), 6);
    sp.scssystemid = ctx->scssystemid;
    strncpy(sp.node_name, ctx->node_name, SCS_START_NODENAME_LEN);
    sp.node_name[SCS_START_NODENAME_LEN] = '\0';
    /* GROUNDED joiner values, unchanged from vms-21e: every joiner 0x41 frame
     * carries send_seq=1 / recv_ack=0 (spec sec 4i.A) and echoes the member's
     * advertised node-incarnation at [22:24] (spec sec 4i.B). */
    sp.send_seq = ps->vc.seq.send_seq;
    sp.recv_ack = 0;
    sp.incarnation = ps->incarnation ? ps->incarnation : 1;

    if (act == SCS_VC_ACT_SEND_ACK) {
        uint8_t aframe[SCS_START_ACK_FRAME_LEN];
        if (scs_start_build_ack(&sp, aframe) != 0) {
            return 0;
        }
        return send_frame_raw(ctx->sock, ctx->ifindex, ps_port_addr(ps),
                             aframe, sizeof(aframe)) > 0 ? 1 : 0;
    }
    if (act == SCS_VC_ACT_SEND_START || act == SCS_VC_ACT_SEND_STACK) {
        sp.config_round = (act == SCS_VC_ACT_SEND_START) ? 0 : 1;
        uint8_t sframe[SCS_START_FRAME_LEN];
        if (scs_start_build(&sp, sframe) != 0) {
            return 0;
        }
        return send_frame_raw(ctx->sock, ctx->ifindex, ps_port_addr(ps),
                             sframe, sizeof(sframe)) > 0 ? 1 : 0;
    }
    return 0;
}

/*
 * scsd_open_result_text - what the p. 2-21 open transition did, as one clause
 * for the log line. Purely a name for an enum value: it asserts nothing about
 * whether the daemon can reach it, because a log branch that claims a rule the
 * daemon cannot execute is dead code wearing a fact's clothes (that is what the
 * two "UNEXPECTED: believed unreachable" clauses here were before vms-17f).
 * Reachability is stated once, below, with the test that measures it.
 */
static const char *scsd_open_result_text(enum scs_open_result r)
{
    switch (r) {
    case SCS_OPEN_NEW_SB:
        return "node learned for the first time";
    case SCS_OPEN_EXISTING_REFRESHED:
        return "old system block REFRESHED (rejoin, p. 2-21 Note)";
    case SCS_OPEN_EXISTING_SB:
        return "queued to the existing system block";
    default:
        return "OPEN FAILED -- no path block or no system block";
    }
}

/*
 * scsd_vc_on_open - the p. 2-21 open transition, run ONCE per circuit whenever
 * the state machine reaches OPEN (by STACK, by a bare ACK, or by the p. 2-16
 * implied ACK). Body unchanged from vms-21e/vms-246/vms-7be; only its trigger
 * moved from "the peer sent its round-2 ack" to "the circuit reached OPEN".
 *
 * REACHABILITY AS MEASURED (vms-17f, extended by vms-22e), so a green test run
 * is not misread. Each line is a counter this function increments, and each is
 * asserted by a named case in tests/vmsscs/test_scsd_wire.c that drives
 * scsd_handle_frame() or scsd_vc_on_open() directly:
 *
 *   SCS_OPEN_NEW_SB              REACHED. Every peer discovered for the first
 *                                time. (test_vc_happy_path_frame_sequence and
 *                                the rejoin case below.)
 *   SCS_OPEN_EXISTING_REFRESHED  REACHED SINCE vms-17f, and only since: it needs
 *                                the departure sweep to have closed the previous
 *                                Path Block and released the peer slot, so that
 *                                the returning node builds a second, FORMATIVE
 *                                PB whose open finds the old SB with an empty PB
 *                                queue. (test_rejoin_reaches_the_p221_refresh.)
 *   SCS_OPEN_EXISTING_SB         NOT REACHED, and not reachable as OVMX is
 *                                built: it needs one node presenting TWO ports,
 *                                so that its SB still has another PB queued to
 *                                it when the second circuit opens. OVMX binds a
 *                                single interconnect and creates one PB per peer
 *                                MAC. Left in scsd_open_result_text() as a name,
 *                                claiming nothing.
 *   SCS_OPEN_ERROR               Only from a PB with no SB attached.
 *   SCS_OPEN_ABANDONED_MASQUERADE
 *                                NOT REACHED BY THE WIRE, and cannot be today:
 *                                the p. 2-21 footnote comparisons need BOTH
 *                                sides populated, and the formative side never
 *                                is -- struct scs_start_view carries no node
 *                                name and no incarnation, so the only thing
 *                                SCSD learns about a REMOTE node is its 48-bit
 *                                System Address (scs_pb_learn_system_addr),
 *                                leaving node_name empty and incarnation 0. All
 *                                three tests are therefore INDETERMINATE on the
 *                                live path and scs_config_masquerade_check()
 *                                returns SCS_MASQ_PASS. The branch below is NOT
 *                                dead code wearing a fact's clothes: it is
 *                                driven end-to-end by
 *                                test_masquerade_open_is_logged_and_suppresses_vcopen
 *                                in test_scsd_wire.c, which seeds a peer_state
 *                                whose SB does carry a name and calls this
 *                                function, and which asserts the SCSD-W-VCMASQ
 *                                text names the failing test AND that no
 *                                STARTDONE/VCOPEN line follows.
 */
static void scsd_vc_on_open(const struct scsd_vc_ctx *ctx, struct peer_state *ps)
{
    /* vms-246: the phase-2 config-round counters are SEPARATE from the SCS VC;
     * both sides reset the VC to send_seq=1/recv_seq=0 when START completes. */
    scs_vc_reset_seq(&ps->vc);
    enum scs_open_result open_res = scs_pb_open(ctx->cfg, ps->pb);
    if (open_res == SCS_OPEN_ABANDONED_MASQUERADE) {
        /* vms-22e: the p. 2-21 footnote tests rejected the formative System
         * Block. Log WHICH test failed and stop -- the circuit is not open, so
         * claiming STARTDONE/VCOPEN below would be a lie. The Path Block is left
         * abandoned; tearing it down is vms-17f's surface, not this function's.
         * NOTE this is unreached in production today. A comparison needs BOTH
         * sides populated, and the formative side never is: the only thing SCSD
         * learns about a REMOTE node is its 48-bit System Address
         * (scs_pb_learn_system_addr), with node_name empty and incarnation 0.
         * (SCSD does give its OWN System Block a node name, above in main(), but
         * that is the other side of the comparison and cannot supply the
         * missing one.) See the ANTI-MASQUERADE block in scs_config.h. */
        pb_open_masquerades++;
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-W-VCMASQ, virtual-circuit formation ABANDONED with peer"
                " %02x:%02x:%02x:%02x:%02x:%02x -- masquerade test failed: %s"
                " (p. 2-21 footnote)\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                scs_masquerade_result_name(
                    (enum scs_masquerade_result)ps->pb->masquerade_fail));
        fflush(stderr);
        return;
    }
    if (open_res >= SCS_OPEN_NEW_SB && open_res <= SCS_OPEN_EXISTING_REFRESHED) {
        pb_open_results[(int)open_res]++;
    } else {
        pb_open_errors++;
    }
    /* vms-abc: the circuit carries traffic again, so the "this circuit refuses
     * sends" announcement is spent. Clearing it here (and ONLY here) is what
     * makes the latch per-BREAK rather than once-per-process: a circuit that
     * opens, breaks, re-opens and breaks again logs SCSD-E-NOVC twice. */
    ps->novc_logged = 0;
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
           scsd_open_result_text(open_res),
           scs_config_sb_count(ctx->cfg),
           scs_sb_pb_count(ps->pb->sb));
    fflush(stdout);
}

/*
 * scsd_vc_ack_due - THE WIRE-ORDERING DECISION, isolated in one predicate.
 *
 * The circuit being OPEN is not by itself permission to transmit OVMX's round-2
 * ack; WHEN that 46-byte frame goes out is what decides whether a fresh join
 * looks byte-and-order identical to pre-vms-4071. Three ways it becomes due:
 *
 *  1. DEFAULT / PRESERVED (`peer_round2_seen`): the peer's own round-2 frame has
 *     arrived. This is precisely the pre-vms-4071 trigger -- the old code emitted
 *     the ack from the `else` branch gated on scs_start_view.is_ack. Keeping it
 *     as the default is what makes the fresh-join interleaving unchanged.
 *  2. OPT-IN (`OVMX_VC_EARLY_ACK=1`): emit as soon as the circuit is OPEN, i.e.
 *     on the peer's round-1 STACK. That is the literal p. 2-14 rule, but it puts
 *     the ack one peer-frame earlier and NO lab capture has yet shown the
 *     reference member accepts it, so it is off unless asked for.
 *  3. IMPLIED ACK (p. 2-16, `fsm.implied_acks`): the peer is already sending
 *     circuit traffic, so it considers the circuit OPEN and will never send a
 *     round-2 for case 1 to wait on. Waiting would deadlock the dialogue. This
 *     path is unreachable on a clean join (it needs an SCS sequenced-message
 *     class to arrive while the PB is in START RECEIVED).
 *
 * Case 3 is an OVMX design choice, labeled per rule 8: p. 2-16 says only that
 * the circuit is marked OPEN, not that anything is transmitted. It is the NISCA
 * encoding that requires both nodes to carry every config round (spec sec 4g
 * phase 2: 0/0/1/1/2/2, "both nodes carry the same round").
 */
/*
 * scsd_vc_peer_round2 - THE DEFAULT ack trigger, in one place so the daemon and
 * its test cannot drift apart about what it is: the peer's round-2 46-byte
 * frame, which scs_vc_classify_round() maps to SCS_VC_EV_ACK. This is exactly
 * the condition the pre-vms-4071 code branched on (scs_start_view.is_ack).
 */
static int scsd_vc_peer_round2(enum scs_vc_event ev)
{
    return ev == SCS_VC_EV_ACK;
}

static int scsd_vc_ack_due(const struct peer_state *ps, int peer_round2_seen)
{
    if (peer_round2_seen) {
        return 1;
    }
    if (scs_vc_early_ack_enabled()) {
        return 1;
    }
    return ps->pb != NULL && ps->pb->fsm.implied_acks > 0;
}

/*
 * scsd_vc_settle - after any state-machine step, do the two things that are
 * NISCA-specific rather than SCA-abstract:
 *
 *  - If the circuit is OPEN, OVMX has not yet put its round-2 ack out, and
 *    scsd_vc_ack_due() says it is time, emit it and run the p. 2-21 open
 *    transition. `peer_round2_seen` is 1 only at the call site that just
 *    processed the peer's 46-byte round-2 frame.
 *  - If formation was ABANDONED, re-arm the Path Block's formation machine so a
 *    later START from the same peer can start the dialogue over. p. 2-14 says
 *    only that formation is abandoned, not what happens next; OVMX cannot tear
 *    the Path Block down here because the peer-slot/PB lifecycle is vms-17f, so
 *    it resets the machine in place and says so in the log.
 */
static void scsd_vc_settle(const struct scsd_vc_ctx *ctx, struct peer_state *ps,
                           enum scs_vc_action act, int peer_round2_seen,
                           long *start_ack_sent)
{
    if (ps->pb == NULL) {
        return;
    }
    if (act == SCS_VC_ACT_ABANDON) {
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-W-VCABANDON, virtual-circuit formation ABANDONED with peer"
                " %02x:%02x:%02x:%02x:%02x:%02x after %lu reissue(s) (p. 2-14)"
                " -- path block re-armed to CLOSED for a fresh dialogue\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                ps->pb->fsm.reissues);
        fflush(stderr);
        scs_vc_fsm_init(ps->pb);
        ps->start_replied = 0;
        return;
    }
    if (ps->pb->vc_state == SCS_VC_OPEN && !ps->start_acked &&
        scsd_vc_ack_due(ps, peer_round2_seen)) {
        if (scsd_vc_emit(ctx, ps, SCS_VC_ACT_SEND_ACK)) {
            (*start_ack_sent)++;
            ps->start_acked = 1;
            scsd_vc_on_open(ctx, ps);
        }
    }
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
        send_frame_vc(sock, ifindex, ps, ps->pb,
                      "add-member op 0x14 (node model)", frame, sizeof(frame)) > 0) {
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
        send_frame_vc(sock, ifindex, ps, ps->pb,
                      "add-member op 0x01 (cluster parameters)", frame, sizeof(frame)) > 0) {
        sent++;
    }

    /* op 0x02 config/topology. (vms-d94: holding 0x02 to a later step was tested
     * and did NOT change NEW->MEMBER, so the initial burst sends all three.) */
    mp.recv_ack = ps->vc.seq.recv_seq;
    mp.send_seq = scs_seq_advance(&ps->vc.seq);
    mp.sysap_send_msg = ps->sysap_send++;
    mp.sysap_ack_msg = ps->sysap_recv;
    if (scs_member_build_config(&mp, frame) == 0 &&
        send_frame_vc(sock, ifindex, ps, ps->pb,
                      "add-member op 0x02 (config/topology)", frame, sizeof(frame)) > 0) {
        sent++;
    }

    ps->cm_config_sent = 1;
    return sent;
}

/* =====================================================================
 * vms-fdd -- SCA CONNECT DATA (VAXcluster Principles p. 2-25, p. 2-28)
 * =====================================================================
 *
 * p. 2-25: the initiating SYSAP supplies up to 16 bytes of connect data in
 * CONNECT_REQ and the target up to 16 in ACCEPT_REQ, and the two connection
 * managers "use this data to effectively identify to each other which version
 * of VMS each is associated with" -- either end may refuse on it. p. 2-28 puts
 * the field in the CDT. The wire location, the census that grounds the value
 * and the honest gap are the CONNECT DATA verdict in scs_connect.h.
 *
 * TWO SIDES, AND ONLY ONE OF THEM IS OURS:
 *   - OUTBOUND: scs_connect_build_request()/_response() stamp the bytes; the
 *     two helpers below record the same bytes on the CDT the CONNECT / ACCEPT
 *     service just allocated, which is what p. 2-28 asks for.
 *   - INBOUND: OVMX DECODES AND LOGS the peer's value on every CONNECT_REQ and
 *     ACCEPT_REQ it receives, and does NOT act on it. OVMX has no version
 *     policy: it never rejects a peer on connect data, because we have exactly
 *     one VMS version on the lab wire (V7.3) and no observation of what a
 *     refusal looks like. Logging it is the honest half; a version policy
 *     built on one data point would be an invented one.
 */
static void scsd_cdt_record_connect_data(struct scs_cdt *cdt)
{
    if (cdt == NULL || !scs_connect_data_enabled()) {
        return;
    }
    (void)scs_cdt_set_connect_data(cdt, scs_connect_data_vaxcluster,
                                   SCS_CONNECT_DATA_LEN);
}

/* Decode + log the peer's connect data. Returns 1 if the frame carried a
 * decodable field (i.e. it was a CONNECT_REQ or ACCEPT_REQ), 0 otherwise. */
static int scsd_log_peer_connect_data(const uint8_t *buf, size_t n)
{
    uint8_t cd[SCS_CONNECT_DATA_LEN];
    if (scs_connect_data_get(buf, n, cd) != 0) {
        return 0;
    }
    /* [46:48] = abs 60:62; scs_connect_data_get() already required it to be
     * one of these two, and the frame to be long enough to hold it. */
    uint16_t cmsg = (uint16_t)(buf[60] | ((uint16_t)buf[61] << 8));
    /* [62:78] = abs 76:92, the sender's local SYSAP name (spec sec 4h(2)).
     * Measured ASCII in 1425/1425 VAX-sourced connect frames (and in all 466
     * OVMX-sourced ones), but this is peer-supplied
     * and goes straight into a log line, so sanitize rather than trust it. */
    char sysap[17];
    for (int i = 0; i < 16; i++) {
        uint8_t c = buf[76 + i];
        sysap[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    sysap[16] = '\0';
    char rendered[80];
    log_ts(stdout);
    printf(" SCSD-I-CONNDATA, peer %s connect data (p. 2-25) from"
           " %s: %s%s\n",
           cmsg == SCS_CONN_MSGTYPE_ACCEPT_REQ ? "ACCEPT_REQ" : "CONNECT_REQ",
           sysap,
           scs_connect_data_fmt(cd, rendered, sizeof(rendered)),
           memcmp(cd, scs_connect_data_vaxcluster, SCS_CONNECT_DATA_LEN) == 0
               ? " (same as ours)" : "");
    fflush(stdout);
    return 1;
}

/*
 * ===== vms-7fe: THE SDIR REFUSAL SENDER =====
 *
 * p. 2-48: "If none is found, SCS replies with a CONNECT_RSP containing the
 * 'no such SYSAP' error." p. 2-50: a second concurrent request gets "a response
 * that essentially says 'busy ... try again later'".
 *
 * ONLY THE FIRST OF THOSE TWO IS REACHABLE FROM THIS FILE, and the distinction
 * is load-bearing enough to state before anything else. The p. 2-50 busy reply
 * needs a listening CDT still in CONNECT RECEIVED when a DIFFERENT requester's
 * frame arrives; scsd_handle_frame() answers synchronously and returns the SDIR
 * to LISTEN before it returns (scs_sdir.h OVMX DESIGN CHOICE 3), so the receive
 * loop never holds that state between frames. MEASURED, not predicted:
 * tests/vmsscs/test_scsd_wire.c drives scsd_handle_frame() over every captured
 * and synthesized frame it holds, sums `sdir_busy_replies` across every case,
 * and asserts the total is 0; the exit summary prints busy-sent so a live lab
 * run stays falsifiable too. The BUSY
 * branch below is written because scs_sdir_connect_req() can return
 * SCS_SDIR_BUSY through its API, not because scsd.c has ever taken it; the only
 * caller that takes it is tests/vmsscs/test_scs_sdir.c, at module level. NO
 * TEST IN test_scsd_wire.c ASSERTS A BUSY FRAME, because there is none to
 * assert.
 *
 * WHY THIS IS NOT ONE OF THE THREE SERVICE EMITTERS, and why it is allowed to
 * call a connection-control builder (tests/vmsscs/test_scsd_send_sites.py check
 * 7 lists it beside them): the census exists to stop a hand-built frame putting
 * A CONNECTION on the wire that no CDT describes. This frame does the opposite
 * -- it DECLINES a connection. There is no CDT because p. 2-48's scan failed
 * before any SYSAP saw the request, so there is nothing for scs_accept() to
 * allocate and no Figure 2-14 transition to take; the situation is p. 2-56's
 * REJECT case ("does not involve a CDT at all, but merely the sending of a
 * message ... implemented entirely in the port driver") carrying a CONNECT_RSP
 * rather than a REJECT_REQ, because p. 2-48 says CONNECT_RSP. Routing it
 * through scs_reject() would have made the service ask for SEND_REJECT_REQ and
 * this function build a CONNECT_RSP, i.e. an emitter lying about its own frame.
 *
 * THE FRAME. The 66-byte CONNECT_RSP class, built by the SAME builder that
 * emits OVMX's positive SCS$DIRECTORY CONNECT-ECHO -- remote Con.ID echoed,
 * local Con.ID 0, [46:48] = 1. All of that is GROUNDED (spec sec 4(h)(1a), 16
 * frames / 16 dialogues; sec 4(h)(1) for the handle fill pattern; the 66-byte
 * class is observed under BOTH the 0x5b and 0x4b opcodes, 31 of 31 frames,
 * which is why `opcode` is a parameter rather than the template's baked 0x5b).
 *
 * THE STATUS WORD AT [48:50] IS NOT GROUNDED. It is an OVMX choice in both
 * placement and value -- see the WIRE STATUS block in scs_sdir.h, which also
 * records the measurement that in the configuration OVMX runs this function is
 * never called, because both CONNECT_REQs the reference VAX addresses to OVMX
 * name SYSAPs OVMX LISTENs for.
 */
static int scsd_send_sdir_refusal(int sock, int ifindex, struct peer_state *ps,
                                  const uint8_t *our_hw_mac,
                                  const uint8_t *our_src_logical,
                                  uint8_t opcode, uint32_t requester_conid,
                                  uint16_t status, const char *why)
{
    struct scs_dir_params rp;
    memset(&rp, 0, sizeof(rp));
    memcpy(rp.dst_mac, ps_port_addr(ps), 6);
    memcpy(rp.src_mac, our_hw_mac, 6);
    memcpy(rp.src_logical, our_src_logical, 6);
    memcpy(rp.peer_logical, ps_sys_addr(ps), 6);
    rp.remote_conid = requester_conid; /* echoed; local stays 0 -- CONNECT_RSP */
    rp.recv_ack = ps->vc.seq.recv_seq;
    rp.send_seq = scs_seq_advance(&ps->vc.seq);
    rp.incarnation = ps->incarnation;

    uint8_t frame[SCS_DIR_ECHO_FRAME_LEN];
    if (scs_dir_build_connect_echo(&rp, frame) != 0) {
        return 0;
    }
    /* Echo the request's opcode (0x5b directory / 0x4b sequenced-application);
     * the 66-byte CONNECT_RSP class is observed under both. */
    frame[14 + 16] = opcode;
    /* THE OVMX-CHOSEN STATUS WORD. [48:50], little endian. */
    frame[14 + 48] = (uint8_t)(status & 0xffu);
    frame[14 + 49] = (uint8_t)((status >> 8) & 0xffu);

    if (send_frame_vc(sock, ifindex, ps, ps->pb, why, frame, sizeof(frame)) <= 0) {
        sdir_refusals_unsent++;
        return 0;
    }
    if (status == SCS_SDIR_STATUS_BUSY) {
        /* NOT REACHED IN ANY RUN OF THIS DAEMON -- see the function header.
         * Counted rather than asserted away: test_scsd_wire.c reds if this
         * line ever executes, and the exit summary prints busy-sent so a live
         * run reports it too. */
        sdir_busy_replies++;
    } else {
        sdir_no_such_sysap++;
    }
    log_ts(stdout);
    printf(" SCSD-I-SDIRREFUSE, %s -> CONNECT_RSP status=0x%04X (OVMX-chosen,"
           " not grounded) opcode=0x%02x requester=0x%08X\n",
           why, (unsigned)status, opcode, requester_conid);
    fflush(stdout);
    return 1;
}

/*
 * vms-7fe: THE p. 2-48 SCAN, as the receive path runs it.
 *
 * One helper for both inbound-CONNECT_REQ paths (the 0x5b SCS$DIRECTORY connect
 * and the 0x4b VMS$VAXcluster connect), so the rule is written once. Returns
 * non-zero when the caller may proceed to ACCEPT -- i.e. the target SYSAP is
 * listening and the request was delivered to its listening CDT. Returns 0 when
 * the request was refused, having already put the refusal on the wire.
 *
 * `*sdir_out` receives the matched SDIR so the caller can return the listening
 * CDT to LISTEN with scs_sdir_connect_answered() once it has answered.
 *
 * UNDER OVMX_NO_SDIR THIS IS SKIPPED ENTIRELY by its callers -- the scan does
 * not run, no refusal is built, and the accept proceeds exactly as it did
 * before this item.
 */
/*
 * vms-7fe: IS THE p. 2-48 SCAN LIVE RIGHT NOW?
 *
 * Two conditions, and the second is not cosmetic. THE QUEUE MUST BE POPULATED.
 * scsd_cdl_ready is only set when the connection state machine is enabled, so
 * under the PRE-EXISTING kill switch OVMX_NO_CONN_FSM=1 the port never binds a
 * CDL, LISTEN never runs, and the SDIR queue is EMPTY. An empty queue read
 * literally says "no SYSAP on this node is listening", which would make the
 * scan refuse EVERY inbound connect request with p. 2-48's "no such SYSAP" --
 * turning a switch that is documented to change no byte into one that stops
 * OVMX joining at all. An empty queue means LISTEN never ran, not that nobody
 * is listening, and the honest response to "I do not know" is to behave exactly
 * as the pre-vms-7fe daemon did rather than to invent a refusal.
 *
 * Re-read on every call (never cached), like every other switch here.
 */
static int scsd_sdir_live(void)
{
    return scs_sdir_enabled() && scs_sdir_count(scs_svc_sdir(scsd_svc())) > 0;
}

static int scsd_sdir_admit(int sock, int ifindex, const uint8_t *our_hw_mac,
                           const uint8_t *our_src_logical, struct peer_state *ps,
                           const uint8_t *buf, size_t n, uint8_t opcode,
                           uint32_t requester_conid, const char *dflt_target,
                           const struct scs_sdir **sdir_out)
{
    if (sdir_out != NULL) {
        *sdir_out = NULL;
    }
    char target[SCS_CDT_SYSAP_NAME_LEN + 1];
    if (scs_sdir_target_name(buf, n, target) != 0 || target[0] == '\0') {
        /* Too short to carry the grounded [62:78] name field. Fall back to the
         * name the enclosing branch already classified the frame as, rather
         * than refusing a request whose target we simply could not read. */
        strncpy(target, dflt_target, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    }

    sdir_connect_scans++;
    enum scs_sdir_result r = scs_sdir_connect_req(scs_svc_sdir_mut(scsd_svc()),
                                                  target, NULL, requester_conid,
                                                  sdir_out);
    if (r == SCS_SDIR_DELIVERED) {
        return 1;
    }
    if (r == SCS_SDIR_BADARG) {
        return 0;
    }
    /* The SCS_SDIR_BUSY arm is unreachable from this daemon (scs_sdir.h DESIGN
     * CHOICE 3): a synchronous answer cannot leave a listening CDT in CONNECT
     * RECEIVED for a second requester to collide with, so every refusal scsd.c
     * has ever sent is the no-such-SYSAP one. The arm exists because the module
     * API can return BUSY, and it is written here rather than asserted away so
     * that a future asynchronous ACCEPT does not silently send status 0. */
    uint16_t status = (r == SCS_SDIR_BUSY) ? SCS_SDIR_STATUS_BUSY
                                           : SCS_SDIR_STATUS_NO_SUCH_SYSAP;
    char why[64];
    snprintf(why, sizeof(why), "CONNECT_RSP %s for '%s'",
             scs_sdir_result_name(r), target);
    (void)scsd_send_sdir_refusal(sock, ifindex, ps, our_hw_mac, our_src_logical,
                                 opcode, requester_conid, status, why);
    return 0;
}

/*
 * ===== vms-561: THE THREE EMITTERS =====
 *
 * p. 2-56's port-driver half of the five services. Each is called BY
 * scs_connect() / scs_accept() (src/vmsscs/scs_svc.c), once per transition
 * whose action names a packet, BEFORE the transition is committed -- which is
 * the order the pre-migration code used (send the frame, then step the machine)
 * and is why this refactor changes no byte on the wire.
 *
 * Each returns one of SCS_SVC_EMIT_SENT / _NOBUILDER / _REFUSED; see the emit
 * contract in scs_svc.h for what the service does with each. Every send goes
 * through send_frame_vc(), the p. 2-31 choke point, so these three are CHOKED
 * entries in the SEND SITE TABLE and no exemption is involved.
 *
 * They hold no state: everything they need is prepared by their caller and
 * handed over in a struct scsd_svc_emit_ctx, and everything they report goes
 * back the same way. That is deliberate -- it keeps the sequence-number
 * allocation, the peer bookkeeping and the run counters in the caller, where
 * they were before, rather than scattering them into callbacks.
 */
struct scsd_svc_emit_ctx {
    int sock;
    int ifindex;
    struct peer_state *ps;
    struct scs_pb *vc;                            /* the circuit the frame rides */
    const struct scs_connect_params *connect_params; /* 0x4b request/response */
    struct scs_dir_params *dir_params;            /* SCS$DIRECTORY echo + response */

    /* Reported back to the caller. */
    int sent;      /* the CONNECT_REQ (CONNECT) or ACCEPT_REQ (ACCEPT) went out */
    int echo_sent; /* SCS$DIRECTORY only: the op=1 CONNECT-ECHO went out */
};

/* CONNECT: the 0x4b VMS$VAXcluster CONNECT-REQUEST (spec sec 4g phase 4). */
static int scsd_svc_emit_connect_req(void *ctx, struct scs_cdt *cdt,
                                     enum scs_conn_action act,
                                     const struct scs_svc_args *args,
                                     const char **what)
{
    struct scsd_svc_emit_ctx *e = (struct scsd_svc_emit_ctx *)ctx;
    (void)args;
    if (act != SCS_CONN_ACT_SEND_CONNECT_REQ) {
        return scsd_svc_no_builder(cdt, act);
    }
    uint8_t cframe[SCS_CONNECT_FRAME_LEN];
    if (scs_connect_build_request(e->connect_params, cframe) != 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    *what = "VMS$VAXcluster CONNECT-REQUEST";
    if (send_frame_vc(e->sock, e->ifindex, e->ps, e->vc, *what,
                      cframe, sizeof(cframe)) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    e->sent = 1;
    return SCS_SVC_EMIT_SENT;
}

/*
 * ACCEPT, SCS$DIRECTORY. Two frames for the two transitions of Figure 2-14's
 * NODE_2 column, and spec sec 4(h)(1) grounds the pairing:
 *   CONNECT_RSP = op=1 CONNECT-ECHO   (SCA23, "VAX2 echoes VAX1's handle, its
 *                                      own not yet assigned")
 *   ACCEPT_REQ  = op=2 CONNECT-RESPONSE (SCA25, "VAX2 supplies its own handle
 *                                      -- pair now bound")
 * Each is a sequenced message, so each advances OVMX's send_seq (spec sec
 * 4h(4)) at the moment it is built -- exactly as the pre-migration code did,
 * and in the same order.
 */
static int scsd_svc_emit_dir_accept(void *ctx, struct scs_cdt *cdt,
                                    enum scs_conn_action act,
                                    const struct scs_svc_args *args,
                                    const char **what)
{
    struct scsd_svc_emit_ctx *e = (struct scsd_svc_emit_ctx *)ctx;
    (void)args;
    struct scs_dir_params *dp = e->dir_params;

    if (act == SCS_CONN_ACT_SEND_CONNECT_RSP) {
        dp->recv_ack = e->ps->vc.seq.recv_seq;
        dp->send_seq = scs_seq_advance(&e->ps->vc.seq);
        uint8_t eframe[SCS_DIR_ECHO_FRAME_LEN];
        if (scs_dir_build_connect_echo(dp, eframe) != 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        *what = "SCS$DIRECTORY op=1 CONNECT-ECHO";
        /* The pre-migration code took this transition on a successful BUILD,
         * not on a successful send, so a refused send here must not abandon the
         * ACCEPT that follows. Report NOBUILDER, which commits the transition
         * and counts the frame as not sent. */
        if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                          eframe, sizeof(eframe)) <= 0) {
            return SCS_SVC_EMIT_NOBUILDER;
        }
        e->echo_sent = 1;
        return SCS_SVC_EMIT_SENT;
    }

    if (act == SCS_CONN_ACT_SEND_ACCEPT_REQ) {
        dp->recv_ack = e->ps->vc.seq.recv_seq;
        dp->send_seq = scs_seq_advance(&e->ps->vc.seq);
        uint8_t rframe[SCS_DIR_RESP_FRAME_LEN];
        if (scs_dir_build_connect_response(dp, rframe) != 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        *what = "SCS$DIRECTORY op=2 CONNECT-RESPONSE";
        if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                          rframe, sizeof(rframe)) <= 0) {
            return SCS_SVC_EMIT_REFUSED;
        }
        e->sent = 1;
        return SCS_SVC_EMIT_SENT;
    }
    return scsd_svc_no_builder(cdt, act);
}

/*
 * ACCEPT, member-opened VMS$VAXcluster. ONE frame for TWO transitions, and the
 * asymmetry is the honest part: the machine requires a CONNECT_RSP first and
 * OVMX BUILDS NONE. That is an OVMX gap, not a wire gap -- the real VAX does
 * send that frame (the 66-byte message-type-1 class, 16 of 16 dialogues, spec
 * sec 4(h)(1a)) -- so the CONNECT_RSP arm reports NOBUILDER and is logged, and
 * only the ACCEPT_REQ (the 110-byte 0x4b CONNECT-RESPONSE carrying both
 * Con.IDs, golden frame 50, message type 2) goes on the wire.
 */
static int scsd_svc_emit_member_accept(void *ctx, struct scs_cdt *cdt,
                                       enum scs_conn_action act,
                                       const struct scs_svc_args *args,
                                       const char **what)
{
    struct scsd_svc_emit_ctx *e = (struct scsd_svc_emit_ctx *)ctx;
    (void)args;
    if (act != SCS_CONN_ACT_SEND_ACCEPT_REQ) {
        return scsd_svc_no_builder(cdt, act);
    }
    uint8_t rframe[SCS_CONNECT_FRAME_LEN];
    if (scs_connect_build_response(e->connect_params, rframe) != 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    *what = "VMS$VAXcluster 0x4b CONNECT-RESPONSE";
    if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                      rframe, sizeof(rframe)) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    e->sent = 1;
    return SCS_SVC_EMIT_SENT;
}

/*
 * ================= vms-591: THE DISCONNECT DIALOGUE (Figure 2-16) ==========
 *
 * The port-driver half of DISCONNECT: the emitter for the two actions the
 * connection state machine names on the teardown arrows, SEND_DISCONNECT_REQ
 * and SEND_DISCONNECT_RSP. Both frames are built by src/vmsscs/scs_disc.c from
 * byte-exact captured templates; read scs_disc.h for the grounding, including
 * why the DISCONNECT_RSP -- which the spec said was not on our wire -- is.
 *
 * WHICH CONNECTION. The frame is addressed from the CDT: local Con.ID is the
 * CDT's own handle, remote Con.ID is the peer's handle recorded on it. That
 * matters because a peer has up to three connections to OVMX at once and the
 * daemon must be able to tear down exactly the one being disconnected.
 *
 * EVERY SEND IS SEQUENCED. Both frames are SCS sequenced messages, so each
 * takes its own send_seq from the peer's live VC (scs_seq_advance) at the
 * moment it is built, exactly as the SCS$DIRECTORY accept pair does. A
 * DISCONNECT frame carrying a stale sequence number is the failure mode
 * vms-c6d already had to fix once on the 0x4b connect.
 *
 * CHOKED, per the SEND SITE TABLE: both sends go through send_frame_vc(), so
 * p. 2-31 applies -- OVMX will not disconnect over a circuit that is not OPEN.
 * That is the right answer and not a limitation: if the circuit is gone, the
 * connections it carried are already broken by scs_vc_break(), which drives
 * every one of them to CLOSED through SCS_CONN_EV_VC_LOST. There is nothing
 * left to disconnect and no circuit to disconnect it over.
 */
struct scsd_disc_emit_ctx {
    int sock;
    int ifindex;
    struct peer_state *ps;
    const uint8_t *our_hw_mac;
    const uint8_t *our_src_logical;
    uint16_t reason;   /* p. 2-26's optional reason code (vms-6b3) */
    int matching;      /* the [60:62] flag: is this REQ answering the peer's? */
    int req_sent;
    int rsp_sent;
};

static int scsd_svc_emit_disconnect(void *ctx, struct scs_cdt *cdt,
                                    enum scs_conn_action act,
                                    const struct scs_svc_args *args,
                                    const char **what)
{
    struct scsd_disc_emit_ctx *e = (struct scsd_disc_emit_ctx *)ctx;
    struct scs_disc_params dp;
    uint8_t frame[SCS_DISC_REQ_FRAME_LEN];
    size_t len;
    (void)args;

    if (act != SCS_CONN_ACT_SEND_DISCONNECT_REQ &&
        act != SCS_CONN_ACT_SEND_DISCONNECT_RSP) {
        return scsd_svc_no_builder(cdt, act);
    }
    /* No CDT means no Con.ID pair, and a DISCONNECT frame without one names no
     * connection. Descriptorless mode (scs_svc.h) therefore cannot disconnect;
     * say so rather than emitting a frame addressed to connection zero. */
    if (cdt == NULL) {
        return scsd_svc_no_builder(cdt, act);
    }

    memset(&dp, 0, sizeof(dp));
    memcpy(dp.dst_mac, ps_port_addr(e->ps), 6);
    memcpy(dp.src_mac, e->our_hw_mac, 6);
    memcpy(dp.src_logical, e->our_src_logical, 6);
    memcpy(dp.peer_logical, ps_sys_addr(e->ps), 6);
    dp.remote_conid = cdt->remote_conid;
    dp.local_conid = cdt->local_conid;
    dp.incarnation = e->ps->incarnation;
    dp.recv_ack = e->ps->vc.seq.recv_seq;
    dp.send_seq = scs_seq_advance(&e->ps->vc.seq);
    dp.reason = e->reason;
    dp.matching = e->matching;

    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        if (scs_disc_build_request(&dp, frame) != 0) {
            /* The ONLY way this fails with a non-NULL params is the kill
             * switch. NOBUILDER is then literally true -- with
             * OVMX_NO_CLEAN_SHUTDOWN=1 OVMX has no DISCONNECT_REQ builder,
             * which is the state it was in before this item. */
            return scsd_svc_no_builder(cdt, act);
        }
        len = SCS_DISC_REQ_FRAME_LEN;
        *what = "DISCONNECT_REQ";
    } else {
        if (scs_disc_build_response(&dp, frame) != 0) {
            return scsd_svc_no_builder(cdt, act);
        }
        len = SCS_DISC_RSP_FRAME_LEN;
        *what = "DISCONNECT_RSP";
    }

    if (send_frame_vc(e->sock, e->ifindex, e->ps, e->ps->pb, *what,
                      frame, len) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        e->req_sent++;
        disc_req_sent++;
    } else {
        e->rsp_sent++;
        disc_rsp_sent++;
    }
    return SCS_SVC_EMIT_SENT;
}

/*
 * scsd_disc_args - the service argument set for a teardown. Only the emitter,
 * the reason and (through the ctx) the matching flag matter here: DISCONNECT
 * allocates nothing, so none of the CONNECT/ACCEPT arguments apply.
 */
static struct scs_svc_args scsd_disc_args(struct scsd_disc_emit_ctx *e, uint16_t reason)
{
    struct scs_svc_args a;
    memset(&a, 0, sizeof(a));
    e->reason = reason;
    a.reason = reason;
    a.emit = scsd_svc_emit_disconnect;
    a.emit_ctx = e;
    return a;
}

/*
 * scsd_disconnect_dialogue - drive Figure 2-16 for ONE received DISCONNECT
 * message on ONE connection.
 *
 * p. 2-26, the rule this implements:
 *
 *   "the symmetry that SCA builds into the concept of a connection requires
 *    that the other SYSAP respond by also invoking the DISCONNECT service"
 *
 * so a received DISCONNECT_REQ is answered in TWO steps, not one:
 *
 *   1. DELIVER it. From OPEN that is Figure 2-16's OPEN --RCV_DISCONNECT_REQ-->
 *      DISC RECEIVED with action send DISCONNECT_RSP and the SYSAP-disconnected
 *      notification. From DISC SENT it is the p. 2-27 SIMULTANEOUS case,
 *      straight to DISC MATCH with the same answer. From DISC ACK it is the
 *      last arrow of the figure, to CLOSED.
 *
 *   2. If, and only if, that left the connection in DISC RECEIVED, the owning
 *      SYSAP now INVOKES ITS OWN DISCONNECT -- DISC RECEIVED --SVC_DISCONNECT-->
 *      DISC MATCH, sending the MATCHING DISCONNECT_REQ ([60:62] = 1). In the
 *      simultaneous case step 2 is skipped because our own DISCONNECT_REQ has
 *      already gone out; that is what "transitioning through only three states"
 *      (p. 2-27) means, and the state test is how this code knows which case it
 *      is in rather than guessing from timing.
 *
 * WHICH SYSAP INVOKES IT. OVMX has no asynchronous SYSAP to hand the
 * notification to and no disconnect callback on the CDT, so scsd.c invokes the
 * symmetric DISCONNECT itself, synchronously, in the receive path. That is the
 * SAME labeled difference scs_svc.h already records for ACCEPT ("an OVMX SYSAP
 * cannot currently take time to decide"), stated again here rather than left to
 * be inferred: the p. 2-26 SYSAP notification is LOGGED (the CONNFSM line's
 * notify=SYSAP-disconnected bit) and is not DELIVERED to any handler, because
 * there is no handler to deliver it to. The one handler a CDT does carry is the
 * VC-LOSS handler, and calling that for a disconnect would report a circuit
 * loss that did not happen -- scs_svc.h refuses it for exactly that reason.
 *
 * Returns 1 if the connection reached CLOSED and its CDT was released (after
 * which `cdt` must not be touched again).
 */
static int scsd_disconnect_dialogue(int sock, int ifindex, struct peer_state *ps,
                                    struct scs_cdt *cdt, enum scs_conn_event ev,
                                    const uint8_t our_hw_mac[6],
                                    const uint8_t our_src_logical[6])
{
    struct scsd_disc_emit_ctx e;
    struct scs_svc_port *port = scsd_svc();
    enum scs_conn_state before;
    int closed = 0;

    if (port == NULL || cdt == NULL || ps == NULL) {
        return 0;
    }
    memset(&e, 0, sizeof(e));
    e.sock = sock;
    e.ifindex = ifindex;
    e.ps = ps;
    e.our_hw_mac = our_hw_mac;
    e.our_src_logical = our_src_logical;

    before = scs_conn_state_of(cdt);
    if (ev == SCS_CONN_EV_RCV_DISCONNECT_REQ) {
        disc_req_recv++;
    } else {
        disc_rsp_recv++;
    }

    /* Step 1. Answering a peer's DISCONNECT_REQ is an answer, not an
     * initiation, so the DISCONNECT_RSP carries no matching flag at all (the
     * 58-byte class has no such field) and the reason code is the peer's
     * business, not ours. */
    {
        struct scs_svc_args a = scsd_disc_args(&e, SCS_REASON_NONE);
        struct scsd_svc_mark m = scsd_svc_mark();
        (void)scs_svc_deliver(port, cdt, ev, &a, &closed);
        scsd_svc_settle(m);
    }
    if (closed) {
        disc_closed++;
        log_ts(stdout);
        printf(" SCSD-I-DISCCLOSED, connection closed and CDT released after"
               " the %s teardown\n",
               before == SCS_CONN_DISC_MATCH ? "matching" : "peer-initiated");
        fflush(stdout);
        return 1;
    }

    /* p. 2-27: BOTH sides had already sent a DISCONNECT_REQ. We answered it
     * above and are now in DISC MATCH with nothing further to invoke. */
    if (before == SCS_CONN_DISC_SENT && ev == SCS_CONN_EV_RCV_DISCONNECT_REQ &&
        scs_conn_state_of(cdt) == SCS_CONN_DISC_MATCH) {
        disc_simultaneous++;
        log_ts(stdout);
        printf(" SCSD-I-DISCSIMUL, conid=0x%08X: simultaneous disconnect"
               " (p. 2-27) -- both ends had sent DISCONNECT_REQ; answered and"
               " went straight to DISC MATCH\n", (unsigned)cdt->local_conid);
        fflush(stdout);
        return 0;
    }

    /* Step 2. THE SYMMETRIC HALF -- the one OVMX never performed. */
    if (scs_conn_state_of(cdt) == SCS_CONN_DISC_RECEIVED) {
        struct scs_svc_args a = scsd_disc_args(&e, SCS_REASON_PEER_DISCONNECT);
        struct scsd_svc_mark m = scsd_svc_mark();
        e.matching = 1;
        (void)scs_disconnect(port, cdt, &a);
        scsd_svc_settle(m);
        log_ts(stdout);
        printf(" SCSD-I-DISCMATCH, conid=0x%08X: SYSAP invoked its own"
               " DISCONNECT (p. 2-26) -- matching DISCONNECT_REQ %s\n",
               (unsigned)cdt->local_conid,
               e.req_sent ? "sent" : "NOT sent (no builder or refused)");
        fflush(stdout);
    }
    return 0;
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
 *
 * vms-398 -- WHICH VIRTUAL CIRCUIT (VAXcluster Principles p. 2-47). CONNECT
 * takes either a named circuit or none: "CONNECT ... uses CONFIG_SYS to obtain
 * the address of the first Path Block queued to the System Block for the
 * specified node ... [and] examines each Path Block in turn until it finds one
 * whose virtual circuit is OPEN".
 *   named_vc != NULL - the caller named the circuit; it is used as given.
 *   named_vc == NULL - the caller named only the NODE. The circuit is selected
 *                      from the configuration database by the peer's 48-bit SCS
 *                      System Address, via scs_config_select_vc() (CONFIG_SYS +
 *                      the OPEN scan). This is what BOTH of the daemon's own
 *                      call sites pass.
 * If no OPEN circuit exists for the node, NOTHING IS SENT and the refusal is
 * logged: there is no honest frame to build without a circuit (a fabricated one
 * would carry a zero peer-logical), and a silent success is exactly the failure
 * mode CLAUDE.md rule 9 / INV-6 forbids.
 *
 * The circuit's characteristics -- the remote port address the frame is
 * addressed to and the System Block whose System Address goes in the SCA
 * peer-logical field -- are read back through CONFIG_PATH (p. 2-47), not by
 * reaching into the Path Block, so the daemon sees the same view of a path that
 * any other CONFIG_PATH caller would.
 */
static int send_joiner_connect_request(int sock, int ifindex, struct scs_config *cfg,
                                       struct peer_state *ps, struct scs_pb *named_vc,
                                       const uint8_t our_hw_mac[6],
                                       const uint8_t our_src_logical[6])
{
    struct scs_pb *vc = named_vc;
    if (vc == NULL) {
        vc = scs_config_select_vc(cfg, ps_sys_addr(ps));
    }
    struct scs_config_path_info path;
    /* vms-abc: the refusal is the CHOKE POINT's, not a bespoke one. It used to
     * be an inline fprintf here, which is where the ~1 Hz SCSD-E-NOVC flood
     * after a break came from: the daemon re-enters this function once per
     * directed HELLO and every entry printed. Routing it through
     * scsd_refuse_without_open_vc() gives it the same per-circuit latch and the
     * same vc_sends_refused accounting as every other refused send; the p. 2-47
     * specifics that used to be in the format string are carried in `what`.
     * NOTE the condition is now STRICTER: the old test was "CONFIG_PATH
     * describes it", which a CLOSED PB satisfies -- only scs_config_select_vc()
     * checked OPEN, and only on the named_vc == NULL path. A caller that NAMED
     * a CLOSED circuit got a frame. */
    if (scsd_refuse_without_open_vc(
            ps, vc,
            "VMS$VAXcluster CONNECT-REQUEST (p. 2-47: CONNECT needs an open"
            " circuit; none was named and CONFIG_SYS found none for this node)",
            &path)) {
        return 0;
    }

    struct scs_connect_params cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.dst_mac, path.remote_port_addr, 6);
    memcpy(cp.src_mac, our_hw_mac, 6);
    memcpy(cp.src_logical, our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
    memcpy(cp.peer_logical, path.sb != NULL ? path.sb->system_id : zero_addr, 6);
    cp.local_conid = OVMX_JOINER_CONID;
    cp.remote_conid = 0; /* CONNECT-REQUEST: the member's Con.ID is not yet known */
    cp.recv_ack = ps->vc.seq.recv_seq; /* always ack the member's latest send_seq */
    /* vms-abc: FIRST send or RETRANSMIT? The sequence number is allocated once
     * and reused, so "joiner_req_seq is still 0" IS the discriminator, and it
     * has to be read BEFORE the allocation below. */
    int first_send = (ps->joiner_req_seq == 0);
    if (first_send) {
        ps->joiner_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    cp.send_seq = ps->joiner_req_seq; /* retransmits reuse the same seq */
    cp.incarnation = ps->incarnation;

    /* vms-561: THE CONNECT SERVICE (pp. 2-22..2-23, 2-47, 2-56). The frame is
     * still built and sent by this file -- scsd_svc_emit_connect_req() below is
     * handed the params prepared above -- but the CDT, the p. 2-28/2-29 SYSAP
     * entry points and the Figure 2-14 transition are the service's, not
     * scsd.c's. The circuit is passed as args.vc because THIS function has
     * already run p. 2-47's selection (above) in order to address the frame;
     * scs_connect() would otherwise repeat it. */
    struct scsd_svc_emit_ctx ec;
    memset(&ec, 0, sizeof(ec));
    ec.sock = sock;
    ec.ifindex = ifindex;
    ec.ps = ps;
    ec.vc = vc;
    ec.connect_params = &cp;

    struct scs_svc_args a;
    memset(&a, 0, sizeof(a));
    a.local_sysap = "VMS$VAXcluster";
    a.remote_sysap = "VMS$VAXcluster";
    a.target_node = ps_sys_addr(ps);
    a.cfg = cfg;
    a.vc = vc;
    a.vc_loss = scsd_sysap_vc_loss;
    a.sysap_ctx = ps;
    a.conid = OVMX_JOINER_CONID;
    a.emit = scsd_svc_emit_connect_req;
    a.emit_ctx = &ec;

    struct scsd_svc_mark mark = scsd_svc_mark();
    struct scs_cdt *cdt = NULL;
    enum scs_svc_status st = scs_connect(scsd_svc(), &a, &cdt);
    scsd_svc_settle(mark);
    if (st == SCS_SVC_NOCDT) {
        scsd_svc_slot_refused(OVMX_JOINER_CONID, "VMS$VAXcluster");
    }
    if (ec.sent) {
        if (cdt != NULL) {
            ps->cdt_joiner = cdt;
            /* vms-fdd: p. 2-28 puts the SYSAP's connect data in the CDT, and
             * this is the CONNECT service's copy -- the same 16 bytes
             * scs_connect_build_request() just stamped at [94:110]. Carried,
             * not interpreted; the owning SYSAP reads it back off the CDT. */
            scsd_cdt_record_connect_data(cdt);
        }
        ps->joiner_connect_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->last_joiner_req);
        /* vms-abc -- THE p. 2-31 DELIVERY GUARANTEE'S ONLY LIVE FEED.
         *
         * This function is the ONLY caller of scs_vc_record_sent() in OVMX
         * (`grep -rn scs_vc_record_sent src/vmsscs/` -> its definition in
         * scs_vc.c, this comment, and the one call below), so the joiner
         * CONNECT-REQUEST is the only outstanding sequenced message the
         * delivery detector can ever see.
         *
         * It used to call scs_vc_record_sent() on EVERY send. That contradicts
         * this function's own contract two paragraphs up -- "retransmits REUSE
         * it (a retransmit is not a new message)" -- because record_sent()
         * RESETS vc.retransmit_count to 0. The counter therefore never left 0
         * in a real run no matter how many times the member ignored us, and
         * scs_vc_delivery_failed() could not become true in production. Fixing
         * that is what gives the delivery break a live path.
         *
         * NOT A WIRE CHANGE: both calls only touch OVMX's local VC bookkeeping.
         * The frame is byte-identical either way (same builder, same reused
         * cp.send_seq), and the retry CAP is unchanged. What changes is that
         * exhausting the retries is now DETECTABLE. */
        if (first_send) {
            scs_vc_record_sent(&ps->vc, cp.send_seq, monotonic_ms());
        } else {
            scs_vc_mark_retransmitted(&ps->vc, monotonic_ms());
        }
        return 1;
    }
    return 0;
}

/*
 * ===== vms-fb1: THE RECEIVE-DISPATCH SEAM =====
 *
 * Everything the per-frame handler touches, gathered into one object so the
 * handler can be a FUNCTION instead of nine hundred lines inline in main().
 * main()'s loop is now recv() + scsd_handle_frame(), and the exit report is
 * scsd_exit_summary().
 *
 * WHY THIS SHAPE. SCSD_UNIT_TEST renames main() away, so until now every branch
 * below the recv() was compiled but reachable from no test. That is a MEASURED
 * gap, not a hypothetical one: the vms-dd5 adversary pass applied three mutants
 * inside this loop (retargeting the SCS$DIRECTORY and VMS$VAXcluster conn_step
 * calls, and deleting the exit-summary scs_conn_report_stuck call) and all three
 * SURVIVED the full suite. vms-4071 hit the same wall from the other side. With
 * the dispatch behind a function, a test supplies its own struct scsd_rx and a
 * captured frame and drives the SAME production code the daemon runs.
 *
 * NOTHING ABOUT THE WIRE CHANGES HERE: the bodies below are the loop bodies,
 * moved, with `continue` becoming `return` (the loop body contained no inner
 * loop, so every continue was a loop-level one) and loop-local state reached
 * through `rx->`.
 */
struct scsd_rx {
    int sock;
    int ifindex;
    const uint8_t *our_hw_mac;
    const uint8_t *our_src_logical;
    const uint8_t *lab_nonce;
    struct scs_hello_params *hello_params;
    struct scs_config *cfg;
    struct scs_pdt *pdt;
    struct peer_state *peers;
    struct scsd_vc_ctx *vc_ctx;
    const char *ifname;
    int respond;
    int do_connect;
    int emit_hello;

    /* Run counters, reported by scsd_exit_summary(). */
    long counts[5];
    long total_frames;
    long hello_sent;
    long directed_sent;
    long connect_req_sent;
    long connect_resp_sent;
    long start_sent;
    long start_ack_sent;
    long credit_sent;        /* vms-691: total 0x48 credit-returns sent */
    long retransmit_sent;    /* vms-691: connect-request retransmits sent */
    long dir_conn_resp_sent; /* vms-246: SCS$DIRECTORY CONNECT-RESPONSEs sent */
    long dir_lookup_sent;    /* vms-246: SCS$DIR_LOOKUP responses sent */
    long poll_connect_sent;  /* vms-66f: SCS$DIR_LOOKUP -> SCS$DIRECTORY CONNECT_REQs */
    long poll_inquiry_sent;  /* vms-66f: p. 2-50 inquiries put on the wire */
    long poll_answers;       /* vms-66f: lookup RESPONSEs fed back to the poller */
    long poll_found;         /* vms-66f: affirmative discoveries notified */
    long cm_config_frames;   /* vms-224: op 0x14/0x01/0x02 CM config frames sent */
    long cm_response_sent;   /* vms-224: 0x81 responses to member 0x03/0x05 txns */
    long padded_sent;        /* vms-9f3: padded directed HELLOs sent (sec 4k) */

    /* vms-9f3: reusable padded-HELLO send buffer (up to 1514B on the wire),
     * held here to keep it off the handler's stack frame. */
    uint8_t pframe[SCS_HELLO_PADDED_MAX_FRAME];
};

/*
 * scsd_hello_beacon_emit - build and transmit ONE periodic multicast HELLO, the
 * beacon a node announces itself with. Returns what send_frame_channel()
 * returned, or -1 if the frame could not be built or could not be sent.
 *
 * WHY IT IS A FUNCTION. It was six lines inside main()'s timer loop, and the
 * SCSD_UNIT_TEST seam RENAMES main() AWAY -- so this send, alone among the
 * daemon's senders, was reachable from no test at all. That is also how it came
 * to call sendto() on the socket directly and sit in neither half of the SEND
 * SITE TABLE: nothing could look at it. Hoisted here it is an ordinary static
 * helper, driven by tests/vmsscs/test_scsd_wire.c
 * (test_the_hello_beacon_transmits_through_the_channel_exemption) against the
 * capture buffer, exactly as every other sender in this file is.
 *
 * THE CADENCE STAYS IN THE LOOP. This function does not consult the clock and
 * has no idea how often it is called; main() still owns `last_hello` and the
 * --hello-interval comparison. Only the build-and-send moved, so the wire sees
 * the same frames at the same times.
 *
 * It is a HELLO, so it takes the send_frame_channel() exemption: it is
 * addressed to the cluster MULTICAST group rather than to any port address, and
 * it is transmitted when no peer, no System Block, no Path Block and therefore
 * no virtual circuit is known at all -- it is how peers are discovered in the
 * first place. See the EXEMPT half of the SEND SITE TABLE.
 */
static ssize_t scsd_hello_beacon_emit(struct scsd_rx *rx, struct scs_hello_params *hp,
                                      int sock, int ifindex)
{
    uint8_t frame[SCS_HELLO_FRAME_LEN];
    hp->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
    if (scs_hello_build_frame(hp, frame) != 0) {
        return -1;
    }
    ssize_t sent = send_frame_channel(sock, ifindex, hp->dst_mac, frame, sizeof(frame));
    if (sent < 0) {
        fprintf(stderr, "SCSD-E-SENDFAIL, HELLO beacon transmit failed: %s\n",
                strerror(errno));
        return -1;
    }
    rx->hello_sent++;
    log_ts(stdout);
    printf(" SCSD-I-HELLOSENT, node='%s' seq=%ld bytes=%zd\n",
           hp->node_name, rx->hello_sent, sent);
    fflush(stdout);
    return sent;
}

/*
 * scsd_joiner_retransmit_pending - vms-abc: EXACTLY the condition under which
 * the daemon retransmits its own outstanding joiner CONNECT-REQUEST, extracted
 * from the branch that used to spell it inline (the directed-HELLO handler,
 * SCSD-I-CONNREQ "retransmit").
 *
 * WHY IT IS A FUNCTION AND NOT AN `if`. This predicate is the whole
 * reachability argument for the p. 2-31 DELIVERY break: retransmits are what
 * advance vc.retransmit_count, and nothing else in OVMX advances it. A test
 * that drives the retransmit therefore has to be able to assert the SAME
 * predicate the dispatch branches on -- otherwise the test's claim "the daemon
 * would have taken this path" is an unchecked assertion in a comment, which is
 * how the previous revision of this file ended up testing `connect_sent`, a
 * field production never writes. With the predicate shared, narrowing the guard
 * reds the test.
 *
 * `pb != NULL` is not in the original inline guard because the enclosing branch
 * already established it; it is spelled out here so the predicate is safe to
 * call on any peer slot.
 *
 * THE OPEN-CIRCUIT TERM IS NOT COSMETIC -- it fixes a live symptom. Every other
 * term stays TRUE FOREVER after a delivery break: the break leaves the Path
 * Block in place (scs_vc.h note 4 -- breaking a circuit does not deallocate
 * it), start_acked and joiner_connect_sent are latched, and joiner_connected is
 * CLEARED by scsd_sysap_vc_loss(), which this predicate reads NEGATED. So the
 * daemon kept re-entering this branch once per directed HELLO, roughly 1 Hz,
 * for the rest of the run -- each time calling send_joiner_connect_request(),
 * which found no OPEN circuit and logged SCSD-E-NOVC. Honest (it never faked a
 * send) but endless, and the retransmit it was attempting is exactly what
 * p. 2-31 forbids on a broken circuit. Asking the Path Block up front stops the
 * attempt at its source rather than rate-limiting its complaint. The circuit is
 * read through CONFIG_PATH (p. 2-47) for the same reason send_frame_vc() does.
 *
 * IT IS NOT THE ENFORCEMENT, though, and must not be read as such: this
 * predicate stops one ATTEMPT early. Whether a frame may leave is decided in
 * send_frame_vc() for every sender in the file. If this term were deleted the
 * frame would still not go out -- only the log would get noisy again. That is
 * the difference between an optimisation and a guard, and the reason the fix
 * for the flood is here while the fix for the emission is there.
 */
static int scsd_joiner_retransmit_pending(const struct scsd_rx *rx,
                                          const struct peer_state *ps)
{
    if (!(rx != NULL && rx->do_connect && ps != NULL && ps->pb != NULL &&
          ps->start_acked && ps->joiner_connect_sent && !ps->joiner_connected)) {
        return 0;
    }
    struct scs_config_path_info path;
    return scs_config_path(ps->pb, &path) && path.vc_state == SCS_VC_OPEN;
}

/*
 * ===========================================================================
 * vms-66f: THE SCS PROCESS POLLER (SCS$DIR_LOOKUP), ON THE WIRE
 * ===========================================================================
 *
 * The poller itself is src/vmsscs/scs_poll.c (p. 2-50: the cadence, the
 * one-node-at-a-time rule, the notify-on-Yes-only rule, the per-(SYSAP,node)
 * disable). What lives HERE is only the port-driver half: turning the two
 * actions it needs into the two frames scs_dir.c builds, finding the peer the
 * frames ride to, and feeding received answers back in.
 *
 * WHAT THIS DOES *NOT* DO, AND WHY -- READ BEFORE "FINISHING THE JOB".
 * The item that produced this file was written expecting the poller to REPLACE
 * a speculative client walk: OVMX opening a VMS$VAXcluster connection without
 * first asking whether the target is listening. Two facts, both checked rather
 * than assumed, changed the shape of the change:
 *
 *  (1) NO SUCH WALK EXISTS ON THIS BASE. Before this item, scsd.c never sent an
 *      outbound SCS$DIRECTORY connect at all -- scs_dir.c held three templates
 *      and every one of them was a RESPONSE. OVMX could answer a lookup and
 *      could not make one. There was nothing to delete.
 *
 *  (2) THE REFERENCE JOINER POLLS *AFTER* IT IS IN, NOT BEFORE.
 *
 *      CORRECTED (vms-66f, adversary-caught): an earlier revision of this
 *      comment said --
 *      REFUTED-QUOTE-BEGIN
 *        "the directory exchange runs in ONE direction ... VAX2 only
 *        answers. VAX2 then opens its own VMS$VAXcluster connection WITHOUT
 *        having polled anybody."
 *      REFUTED-QUOTE-END
 *      Both of those are REFUTED by the capture they
 *      cite, and the roles were inverted. Re-derive with
 *      tools/cluster/scs_dir_role_measure.py (14 checks, 0 failures,
 *      2026-08-05); frame numbers below are 0-based raw pcap indices of
 *      formation-ci1-joinwindow.pcap:
 *
 *        - VAX2 NEVER sends a VMS$VAXcluster CONNECT_REQ. The file holds
 *          exactly ONE ([46:48]=0, name pair VMS$VAXcluster/VMS$VAXcluster) and
 *          it is frame 47, VAX1 -> VAX2. VAX2's answer, frame 50, VAX2 -> VAX1,
 *          is [46:48]=2, an ACCEPT_REQ. The ESTABLISHED MEMBER is the active half.
 *        - VAX2 DOES poll: frame 1237, VAX2 -> VAX1, [46:48]=0, name pair
 *          ("SCS$DIRECTORY", "SCS$DIR_LOOKUP"), with its own lookup round
 *          following on it (1244/1248 asking, 1247/1250 answering).
 *
 *      What survives is ORDERING: VAX2's poll is at t+33.804 s, 0.36 s AFTER
 *      the VMS$VAXcluster connection formed at t+33.444 s. A joiner polls once
 *      it is in. So gating OVMX's joiner connect on a poller answer still moves
 *      OVMX away from the reference wire -- but because of the SEQUENCE, not
 *      because the joiner never polls. That connect is left exactly as it was.
 *      Full census and both refuted sentences: spec sec 4(h)(2a).
 *
 * What the poller adds is the discovery path OVMX genuinely lacked: a node OVMX
 * can see but which has never opened a directory connection TO it was, until
 * now, a node whose SYSAPs OVMX could never learn about. That is the gap
 * p. 2-50 exists to fill, and it is what is wired below.
 *
 * KILL-SWITCH: OVMX_NO_PROCESS_POLLER=1. It gates scs_poll_tick(), which is the
 * only thing that starts a cycle, so with it set neither frame below is ever
 * built. Measured, not asserted -- see the item's bracket.
 */
/* The poller's emitters need the socket and the peer table; both live in
 * struct scsd_rx, so that is what the ctx is. */
static struct scsd_rx *scsd_poll_rx = NULL;

/* Find the peer whose SCS System Address is `node` (the CONFIG_SYS key the
 * poller passes as args->target_node). NULL if the peer is gone. */
static struct peer_state *scsd_peer_by_sys(struct scsd_rx *rx, const uint8_t node[6])
{
    if (rx == NULL || node == NULL) {
        return NULL;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb != NULL && memcmp(ps_sys_addr(ps), node, 6) == 0) {
            return ps;
        }
    }
    return NULL;
}

/* Fill the shared envelope every poller frame needs from a peer. */
static void scsd_poll_dir_params(struct scsd_rx *rx, struct peer_state *ps,
                                 struct scs_dir_params *dp)
{
    memset(dp, 0, sizeof(*dp));
    memcpy(dp->dst_mac, ps_port_addr(ps), 6);
    memcpy(dp->src_mac, rx->our_hw_mac, 6);
    memcpy(dp->src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
    memcpy(dp->peer_logical, ps_sys_addr(ps), 6);
    dp->local_conid = SCS_DIR_OVMX_POLL_CONID;
    dp->incarnation = ps->incarnation; /* sec 4i echo, same rule as the responder */
    dp->recv_ack = ps->vc.seq.recv_seq;
    dp->send_seq = scs_seq_advance(&ps->vc.seq);
}

/*
 * CONNECT / DISCONNECT for the poller's own connection. Goes through
 * send_frame_vc(), the p. 2-31 choke point, so this is a CHOKED entry in the
 * SEND SITE TABLE and takes no exemption.
 */
static int scsd_poll_emit(void *ctx, struct scs_cdt *cdt, enum scs_conn_action act,
                          const struct scs_svc_args *args, const char **what)
{
    struct scsd_rx *rx = (struct scsd_rx *)ctx;
    if (act != SCS_CONN_ACT_SEND_CONNECT_REQ) {
        /* DISCONNECT_REQ, ACCEPT_RSP: OVMX builds neither (scs_svc.h). Say so. */
        return scsd_svc_no_builder(cdt, act);
    }
    struct peer_state *ps = scsd_peer_by_sys(rx, args->target_node);
    if (ps == NULL) {
        return SCS_SVC_EMIT_REFUSED;
    }
    struct scs_dir_params dp;
    scsd_poll_dir_params(rx, ps, &dp);
    uint8_t frame[SCS_DIR_CONNREQ_FRAME_LEN];
    if (scs_dir_build_connect_request(&dp, frame) != 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    *what = "SCS$DIR_LOOKUP -> SCS$DIRECTORY CONNECT-REQUEST";
    if (send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb, *what,
                      frame, sizeof(frame)) <= 0) {
        return SCS_SVC_EMIT_REFUSED;
    }
    rx->poll_connect_sent++;
    log_ts(stdout);
    printf(" SCSD-I-POLLCONN, SCS$DIR_LOOKUP connects to SCS$DIRECTORY on"
           " %02x:%02x:%02x:%02x:%02x:%02x local_conid=0x%08X seq=%u\n",
           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
           (unsigned)SCS_DIR_OVMX_POLL_CONID, dp.send_seq);
    fflush(stdout);
    return SCS_SVC_EMIT_SENT;
}

/* ONE inquiry: "is <sysap> in your list of listening SYSAPs?" (p. 2-50). */
static int scsd_poll_inquire(void *ctx, struct scs_cdt *cdt, const uint8_t node[6],
                             const char *sysap)
{
    struct scsd_rx *rx = (struct scsd_rx *)ctx;
    (void)cdt;
    struct peer_state *ps = scsd_peer_by_sys(rx, node);
    if (ps == NULL) {
        return 0;
    }
    struct scs_dir_params dp;
    scsd_poll_dir_params(rx, ps, &dp);

    struct scs_dir_lookup_params lp;
    memset(&lp, 0, sizeof(lp));
    memcpy(lp.dst_mac, dp.dst_mac, 6);
    memcpy(lp.src_mac, dp.src_mac, 6);
    memcpy(lp.src_logical, dp.src_logical, 6);
    memcpy(lp.peer_logical, dp.peer_logical, 6);
    lp.remote_conid = ps->poll_remote_conid;
    lp.local_conid = SCS_DIR_OVMX_POLL_CONID;
    lp.recv_ack = dp.recv_ack;
    lp.send_seq = dp.send_seq;
    lp.incarnation = dp.incarnation;
    /* Once the connection is up the exchange uses 0x4b (spec sec 4h / 4g
     * phase-3), which is what SCA#37 shows the reference poller doing. */
    lp.opcode = SCS_MSGTYPE_SEQAPP;
    lp.op = SCS_DIR_OP_LOOKUP;
    memset(lp.name, ' ', sizeof(lp.name));
    size_t nl = strlen(sysap);
    if (nl > SCS_DIR_NAME_LEN) {
        nl = SCS_DIR_NAME_LEN;
    }
    memcpy(lp.name, sysap, nl);

    uint8_t frame[SCS_DIR_LOOKUP_FRAME_LEN];
    if (scs_dir_build_lookup_request(&lp, frame) != 0) {
        return 0;
    }
    if (send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                      "SCS$DIR_LOOKUP inquiry", frame, sizeof(frame)) <= 0) {
        return 0;
    }
    rx->poll_inquiry_sent++;
    log_ts(stdout);
    printf(" SCSD-I-POLLASK, SCS$DIR_LOOKUP asks %02x:%02x:%02x:%02x:%02x:%02x"
           " about '%s' seq=%u\n",
           ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
           sysap, lp.send_seq);
    fflush(stdout);
    return 1;
}

/*
 * p. 2-50's "SYSAP_A is notified about the discovery of SYSAP_X; and SYSAP_A
 * can then connect to SYSAP_X." OVMX's interested SYSAP is its connection
 * manager, so the notification opens the VMS$VAXcluster connection -- the SAME
 * call the grounded post-directory path makes, from a different trigger.
 */
static void scsd_poll_found(const char *sysap, const uint8_t node[6], void *ctx)
{
    struct scsd_rx *rx = (struct scsd_rx *)ctx;
    struct peer_state *ps = scsd_peer_by_sys(rx, node);
    if (ps == NULL) {
        return;
    }
    log_ts(stdout);
    printf(" SCSD-I-POLLFOUND, SCS$DIR_LOOKUP discovered '%s' listening on"
           " %02x:%02x:%02x:%02x:%02x:%02x -- notifying the SYSAP\n",
           sysap, ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
           ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
    fflush(stdout);
    rx->poll_found++;
    if (!ps->joiner_connected &&
        send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                    rx->our_hw_mac, rx->our_src_logical)) {
        rx->connect_req_sent++;
        log_ts(stdout);
        printf(" SCSD-I-CONNREQ, sent OUR VMS$VAXcluster CONNECT-REQUEST"
               " local_conid=0x%08X seq=%u (poller discovery)\n",
               OVMX_JOINER_CONID, ps->joiner_req_seq);
        fflush(stdout);
    }
}

/*
 * The poller, bound on first use for exactly the reason scsd_svc() is: the
 * SCSD_UNIT_TEST seam renames main() away, so anything bound only in main()
 * would be dead in every wire test.
 */
static struct scs_poller *scsd_poll(struct scsd_rx *rx)
{
    if (!scsd_poller_ready) {
        scsd_poller_ready = 1;
        scsd_poll_rx = rx;
        scs_poll_init(&scsd_poller, scsd_svc(), rx->cfg);
        scs_poll_set_emitters(&scsd_poller, scsd_poll_emit, rx,
                              scsd_poll_inquire, rx);
        /*
         * p. 2-50: "VMS software, in fact, requests polling for SYSAP names on
         * all nodes." OVMX registers the ONE SYSAP it would actually connect to
         * on discovery -- its connection manager. Registering names OVMX would
         * do nothing about (MSCP$DISK, MSCP$TAPE) would put inquiries on the
         * wire whose answers no code consumes, which is noise, not discovery.
         */
        (void)scs_poll_request(&scsd_poller, "VMS$VAXcluster", NULL,
                               scsd_poll_found, rx);
    }
    return &scsd_poller;
}

/*
 * scsd_handle_frame - dispatch ONE received Ethernet frame. `n` is the length
 * recv() returned; the caller has already handled the error returns. This is
 * the daemon's entire receive path.
 */
static void scsd_handle_frame(struct scsd_rx *rx, const uint8_t *buf, ssize_t n)
{
    if (n < 14) {
        return; /* shorter than a bare Ethernet header -- ignore */
    }

    /* The socket protocol filter already restricts delivery to
     * ethertype 0x6007, but re-check explicitly: it documents intent
     * and matches the absolute frame-offset convention (spec sec 2 /
     * dissect_sca.py) where the SCA payload begins at offset 14. */
    uint16_t ethertype = (uint16_t)(((unsigned)buf[12] << 8) | buf[13]);
    if (ethertype != SCA_ETHERTYPE) {
        return;
    }

    const uint8_t *sca_payload = buf + 14;
    size_t payload_len = (size_t)n - 14;

    uint16_t total_sca_len = 0;
    scs_class_t cls = scs_classify_sca_payload(sca_payload, payload_len, &total_sca_len);

    rx->counts[cls]++;
    rx->total_frames++;

    /* vms-17f: the peer is alive. Stamped for EVERY 0x6007 frame carrying a
     * known peer's source MAC, before the unicast-to-us gate below, because a
     * node that is only beaconing multicast has not departed. Allocates
     * nothing -- an unknown source is ignored here and picked up (or not) by
     * the branch that handles its frame. */
    peer_touch(rx->peers, buf + OFF_ETH_SRC, monotonic_ms());

    log_ts(stdout);
    printf(" SCSD-I-FRAME, class=%-9s total_sca_len=%u eth_len=%zd"
           " src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
           scs_class_name(cls), total_sca_len, n,
           buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
           buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    fflush(stdout);

    /* --- vms-5fe responder --- only act on frames unicast to our HW MAC
     * (our own multicast beacon prompts the peer's directed HELLO). */
    if (!rx->respond || !mac_eq(buf + OFF_ETH_DST, rx->our_hw_mac)) {
        return;
    }
    const uint8_t *src_mac = buf + OFF_ETH_SRC;

    /* vms-fdd: DECODE AND LOG THE PEER'S CONNECT DATA (p. 2-25). Sited here,
     * once, ahead of every branch, deliberately: the connect frames of the
     * different SYSAPs are answered in several different places below (the
     * joiner-accept bind (b1), the SCS$DIRECTORY responder (b2), and the
     * member-opened VMS$VAXcluster accept (c)), several of which `return`, so
     * a per-branch log would silently miss whichever SYSAP's frame took
     * another path. scs_connect_data_get() is the filter -- it accepts ONLY
     * the 110-byte class with connection-control message type CONNECT_REQ(0)
     * or ACCEPT_REQ(2) -- so this fires on exactly the connect frames and on
     * nothing else, and it reads no state and emits no frame. */
    (void)scsd_log_peer_connect_data(buf, (size_t)n);

    /* --- vms-4071: THE IMPLIED ACK (p. 2-16). "what if, in the meantime,
     * the remote node performs an operation that requires the circuit to be
     * OPEN, and this operation results in a packet being sent to the local
     * node? In this case, SCA states that the local node will treat that
     * packet as an 'implied ACK' and simply mark the circuit as being
     * OPEN." Only the SCS sequenced-message classes qualify -- 0x41 and the
     * HELLO are datagrams (p. 2-40) and precede the circuit, so feeding
     * them here would open circuits that are still forming. Dormant on a
     * clean join: the member does not send circuit traffic before the
     * dialogue finishes. */
    if (rx->do_connect && n >= 32 && scs_vc_is_circuit_packet(buf[30])) {
        struct scs_pb *ipb = scs_config_find_pb(rx->cfg, rx->pdt, src_mac);
        if (ipb != NULL && ipb->vc_state == SCS_VC_START_RECEIVED) {
            struct peer_state *ips = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            enum scs_vc_action iact =
                scs_vc_fsm_recv(ipb, SCS_VC_EV_OTHER, monotonic_ms());
            log_ts(stdout);
            printf(" SCSD-I-VCIMPACK, implied ACK from peer"
                   " %02x:%02x:%02x:%02x:%02x:%02x (opcode 0x%02x) -- VC %s\n",
                   src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                   buf[30], scs_vc_state_name(ipb->vc_state));
            fflush(stdout);
            if (ips != NULL && ips->pb == ipb) {
                /* peer_round2_seen=0: no round-2 frame arrived, and none
                 * ever will -- scsd_vc_ack_due() lets fsm.implied_acks
                 * stand in for it so the dialogue does not deadlock. */
                scsd_vc_settle(rx->vc_ctx, ips, iact, 0, &rx->start_ack_sent);
            }
        }
    }

    /* --- vms-691: VC engine -- credit-ack every sequenced message the peer
     * sends us. Each 0x5b directory / 0x4b connect / 190-byte VC message
     * (send_seq != 0 at [20:22], abs 34) is answered by EXACTLY ONE 0x48
     * credit-return (strict 1-for-1, spec sec 4h(3)). This is what stops
     * the VAX's "%PEA0 Excessive packet losses / Closed Virtual Circuit"
     * teardown. The 0x41 START phase uses its own config-round ack
     * mechanism (branch (b) below), so 0x41 is excluded here. We do NOT
     * `continue`: 0x4b/190 frames still fall through to branch (c) for
     * Con.ID binding. */
    if (rx->do_connect && n >= 36 && buf[31] == SCS_FORMAT_CONST &&
        (buf[30] == SCS_MSGTYPE_DIRLOOKUP || buf[30] == SCS_DIR_OPCODE_RETX ||
         buf[30] == SCS_MSGTYPE_SEQAPP || cls == SCS_CLASS_SCS_FIXED)) {
        uint16_t peer_send_seq = (uint16_t)(buf[34] | ((uint16_t)buf[35] << 8)); /* [20:22] */
        uint16_t peer_recv_ack = (uint16_t)(buf[32] | ((uint16_t)buf[33] << 8)); /* [18:20] */
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps != NULL) {
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            /* The peer's leading counter acks OVMX's own sequenced sends. */
            scs_vc_note_peer_ack(&ps->vc, peer_recv_ack);
            ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */

            /* --- vms-abc: THE p. 2-31 SEQUENTIALITY GUARANTEE. "if either the
             * guarantee of message delivery or the guarantee of message
             * sequentiality cannot be satisfied, the virtual circuit between
             * the ports involved will be explicitly broken (if it isn't
             * already). If this happens, then every connection supported by
             * this virtual circuit is also broken, and the SYSAPs participating
             * in these connections are notified of the event."
             *
             * scs_vc_note_recv_checked() is scs_vc_note_recv() plus that check:
             * the recv_seq advance is identical, so nothing changes on the
             * in-order, duplicate/retransmit or first-message paths -- which is
             * every frame in every capture we hold (see the measurement in
             * scs_vc.h). Only a genuine gap behaves differently, and then OVMX
             * must NOT credit-ack: returning a 0x48 for a message that implies
             * others were lost tells the peer OVMX received messages it never
             * saw. That is what the code below used to do. */
            unsigned missing = 0;
            enum scs_vc_seq_verdict sv =
                scs_vc_note_recv_checked(&ps->vc, peer_send_seq, &missing);
            if (sv == SCS_VC_SEQ_GAP) {
                vc_seq_gaps++;
                log_ts(stdout);
                printf(" SCSD-W-SEQGAP, peer %02x:%02x:%02x:%02x:%02x:%02x sent"
                       " send_seq=%u with recv_seq=%u -- %u sequenced message(s)"
                       " missing; the p. 2-31 sequentiality guarantee has failed\n",
                       src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4],
                       src_mac[5], peer_send_seq,
                       (unsigned)(uint16_t)(peer_send_seq - missing - 1u), missing);
                fflush(stdout);
                unsigned broken = scs_vc_break(&ps->vc, ps->pb,
                                               SCS_VC_BREAK_SEQ_GAP, stdout);
                if (scs_vc_break_enabled()) {
                    vc_breaks++;
                    vc_conns_broken += broken;
                    /* The circuit is gone. No credit-return, and no further
                     * dispatch of a frame that arrived on it. */
                    return;
                }
                /* OVMX_NO_VC_BREAK: fall through and limp on, exactly as OVMX
                 * did before this item. */
            }

            if (scs_vc_owes_credit(peer_send_seq)) {
                uint8_t cframe[SCS_CREDIT_FRAME_LEN];
                if (scs_vc_build_credit_for(&ps->vc, ps_port_addr(ps), rx->our_hw_mac,
                                            rx->our_src_logical, ps_sys_addr(ps), cframe) == 0 &&
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "0x48 credit-return", cframe,
                                  sizeof(cframe)) > 0) {
                    ps->credit_sent++;
                    rx->credit_sent++;
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
    if (rx->do_connect && cls == SCS_CLASS_SCS_FIXED) {
        struct scs_member_view mv;
        if (scs_member_parse(buf, (size_t)n, &mv) == 0 &&
            mv.msgtype == SCS_MEMBER_MSGTYPE &&
            (mv.remote_conid == OVMX_LOCAL_CONID ||
             mv.local_conid == OVMX_LOCAL_CONID)) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
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
                    memcpy(mp.src_mac, rx->our_hw_mac, 6);
                    memcpy(mp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
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
                        send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                      "0x81 CM transaction response", rframe,
                                      sizeof(rframe)) > 0) {
                        ps->cm_responses++;
                        rx->cm_response_sent++;
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
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
        }
        ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */
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
        rx->hello_params->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
        if (scs_hello_build_directed_frame(rx->hello_params, src_mac, rx->lab_nonce,
                                           SCS_HELLO_JOINER_INCARNATION,
                                           pad_resp_pfw, ackframe) == 0 &&
            send_frame_channel(rx->sock, rx->ifindex, src_mac, ackframe, sizeof(ackframe)) > 0) {
            ps->padded_replies++;
            ps->last_padded = pnow;
            rx->directed_sent++;
            log_ts(stdout);
            printf(" SCSD-I-PADACK, acked padded HELLO probe (%u SCA rx, abs30 %02x->%02x)"
                   " with plain b4 CONFIRM to %02x:%02x:%02x:%02x:%02x:%02x (ack #%ld)\n",
                   rx_sca, buf[30], pad_resp_pfw,
                   src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                   ps->padded_replies);
            fflush(stdout);
        }
        return;
    }

    /* (a) Directed HELLO -> reply with our directed HELLO (NISCA channel,
     * spec sec 4b). The frame is already known to be unicast to our HW MAC
     * (gated above), so any non-zero directed flag marks a directed HELLO
     * aimed at us. Observed on the wire: an established member uses 0x0001,
     * but a member SOLICITING a not-yet-joined node (us) uses 0x0002 --
     * accept both. */
    if (cls == SCS_CLASS_HELLO && n >= OFF_HELLO_DIRFLG + 2 &&
        (buf[OFF_HELLO_DIRFLG] != 0x00 || buf[OFF_HELLO_DIRFLG + 1] != 0x00)) {
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
        }
        ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG);

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
            rx->hello_params->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
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
            if (scs_hello_build_directed_frame(rx->hello_params, src_mac, rx->lab_nonce,
                                               SCS_HELLO_JOINER_INCARNATION,
                                               resp_pfw, dframe) == 0 &&
                send_frame_channel(rx->sock, rx->ifindex, src_mac, dframe, sizeof(dframe)) > 0) {
                rx->directed_sent++;
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
            rx->hello_params->timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
            /* abs-92 = SCS_HELLO_JOINER_INCARNATION (1): OVMX's own padded
             * HELLO carries the incarnation it attributes to the peer on a
             * fresh first contact, NOT the member's advertised value (spec
             * sec 4i.B; golden pair carries abs-92 = 1). */
            if (scs_hello_build_padded_directed_frame(rx->hello_params, src_mac, rx->lab_nonce,
                                                      SCS_HELLO_JOINER_INCARNATION,
                                                      (uint16_t)SCS_HELLO_PADDED_MAX_SCA,
                                                      rx->pframe, sizeof(rx->pframe), &plen) == 0 &&
                send_frame_channel(rx->sock, rx->ifindex, src_mac, rx->pframe, plen) > 0) {
                ps->padded_initiated = 1;
                clock_gettime(CLOCK_MONOTONIC, &ps->last_padded);
                rx->padded_sent++;
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
        if (scsd_joiner_retransmit_pending(rx, ps)) {
            long now_ms = monotonic_ms();
            long last_ms = ps->last_joiner_req.tv_sec * 1000L +
                           ps->last_joiner_req.tv_nsec / 1000000L;
            if ((now_ms - last_ms) >= 1000) {
                /* vms-398: name the NODE, not the circuit -- CONNECT picks
                 * the OPEN virtual circuit via CONFIG_SYS (p. 2-47). */
                if (send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                                rx->our_hw_mac, rx->our_src_logical)) {
                    rx->connect_req_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-CONNREQ, retransmit OUR VMS$VAXcluster"
                           " CONNECT-REQUEST local_conid=0x%08X seq=%u\n",
                           OVMX_JOINER_CONID, ps->joiner_req_seq);
                    fflush(stdout);
                }
            }
        }
        return;
    }

    /* (b) vms-21e: phase-2 START/config (opcode 0x41). After the channel
     * forms, the established node streams 0x41 START frames and WAITS for
     * OVMX's own 0x41 START before proceeding to the 0x4b connect. Answer
     * as the joiner does (spec sec 4g phase 2): reply round-0 + round-1
     * START, then the round-2 46-byte ack when the peer acks. */
    if (rx->do_connect && n >= 32 && buf[30] == SCS_START_OPCODE) {
        struct scs_start_view sv;
        if (scs_start_parse(buf, (size_t)n, &sv) != 0) {
            return;
        }
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
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
        ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* START src-logical, abs 24 */

        log_ts(stdout);
        printf(" SCSD-I-STARTRX, %s round=%u peer_seq=%u peer_sysid=%u\n",
               sv.is_ack ? "ack" : "START", sv.config_round, sv.send_seq,
               sv.is_ack ? 0 : sv.scssystemid);
        fflush(stdout);

        /* ===== vms-4071: drive the VC FORMATION state machine =====
         *
         * This replaces the pair of unconditional scs_pb_set_vc_state()
         * writes that used to stand in for the dialogue. Every transition
         * now comes from scs_vc_fsm_recv()'s implementation of the p. 2-14
         * acceptable-response table, and OVMX's emissions are whatever
         * action the machine returns. See the scsd_vc_* helpers above for
         * the config-round <-> START/STACK/ACK mapping and for the two
         * declared wire-visible differences. */
        uint64_t vc_now_ms = monotonic_ms();
        enum scs_vc_event vc_ev = scs_vc_classify_round(sv.is_ack, sv.config_round);

        /* ---- PHASE 1: TRANSMIT. NOTHING BETWEEN THE TWO SENDS. ----
         * The pre-vms-4071 code emitted round-0 and round-1 from a two-pass
         * `for` loop with nothing at all between the sendto() calls. An
         * earlier revision of this block put a log_ts+printf+fflush after
         * EACH send, which measurably widened that gap: of four lab-2 runs
         * of that revision (vaxlab-3, 2026-08-03, tags W1A/W1B/W5A/W6A) one
         * -- W6A -- captured the peer's round-1 arriving BETWEEN OVMX's
         * round-0 and round-1; neither pre-vms-4071 control run (W4A, W7A)
         * showed that, both keeping the two sends adjacent. Same frames and
         * the same order FROM OVMX either way, but a wider window is still a
         * wire-timing change, so the logging is deferred to phase 2 below.
         * Re-measured after the split: W8A is adjacent again.
         *
         * NOT UNIT-TESTED, and it cannot be through the SCSD_UNIT_TEST seam:
         * this code is inside main()'s receive loop, which that seam renames
         * away. tests/vmsscs/test_scsd_wire.c covers the helpers this block
         * calls; the adjacency itself is held only by the lab captures named
         * above. */
        int sent_start = 0;
        int sent_stack = 0;
        enum scs_vc_state state_after_start = ps->pb->vc_state;

        /* CLOSED -> START SENT. Trigger deliberately unchanged from vms-21e:
         * OVMX issues its own START only when a peer identity-bearing frame
         * arrives, never off a bare 46-byte ack (an ACK in START SENT is an
         * unacceptable response and would abandon the circuit at once). */
        if (vc_ev != SCS_VC_EV_ACK && ps->pb->vc_state == SCS_VC_CLOSED &&
            !ps->pb->fsm.abandoned) {
            if (scs_vc_fsm_send_start(ps->pb, vc_now_ms) == SCS_VC_ACT_SEND_START &&
                scsd_vc_emit(rx->vc_ctx, ps, SCS_VC_ACT_SEND_START)) {
                rx->start_sent++;
                ps->start_replies++;
                ps->start_replied = 1;
                sent_start = 1;
                state_after_start = ps->pb->vc_state;
            }
        }

        enum scs_vc_action vc_act = scs_vc_fsm_recv(ps->pb, vc_ev, vc_now_ms);

        if (vc_act == SCS_VC_ACT_SEND_STACK) {
            if (scsd_vc_emit(rx->vc_ctx, ps, SCS_VC_ACT_SEND_STACK)) {
                rx->start_sent++;
                ps->start_replies++;
                sent_stack = 1;
            }
        }

        /* ---- PHASE 2: LOG. Nothing below touches the wire. ---- */
        if (sent_start) {
            log_ts(stdout);
            printf(" SCSD-I-STARTTX, sent round-0 START (VC %s)"
                   " (sysid=%u node='%s' send_seq=%u recv_ack=0 incarnation=%u)\n",
                   scs_vc_state_name(state_after_start), rx->vc_ctx->scssystemid, rx->vc_ctx->node_name,
                   ps->vc.seq.send_seq, ps->incarnation ? ps->incarnation : 1);
            fflush(stdout);
        }
        log_ts(stdout);
        printf(" SCSD-I-VCFSM, %s received -> %s, VC %s\n",
               scs_vc_event_name(vc_ev), scs_vc_action_name(vc_act),
               scs_vc_state_name(ps->pb->vc_state));
        fflush(stdout);
        if (sent_stack) {
            log_ts(stdout);
            printf(" SCSD-I-STARTTX, sent round-1 STACK (VC %s)\n",
                   scs_vc_state_name(ps->pb->vc_state));
            fflush(stdout);
        }
        /* OVMX's round-2 ack is emitted by scsd_vc_settle(), exactly once
         * per circuit, followed by the p. 2-21 open transition. By DEFAULT
         * the trigger is the third argument below -- "the peer's round-2
         * 46-byte frame is what we just processed" -- which is the identical
         * trigger the pre-vms-4071 `else if (sv.is_ack)` branch used, so the
         * fresh-join interleaving is unchanged. OVMX_VC_EARLY_ACK=1 moves it
         * to the OPEN transition instead (scsd_vc_ack_due). */
        scsd_vc_settle(rx->vc_ctx, ps, vc_act, scsd_vc_peer_round2(vc_ev),
                       &rx->start_ack_sent);
        return;
    }

    /* (b1) vms-d94: the member ACCEPTS OUR active-joiner VMS$VAXcluster
     * CONNECT-REQUEST. GROUNDED on the live wire (d94-fix2.pcap idx34): once
     * OVMX sends its CONNECT-REQUEST promptly after START, the member stops
     * re-issuing START and replies with a frame (observed op 0x5b, the
     * directory/resolution class) whose remote Con.ID == OVMX_JOINER_CONID
     * (our handle, echoed) and whose local Con.ID is the member's own
     * freshly-supplied handle -- i.e. the CONNECT-RESPONSE that binds OUR
     * joiner connection. scs_dir_parse does not classify it (remote != 0, op
     * != 0x0a), so catch it here by the Con.ID signature and drive the
     * add-member burst on the bound joiner VC.
     *
     * The opcode set below deliberately includes the 0x4b SEQAPP class: the
     * member's answer is 0x5b on some runs and 0x4b on others, and BOTH must
     * bind. Because this block returns, it is the ONLY joiner-accept path in
     * the daemon -- branch (c) below carries no duplicate, and the comment
     * there records why re-adding one would be dead code. MEASURED: replaying
     * all 19 OVMX lab captures (141,338 frames) through scsd_handle_frame()
     * runs this bind 39 times and any (c)-side one 0 times. */
    if (rx->do_connect && n >= 72 &&
        (buf[30] == SCS_DIR_OPCODE || buf[30] == SCS_DIR_OPCODE_RETX ||
         buf[30] == SCS_MSGTYPE_SEQAPP)) {
        uint32_t rconid = (uint32_t)buf[64] | ((uint32_t)buf[65] << 8) |
                          ((uint32_t)buf[66] << 16) | ((uint32_t)buf[67] << 24);
        uint32_t lconid = (uint32_t)buf[68] | ((uint32_t)buf[69] << 8) |
                          ((uint32_t)buf[70] << 16) | ((uint32_t)buf[71] << 24);

        /* vms-dd5: THE CONNECTION-CONTROL CLASSIFIER. The destination Con.ID
         * of a connection-control frame names one of OUR CDTs (p. 2-29: the
         * low 16 bits index the CDL), and [46:48] names which SCA message it
         * is (spec sec 4(h)(1a)). Together those are enough to drive the
         * documented state machine off real frames instead of off inferred
         * side effects. RECEIVE-SIDE ONLY -- it emits nothing and changes no
         * byte; where the machine says a frame is owed, conn_step logs
         * SCSD-W-CONNNOACT.
         *
         * DELIBERATELY NOT HANDLED HERE: message types 0 (CONNECT_REQ) and
         * 2 (ACCEPT_REQ). Those are already fed by the explicit branches
         * that ANSWER them -- the SCS$DIRECTORY connect branch and the two
         * VMS$VAXcluster accept branches -- and feeding them twice would
         * double-step the machine. Everything else (1, 3, 4, 5, 6, 7) has no
         * other call site: before this item OVMX did not react to any of
         * them at all, which is exactly why a peer parking a connection was
         * invisible. */
        /* [46:48] is absolute 60:62; the enclosing guard already required
         * n >= 72. */
        uint16_t cmsg = (uint16_t)((uint32_t)buf[60] | ((uint32_t)buf[61] << 8));
        enum scs_conn_event cev;
        if (cmsg != 0 && cmsg != 2 && scs_conn_event_for_msgtype(cmsg, &cev)) {
            struct scs_cdt *tgt = scs_cdl_lookup(&scsd_cdl, rconid);
            if (tgt != NULL) {
                /* vms-6b3: THE REASON CODE, decoded before the transition so
                 * the log reads in wire order. p. 2-26: REJECT_REQ and
                 * DISCONNECT_REQ each carry an optional 16-bit reason code.
                 * This is the counterpart of the peer's SDA "Rej/Disconn
                 * Reason" field: without it, a peer that told us WHY it refused
                 * us said it into a void. Gated on the same Con.ID ownership
                 * test as the transition -- another node's connection-control
                 * traffic on the shared LAN is not ours to report.
                 *
                 * THE OFFSET IS AN OVMX DESIGN CHOICE, NOT A DECODED VMS FIELD
                 * (scs_reason.h; docs/cluster-protocol-spec.md sec 5). Every
                 * one of the 673 VMS-origin REJECT/DISCONNECT frames we hold
                 * reads 0 there, so on real VMS traffic this line reports NONE
                 * -- which is exactly what SDA reports -- and it would only
                 * ever differ against a peer using the same placement. */
                if (scs_reason_carried_by(cmsg)) {
                    uint16_t reason = 0;
                    if (scs_reason_get(buf, (size_t)n, cmsg, &reason) == 1) {
                        conn_reason_seen++;
                        if (reason != 0) {
                            conn_reason_nonzero++;
                        }
                        log_ts(stdout);
                        printf(" SCSD-I-CONNREASON, conid=0x%08X: %s carries"
                               " reason code %u (%s)%s\n",
                               (unsigned)rconid,
                               cmsg == SCS_REASON_MSGTYPE_REJECT_REQ
                                   ? "REJECT_REQ" : "DISCONNECT_REQ",
                               (unsigned)reason, scs_reason_name(reason),
                               reason == 0 ? " -- no reason supplied, which is"
                                             " what every VMS frame we have"
                                             " observed carries" : "");
                        fflush(stdout);
                    }
                }
                /* vms-591: THE DISCONNECT MESSAGES ARE ANSWERED, NOT MERELY
                 * RECORDED. conn_step() drives the machine and emits nothing,
                 * which for the eight formation/rejection messages is correct
                 * (their answers are emitted by the branches that own them).
                 * For the two teardown messages it was NOT: a peer
                 * DISCONNECT_REQ moved the CDT to DISC RECEIVED and the
                 * DISCONNECT_RSP that Figure 2-16 draws on the SAME arrow was
                 * never sent, and the symmetric own-disconnect p. 2-26
                 * requires was never invoked. That is what this branch fixes;
                 * see scsd_disconnect_dialogue().
                 *
                 * lconid IS THE PEER'S HANDLE and it is recorded before the
                 * dialogue runs: OVMX must address its DISCONNECT_RSP and its
                 * own DISCONNECT_REQ to the peer's Con.ID, and on a connection
                 * OVMX did not open (the member-opened VMS$VAXcluster one) the
                 * CDT may not have it yet. A frame carrying remote Con.ID 0
                 * names no connection at the peer. */
                if (cev == SCS_CONN_EV_RCV_DISCONNECT_REQ ||
                    cev == SCS_CONN_EV_RCV_DISCONNECT_RSP) {
                    struct peer_state *dps =
                        peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
                    if (dps != NULL) {
                        if (lconid != 0) {
                            scs_cdt_set_remote_conid(tgt, lconid);
                        }
                        (void)scsd_disconnect_dialogue(rx->sock, rx->ifindex, dps,
                                                       tgt, cev, rx->our_hw_mac,
                                                       rx->our_src_logical);
                    } else {
                        conn_step(tgt, cev, NULL);
                    }
                } else {
                    conn_step(tgt, cev, NULL);
                }
            }
        }

        if (rconid == OVMX_JOINER_CONID && lconid != 0) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL && !ps->joiner_connected) {
                ps->joiner_remote_conid = lconid;
                ps->joiner_connected = 1;
                /* p. 2-50: "Once SYSAP_A has established a connection with
                 * SYSAP_X, it will disable polling for SYSAP_X on NODE_X."
                 * Polling for VMS$VAXcluster continues on every OTHER node. */
                scs_poll_connected(scsd_poll(rx), "VMS$VAXcluster", ps_sys_addr(ps));
                /* vms-dd5: the member's answer to OUR CONNECT_REQ, carrying
                 * BOTH Con.IDs -- message type 2, ACCEPT_REQ (spec sec
                 * 4(h)(1a)). If the peer's message-type-1 CONNECT_RSP was
                 * seen, the classifier above already moved this connection
                 * to CONNECT ACK and this step is the DOCUMENTED Figure 2-14
                 * transition to OPEN; if that frame was lost, this arrives in
                 * CONNECT SENT and runs through the LABELED OVMX row instead,
                 * and its log line says which. Either way the ACCEPT_RSP the
                 * machine then requires has no OVMX builder, so it is
                 * reported unemitted rather than silently skipped. */
                scs_cdt_set_remote_conid(ps->cdt_joiner, lconid);
                conn_step(ps->cdt_joiner, SCS_CONN_EV_RCV_ACCEPT_REQ, NULL);
                log_ts(stdout);
                printf(" SCSD-I-JOINBOUND, member accepted OUR VMS$VAXcluster"
                       " connect: local=0x%08X remote=0x%08X\n",
                       OVMX_JOINER_CONID, lconid);
                fflush(stdout);
                if (!ps->joiner_cm_sent) {
                    int c = cm_send_config_burst(rx->sock, rx->ifindex, ps, rx->our_hw_mac,
                                                 rx->our_src_logical,
                                                 OVMX_JOINER_CONID, lconid);
                    rx->cm_config_frames += c;
                    ps->joiner_cm_sent = 1;
                    log_ts(stdout);
                    printf(" SCSD-I-CMCONFIG, sent add-member config burst"
                           " (op 0x14/0x01/0x02, %d frames, VOTES=0 non-voting)"
                           " on OUR joiner VC\n", c);
                    fflush(stdout);
                }
            }
            return;
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
    if (rx->do_connect &&
        (buf[30] == SCS_DIR_OPCODE || buf[30] == SCS_DIR_OPCODE_RETX ||
         buf[30] == SCS_MSGTYPE_SEQAPP)) {
        struct scs_dir_view dv;

        /* (b2.0) vms-66f: frames addressed to the connection OUR SCS$DIR_LOOKUP
         * opened. These are the ANSWER half of p. 2-50 and are the only frames
         * in this file whose [50:54] names the poller's own handle, so the test
         * is exact and cannot capture the peer-opened directory connection. */
        if (scs_dir_parse(buf, (size_t)n, &dv) == 0 &&
            dv.remote_conid == SCS_DIR_OVMX_POLL_CONID) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            scs_vc_note_recv(&ps->vc, dv.send_seq);
            if (dv.local_conid != 0) {
                ps->poll_remote_conid = dv.local_conid;
            }
            if (dv.is_lookup_response) {
                /* p. 2-50's "Yes"/"No". scs_poll_answer() decides who, if
                 * anyone, is notified -- this branch decides nothing. */
                rx->poll_answers++;
                log_ts(stdout);
                printf(" SCSD-I-POLLANS, SCS$DIRECTORY answered '%s' -> %s\n",
                       dv.name, scs_dir_answer_name(dv.answer));
                fflush(stdout);
                (void)scs_poll_answer(scsd_poll(rx), dv.name, dv.answer, monotonic_ms());
            } else if (dv.op == SCS_DIR_MSGTYPE_ACCEPT_REQ) {
                /* "and the Directory Service accepts the connection" (p. 2-50):
                 * the message-type-2 ACCEPT_REQ, GROUNDED in sec 4h(1a). */
                (void)scs_poll_opened(scsd_poll(rx), monotonic_ms());
            }
            return;
        }

        if (scs_dir_parse(buf, (size_t)n, &dv) == 0 &&
            (dv.is_dir_connect_request || dv.is_lookup_request)) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps == NULL) {
                return;
            }
            if (!ps->vc.initialized) {
                scs_vc_init(&ps->vc);
            }
            ps->dir_seen = 1;
            ps_learn_sys_addr(rx->cfg, ps, buf + OFF_HELLO_SRCLOG); /* src-logical, abs 24 */
            /* Ensure recv_ack is current even if the credit block did not run
             * (e.g. a 0x7b retransmit); note_recv only advances the high-water. */
            scs_vc_note_recv(&ps->vc, dv.send_seq);

            /* vms-abc: the up-front refusal is kept EVEN THOUGH both sends
             * below now go through send_frame_vc(). It is not redundant: this
             * branch has side effects that are not sends -- it learns
             * dv.local_conid into ps->dir_remote_conid and conn_bind()s the
             * directory CDT -- and letting those run on a CLOSED circuit
             * re-binds a connection whose SYSAP was just told the circuit is
             * gone. The choke point stops the FRAMES; this stops the STATE. */
            if (dv.is_dir_connect_request && !ps->dir_connected &&
                !scsd_refuse_without_open_vc(ps, ps->pb,
                                             "SCS$DIRECTORY CONNECT-RESPONSE", NULL)) {
                /*
                 * vms-7fe: THE p. 2-48 SCAN COMES FIRST. "When a CONNECT_REQ is
                 * received on a VAX system, SCS scans this queue looking for an
                 * SDIR containing a SYSAP name matching the target name
                 * contained in the connect request." The target name is the
                 * GROUNDED 16-byte field at payload [62:78] (spec sec 4(h)(2)).
                 * A miss refuses inside the helper and this branch then does
                 * nothing at all. (The helper's OTHER refusal, p. 2-50's busy
                 * listener, cannot arise here: this branch answers
                 * synchronously, so no listening CDT is in CONNECT RECEIVED
                 * when the next frame arrives -- scs_sdir.h DESIGN CHOICE 3,
                 * measured by test_scsd_wire.c's end-of-run busy total.)
                 *
                 * WHAT REACHES THIS BRANCH, AND HOW NEAR-CERTAIN THE HIT IS.
                 * scs_dir_parse() only sets is_dir_connect_request when the
                 * name field's FIRST 13 BYTES are "SCS$DIRECTORY" (vms-246,
                 * scs_dir.c) -- so for every frame the reference VAX sends, the
                 * name is exactly "SCS$DIRECTORY", a name OVMX LISTENs for, and
                 * THE SCAN HITS. It is not unconditional, though, and the
                 * difference is not academic: the classifier compares 13 bytes
                 * of a 16-byte field while the scan looks up the whole trimmed
                 * name, so a frame naming e.g. "SCS$DIRECTORYX" is classified
                 * here and MISSES -- and is now refused instead of silently
                 * binding the directory connection, which is what this branch
                 * did before this item.
                 *
                 * COVERAGE, and this comment is the second attempt at it: the
                 * entire guard below was once deleted with the whole vmsscs
                 * suite still green. tests/vmsscs/test_scsd_wire.c cases
                 * (2c)/(2d)/(2e) now cover the hit, the miss and the p. 2-50
                 * state read/restore on THIS branch; deleting the guard reds 21
                 * checks (measured, 2026-08-05).
                 *
                 * WHAT THAT COVERAGE IS AND IS NOT, because the bound above
                 * limits it: (2c)'s hit is the ONLY one of the three that runs
                 * on a captured frame. (2d)'s miss needs the substituted name
                 * -- no frame a real VAX sends reaches this branch and misses,
                 * so the refusal wire shape is pinned but never observed, and
                 * its status word is an OVMX invention (spec sec 5). (2e)'s
                 * arrangement -- a listening CDT already in CONNECT RECEIVED --
                 * THIS LOOP CANNOT PRODUCE at all, so it does not guard the
                 * synchronous answer it depends on. And no case asserts a
                 * p. 2-50 busy frame, because none is emittable here.
                 */
                const struct scs_sdir *dsdir = NULL;
                if (scsd_sdir_live() &&
                    !scsd_sdir_admit(rx->sock, rx->ifindex, rx->our_hw_mac,
                                     rx->our_src_logical, ps, buf, (size_t)n,
                                     dv.opcode, dv.local_conid, "SCS$DIRECTORY",
                                     &dsdir)) {
                    return;
                }

                /* Learn the peer's SCS$DIRECTORY handle (its local Con.ID),
                 * then reply op=1 CONNECT-ECHO + op=2 CONNECT-RESPONSE. Each
                 * is a sequenced message: advance OVMX's send_seq per frame
                 * (spec sec 4h(4)). */
                ps->dir_remote_conid = dv.local_conid;
                /* vms-dd5: OVMX is the TARGET on this connection -- Figure
                 * 2-14's NODE_2 column, whose four messages are on the
                 * wire as connection-control message types 0/1/2/3
                 * (docs/cluster-protocol-spec.md sec 4(h)(1a), GROUNDED over
                 * 16 dialogues). This frame is message type 0 = CONNECT_REQ:
                 * 110 bytes, destination Con.ID 0, SYSAP name present. */
                struct scs_dir_params dp;
                memset(&dp, 0, sizeof(dp));
                memcpy(dp.dst_mac, ps_port_addr(ps), 6);
                memcpy(dp.src_mac, rx->our_hw_mac, 6);
                memcpy(dp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
                memcpy(dp.peer_logical, ps_sys_addr(ps), 6);
                dp.remote_conid = ps->dir_remote_conid;
                dp.local_conid = SCS_DIR_OVMX_CONID;
                /* vms-246 (§4i established-join): echo the member's current
                 * node-incarnation into the directory [22:24], the same
                 * value it stamps in its own 0x5b connect-request and its
                 * 0x41 START (§4i.B). Read off the wire (ps->incarnation);
                 * 0 leaves the fresh template value 1. */
                dp.incarnation = ps->incarnation;

                /* vms-561: THE ACCEPT SERVICE (p. 2-23, p. 2-56). One call
                 * drives Figure 2-14's whole NODE_2 column -- CLOSED
                 * --RCV_CONNECT_REQ--> CONNECT REC (emitting the op=1
                 * CONNECT-ECHO, which is the CONNECT_RSP) and CONNECT REC
                 * --SVC_ACCEPT--> ACCEPT SENT (emitting the op=2
                 * CONNECT-RESPONSE, which is the ACCEPT_REQ). The CDT, the
                 * p. 2-28 VC-loss handler and both transitions belong to the
                 * service; scsd_svc_emit_dir_accept() above builds and sends
                 * the two frames, in that order, with the same per-frame
                 * send_seq advance as before. */
                struct scsd_svc_emit_ctx ec;
                memset(&ec, 0, sizeof(ec));
                ec.sock = rx->sock;
                ec.ifindex = rx->ifindex;
                ec.ps = ps;
                ec.vc = ps->pb;
                ec.dir_params = &dp;

                struct scs_svc_args da;
                memset(&da, 0, sizeof(da));
                da.local_sysap = "SCS$DIRECTORY";
                da.remote_sysap = "SCS$DIRECTORY";
                da.vc = ps->pb;
                da.cfg = rx->cfg;
                da.target_node = ps_sys_addr(ps);
                da.vc_loss = scsd_sysap_vc_loss;
                da.sysap_ctx = ps;
                da.conid = SCS_DIR_OVMX_CONID;
                da.remote_conid = ps->dir_remote_conid;
                da.emit = scsd_svc_emit_dir_accept;
                da.emit_ctx = &ec;

                struct scsd_svc_mark dmark = scsd_svc_mark();
                struct scs_cdt *dcdt = NULL;
                enum scs_svc_status dst = scs_accept(scsd_svc(), &da, &dcdt);
                scsd_svc_settle(dmark);
                /* p. 2-50, as OVMX times it (scs_sdir.h DESIGN CHOICE 3): the
                 * listening CDT returns to LISTEN now that the answer has been
                 * emitted -- or abandoned, which is why this is unconditional.
                 * p. 2-49: the local Con.ID that went out is SCS_DIR_OVMX_CONID,
                 * the SEPARATE CDT's, never dsdir's listening Con.ID. */
                scs_sdir_connect_answered(scs_svc_sdir_mut(scsd_svc()), dsdir);
                if (dst == SCS_SVC_NOCDT) {
                    scsd_svc_slot_refused(SCS_DIR_OVMX_CONID, "SCS$DIRECTORY");
                }
                if (dcdt != NULL) {
                    ps->cdt_dir = dcdt;
                }
                if (ec.sent) {
                    ps->dir_connected = 1;
                    rx->dir_conn_resp_sent++;
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
                    /* vms-398: the caller here names the NODE the START
                     * completed with, not a circuit, so CONNECT selects the
                     * virtual circuit itself via CONFIG_SYS (p. 2-47). */
                    if (ps->start_acked && !ps->joiner_connected &&
                        send_joiner_connect_request(rx->sock, rx->ifindex, rx->cfg, ps, NULL,
                                                    rx->our_hw_mac, rx->our_src_logical)) {
                        rx->connect_req_sent++;
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
                memcpy(lp.src_mac, rx->our_hw_mac, 6);
                memcpy(lp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
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
                /*
                 * vms-7fe: THE ANSWER NOW COMES FROM THE SDIR QUEUE.
                 *
                 * p. 2-22: "The SCS Directory Service is in fact a SYSAP that
                 * responds to inquires from other nodes wanting to know if
                 * particular SYSAP names are in this list." Until this item the
                 * "list" was a hardcoded `memcmp(dv.name, "VMS$VAXcluster", 14)`
                 * right here -- an affirmative table of one, which could not
                 * disagree with what OVMX actually served and, in the
                 * SCS$DIRECTORY case, did: it answered NOT PRESENT HERE for a
                 * SYSAP the daemon was serving in the same breath.
                 *
                 * WIRE EFFECT, stated exactly. The only queried name whose
                 * answer changes is SCS$DIRECTORY (now affirmative). Every other
                 * name the reference VAX asks about -- MSCP$TAPE, MSCP$DISK --
                 * is not in the queue and still gets the GROUNDED 16-byte
                 * "NOT PRESENT HERE" marker at [78:94] (spec sec 4(h)(2)), byte
                 * for byte as before, because OVMX LISTENs for neither and
                 * deliberately does not (see scsd_svc()).
                 *
                 * OVMX_NO_SDIR restores the old compare verbatim.
                 */
                if (scsd_sdir_live()) {
                    lp.affirmative = scs_sdir_lookup(scs_svc_sdir_mut(scsd_svc()),
                                                     dv.name) != NULL;
                } else {
                    lp.affirmative = (memcmp(dv.name, "VMS$VAXcluster", 14) == 0);
                }
                uint8_t lframe[SCS_DIR_LOOKUP_FRAME_LEN];
                if (scs_dir_build_lookup_response(&lp, lframe) == 0 &&
                    send_frame_vc(rx->sock, rx->ifindex, ps, ps->pb,
                                  "SCS$DIR_LOOKUP response", lframe,
                                  sizeof(lframe)) > 0) {
                    ps->dir_lookups_answered++;
                    rx->dir_lookup_sent++;
                    log_ts(stdout);
                    printf(" SCSD-I-DIRLOOKUP, resolved '%s' -> %s (op=0x%02x)\n",
                           dv.name, lp.affirmative ? "AFFIRMATIVE" : "NOT PRESENT HERE",
                           dv.opcode);
                    fflush(stdout);
                }
            }
            return;
        }
    }

    /* (c) SCS envelope directed to us -> inspect; complete the connect. */
    if (rx->do_connect && (cls == SCS_CLASS_OTHER || cls == SCS_CLASS_SCS_FIXED)) {
        struct scs_connect_view v;
        if (scs_connect_parse(buf, (size_t)n, &v) != 0) {
            return;
        }
        log_ts(stdout);
        printf(" SCSD-I-SCSENV, msgtype=0x%02x fmt=0x%02x len=%u"
               " remote_conid=0x%08X local_conid=0x%08X\n",
               v.msgtype, v.format, v.total_sca_len, v.remote_conid, v.local_conid);
        fflush(stdout);

        if (v.msgtype != SCS_MSGTYPE_SEQAPP || !v.has_conid) {
            return;
        }
        struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
        if (ps == NULL) {
            return;
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
            memcpy(cp.src_mac, rx->our_hw_mac, 6);
            memcpy(cp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
            memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
            cp.local_conid = OVMX_LOCAL_CONID;
            cp.remote_conid = v.local_conid;
            cp.recv_ack = ps->vc.seq.recv_seq;
            cp.send_seq = scs_seq_advance(&ps->vc.seq);
            cp.incarnation = ps->incarnation; /* §4i.B established-join echo (0 => fresh 1) */

            /* vms-561: THE ACCEPT SERVICE again, on the member-opened
             * connection. OVMX is the TARGET here, and one emitted frame
             * covers two transitions:
             *   RCV_CONNECT_REQ -- the member's 0x4b with destination Con.ID
             *     0, message type 0 (spec sec 4g phase 4, golden frame 47).
             *     The machine requires a CONNECT_RSP and OVMX BUILDS NONE, so
             *     scsd_svc_emit_member_accept() answers NOBUILDER and the
             *     SCSD-W-CONNNOACT line records it every run. An OVMX gap, not
             *     a wire gap: the real VAX does send that frame.
             *   SVC_ACCEPT -- the 110-byte CONNECT-RESPONSE (golden frame 50,
             *     message type 2 = ACCEPT_REQ, sec 4(h)(1a)).
             *
             * `first` is read BEFORE the service runs, and drives
             * args.retransmit: a member that re-sends its CONNECT-REQUEST gets
             * the labeled OVMX retransmit row (ACCEPT SENT -> ACCEPT SENT,
             * repeating the ACCEPT_REQ), not a second ACCEPT.
             *
             * vms-561 ALSO ADDS THE UP-FRONT CIRCUIT REFUSAL that the
             * SCS$DIRECTORY branch has carried since vms-abc, and for the same
             * reason: send_frame_vc() stops the FRAME, but by then ACCEPT has
             * already allocated a descriptor and advanced a state for a
             * connection whose circuit is gone. The choke point stops the
             * frames; this stops the state. It costs no extra refusal -- the
             * send that used to be refused inside send_frame_vc() is the one
             * this refuses instead, so vc_sends_refused moves by the same 1. */
            int first = !ps->connected;
            if (scsd_refuse_without_open_vc(ps, ps->pb,
                                            "VMS$VAXcluster 0x4b CONNECT-RESPONSE",
                                            NULL)) {
                return;
            }

            /*
             * vms-7fe: the p. 2-48 scan on the OTHER inbound CONNECT_REQ. The
             * target name at payload [62:78] reads "VMS$VAXcluster" here, which
             * OVMX LISTENs for, so this HITS and the accept below is unchanged.
             * A retransmitted request carries the same requester Con.ID and is
             * re-delivered rather than refused busy (scs_sdir.h DESIGN CHOICE
             * 4) -- which matters here more than anywhere, because the
             * established VAX retransmits this exact frame until it accepts our
             * answer and `first` is already false on every repeat. Note what
             * that sentence does NOT say: a busy refusal is not merely avoided
             * here, it is unreachable from this loop at all, because the answer
             * below is synchronous (DESIGN CHOICE 3) and the SDIR is back in
             * LISTEN before the next frame is read -- which test_scsd_wire.c
             * measures with an end-of-run busy total, not a per-case check.
             */
            const struct scs_sdir *msdir = NULL;
            if (scsd_sdir_live() &&
                !scsd_sdir_admit(rx->sock, rx->ifindex, rx->our_hw_mac,
                                 rx->our_src_logical, ps, buf, (size_t)n,
                                 v.msgtype, v.local_conid, "VMS$VAXcluster",
                                 &msdir)) {
                return;
            }

            struct scsd_svc_emit_ctx mec;
            memset(&mec, 0, sizeof(mec));
            mec.sock = rx->sock;
            mec.ifindex = rx->ifindex;
            mec.ps = ps;
            mec.vc = ps->pb;
            mec.connect_params = &cp;

            struct scs_svc_args ma;
            memset(&ma, 0, sizeof(ma));
            ma.local_sysap = "VMS$VAXcluster";
            ma.remote_sysap = "VMS$VAXcluster";
            ma.vc = ps->pb;
            ma.cfg = rx->cfg;
            ma.target_node = ps_sys_addr(ps);
            ma.vc_loss = scsd_sysap_vc_loss;
            ma.sysap_ctx = ps;
            ma.conid = OVMX_LOCAL_CONID;
            ma.remote_conid = v.local_conid;
            ma.retransmit = !first;
            ma.emit = scsd_svc_emit_member_accept;
            ma.emit_ctx = &mec;

            struct scsd_svc_mark mmark = scsd_svc_mark();
            struct scs_cdt *mcdt = NULL;
            enum scs_svc_status mst = scs_accept(scsd_svc(), &ma, &mcdt);
            scsd_svc_settle(mmark);
            /* p. 2-50 / scs_sdir.h DESIGN CHOICE 3, as above. */
            scs_sdir_connect_answered(scs_svc_sdir_mut(scsd_svc()), msdir);
            if (mst == SCS_SVC_NOCDT) {
                scsd_svc_slot_refused(OVMX_LOCAL_CONID, "VMS$VAXcluster");
            }
            if (mcdt != NULL) {
                ps->cdt_member = mcdt;
                /* vms-fdd: the ACCEPT service's p. 2-28 copy of the connect
                 * data scs_connect_build_response() stamped. */
                scsd_cdt_record_connect_data(mcdt);
            }
            if (mec.sent) {
                ps->remote_conid = v.local_conid;
                rx->connect_resp_sent++;
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
                 * connection, bound in branch (b1) above. */
            }
        }
        /* vms-dd5: THERE IS NO `else if (v.remote_conid == OVMX_JOINER_CONID)`
         * BRANCH HERE, and re-adding one would be dead code. Branch (b1)
         * above owns the joiner-accept path, and its guard is IMPLIED by the
         * guard of this block:
         *   - reaching here needs v.msgtype == SCS_MSGTYPE_SEQAPP, i.e.
         *     buf[30] == 0x4b, which is in (b1)'s opcode set;
         *   - reaching here needs v.has_conid, which scs_connect_parse only
         *     sets for the 110-/190-byte classes with len >= 72, so (b1)'s
         *     `n >= 72` holds too;
         *   - (b1) RETURNS whenever rconid == OVMX_JOINER_CONID && lconid != 0.
         * So the only frame that could ever have reached a joiner branch here
         * is a 110-/190-byte-class frame whose destination Con.ID is our
         * joiner handle and whose SOURCE Con.ID is 0 -- and binding a
         * connection to a null remote Con.ID is wrong by construction (0 is
         * the "handle not yet assigned" value, spec sec 4(h)(1)). MEASURED,
         * not argued: (a) across all 41 pcaps in the reference-lab capture set
         * there is no 110-/190-class frame with destination Con.ID != 0 and
         * source Con.ID == 0; (b) instrumenting both sites and replaying all
         * 19 OVMX lab captures (141,338 frames) through this very function ran
         * the (b1) site 39 times and this one 0 times, including the
         * MEMBER-achieved success capture. The branch that used to be here was
         * a duplicate of (b1) that never executed; it is deleted, and
         * test_null_source_conid_binds_nothing() in tests/vmsscs/
         * test_scsd_wire.c reds if it comes back. */
        return;
    }
}

/* =====================================================================
 * vms-17f -- PEER DEPARTURE, AND WHY THE p. 2-21 REFRESH WAS UNREACHABLE
 * =====================================================================
 *
 * WHAT WAS BROKEN. scs_config.c has implemented the p. 2-21 Note correctly since
 * vms-7be -- "If there are no other Path Blocks queued to the old System Block
 * ... the old System Block is refreshed based on the contents of the formative
 * System Block ... Typically, this happens when the remote node was once in the
 * cluster, departed, and is now rebooting." SCSD could not reach it, for three
 * reasons that all had to be fixed together:
 *
 *   1. scs_pb_close() had NO CALLER anywhere in this file. A circuit, once open,
 *      stayed open for the life of the process.
 *   2. peer_find_or_add() matches on port address and only ever ALLOCATES. A
 *      slot was never released, so a returning node re-entered its stale one.
 *   3. scs_pb_open() ran behind `if (!ps->start_acked)`, and start_acked was
 *      never reset.
 *
 * So a rejoining VAX landed in the SAME peer slot on the SAME already-OPEN Path
 * Block, took scs_pb_open()'s `!pb->on_pdt` early return, and got
 * SCS_OPEN_EXISTING_SB. The REFRESH branch was structurally unreachable and its
 * log clause said so.
 *
 * WHAT THIS SWEEP DOES. When a peer has been silent past the listen timeout it
 * is declared departed: scs_pb_depart() runs the p. 2-28 ordered teardown (see
 * scs_depart.h) and the peer slot is RELEASED. The System Block survives, by
 * design (p. 2-17). When that node comes back, peer_find_or_add() builds a
 * fresh slot -- start_acked back to 0 with the rest of the zeroed struct -- and
 * a fresh FORMATIVE Path Block, formation runs from CLOSED again, and
 * scs_pb_open() finds the old SB with an empty PB queue. That is the p. 2-21
 * Note, executed.
 *
 * THIS IS WIRE-VISIBLE, and the change is exactly this: a returning peer now
 * gets a full formation dialogue (round-0 START, round-1 STACK, round-2 ack)
 * where before it got nothing, because OVMX thought the circuit was still open.
 * OVMX_NO_PEER_DEPART=1 restores the old behaviour completely -- no teardown, no
 * slot release, no second formation.
 *
 * WHAT IS NOT CLAIMED. This does not fix vms-2f3 (OVMX's own rejoin failure) and
 * must not be read as doing so. It is the wiring that failure's diagnosis
 * depends on: eleven hypotheses about frame content have been falsified there,
 * and the strategy note is explicit that the rejoin is a connection-STATE
 * failure. This gives the state a place to be wrong in. Whether it is the fault
 * is a lab question, not a code-reading question.
 *
 * ===== MEASURED IN THE LAB, 2026-08-04 (lab-2 pods, tests/lab/README.md) =====
 *
 * Method: run SCSD against a live 2-node VMScluster, SIGKILL one node's SIMH
 * process (a hard node failure -- NOT a shutdown, which would be a graceful
 * departure and a different case), leave it dead well past the listen timeout,
 * then reboot it with an UNCHANGED SCSNODE/SCSSYSTEMID. That is the p. 2-21
 * Note's own scenario. Captures and SCSD logs are on the lab volume at
 * /data/training/vax/k8s-labs/<pod>/logs/{17f-<tag>.pcap,scsd-<tag>.log}.
 *
 *   D2  vaxlab-2, VAX1 killed -- the node OVMX had an OPEN circuit and TWO SCS
 *       connections with. SCSD: "peer aa:00:04:00:01:04 silent 20291ms ...
 *       path block CLOSED, peer slot released, 2 connection(s) lost". When VAX1
 *       came back: "path block OPEN, old system block REFRESHED (rejoin,
 *       p. 2-21 Note)" and PB-OPEN: new-sb=1 refreshed=1. THE ITEM'S CLAIM,
 *       on the wire.
 *   D0  vaxlab-3, IDENTICAL scenario with OVMX_NO_PEER_DEPART=1. PB-OPEN:
 *       new-sb=1 refreshed=0, PEER-DEPARTURES=0, zero SCSD-I-PEERGONE lines.
 *       The switch suppresses the behaviour completely (guardrail 23: the
 *       counter was RUN both ways before anything was written down here).
 *   TZ  vaxlab-2, a fresh identity, 150 s, nobody killed. PB-OPEN: new-sb=1,
 *       PEER-DEPARTURES=0 -- the ordinary join is unchanged and no healthy peer
 *       is aged out.
 *
 * EVERY DEPARTURE RE-DERIVED FROM THE RAW CAPTURE, not from SCSD's own log: in
 * D2 the three peers SCSD departed last transmitted at 17:09:23.9 / 17:09:54.7 /
 * (VAX1) 17:07:10.6, and SCSD declared them gone 20.9 s / 20.5 s / 20.3 s later.
 * No peer that was still transmitting was departed. VAX1's silence in that
 * capture is 165.6 s end to end -- the kill, the 95 s dead window and the reboot.
 */

/*
 * scsd_peer_departure_sweep - age every peer slot against `now_ms` and tear down
 * the ones that have gone quiet. Returns the number of peers declared departed.
 *
 * Separated from main()'s loop for the reason vms-fb1 separated the receive
 * dispatch: a timer block inside main() is renamed away by SCSD_UNIT_TEST and
 * can therefore be mutated freely without reddening a test. This is the
 * production sweep and tests/vmsscs/test_scsd_wire.c calls this function.
 *
 * KNOWN LIMIT, stated rather than hidden: main() calls this once per loop
 * iteration, and the loop is driven by recv(). The 1-second SO_RCVTIMEO that
 * makes it wake on an idle wire is set only in --emit-hello mode (which
 * --respond and --connect both imply), so a receive-only SCSD watching a wire
 * that goes COMPLETELY silent blocks in recv() and does not sweep until some
 * frame arrives. Every mode that can form a circuit sets the timeout, so no
 * circuit OVMX actually opens is affected; a listener that opens none has
 * nothing to tear down.
 */
static unsigned scsd_peer_departure_sweep(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || rx->peers == NULL) {
        return 0;
    }
    if (!scs_depart_enabled()) {
        /* THE KILL SWITCH: no peer is ever declared departed.
         *
         * REDUNDANT BY DESIGN, and measured to be: scs_pb_depart() checks the
         * same switch, and the `res != SCS_PB_CLOSE_OK` guard below then
         * declines to release the slot -- so deleting these three lines changes
         * no observable behaviour (mutation M10 of the vms-17f battery survives
         * the whole suite, and is listed as an equivalent mutant rather than
         * quietly dropped). It is kept because the gate belongs at the top of
         * the policy that reads the timeout, not only inside the mechanism. */
        return 0;
    }
    uint64_t timeout_ms = scs_depart_listen_timeout_ms();
    unsigned departed = 0;

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL || ps->last_rx_ms == 0) {
            continue; /* free slot, or one never heard from -- nothing to age */
        }
        if (now_ms < ps->last_rx_ms || now_ms - ps->last_rx_ms < timeout_ms) {
            continue;
        }

        uint8_t gone_mac[6];
        memcpy(gone_mac, ps_port_addr(ps), 6);
        uint64_t silent_ms = now_ms - ps->last_rx_ms;

        struct scs_depart_stats st;
        enum scs_pb_close_result res =
            scs_pb_depart(scsd_cdl_ready ? &scsd_cdl : NULL, rx->cfg, ps->pb, &st);

        if (res == SCS_PB_CLOSE_CONNECTIONS_QUEUED) {
            /* A SYSAP error handler queued a NEW connection to the dying
             * circuit. Nothing here can safely destroy a structure a SYSAP is
             * holding, so the slot is LEFT ALONE and re-aged next sweep. Loud,
             * because it means the teardown did not complete (INV-6: never fake
             * success for an executive facility). */
            depart_refusals++;
            log_ts(stderr);
            fprintf(stderr,
                    " SCSD-W-DEPARTBUSY, peer %02x:%02x:%02x:%02x:%02x:%02x silent"
                    " %llums but its path block still carries %u connection(s) --"
                    " NOT torn down, will retry\n",
                    gone_mac[0], gone_mac[1], gone_mac[2], gone_mac[3], gone_mac[4],
                    gone_mac[5], (unsigned long long)silent_ms, scs_pb_cdt_count(ps->pb));
            fflush(stderr);
            continue;
        }
        if (res != SCS_PB_CLOSE_OK) {
            /* The teardown did not happen -- the kill switch is set, or the PB
             * was not in use. Releasing the slot anyway would be a departure
             * that tore nothing down, i.e. exactly the "report success for work
             * that was not done" shape INV-6 forbids. Leave it alone. */
            continue;
        }

        peer_departures++;
        depart_connections_lost += st.connections_lost;
        departed++;

        log_ts(stdout);
        printf(" SCSD-I-PEERGONE, peer %02x:%02x:%02x:%02x:%02x:%02x silent %llums"
               " (listen timeout %llums) -- path block CLOSED, peer slot released,"
               " %u connection(s) lost, %u credit waiter(s) flushed,"
               " %u MFREEQ buffer(s) returned, %u DFREEQ buffer(s) returned;"
               " its system block STAYS in the"
               " configuration queue (p. 2-17) so a rejoin can refresh it (p. 2-21)."
               " Configuration queue now holds %u system block(s)\n",
               gone_mac[0], gone_mac[1], gone_mac[2], gone_mac[3], gone_mac[4],
               gone_mac[5], (unsigned long long)silent_ms,
               (unsigned long long)timeout_ms, st.connections_lost,
               st.waiters_flushed, st.mfreeq_reclaimed, st.dfreeq_reclaimed,
               scs_config_sb_count(rx->cfg));
        fflush(stdout);

        /* Release the slot, WHOLE. `pb = NULL` alone would be enough to free it
         * for reuse (peer_find_or_add zeroes any slot it allocates), but a free
         * slot must not sit there holding a departed node's connection handles,
         * incarnation and start_acked: the only thing that distinguishes free
         * from in-use is that one pointer, and every other field would read as
         * the returning node's if anything ever looked without checking it.
         * The memset is also what resets start_acked (and the three CDT
         * pointers, whose CDTs scs_pb_depart just released), so the
         * returning node re-drives formation from CLOSED instead of being pinned
         * to state from its previous incarnation. */
        memset(ps, 0, sizeof(*ps));
    }
    return departed;
}

/*
 * scsd_retransmit_tick - vms-691's retransmit of OVMX's own unacked sequenced
 * message (the CONNECT-REQUEST), plus vms-abc's p. 2-31 DELIVERY guarantee.
 *
 * WHY IT IS A FUNCTION. This was an inline block in main()'s loop, and
 * SCSD_UNIT_TEST renames main() away, so it was compiled but reachable from no
 * test -- the same measured gap vms-fb1 closed for the receive path. The body
 * is that block, moved, with the loop-local state reached through `rx->`.
 *
 * THE BEHAVIOUR CHANGE (vms-abc): reaching the retransmit cap used to be a bare
 * `continue` -- OVMX stopped retransmitting a message the peer had never
 * acknowledged and carried on with the circuit still OPEN. p. 2-31: "if either
 * the guarantee of message delivery or the guarantee of message sequentiality
 * cannot be satisfied, the virtual circuit between the ports involved will be
 * explicitly broken (if it isn't already)." Exhausting the retransmits IS the
 * delivery guarantee failing, so the circuit is now broken instead. The CAP
 * itself is unchanged -- SCS_VC_DELIVERY_RETRY_LIMIT and VC_RETRANSMIT_MAX are
 * both 5 and a _Static_assert above holds them equal -- so the point at which
 * OVMX gives up is exactly where it was; only what it does there changed.
 * OVMX_NO_VC_BREAK=1 restores the limp-on.
 *
 * ===== TWO LOOP BODIES, AND ONLY ONE OF THEM IS LIVE. Read this before
 * changing either guard. =====
 *
 * (A) THE DELIVERY SCAN (vms-abc) is gated ONLY on scs_vc_delivery_failed(),
 *     which is itself gated on `vc.have_unacked`. It is LIVE. OVMX's one
 *     outstanding sequenced message is the vms-d94 joiner CONNECT-REQUEST;
 *     send_joiner_connect_request() is the only LIVE caller of
 *     scs_vc_record_sent()/scs_vc_mark_retransmitted() in the tree (scan (B)
 *     below calls mark_retransmitted too, but (B) is dead), and the
 *     directed-HELLO handler retransmits it at ~1 Hz while the member has not
 *     accepted (see scsd_joiner_retransmit_pending()). So a member that answers
 *     HELLOs but never accepts our connection drives retransmit_count to
 *     SCS_VC_DELIVERY_RETRY_LIMIT within ~5 s and the NEXT main-loop tick
 *     breaks the circuit -- which is the p. 2-31 outcome. THIS IS WHY THE SCAN
 *     IS NOT NESTED UNDER (B)'s GUARD: it must run for every peer with an
 *     outstanding message, not only for the ones (B) would retransmit for.
 *
 * (B) THE vms-691 RETRANSMIT below it -- the one that re-sends a CONNECT-REQUEST
 *     at OVMX_LOCAL_CONID -- is DEAD, and vms-abc deliberately did not revive
 *     it. Its guard tests `ps->connect_sent`, which is ASSIGNED NOWHERE in this
 *     file (`grep -n connect_sent src/vmsscs/scsd.c`: the declaration, this
 *     guard, and two exit-report lines; every assignment in the file is to the
 *     DIFFERENT field `joiner_connect_sent`). It has not run since vms-d94 moved
 *     the connect onto the joiner-opened connection. It is NOT deleted here
 *     because deleting it and reviving it are both wire decisions belonging to
 *     the connect/retransmit path: reviving it would start emitting retransmits
 *     at OVMX_LOCAL_CONID that OVMX does not send today. Reported, not fixed --
 *     see the findings on this item. Nothing in vms-abc's coverage depends on
 *     (B): the delivery break is proved through (A) alone.
 */
static void scsd_retransmit_tick(struct scsd_rx *rx, uint64_t now_ms)
{
    if (rx == NULL || !rx->do_connect) {
        return;
    }
    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        /* (A) vms-abc: the p. 2-31 DELIVERY guarantee. LIVE -- see the header. */
        if (scs_vc_delivery_failed(&ps->vc)) {
            unsigned broken = scs_vc_break(&ps->vc, ps->pb, SCS_VC_BREAK_DELIVERY, stdout);
            if (scs_vc_break_enabled()) {
                vc_breaks++;
                vc_conns_broken += broken;
            }
            continue;
        }
        /* (B) vms-691's retransmit. DEAD -- see the header. */
        if (ps->connected || !ps->connect_sent) {
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
        memcpy(cp.src_mac, rx->our_hw_mac, 6);
        memcpy(cp.src_logical, rx->our_src_logical, 6); /* vms-9f3: abs-24 logical addr */
        memcpy(cp.peer_logical, ps_sys_addr(ps), 6);
        cp.local_conid = OVMX_LOCAL_CONID;
        cp.remote_conid = 0;
        /* vms-c6d: re-send the SAME outstanding sequenced message -- reuse the
         * recorded unacked send_seq (do NOT advance) and the current recv_ack. */
        cp.recv_ack = ps->vc.seq.recv_seq;
        cp.send_seq = ps->vc.unacked_seq;
        cp.incarnation = ps->incarnation;
        /* vms-561: even this DEAD branch goes through the CONNECT service. It
         * is migrated rather than left alone for two reasons: the item's rule
         * is that no site builds a connect frame by hand, and a hand-built one
         * left here is exactly the site a future revival would copy. It reuses
         * scsd_svc_emit_connect_req() unchanged -- same builder, same params,
         * same choke point -- so nothing about it is new except who owns the
         * descriptor. It remains unreachable (`connect_sent` is assigned
         * nowhere) and tools/cluster/scsd_wire_diff.sh does NOT exercise it;
         * that is stated rather than glossed. */
        struct scsd_svc_emit_ctx rec;
        memset(&rec, 0, sizeof(rec));
        rec.sock = rx->sock;
        rec.ifindex = rx->ifindex;
        rec.ps = ps;
        rec.vc = ps->pb;
        rec.connect_params = &cp;

        struct scs_svc_args ra;
        memset(&ra, 0, sizeof(ra));
        ra.local_sysap = "VMS$VAXcluster";
        ra.remote_sysap = "VMS$VAXcluster";
        ra.vc = ps->pb;
        ra.cfg = rx->cfg;
        ra.target_node = ps_sys_addr(ps);
        ra.vc_loss = scsd_sysap_vc_loss;
        ra.sysap_ctx = ps;
        ra.conid = OVMX_LOCAL_CONID;
        ra.emit = scsd_svc_emit_connect_req;
        ra.emit_ctx = &rec;

        struct scsd_svc_mark rmark = scsd_svc_mark();
        struct scs_cdt *rcdt = NULL;
        (void)scs_connect(scsd_svc(), &ra, &rcdt);
        scsd_svc_settle(rmark);
        if (rec.sent) {
            scs_vc_mark_retransmitted(&ps->vc, now_ms);
            rx->retransmit_sent++;
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

/*
 * ============ vms-591: CLEAN SHUTDOWN -- OVMX STOPS VANISHING ==============
 *
 * Before this item the daemon's whole shutdown was `g_stop = 1` and then exit.
 * Every connection it held was left formed from the peer's point of view, and
 * the peer's CDT for it stayed pending until its own timers gave up. p. 2-26
 * requires a SYSAP that is done with a connection to invoke DISCONNECT; a
 * process that just exits has invoked nothing.
 *
 * WHAT THIS DOES, in order:
 *   1. For each peer, invoke DISCONNECT on every connection on its circuit
 *      (scs_svc_disconnect_all). Each sends a DISCONNECT_REQ with the matching
 *      flag CLEAR -- we are initiating, not answering -- and moves the
 *      connection OPEN -> DISC SENT.
 *   2. Keep receiving for a BOUNDED time so the peer's DISCONNECT_RSP (and its
 *      own matching DISCONNECT_REQ) can be answered by the normal receive
 *      path, which is what walks each connection the rest of the way to
 *      CLOSED. No special-case receive loop: the same scsd_handle_frame() runs.
 *   3. Stop when every connection is CLOSED, or when the deadline expires.
 *
 * ============================ THE TIMEOUT ==================================
 *
 * SCSD_SHUTDOWN_WAIT_MS = 500, and A SHUTDOWN THAT HANGS WAITING FOR A PEER IS
 * WORSE THAN ONE THAT DOES NOT. The bound is what makes this safe; the value is
 * measured rather than picked:
 *
 *   Over all 47 lab captures, every VMS-origin DISCONNECT_REQ is answered by a
 *   DISCONNECT_RSP -- none unanswered -- and not one answer takes 10 ms.
 *   Seconds; re-derive with tools/cluster/scs_disc_measure.py:
 *
 * DISC-CENSUS-LAT: requests=220 answered=220 unanswered=0
 * DISC-CENSUS-LAT: min=0.000006 p50=0.000286 p90=0.001041 p99=0.003459 max=0.006919
 * DISC-CENSUS-LAT: over10ms=0
 *
 * 500 ms is 72x the largest REQ->RSP latency ever observed on our wire, and it
 * is still a fifth of a HELLO period, so a peer that is alive at all has had
 * many chances to answer. EXPLICIT NON-CLAIM, in the same terms spec sec 4(M)
 * uses: 6.919 ms is the largest latency in our captures, not an upper bound. A
 * loaded node could exceed it. The choice rests on the margin -- and, more
 * importantly, on the fact that EXCEEDING IT COSTS NOTHING BUT A LOG LINE. If
 * the deadline expires with connections still open, they are REPORTED
 * (SCSD-W-DISCPEND, counted in disc_shutdown_pending and printed in the exit
 * summary) and the daemon exits anyway. It never blocks on a peer.
 *
 * AND ON THE LAB IT ALWAYS DOES EXPIRE. Those latency figures are VAX-to-VAX.
 * Measured over four lab-2 runs (scs_disc.h, THE LAB VERDICT), a real VAX
 * ANSWERS NO DISCONNECT_REQ OVMX SENDS -- not in 500 ms, and not in the 20 s a
 * capture ran past one. It logs "%PEA0, Inappropriate SCA Control Message"
 * instead. So on today's wire this wait ALWAYS runs to its deadline and always
 * reports SCSD-W-DISCPEND, and raising it would only make shutdown slower. It
 * is kept, and kept short, because the bound is what stops a peer that answers
 * SOMETIMES from turning into a hang -- and because when the refusal is
 * understood, the wait is what will let the dialogue finish.
 *
 * OVMX_SHUTDOWN_WAIT_MS overrides the value; 0 disables the wait entirely (send
 * and go). OVMX_NO_CLEAN_SHUTDOWN=1 skips the whole function -- and, because
 * the kill switch is enforced inside scs_disc_build_*(), would also make every
 * frame here unbuildable even if this check were removed.
 */
#define SCSD_SHUTDOWN_WAIT_MS 500

static unsigned scsd_open_connection_count(void)
{
    unsigned n = 0;
    if (!scsd_cdl_ready) {
        return 0;
    }
    for (unsigned i = 0; i < SCS_CDL_ENTRIES; i++) {
        const struct scs_cdt *cdt = scsd_cdl.entry[i];
        if (cdt == NULL || !cdt->in_use || cdt->listening) {
            continue;
        }
        if (scs_conn_state_of(cdt) != SCS_CONN_CLOSED) {
            n++;
        }
    }
    return n;
}

static long scsd_shutdown_wait_ms(void)
{
    const char *e = getenv("OVMX_SHUTDOWN_WAIT_MS");
    if (e != NULL && e[0] != '\0') {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end != NULL && *end == '\0' && v >= 0) {
            return v;
        }
    }
    return SCSD_SHUTDOWN_WAIT_MS;
}

static void scsd_shutdown_teardown(struct scsd_rx *rx, uint8_t *buf, size_t bufsz)
{
    struct scs_svc_port *port = scsd_svc();
    unsigned driven = 0;
    long wait_ms;
    uint64_t deadline;

    if (port == NULL || !rx->do_connect) {
        return;
    }
    if (!scs_disc_enabled()) {
        log_ts(stdout);
        printf(" SCSD-I-NOCLEANSHUT, OVMX_NO_CLEAN_SHUTDOWN=1 -- no DISCONNECT"
               " frame will be sent and no teardown wait will run (this is the"
               " pre-vms-591 behaviour: exit without disconnecting)\n");
        fflush(stdout);
        return;
    }

    for (int i = 0; i < OVMX_MAX_PEERS; i++) {
        struct peer_state *ps = &rx->peers[i];
        if (ps->pb == NULL) {
            continue;
        }
        struct scsd_disc_emit_ctx e;
        memset(&e, 0, sizeof(e));
        e.sock = rx->sock;
        e.ifindex = rx->ifindex;
        e.ps = ps;
        e.our_hw_mac = rx->our_hw_mac;
        e.our_src_logical = rx->our_src_logical;
        e.matching = 0; /* WE are initiating: [60:62] = 0 (scs_disc.h) */

        struct scs_svc_args a = scsd_disc_args(&e, SCS_REASON_SYSAP_SHUTDOWN);
        struct scsd_svc_mark m = scsd_svc_mark();
        unsigned k = scs_svc_disconnect_all(port, ps->pb, &a);
        scsd_svc_settle(m);
        if (k > 0) {
            driven += k;
            log_ts(stdout);
            printf(" SCSD-I-SHUTDISC, invoked DISCONNECT on %u connection(s) to"
                   " peer %02x:%02x:%02x:%02x:%02x:%02x (%d DISCONNECT_REQ sent,"
                   " reason %u=%s)\n",
                   k, ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                   ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                   e.req_sent, (unsigned)SCS_REASON_SYSAP_SHUTDOWN,
                   scs_reason_name(SCS_REASON_SYSAP_SHUTDOWN));
            fflush(stdout);
        }
    }
    if (driven == 0) {
        return;
    }

    /* Wait, bounded, for the peer to complete the dialogue. The frames are
     * handled by the ordinary receive path -- scsd_disconnect_dialogue() is
     * reached from scsd_handle_frame() exactly as it is during a run. */
    wait_ms = scsd_shutdown_wait_ms();
    deadline = monotonic_ms() + (uint64_t)wait_ms;
    while (wait_ms > 0 && monotonic_ms() < deadline) {
        if (scsd_open_connection_count() == 0) {
            break;
        }
        ssize_t n = recv(rx->sock, buf, bufsz, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue; /* the socket's SO_RCVTIMEO is the inner bound */
            }
            break;
        }
        scsd_handle_frame(rx, buf, n);
    }

    disc_shutdown_pending = scsd_open_connection_count();
    log_ts(stdout);
    if (disc_shutdown_pending == 0) {
        printf(" SCSD-I-SHUTDONE, every connection reached CLOSED before exit\n");
    } else {
        printf(" SCSD-W-DISCPEND, %lu connection(s) had NOT reached CLOSED when"
               " the %ld ms shutdown wait expired -- exiting anyway rather than"
               " blocking on a peer\n", disc_shutdown_pending, wait_ms);
    }
    fflush(stdout);
}

/*
 * scsd_exit_summary - the end-of-run report, including the vms-dd5 connection
 * state-machine accounting and the stuck-connection scan.
 */
static void scsd_exit_summary(struct scsd_rx *rx, FILE *out)
{
    log_ts(out);
    fprintf(out, " SCSD-I-SUMMARY, %ld total 0x6007 frames received on '%s'%s\n",
            rx->total_frames, rx->ifname,
            rx->emit_hello ? " (and HELLO beacon was active)" : "");
    fprintf(out, "  HELLO=%ld SCS-FIXED=%ld SOLICIT=%ld OTHER=%ld RUNT=%ld\n",
            rx->counts[SCS_CLASS_HELLO], rx->counts[SCS_CLASS_SCS_FIXED],
            rx->counts[SCS_CLASS_SOLICIT], rx->counts[SCS_CLASS_OTHER], rx->counts[SCS_CLASS_RUNT]);
    if (rx->emit_hello) {
        fprintf(out, "  HELLO-SENT=%ld\n", rx->hello_sent);
    }
    if (rx->respond) {
        fprintf(out, "  DIRECTED-HELLO-SENT=%ld START-SENT=%ld START-ACK-SENT=%ld"
                " CONNECT-REQ-SENT=%ld CONNECT-RESP-SENT=%ld CREDIT-SENT=%ld RETX-SENT=%ld\n",
                rx->directed_sent, rx->start_sent, rx->start_ack_sent, rx->connect_req_sent, rx->connect_resp_sent,
                rx->credit_sent, rx->retransmit_sent);
        fprintf(out, "  DIR-CONNECT-RESP-SENT=%ld DIR-LOOKUP-RESP-SENT=%ld\n",
                rx->dir_conn_resp_sent, rx->dir_lookup_sent);
        /* vms-dd5: "A state that is entered and never left is detectable." Every
         * connection that is not OPEN at process exit is named here with its
         * CONID, which is the failure shape docs/HANDOFF-vms-2f3.md sec
         * 4M.18/4M.28 records on the peer side and which OVMX previously could
         * not represent at all. */
        fprintf(out, "  CONN-FSM: transitions=%lu illegal-events=%lu"
                " actions-required-but-not-emitted=%lu\n",
                conn_transitions, conn_illegal_events, conn_unemitted_actions);
        /* vms-6b3: p. 2-26's reason code as received. If the kill switch is
         * set, say so on the same line -- a run log must never read as "no peer
         * gave a reason" when the truth is "decoding was off". */
        fprintf(out, "  CONN-REASON: rej/disconn frames decoded=%lu"
                " carrying a nonzero reason=%lu%s\n",
                conn_reason_seen, conn_reason_nonzero,
                scs_reason_enabled() ? "" : " (OVMX_NO_REASON_CODE set --"
                                            " DECODING WAS OFF)");
        /* vms-591: the Figure 2-16 teardown, both halves. Printed
         * unconditionally: the number that matters is req-sent, because before
         * this item it was structurally 0 -- OVMX only ever reacted. If the
         * kill switch is set, say so on the same line, so a run log can never
         * read as "the peer never disconnected" when the truth is "we could
         * not have answered". */
        fprintf(out, "  DISCONNECT: req-recv=%lu rsp-sent=%lu req-sent=%lu"
                " rsp-recv=%lu closed=%lu simultaneous=%lu"
                " still-open-at-exit=%lu%s\n",
                disc_req_recv, disc_rsp_sent, disc_req_sent, disc_rsp_recv,
                disc_closed, disc_simultaneous, disc_shutdown_pending,
                scs_disc_enabled() ? "" : " (OVMX_NO_CLEAN_SHUTDOWN set --"
                                          " NO DISCONNECT FRAME WAS SENT)");
        /* vms-abc: the p. 2-31 message guarantees. Printed unconditionally --
         * "0 gaps, 0 breaks" is the answer on a healthy run and it must be
         * VISIBLE, not inferred from the absence of a warning. If the kill
         * switch is set, say so on the same line: a run log must never read as
         * "no circuit needed breaking" when the truth is "breaking was off". */
        /* sends-refused is the OTHER half of the guarantee, and the half a log
         * scan would otherwise miss: the SCSD-E-NOVC line is latched to one per
         * circuit-break, so the only place the TOTAL number of frames p. 2-31
         * kept off the wire appears is here. */
        fprintf(out, "  VC-GUARANTEES: seq-gaps=%lu vc-breaks=%lu"
                " connections-broken=%lu sysap-vc-loss-notifications=%lu"
                " sends-refused=%lu%s\n",
                vc_seq_gaps, vc_breaks, vc_conns_broken,
                sysap_vc_loss_notifications, vc_sends_refused,
                scs_vc_break_enabled() ? "" : "  [" SCS_VC_NO_BREAK_ENV
                                              " SET -- circuits were NOT broken]");
        (void)scs_conn_report_stuck(&scsd_cdl, out);
        /* vms-17f: the p. 2-20/2-21 open transitions this run took, and the
         * departure sweep's work. PB-OPEN-REFRESHED is the p. 2-21 Note firing
         * -- a node that was here, left, and came back. */
        fprintf(out, "  PB-OPEN: new-sb=%lu refreshed=%lu existing-sb=%lu errors=%lu"
                " abandoned-masquerade=%lu\n",
                pb_open_results[SCS_OPEN_NEW_SB],
                pb_open_results[SCS_OPEN_EXISTING_REFRESHED],
                pb_open_results[SCS_OPEN_EXISTING_SB], pb_open_errors,
                pb_open_masquerades);
        fprintf(out, "  PEER-DEPARTURES=%lu CONNECTIONS-LOST=%lu TEARDOWNS-REFUSED=%lu"
                " LISTEN-TIMEOUT-MS=%llu%s\n",
                peer_departures, depart_connections_lost, depart_refusals,
                (unsigned long long)scs_depart_listen_timeout_ms(),
                scs_depart_enabled() ? "" : " (OVMX_NO_PEER_DEPART: sweep DISABLED)");
        /* vms-b1d: the p. 2-42 datagram buffer account. A datagram discard is
         * silent ON THE WIRE by design; it must not also be invisible HERE, or
         * the facility is indistinguishable from one that does nothing (INV-6).
         * These lines print zeros today and that is the honest state: nothing
         * in this daemon routes a received datagram through
         * scs_dgram_cdl_deliver(), so no buffer has ever been taken. See the
         * reachability note in scs_dgram.h. */
        (void)scs_dgram_report(&scsd_cdl, out);
        fprintf(out, "  CM-CONFIG-FRAMES=%ld CM-RESPONSES-SENT=%ld PADDED-HELLO-SENT=%ld\n",
                rx->cm_config_frames, rx->cm_response_sent, rx->padded_sent);
        /* vms-7fe: the SDIR queue as it actually behaved this run. The two
         * refusal counts are the OVMX-chosen reply codes; BUSY is 0 for the
         * reason scs_sdir.h DESIGN CHOICE 3 states -- test_scsd_wire.c measures
         * that over the daemon's own receive path, and printing it here is how
         * a LIVE run stays falsifiable as well. */
        {
            const struct scs_sdir_queue *q = scs_svc_sdir(scsd_svc());
            fprintf(out, "  SDIR listening=%u", scs_sdir_count(q));
            for (const struct scs_sdir *s = scs_sdir_first(q); s != NULL;
                 s = scs_sdir_next(s)) {
                fprintf(out, " [%s conid=0x%08X %s]", s->sysap, s->conid,
                        scs_sdir_state_name(s->state));
            }
            fprintf(out,
                    " scans=%lu hits=%lu delivered=%lu retransmits=%lu"
                    " connect_scans=%lu no-such-sysap-sent=%lu busy-sent=%lu"
                    " refusals-unsent=%lu enabled=%s\n",
                    q->scans, q->hits, q->delivered, q->retransmits,
                    sdir_connect_scans, sdir_no_such_sysap, sdir_busy_replies,
                    sdir_refusals_unsent, scs_sdir_enabled() ? "YES" : "no (OVMX_NO_SDIR)");
        }
        /*
         * vms-66f: the p. 2-50 poller, as numbers. `skipped` is the interesting
         * one on a 2-node join: once OVMX's connection manager has its
         * VMS$VAXcluster connection, p. 2-50 disables polling for that pair, so
         * a run with connects=0 and skipped>0 is the RULE working, not the
         * poller failing. Reading connects=0 alone would get that backwards.
         */
        fprintf(out,
                "  POLLER SCS$DIR_LOOKUP interval=%us state=%s cycles=%lu"
                " completed=%lu abandoned=%lu disc-unclosed=%lu connect-refused=%lu"
                " last-connect-status=%s last-refused-node=%02x:%02x:%02x:%02x:%02x:%02x connects-sent=%ld inquiries-sent=%ld answers=%ld yes=%lu no=%lu"
                " unknown=%lu unsolicited=%lu notified=%lu skipped=%lu"
                " forced-cdt-release=%lu enabled=%s\n",
                scs_poll_interval_sec(), scs_poll_state_name(scsd_poller.state),
                scsd_poller.cycles_started, scsd_poller.cycles_completed,
                scsd_poller.cycles_abandoned, scsd_poller.disconnects_unclosed,
                scsd_poller.connect_refused,
                scs_svc_status_name(scsd_poller.last_connect_status),
                scsd_poller.last_refused_node[0], scsd_poller.last_refused_node[1],
                scsd_poller.last_refused_node[2], scsd_poller.last_refused_node[3],
                scsd_poller.last_refused_node[4], scsd_poller.last_refused_node[5],
                rx->poll_connect_sent,
                rx->poll_inquiry_sent, rx->poll_answers, scsd_poller.answers_yes,
                scsd_poller.answers_no, scsd_poller.answers_unknown,
                scsd_poller.answers_unsolicited, scsd_poller.notifications,
                scsd_poller.skipped_disabled, scsd_poller.descriptors_forced,
                scs_poll_enabled() ? "YES" : "no (ships OFF; OVMX_PROCESS_POLLER=1 opts in,"
                                     " OVMX_NO_PROCESS_POLLER=1 forces off)");
        for (int i = 0; i < OVMX_MAX_PEERS; i++) {
            if (rx->peers[i].pb == NULL) {
                continue;
            }
            const uint8_t *pa = rx->peers[i].pb->remote_port_addr;
            fprintf(out,
                    "  PEER %02x:%02x:%02x:%02x:%02x:%02x vc=%s channel=%s directed_replies=%ld"
                    " incarnation=%u start_replied=%d start_acked=%d dir_connected=%s"
                    " dir_lookups=%ld connect_sent=%d connected=%s"
                    " rx->credit_sent=%ld retx=%u remote_conid=0x%08X"
                    " cm_config=%s cm_responses=%ld sysap_send=%u sysap_recv=%u"
                    " padded_replies=%ld padded_init=%d peer_padded_sca=%u"
                    " conn[dir=%s member=%s joiner=%s]\n",
                    pa[0], pa[1], pa[2], pa[3], pa[4], pa[5],
                    scs_vc_state_name(rx->peers[i].pb->vc_state),
                    rx->peers[i].channel_up ? "UP" : "down", rx->peers[i].directed_replies,
                    rx->peers[i].incarnation,
                    rx->peers[i].start_replied, rx->peers[i].start_acked,
                    rx->peers[i].dir_connected ? "YES" : "no", rx->peers[i].dir_lookups_answered,
                    rx->peers[i].connect_sent, rx->peers[i].connected ? "YES" : "no",
                    rx->peers[i].credit_sent, rx->peers[i].vc.retransmit_count,
                    rx->peers[i].remote_conid,
                    rx->peers[i].cm_config_sent ? "YES" : "no", rx->peers[i].cm_responses,
                    rx->peers[i].sysap_send, rx->peers[i].sysap_recv,
                    rx->peers[i].padded_replies, rx->peers[i].padded_initiated,
                    rx->peers[i].peer_padded_sca,
                    /* "untracked" is NOT a state: it means no CDT was ever bound
                     * for that connection (machine off, or the node-global
                     * Con.ID slot belonged to another peer). Do not read it as
                     * CLOSED. */
                    rx->peers[i].cdt_dir ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_dir))
                                     : "untracked",
                    rx->peers[i].cdt_member
                        ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_member))
                        : "untracked",
                    rx->peers[i].cdt_joiner
                        ? scs_conn_state_name(scs_conn_state_of(rx->peers[i].cdt_joiner))
                        : "untracked");
        }
    }
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

    /* --- vms-b62: resolve HELLO identity. vms-abc removed the send-side
     * sockaddr_ll that used to live here: send_frame_channel() builds it. --- */
    struct scs_hello_params hello_params;
    /* vms-fb1: the receive-dispatch seam's state, including every run counter.
     * Static: the daemon has exactly one, and it keeps the 1514-byte padded
     * HELLO buffer off main()'s stack frame as the old `static pframe` did. */
    static struct scsd_rx rx;
    memset(&rx, 0, sizeof(rx));
    memset(&hello_params, 0, sizeof(hello_params));

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
    /* vms-dd5: "The CDL is allocated during system initialization" (p. 2-30).
     * The transition log goes to stdout beside every other SCSD-I- line so a run
     * log carries the state history of every connection; if the machine is
     * disabled nothing is initialized and nothing is logged. */
    if (scs_conn_fsm_enabled()) {
        scs_cdl_init(&scsd_cdl);
        scsd_cdl_ready = 1;
        scs_conn_set_log(stdout);
        /* vms-561: bind the service port and declare p. 2-22's "list of
         * listening SYSAPs" -- see scsd_svc(), which does both. Called here so
         * a run log shows the port up before the first frame. */
        (void)scsd_svc();
    } else {
        /* Descriptorless (scs_svc.h): the services still emit, nothing is
         * recorded. scsd_svc_port keeps a NULL CDL so that is what happens. */
        log_ts(stderr);
        fprintf(stderr, " SCSD-I-CONNFSMOFF, OVMX_NO_CONN_FSM is set:"
                        " no connection descriptors, no state machine, no"
                        " transition log, no stuck-connection report\n");
        fflush(stderr);
    }
    /* vms-17f: say once, at startup, exactly what the departure policy is, so a
     * capture is never read as a spontaneous departure (or as the absence of
     * one). A timeout at or below the longest peer silence ever measured on a
     * HEALTHY link is legal -- the lab harness forces one to make a departure
     * happen inside a short run -- but it is never silent. */
    if (!scs_depart_enabled()) {
        log_ts(stderr);
        fprintf(stderr, " SCSD-I-DEPARTOFF, OVMX_NO_PEER_DEPART is set: no peer is"
                        " ever declared departed, no path block is closed and no"
                        " peer slot is released -- the pre-vms-17f behaviour\n");
        fflush(stderr);
    } else {
        uint64_t lt = scs_depart_listen_timeout_ms();
        log_ts(stdout);
        printf(" SCSD-I-DEPARTON, peer listen timeout %llums%s\n",
               (unsigned long long)lt,
               lt <= (uint64_t)SCS_DEPART_HEALTHY_SILENCE_MAX_MS
                   ? " -- AT OR BELOW the longest silence measured on a healthy"
                     " link: departures in this run may be artificial"
                   : "");
        fflush(stdout);
    }
    static const uint8_t lab_nonce[4] = SCS_HELLO_LAB_NONCE_BYTES;
    uint8_t our_hw_mac[6];
    memset(our_hw_mac, 0, sizeof(our_hw_mac));
    uint8_t our_src_logical[6]; /* vms-9f3: OVMX's cluster-LOGICAL LAVC addr (abs 24) */
    memset(our_src_logical, 0, sizeof(our_src_logical));
    /* vms-fb1: every run counter now lives in `rx` so scsd_exit_summary() can
     * report them from a function a test can call. */

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

    static uint8_t buf[SCA_FRAME_MAX];
    struct timespec last_hello = {0, 0};

    /* vms-4071: everything the VC formation state machine needs to put its
     * actions on the wire. Pointers, so the HW MAC resolved in the emit_hello
     * block above stays live. */
    struct scsd_vc_ctx vc_ctx;
    vc_ctx.sock = sock;
    vc_ctx.ifindex = (int)ifindex;
    vc_ctx.hw_mac = our_hw_mac;
    vc_ctx.src_logical = our_src_logical;
    vc_ctx.scssystemid = ovmx_scssystemid;
    vc_ctx.node_name = ovmx_node;
    vc_ctx.cfg = &scs_cfg;

    /* vms-fb1: wire the receive-dispatch seam. Every field below is a pointer
     * to state main() owns; the handler reaches ALL of it through `rx` and
     * holds no state of its own, which is what lets a test stand one up. */
    rx.sock = sock;
    rx.ifindex = (int)ifindex;
    rx.our_hw_mac = our_hw_mac;
    rx.our_src_logical = our_src_logical;
    rx.lab_nonce = lab_nonce;
    rx.hello_params = &hello_params;
    rx.cfg = &scs_cfg;
    rx.pdt = &scs_pdt;
    rx.peers = peers;
    rx.vc_ctx = &vc_ctx;
    rx.ifname = ifname;
    rx.respond = respond;
    rx.do_connect = do_connect;
    rx.emit_hello = emit_hello;

    while (!g_stop) {
        if (emit_hello) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - last_hello.tv_sec >= hello_interval) {
                /* vms-abc: the beacon's build-and-send is scsd_hello_beacon_emit()
                 * -- see that function. Only the CADENCE is here. It used to be
                 * inline AND it used to call sendto() on `sock` directly, which
                 * put a real send site (it increments rx.hello_sent and is
                 * reported in the exit summary) in NEITHER half of a SEND SITE
                 * TABLE that claims to list every sender, inside the one
                 * function no test can reach. The wire is unchanged: the same
                 * frame goes to the same multicast MAC on the same interface at
                 * the same times, because send_frame_raw() builds the same
                 * sockaddr_ll the deleted `hello_dst` held. */
                (void)scsd_hello_beacon_emit(&rx, &hello_params, sock, (int)ifindex);
                last_hello = now;
            }
        }

        /* --- vms-17f: the peer departure sweep. A peer silent past the listen
         * timeout has its Path Block torn down (p. 2-28 order) and its slot
         * released, so that if it comes back it re-drives formation and its
         * open takes the p. 2-21 REFRESH. WIRE-VISIBLE; OVMX_NO_PEER_DEPART=1
         * disables it entirely. See the block above scsd_peer_departure_sweep. */
        (void)scsd_peer_departure_sweep(&rx, monotonic_ms());

        /* --- vms-4071: the p. 2-14 formation reissue timer. "whenever a port
         * driver sends a START or a STACK ... it starts a timer and expects a
         * response. If the timer expires before any response is received ...
         * SCA requires the port driver to reissue the START or STACK (whichever
         * it last sent)", bounded by an "operating system dependent retry
         * limit" after which "formation of the virtual circuit is to be
         * abandoned". THIS IS NEW ON THE WIRE and fires only under loss; the
         * OVMX_VC_NO_RETRY_LIMIT=1 kill-switch restores unbounded retry. */
        if (do_connect) {
            uint64_t now_ms = monotonic_ms();
            unsigned retry_limit = scs_vc_retry_limit();
            for (int i = 0; i < OVMX_MAX_PEERS; i++) {
                struct peer_state *ps = &peers[i];
                if (ps->pb == NULL) {
                    continue;
                }
                if (!scs_vc_fsm_timer_expired(ps->pb, now_ms, SCS_VC_FORMATION_TIMEOUT_MS)) {
                    continue;
                }
                enum scs_vc_action act = scs_vc_fsm_timeout(ps->pb, now_ms, retry_limit);
                if (act == SCS_VC_ACT_SEND_START || act == SCS_VC_ACT_SEND_STACK) {
                    if (scsd_vc_emit(&vc_ctx, ps, act)) {
                        rx.start_sent++;
                        ps->start_replies++;
                        log_ts(stdout);
                        printf(" SCSD-I-VCREISSUE, formation timer expired -- %s to peer"
                               " %02x:%02x:%02x:%02x:%02x:%02x (attempt %u, VC %s)\n",
                               scs_vc_action_name(act),
                               ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                               ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5],
                               ps->pb->fsm.retries, scs_vc_state_name(ps->pb->vc_state));
                        fflush(stdout);
                    }
                }
                /* peer_round2_seen=0: a timer expiry is not a peer frame. */
                scsd_vc_settle(&vc_ctx, ps, act, 0, &rx.start_ack_sent);
            }
        }

        /* --- vms-66f: the p. 2-50 SCS process poll. "A VMS system will poll
         * each other node that it can see approximately once, but not more than
         * once, during each such interval" -- so the set of nodes it can see is
         * refreshed here, from the ONE authority on whether a circuit is open
         * (CONFIG_PATH, p. 2-47), and scs_poll_tick() decides whether anything
         * happens. WIRE-VISIBLE and NEW: this is the first outbound
         * SCS$DIRECTORY traffic OVMX has ever emitted. OVMX_NO_PROCESS_POLLER=1
         * suppresses it entirely. */
        if (do_connect) {
            struct scs_poller *poller = scsd_poll(&rx);
            for (int i = 0; i < OVMX_MAX_PEERS; i++) {
                struct peer_state *ps = &peers[i];
                if (ps->pb == NULL) {
                    continue;
                }
                const uint8_t *sys = ps_sys_addr(ps);
                /*
                 * ASK THE QUESTION CONNECT WILL ASK, not a similar one.
                 * The first revision of this loop tested the Path Block's own
                 * vc_state and MEASURED WRONG on the lab wire: the VC formation
                 * machine reaches OPEN one received frame BEFORE scs_pb_open()
                 * moves the Path Block off the PDT formative queue, so for that
                 * window the circuit is "open" and CONFIG_SYS cannot reach it.
                 * The poller duly registered the node, CONNECT ran the p. 2-47
                 * selection, and the run ended with connect-refused=1
                 * last-connect-status=NOVC and not one frame sent. Using
                 * scs_config_select_vc() -- the selection itself -- closes the
                 * gap by construction: if it answers, CONNECT will too.
                 */
                if (scs_config_select_vc(rx.cfg, sys) != NULL) {
                    (void)scs_poll_add_node(poller, sys);
                } else {
                    scs_poll_drop_node(poller, sys);
                }
            }
            scs_poll_tick(poller, monotonic_ms());
        }

        /* --- vms-691: retransmit OVMX's own unacked sequenced message (the
         * connect-request) on timeout, so a dropped CONNECT-REQUEST does not
         * stall the handshake. Rate-capped (VC_RETRANSMIT_MAX) to respect the
         * reply-amplification guard. Only while the connect is still unbound. */
        /* vms-abc moved the body into scsd_retransmit_tick() so a test can
         * reach it, and made retransmit exhaustion break the circuit. */
        scsd_retransmit_tick(&rx, monotonic_ms());

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
        scsd_handle_frame(&rx, buf, n);
    }

    /* vms-591: disconnect before vanishing. Bounded; see the function. */
    scsd_shutdown_teardown(&rx, buf, sizeof(buf));

    scsd_exit_summary(&rx, stderr);

    close(sock);
    return 0;
}
