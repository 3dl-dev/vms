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
#include "scs_connect.h"
#include "scs_depart.h"
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

/* Counters for the end-of-run summary. */
static unsigned long conn_transitions = 0;
static unsigned long conn_illegal_events = 0;
static unsigned long conn_unemitted_actions = 0;

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
static unsigned long peer_departures = 0;
static unsigned long depart_connections_lost = 0;
static unsigned long depart_refusals = 0;

/*
 * conn_bind - get (or create) the CDT for one of this peer's connections,
 * claiming the EXACT Con.ID OVMX puts on the wire for it. Returns NULL when the
 * machine is off, when the CDL slot is taken by another peer, or on any
 * allocation failure -- every scs_conn_fsm_step() call below tolerates NULL.
 */
static struct scs_cdt *conn_bind(struct peer_state *ps, struct scs_cdt **slot,
                                 uint32_t local_conid, const char *local_sysap,
                                 const char *remote_sysap)
{
    if (ps == NULL || slot == NULL) {
        return NULL;
    }
    if (*slot != NULL) {
        return *slot;
    }
    if (!scs_conn_fsm_enabled() || !scsd_cdl_ready) {
        return NULL; /* THE KILL SWITCH: no descriptors, hence no state, no log */
    }
    struct scs_cdt *cdt = scs_cdl_alloc_conid(&scsd_cdl, local_conid, local_sysap,
                                              remote_sysap, ps->pb);
    if (cdt == NULL) {
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-W-CONNSLOT, CDL slot for Con.ID 0x%08X (%s) is already"
                " claimed -- OVMX's Con.IDs are node-global, so this peer's"
                " connection is NOT tracked by the connection state machine\n",
                (unsigned)local_conid, local_sysap);
        fflush(stderr);
        return NULL;
    }
    scs_conn_fsm_init(cdt);
    *slot = cdt;
    return cdt;
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
        return send_frame_to(ctx->sock, ctx->ifindex, ps_port_addr(ps),
                             aframe, sizeof(aframe)) > 0 ? 1 : 0;
    }
    if (act == SCS_VC_ACT_SEND_START || act == SCS_VC_ACT_SEND_STACK) {
        sp.config_round = (act == SCS_VC_ACT_SEND_START) ? 0 : 1;
        uint8_t sframe[SCS_START_FRAME_LEN];
        if (scs_start_build(&sp, sframe) != 0) {
            return 0;
        }
        return send_frame_to(ctx->sock, ctx->ifindex, ps_port_addr(ps),
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
 * REACHABILITY AS MEASURED (vms-17f), so a green test run is not misread. Each
 * line is a counter this function increments, and each is asserted by a named
 * case in tests/vmsscs/test_scsd_wire.c that drives scsd_handle_frame():
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
 */
static void scsd_vc_on_open(const struct scsd_vc_ctx *ctx, struct peer_state *ps)
{
    /* vms-246: the phase-2 config-round counters are SEPARATE from the SCS VC;
     * both sides reset the VC to send_seq=1/recv_seq=0 when START completes. */
    scs_vc_reset_seq(&ps->vc);
    enum scs_open_result open_res = scs_pb_open(ctx->cfg, ps->pb);
    if (open_res >= SCS_OPEN_NEW_SB && open_res <= SCS_OPEN_EXISTING_REFRESHED) {
        pb_open_results[(int)open_res]++;
    } else {
        pb_open_errors++;
    }
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
    if (vc == NULL || !scs_config_path(vc, &path)) {
        log_ts(stderr);
        fprintf(stderr,
                " SCSD-E-NOVC, no OPEN virtual circuit to peer"
                " %02x:%02x:%02x:%02x:%02x:%02x -- CONNECT-REQUEST NOT sent"
                " (p. 2-47: CONNECT needs an open circuit; none was named and"
                " CONFIG_SYS found none for this node)\n",
                ps_port_addr(ps)[0], ps_port_addr(ps)[1], ps_port_addr(ps)[2],
                ps_port_addr(ps)[3], ps_port_addr(ps)[4], ps_port_addr(ps)[5]);
        fflush(stderr);
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
    if (ps->joiner_req_seq == 0) {
        ps->joiner_req_seq = scs_seq_advance(&ps->vc.seq); /* allocate once */
    }
    cp.send_seq = ps->joiner_req_seq; /* retransmits reuse the same seq */
    cp.incarnation = ps->incarnation;
    uint8_t cframe[SCS_CONNECT_FRAME_LEN];
    if (scs_connect_build_request(&cp, cframe) == 0 &&
        send_frame_to(sock, ifindex, path.remote_port_addr, cframe, sizeof(cframe)) > 0) {
        ps->joiner_connect_sent = 1;
        clock_gettime(CLOCK_MONOTONIC, &ps->last_joiner_req);
        scs_vc_record_sent(&ps->vc, cp.send_seq, monotonic_ms());
        /* vms-dd5: OVMX is the SOURCE on this connection -- Figure 2-14's
         * NODE_1 column. "o SCS SENDS 'CONNECT_REQ' TO NODE_2 / o CONN STATE =
         * 'CONNECT SENT'". The frame just sent IS the CONNECT_REQ (remote
         * Con.ID 0, spec sec 4g phase 4), so this is a GROUNDED mapping. A
         * retransmit re-enters the same state through the labeled OVMX
         * retransmit row rather than scoring an illegal event. */
        conn_step(conn_bind(ps, &ps->cdt_joiner, OVMX_JOINER_CONID, "VMS$VAXcluster",
                            "VMS$VAXcluster"),
                  SCS_CONN_EV_SVC_CONNECT, "CONNECT-REQUEST");
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
    long cm_config_frames;   /* vms-224: op 0x14/0x01/0x02 CM config frames sent */
    long cm_response_sent;   /* vms-224: 0x81 responses to member 0x03/0x05 txns */
    long padded_sent;        /* vms-9f3: padded directed HELLOs sent (sec 4k) */

    /* vms-9f3: reusable padded-HELLO send buffer (up to 1514B on the wire),
     * held here to keep it off the handler's stack frame. */
    uint8_t pframe[SCS_HELLO_PADDED_MAX_FRAME];
};

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

            if (scs_vc_owes_credit(peer_send_seq)) {
                scs_vc_note_recv(&ps->vc, peer_send_seq);
                uint8_t cframe[SCS_CREDIT_FRAME_LEN];
                if (scs_vc_build_credit_for(&ps->vc, ps_port_addr(ps), rx->our_hw_mac,
                                            rx->our_src_logical, ps_sys_addr(ps), cframe) == 0 &&
                    send_frame_to(rx->sock, rx->ifindex, ps_port_addr(ps), cframe,
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
                        send_frame_to(rx->sock, rx->ifindex, ps_port_addr(ps), rframe,
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
            send_frame_to(rx->sock, rx->ifindex, src_mac, ackframe, sizeof(ackframe)) > 0) {
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
                send_frame_to(rx->sock, rx->ifindex, src_mac, dframe, sizeof(dframe)) > 0) {
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
                send_frame_to(rx->sock, rx->ifindex, src_mac, rx->pframe, plen) > 0) {
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
        if (rx->do_connect && ps->start_acked && ps->joiner_connect_sent &&
            !ps->joiner_connected) {
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
                conn_step(tgt, cev, NULL);
            }
        }

        if (rconid == OVMX_JOINER_CONID && lconid != 0) {
            struct peer_state *ps = peer_find_or_add(rx->cfg, rx->pdt, rx->peers, src_mac);
            if (ps != NULL && !ps->joiner_connected) {
                ps->joiner_remote_conid = lconid;
                ps->joiner_connected = 1;
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

            if (dv.is_dir_connect_request && !ps->dir_connected) {
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
                struct scs_cdt *dcdt = conn_bind(ps, &ps->cdt_dir, SCS_DIR_OVMX_CONID,
                                                 "SCS$DIRECTORY", "SCS$DIRECTORY");
                scs_cdt_set_remote_conid(dcdt, ps->dir_remote_conid);
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

                dp.recv_ack = ps->vc.seq.recv_seq;
                dp.send_seq = scs_seq_advance(&ps->vc.seq);
                uint8_t eframe[SCS_DIR_ECHO_FRAME_LEN];
                if (scs_dir_build_connect_echo(&dp, eframe) == 0) {
                    send_frame_to(rx->sock, rx->ifindex, ps_port_addr(ps), eframe, sizeof(eframe));
                    /* The op=1 CONNECT-ECHO is spec sec 4(h)(1)'s SCA23:
                     * "VAX2 echoes VAX1's handle, its own not yet assigned"
                     * -- an acknowledgement that cannot yet carry a handle,
                     * which is exactly Figure 2-14's CONNECT_RSP. Step:
                     * CLOSED --RCV_CONNECT_REQ--> CONNECT REC, sending it. */
                    conn_step(dcdt, SCS_CONN_EV_RCV_CONNECT_REQ, "op=1 CONNECT-ECHO");
                }

                dp.recv_ack = ps->vc.seq.recv_seq;
                dp.send_seq = scs_seq_advance(&ps->vc.seq);
                uint8_t rframe[SCS_DIR_RESP_FRAME_LEN];
                if (scs_dir_build_connect_response(&dp, rframe) == 0 &&
                    send_frame_to(rx->sock, rx->ifindex, ps_port_addr(ps), rframe,
                                  sizeof(rframe)) > 0) {
                    ps->dir_connected = 1;
                    rx->dir_conn_resp_sent++;
                    /* The op=2 CONNECT-RESPONSE is spec sec 4(h)(1)'s
                     * SCA25: "VAX2 supplies its own handle -- pair now
                     * bound", which is Figure 2-14's ACCEPT_REQ ("TARGET
                     * SYSAP INVOKES ACCEPT / SCS SENDS 'ACCEPT_REQ'").
                     * Step: CONNECT REC --SVC_ACCEPT--> ACCEPT SENT. */
                    conn_step(dcdt, SCS_CONN_EV_SVC_ACCEPT, "op=2 CONNECT-RESPONSE");
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
                lp.affirmative = (memcmp(dv.name, "VMS$VAXcluster", 14) == 0);
                uint8_t lframe[SCS_DIR_LOOKUP_FRAME_LEN];
                if (scs_dir_build_lookup_response(&lp, lframe) == 0 &&
                    send_frame_to(rx->sock, rx->ifindex, ps_port_addr(ps), lframe,
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
            uint8_t rframe[SCS_CONNECT_FRAME_LEN];
            if (scs_connect_build_response(&cp, rframe) == 0 &&
                send_frame_to(rx->sock, rx->ifindex, ps_port_addr(ps), rframe, sizeof(rframe)) > 0) {
                ps->remote_conid = v.local_conid;
                rx->connect_resp_sent++;
                int first = !ps->connected;
                ps->connected = 1;
                /* vms-dd5: OVMX is the TARGET here (the member opened this
                 * connection). Two steps for ONE emitted frame, and the
                 * asymmetry is the point:
                 *   RCV_CONNECT_REQ -- the member's 0x4b with destination
                 *     Con.ID 0, message type 0 (spec sec 4g phase 4, golden
                 *     frame 47). The machine requires a CONNECT_RSP here and
                 *     OVMX BUILDS NONE -- note this is an OVMX gap, not a
                 *     wire gap: the real VAX does send that frame (the
                 *     66-byte message-type-1 class, 16 of 16 dialogues, spec
                 *     sec 4(h)(1a)). Passing NULL makes conn_step LOG the
                 *     unemitted action every run instead of quietly
                 *     pretending a frame went out.
                 *   SVC_ACCEPT -- the 110-byte CONNECT-RESPONSE we just sent
                 *     carries BOTH Con.IDs (golden frame 50) and message
                 *     type 2, which sec 4(h)(1a) grounds as ACCEPT_REQ. */
                struct scs_cdt *mcdt = conn_bind(ps, &ps->cdt_member, OVMX_LOCAL_CONID,
                                                 "VMS$VAXcluster", "VMS$VAXcluster");
                scs_cdt_set_remote_conid(mcdt, v.local_conid);
                if (first) {
                    conn_step(mcdt, SCS_CONN_EV_RCV_CONNECT_REQ, NULL);
                    conn_step(mcdt, SCS_CONN_EV_SVC_ACCEPT, "0x4b CONNECT-RESPONSE");
                } else {
                    /* A RETRANSMITTED CONNECT-REQUEST, which the member
                     * sends until it accepts our answer and which we
                     * deliberately re-answer (vms-c6d). The labeled OVMX
                     * retransmit row keeps ACCEPT SENT and repeats the
                     * ACCEPT_REQ -- which is the frame we did just send, so
                     * nothing is reported unemitted here. */
                    conn_step(mcdt, SCS_CONN_EV_RCV_CONNECT_REQ, "0x4b CONNECT-RESPONSE");
                }
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
               " %u MFREEQ buffer(s) returned; its system block STAYS in the"
               " configuration queue (p. 2-17) so a rejoin can refresh it (p. 2-21)."
               " Configuration queue now holds %u system block(s)\n",
               gone_mac[0], gone_mac[1], gone_mac[2], gone_mac[3], gone_mac[4],
               gone_mac[5], (unsigned long long)silent_ms,
               (unsigned long long)timeout_ms, st.connections_lost,
               st.waiters_flushed, st.mfreeq_reclaimed, scs_config_sb_count(rx->cfg));
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
        (void)scs_conn_report_stuck(&scsd_cdl, out);
        /* vms-17f: the p. 2-20/2-21 open transitions this run took, and the
         * departure sweep's work. PB-OPEN-REFRESHED is the p. 2-21 Note firing
         * -- a node that was here, left, and came back. */
        fprintf(out, "  PB-OPEN: new-sb=%lu refreshed=%lu existing-sb=%lu errors=%lu\n",
                pb_open_results[SCS_OPEN_NEW_SB],
                pb_open_results[SCS_OPEN_EXISTING_REFRESHED],
                pb_open_results[SCS_OPEN_EXISTING_SB], pb_open_errors);
        fprintf(out, "  PEER-DEPARTURES=%lu CONNECTIONS-LOST=%lu TEARDOWNS-REFUSED=%lu"
                " LISTEN-TIMEOUT-MS=%llu%s\n",
                peer_departures, depart_connections_lost, depart_refusals,
                (unsigned long long)scs_depart_listen_timeout_ms(),
                scs_depart_enabled() ? "" : " (OVMX_NO_PEER_DEPART: sweep DISABLED)");
        fprintf(out, "  CM-CONFIG-FRAMES=%ld CM-RESPONSES-SENT=%ld PADDED-HELLO-SENT=%ld\n",
                rx->cm_config_frames, rx->cm_response_sent, rx->padded_sent);
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

    /* --- vms-b62: resolve HELLO identity + build the send-side sockaddr --- */
    struct scs_hello_params hello_params;
    struct sockaddr_ll hello_dst;
    /* vms-fb1: the receive-dispatch seam's state, including every run counter.
     * Static: the daemon has exactly one, and it keeps the 1514-byte padded
     * HELLO buffer off main()'s stack frame as the old `static pframe` did. */
    static struct scsd_rx rx;
    memset(&rx, 0, sizeof(rx));
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
    /* vms-dd5: "The CDL is allocated during system initialization" (p. 2-30).
     * The transition log goes to stdout beside every other SCSD-I- line so a run
     * log carries the state history of every connection; if the machine is
     * disabled nothing is initialized and nothing is logged. */
    if (scs_conn_fsm_enabled()) {
        scs_cdl_init(&scsd_cdl);
        scsd_cdl_ready = 1;
        scs_conn_set_log(stdout);
    } else {
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
                uint8_t frame[SCS_HELLO_FRAME_LEN];
                hello_params.timer_tick = hello_timer_tick100(); /* vms-9f3: live 100ns tick */
                if (scs_hello_build_frame(&hello_params, frame) == 0) {
                    ssize_t sent = sendto(sock, frame, sizeof(frame), 0,
                                           (struct sockaddr *)&hello_dst, sizeof(hello_dst));
                    if (sent < 0) {
                        fprintf(stderr, "SCSD-E-SENDFAIL, sendto failed: %s\n", strerror(errno));
                    } else {
                        rx.hello_sent++;
                        log_ts(stdout);
                        printf(" SCSD-I-HELLOSENT, node='%s' seq=%ld bytes=%zd\n",
                               hello_params.node_name, rx.hello_sent, sent);
                        fflush(stdout);
                    }
                }
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
                    rx.retransmit_sent++;
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
        scsd_handle_frame(&rx, buf, n);
    }

    scsd_exit_summary(&rx, stderr);

    close(sock);
    return 0;
}
