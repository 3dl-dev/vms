/*
 * dnet_engine.h - DECnet Phase IV routing ENGINE (rung 1 of the engine lane,
 *                 rd vms-449d / epic vms-30e).
 *
 * This is the piece that MOVES FRAMES. The three landed codecs are pure,
 * engine-agnostic byte/state libraries -- they build and decode a HELLO
 * (dnet_hello.{c,h}), and drive the per-neighbour adjacency lifecycle
 * (dnet_adjacency.{c,h}) -- but none of them touches a socket, a clock, or the
 * wire. This engine binds them into a live routing endnode: it assembles the
 * full on-wire Ethernet HELLO frame, drives the emission cadence (T3) and the
 * adjacency listen timer on an injected monotonic clock, and consumes received
 * frames to keep the neighbour table.
 *
 * OPERATOR RULING (2026-08-31, rd vms-a1c): the DECnet engine is Option B --
 * a USERSPACE NSP/routing engine over AF_PACKET SOCK_RAW, forking the proven
 * src/libdatalink raw-L2 datalink (scs_datalink.h), NOT an in-kernel AF_DECnet
 * forward-port. See docs/decnet-provenance-register.md sec 6.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The AF_PACKET raw socket is a hidden
 * mechanism, the way a transport daemon hides its raw socket behind the
 * SCS/NISCA surface. This engine module owns NO socket: it is pure logic over
 * frame buffers + an injected clock (the same discipline as the codecs it
 * consumes), so it is deterministically unit-testable and substrate-neutral.
 * The VMS-visible face it presents is the DECnet routing surface a real NCP
 * user sees -- an executor node (area.node + name + state), a circuit over the
 * datalink device, and a SHOW ADJACENT NODES adjacency table -- NEVER a raw
 * socket or a Linux interface name. The daemon that owns the actual datalink
 * (decnetd.c) is the ONLY place scs_datalink_{open,send,recv}() is called, the
 * same way scsd.c is the only place SCS's datalink is opened.
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md). Everything here derives ONLY from: (a) the public DEC DNA
 * Phase IV Routing Layer spec (the endnode-HELLO wire form, the 0x6003
 * ethertype, the AA-00-04-00-xx-yy address<->id mapping, the AB-00-00-03-00-00
 * Phase IV multicast, the T3 cadence / BCT3MULT listen timer), (b) the vms-3be
 * lab-oracle capture (docs/decnet-provenance-register.md sec 4.6), and (c)
 * OVMX's own scsd.c raw-Ethernet datalink pattern. No VSI/HPE/DEC source or
 * binary was disassembled, decompiled, or copied. Where a value is an OVMX
 * design choice rather than an oracle/spec fact (e.g. the device->circuit name
 * mapping) it is LABELLED as such below, never presented as VMS-authentic.
 */
#ifndef DNET_ENGINE_H
#define DNET_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dnet_hello.h"
#include "dnet_adjacency.h"
#include "dnet_router_hello.h" /* the Ethernet Router Hello codec (rd vms-0aba) */
#include "dnet_link.h"      /* the NSP logical-link connection service (rung 2) */
#include "dnet_nsp.h"       /* the NSP transport codec */

#ifdef __cplusplus
extern "C" {
#endif

/* Full 14-byte Ethernet II header: dst[6] src[6] ethertype[2]. */
#define DNET_ETH_HDRLEN     14
/* A generous cap for one full Ethernet frame (header + max DECnet payload +
 * slack). Phase IV HELLOs are tiny (60 bytes on the wire); this bounds the
 * receive buffer the daemon hands us. */
#define DNET_FRAME_MAX      1536

/* The Phase IV *endnode* HELLO multicast destination, AB-00-00-03-00-00 --
 * oracle-captured (vms-3be specimen #1 dst) and DNA-documented. Defined in the
 * .c so it has a single storage definition. */
extern const uint8_t DNET_HELLO_MCAST[DNET_ADDR_LEN];

/* The Phase IV *router* HELLO multicast destination, AB-00-00-04-00-00 (the DNA
 * "all end nodes" multicast: a ROUTER multicasts its router-hello here so every
 * endnode on the segment hears it and can pick a designated router). This is
 * the counterpart of DNET_HELLO_MCAST: the vms-3be oracle capture shows a real
 * VAX ENDNODE sending its endnode-hello to AB-00-00-03-00-00 (the "all routers"
 * multicast), which CORROBORATES the DNA assignment that a ROUTER sends the
 * other direction, to AB-00-00-04-00-00. Defined in the .c (single storage
 * definition). No router-hello wire specimen is oracle-committed (see
 * dnet_router_hello.h); the destination is DNA-documented + endnode-corroborated,
 * never fabricated. */
extern const uint8_t DNET_ROUTER_HELLO_MCAST[DNET_ADDR_LEN];

/*
 * Phase IV LONG DATA PACKET routing header (the header that carries an NSP PDU
 * node-to-node, as distinct from the endnode-HELLO control frame). Grounded on
 * the vms-3be NSP capture (docs/decnet-provenance-register.md sec 4.6, specimen
 * #3): the 21-byte long-data header preceding that Connect Initiate is
 *   PAD(1)=0x81  RFLG(1)=0x2e  D-reserved(2)=00 00  DSTID(6)  S-reserved(2)=00 00
 *   SRCID(6)  nextl(1)  visit(1)  svc-class(1)  proto(1).
 * The routing message opens with the Phase IV intra-Ethernet PADDING field the
 * specimen carried as 0x81 (0x80 = padding-present | 0x01 = one byte total,
 * the count byte its own only byte), then the 21-byte long-data routing header.
 *
 * rd vms-a70 (real VAX<->VAX capture, direction A): real VMS SILENTLY DISCARDS
 * a unicast Connect Initiate that OMITS this pad at the routing layer -- the CI
 * never reaches session control, so no conn-confirm and no LOGINOUT. OVMX
 * therefore EMITS the pad on unicast routed data frames (build_data_frame) and
 * STRIPS it on receive (parse_data_frame), matching the captured VAX wire byte
 * for byte. It is a UNICAST-routed-data-frame field ONLY: the endnode-HELLO and
 * router-hello control MULTICASTS are padless on the real wire (vms-3be /
 * vms-df5 specimens) and stay padless here -- they are built by
 * dnet_engine_build_hello_frame / the router-hello encoder, never this path.
 * The clean-room provenance is the uncopyrightable DNA Phase IV routing pad
 * format plus the captured VAX frame; the C is OVMX's own. The NSP PDU
 * (dnet_nsp) follows the header; the data-link 2-byte LE length prefix precedes
 * the pad and counts everything after it (specimen #3: 0x0033 = 51 = 1 pad +
 * 21 rhdr + 29 NSP), exactly as the endnode-HELLO frame carries one. */
#define DNET_RFLAG_LONG_DATA    0x2e   /* long data packet routing flags (specimen #3) */
/* The long-data RFLG carries an "intra-Ethernet" flag bit (0x08) that VARIES by
 * direction on the wire: the originator sets it (OVMX's CI + the accepted VAX CI
 * are 0x2e), and a real OpenVMS VAX CLEARS it on the frames it sends back (its
 * Connect Confirm and data segments are 0x26 -- captured on the isolated lab,
 * a70-A). NSP-frame recognition must therefore mask this flag off, or every
 * unicast reply from a real VAX is misclassified as a non-NSP frame and dropped
 * (writes_recv stays 0, no CC ever consumed). Only the observed-varying bit is
 * masked; the rest of the RFLG still distinguishes a long-data packet from a
 * HELLO (0x0d) / router-hello (0x0b). */
#define DNET_RFLAG_INTRA_ETH    0x08
#define DNET_RFLAG_IS_LONG_DATA(r) \
    (((uint8_t)(r) & (uint8_t)~DNET_RFLAG_INTRA_ETH) == \
     (DNET_RFLAG_LONG_DATA & (uint8_t)~DNET_RFLAG_INTRA_ETH))
#define DNET_DATA_PAD_BYTE      0x81   /* Phase IV intra-Ethernet routing pad: 0x80|len(1) */
#define DNET_DATA_PAD_LEN       1      /* one pad byte, per specimen #3 (0x81 = length 1) */
#define DNET_DATA_LENPREFIX     2      /* data-link LE length prefix */
#define DNET_DATA_RHDR_LEN      21     /* RFLG+2+DSTID(6)+2+SRCID(6)+nextl+visit+svc+proto */
/* Byte offset of the NSP PDU within a full data frame WE BUILD (which always
 * emits the 1-byte pad). The receive path tolerates a padless legacy frame and
 * locates the PDU dynamically, so this is the builder's sizing constant. */
#define DNET_DATA_NSP_OFF       (DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX + \
                                 DNET_DATA_PAD_LEN + DNET_DATA_RHDR_LEN)

/* NCP node names are 1..6 characters (DNA Phase IV). +1 for the NUL. */
#define DNET_NODENAME_MAX   6
/* VMS device / DECnet circuit name field caps (e.g. "EWA0" / "EWA-0"). */
#define DNET_DEVNAME_MAX    15

/* Engine return codes (distinct namespace from the codecs'). */
#define DNET_ENGINE_OK        0
#define DNET_ENGINE_EINVAL  (-1)   /* null / out-of-range argument */
#define DNET_ENGINE_ENOSPACE (-2)  /* output buffer too small */

/*
 * The live routing endnode. Pure state: no socket, no fd, no thread. The
 * daemon (decnetd.c) owns the datalink and injects the clock; this struct is
 * everything the VMS-visible DECnet surface and the wire logic need.
 */
struct dnet_engine {
    /* --- executor identity (the NCP "executor node") --- */
    uint16_t addr;                        /* DECnet address (area<<10 | node) */
    char     node_name[DNET_NODENAME_MAX + 1]; /* NCP node name, e.g. "OVMX1" */
    uint8_t  my_id[DNET_ADDR_LEN];        /* AA-00-04-00-<LE addr> Ethernet id */
    uint8_t  hw_mac[DNET_ADDR_LEN];       /* the datalink device's real HW MAC */
    uint16_t blksize;                     /* advertised max receive block size */

    /* --- the DECnet circuit over the datalink device (NCP "circuit") ---
     * OVMX design choice (LABELLED, not VMS-authentic): the VMS device name and
     * the circuit name are supplied by the daemon; the executive device-table
     * registration of _NET:/EWA0: over /dev/vms is a later rung (vms-a7e), so
     * these are a presentation label here, honestly scoped. */
    char     device[DNET_DEVNAME_MAX + 1];  /* e.g. "EWA0"  */
    char     circuit[DNET_DEVNAME_MAX + 1]; /* e.g. "EWA-0" */

    /* --- routing/adjacency state machine (rung-3 codec) --- */
    struct dnet_adjacency adj;

    /* --- node role + designated-router state (rd vms-0a9) ---
     * `node_type` is this engine's advertised Phase IV role in the low 2 bits of
     * the HELLO IINFO byte: DNET_NODETYPE_ENDNODE (the default -- unchanged
     * endnode behaviour) or DNET_NODETYPE_L1ROUTER when the daemon runs in
     * --router mode. In router mode `router_priority` is the designated-router
     * election priority we advertise (an operator config value, DNA default 64;
     * NOT a fabricated wire byte -- it is a real field the operator sets).
     *
     * In ENDNODE mode the engine also does the endnode half of DR selection: on
     * receiving a router-hello it records the highest-priority router it has
     * heard as its designated router (`dr_id`, `dr_priority`, `have_dr`) and
     * reflects that router in the NEIGHBOR field of the endnode-hellos it emits
     * -- exactly the field a real Phase IV endnode flips from 0.0 to the DR's
     * address once it selects one. With no router heard, `have_dr` stays 0 and
     * the emitted endnode-hello names rtr 0.0 (byte-identical to the vms-3be
     * oracle -- the DR reflection changes the wire ONLY after a real router-hello
     * has been selected). */
    uint8_t  node_type;            /* DNET_NODETYPE_ENDNODE / _L1ROUTER */
    uint8_t  router_priority;      /* router mode: advertised DR-election priority */
    int      have_dr;              /* endnode mode: a designated router is selected */
    uint8_t  dr_id[DNET_ADDR_LEN]; /* endnode mode: selected DR's Ethernet id */
    uint8_t  dr_priority;          /* endnode mode: selected DR's advertised priority */

    /* --- NSP logical-link connection service (rung 2, rd vms-c23) ---
     * One active logical link per engine instance is the rung-2 scope (a multi-
     * link port table is a later rung; OVMX design choice, not a protocol
     * limit). `link_active` gates the wrappers below. Data delivered by an
     * inbound segment is copied into rx_data/rx_datalen for the caller to read
     * after a DNET_LINK_EV_DATA event. */
    struct dnet_link link;
    int              link_active;
    uint8_t          rx_data[DNET_NSP_MAX_DATA];
    uint16_t         rx_datalen;
    unsigned long    nsp_frames_recv;   /* NSP data frames handed to link_rx */
    unsigned long    nsp_frames_dropped;/* undecodable / not-for-us NSP frames */

    /* --- honest counters (reported on the VMS surface; no fabrication) --- */
    unsigned long hello_sent;
    unsigned long hello_recv;      /* well-formed endnode HELLOs accepted */
    unsigned long router_hello_sent; /* router-mode: router-hellos we emitted */
    unsigned long router_hello_recv; /* endnode-mode: router-hellos accepted */
    unsigned long frames_recv;     /* every frame handed to rx_frame */
    unsigned long frames_dropped;  /* wrong ethertype / own echo / undecodable */
    unsigned long adj_up_events;
    unsigned long adj_down_events;
};

/*
 * dnet_id_from_addr - build the DECnet Ethernet id AA-00-04-00-<LE addr> from an
 * area.node pair. E.g. area=1 node=1 -> AA-00-04-00-01-04 (the vms-3be node 1.1).
 * Returns DNET_ENGINE_OK, or DNET_ENGINE_EINVAL on a null out / out-of-range
 * area (>63) or node (>1023).
 */
int dnet_id_from_addr(unsigned area, unsigned node, uint8_t id_out[DNET_ADDR_LEN]);

/*
 * dnet_engine_init - stand up the endnode with executor identity area.node,
 * NCP node_name, the VMS device/circuit labels, and the datalink device's real
 * HW MAC (for reporting only). t3 = our HELLO cadence in seconds (0 =>
 * DNET_T3_DEFAULT, the oracle-captured 15 s); blksize = advertised max receive
 * block size (0 => 1498, the vms-3be value). `now` seeds the emission clock.
 *
 * Returns DNET_ENGINE_OK, or DNET_ENGINE_EINVAL on a null/oversized argument.
 * No identity is invented: the caller must pass a real area.node (the daemon
 * fails honestly rather than defaulting one, INV-6 / the scsd resolve_node_
 * identity discipline).
 */
int dnet_engine_init(struct dnet_engine *e, unsigned area, unsigned node,
                     const char *node_name, const char *device,
                     const char *circuit, const uint8_t hw_mac[DNET_ADDR_LEN],
                     uint16_t t3, uint16_t blksize, dnet_tick_t now);

/*
 * dnet_engine_build_hello_frame - assemble the complete on-wire endnode-HELLO
 * Ethernet frame into `frame_out`: a 14-byte Ethernet header (dst = the Phase
 * IV endnode multicast, src = our Ethernet id, ethertype = 0x6003 big-endian)
 * followed by the endnode-HELLO payload from dnet_hello_encode(). The payload
 * advertises our address, node type endnode, blksize, T3 timer, and rtr 0.0
 * (no designated router) -- matching the vms-3be endnode-HELLO shape.
 *
 * Writes the total frame length to *len_out. Returns DNET_ENGINE_OK, or
 * DNET_ENGINE_ENOSPACE if `cap` is too small, or DNET_ENGINE_EINVAL.
 */
int dnet_engine_build_hello_frame(const struct dnet_engine *e,
                                  uint8_t *frame_out, size_t cap, size_t *len_out);

/*
 * dnet_engine_set_router - flip this engine into Phase IV L1-ROUTER run-mode
 * (rd vms-0a9). After this call the engine advertises IINFO node-type = L1
 * router and dnet_engine_build_router_hello_frame() emits spec-faithful router-
 * hellos. `priority` is the designated-router election priority to advertise
 * (0 => DNET_ROUTER_PRIORITY_DEFAULT, the DNA-documented default 64; a real
 * operator-set field, never a fabricated wire byte). Returns DNET_ENGINE_OK or
 * DNET_ENGINE_EINVAL on a null argument. Idempotent. There is deliberately no
 * inverse: an engine that must be an endnode is simply never set to router.
 */
#define DNET_ROUTER_PRIORITY_DEFAULT  64u  /* DNA Phase IV default router priority */
int dnet_engine_set_router(struct dnet_engine *e, uint8_t priority);

/* 1 if this engine is running as a router (node_type == L1 router), else 0. */
int dnet_engine_is_router(const struct dnet_engine *e);

/*
 * dnet_engine_build_router_hello_frame - assemble the complete on-wire Phase IV
 * ROUTER-HELLO Ethernet frame into `frame_out` (rd vms-0a9): a 14-byte Ethernet
 * header (dst = DNET_ROUTER_HELLO_MCAST, the all-endnodes multicast; src = our
 * Ethernet id; ethertype 0x6003 big-endian) followed by the router-hello payload
 * from dnet_router_hello_encode(). Every emitted field is grounded from the DNA
 * Phase IV routing spec + the decode struct and honest-zeroed where ungrounded
 * (INV-6): rflags = router-hello control type, version = the oracle DNA version,
 * IINFO node-type = L1 router (the bits that make an endnode treat us as a DR
 * candidate), blksize + T3 = the oracle values, priority = our advertised
 * DR-election priority, AREA/MPD = reserved-zero, and the opaque E-list EMPTY
 * (we advertise no other routers heard rather than fabricate an E-list layout).
 * Only valid when the engine is in router mode (else DNET_ENGINE_EINVAL).
 *
 * Writes the total frame length to *len_out. Returns DNET_ENGINE_OK,
 * DNET_ENGINE_ENOSPACE if `cap` is too small, or DNET_ENGINE_EINVAL.
 */
int dnet_engine_build_router_hello_frame(const struct dnet_engine *e,
                                         uint8_t *frame_out, size_t cap,
                                         size_t *len_out);

/*
 * dnet_engine_router_hello_emitted - record that we emitted a router-hello at
 * `now` (router mode): advance the T3 cadence (shared with the endnode path) and
 * bump router_hello_sent. Mirrors dnet_engine_hello_emitted for the router role.
 */
void dnet_engine_router_hello_emitted(struct dnet_engine *e, dnet_tick_t now);

/*
 * dnet_engine_rx_frame - consume one received full Ethernet frame (including the
 * 14-byte header, exactly as AF_PACKET SOCK_RAW / scs_datalink_recv() delivers
 * it) at time `now`. Validates the 0x6003 ethertype and ignores our own
 * multicast echo (src == my_id), then dispatches on the routing control type:
 *   - an ENDNODE-hello (rflags 0x0d) decodes and drives the adjacency SM (the
 *     original behaviour); or
 *   - a ROUTER-hello (rflags 0x0b) whose IINFO says L1/L2 router runs the
 *     endnode DR-selection step (rd vms-0a9): the highest-priority router heard
 *     becomes this endnode's designated router (have_dr/dr_id), which the next
 *     endnode-hello then names in its NEIGHBOR field.
 *
 * Returns 1 if a HELLO was accepted (endnode-hello advanced the neighbour SM, or
 * router-hello was accepted for DR-selection), 0 if the frame was ignored (wrong
 * ethertype, own echo, or not a decodable/relevant HELLO -- frames_dropped is
 * bumped), or DNET_ENGINE_EINVAL on a null argument. When non-NULL, *from_out
 * receives the sender's Ethernet id and *state_out the sender's adjacency state
 * (for a router-hello, that node's current adjacency state, unchanged here).
 * Counters are updated in every case.
 */
int dnet_engine_rx_frame(struct dnet_engine *e, dnet_tick_t now,
                         const uint8_t *frame, size_t len,
                         uint8_t from_out[DNET_ADDR_LEN],
                         enum dnet_adj_state *state_out);

/* T3 cadence: 1 if our next HELLO is due at `now`, else 0. */
int dnet_engine_hello_due(const struct dnet_engine *e, dnet_tick_t now);

/* Record that we emitted a HELLO at `now`: advance the T3 cadence + bump the
 * hello_sent counter. (The daemon decides HOW the frame reached the wire.) */
void dnet_engine_hello_emitted(struct dnet_engine *e, dnet_tick_t now);

/*
 * dnet_engine_tick - advance time to `now`, expiring any adjacency whose listen
 * timer lapsed (each -> DOWN). Returns the number that transitioned to DOWN
 * (also folded into adj_down_events), or DNET_ENGINE_EINVAL on a null argument.
 */
int dnet_engine_tick(struct dnet_engine *e, dnet_tick_t now);

/* --- NSP logical-link data frames (routing-layer carrier for NSP PDUs) ----
 *
 * These assemble/parse the Phase IV long-data-packet routing header (above) that
 * carries an NSP PDU node-to-node. They are the data-plane analogue of
 * build_hello_frame/rx_frame (which handle the HELLO control plane). */

/*
 * dnet_engine_build_data_frame - wrap an already-encoded NSP PDU
 * (nsp_pdu[0..pdu_len-1], MSGFLG onward) in a full on-wire data frame addressed
 * to the peer whose DECnet Ethernet id is dst_id: Ethernet header (dst=dst_id,
 * src=our id, 0x6003) + the 2-byte length prefix + the 21-byte long-data routing
 * header (DSTID=dst_id, SRCID=our id) + the NSP PDU. Writes the total length to
 * *len_out. Returns DNET_ENGINE_OK, DNET_ENGINE_ENOSPACE, or DNET_ENGINE_EINVAL.
 */
int dnet_engine_build_data_frame(const struct dnet_engine *e,
                                 const uint8_t dst_id[DNET_ADDR_LEN],
                                 const uint8_t *nsp_pdu, size_t pdu_len,
                                 uint8_t *frame_out, size_t cap, size_t *len_out);

/*
 * dnet_engine_parse_data_frame - validate a received full data frame and locate
 * its NSP PDU. Checks the 0x6003 ethertype and the long-data RFLG, extracts the
 * sender's and destination's Ethernet ids, and on success points *nsp_pdu at the
 * NSP PDU inside `frame` (no copy) with its length in *pdu_len. `src_id_out` /
 * `dst_id_out` (each DNET_ADDR_LEN, may be NULL) receive the routing ids.
 * Returns DNET_ENGINE_OK, or DNET_ENGINE_EINVAL if it is not a well-formed
 * Phase IV long-data frame (a HELLO control frame returns EINVAL here -- feed
 * those to dnet_engine_rx_frame instead).
 */
int dnet_engine_parse_data_frame(const uint8_t *frame, size_t len,
                                 uint8_t src_id_out[DNET_ADDR_LEN],
                                 uint8_t dst_id_out[DNET_ADDR_LEN],
                                 const uint8_t **nsp_pdu, size_t *pdu_len);

/* --- single-link connection wrappers (what DECNETD drives) -----------------
 *
 * Each builds the outbound frame (NSP PDU + routing header) for the operation
 * into frame_out and returns its length in *len_out. The engine owns the one
 * embedded struct dnet_link. */

/*
 * Open a logical link to peer node remote_area.remote_node, using local_lla as
 * our NSP logical-link address. Builds the Connect Initiate data frame. The
 * connect payload (session-control access control, may be NULL/0), segsize,
 * services and info fill the CI. Sets link_active. Returns DNET_ENGINE_OK,
 * DNET_ENGINE_ENOSPACE/EINVAL, or DNET_ENGINE_EINVAL if a link is already active.
 */
int dnet_engine_link_open(struct dnet_engine *e,
                          unsigned remote_area, unsigned remote_node,
                          uint16_t local_lla,
                          const uint8_t *conn_data, size_t conn_len,
                          uint16_t segsize, uint8_t services, uint8_t info,
                          uint8_t *frame_out, size_t cap, size_t *len_out,
                          dnet_tick_t now);

/* Accept the pending inbound connection (link in CR_RCVD after an rx CI): builds
 * the Connect Confirm data frame. Returns DNET_ENGINE_OK / EINVAL. */
int dnet_engine_link_accept(struct dnet_engine *e, uint16_t local_lla,
                            uint8_t *frame_out, size_t cap, size_t *len_out,
                            dnet_tick_t now);

/* Send a data segment on the running link: builds the data frame. */
int dnet_engine_link_send(struct dnet_engine *e, const uint8_t *data, size_t len,
                          uint8_t *frame_out, size_t cap, size_t *len_out,
                          dnet_tick_t now);

/* Disconnect the link: builds the Disconnect Initiate data frame. */
int dnet_engine_link_close(struct dnet_engine *e, uint16_t reason,
                           uint8_t *frame_out, size_t cap, size_t *len_out,
                           dnet_tick_t now);

/*
 * dnet_engine_link_service - build the NSP LINK SERVICE credit-grant frame the
 * connection initiator must send right after the Connect Confirm (rd vms-6165):
 * it acks the other-data subchannel and opens the flow-control window, which is
 * what makes a real OpenVMS peer stop retransmitting its CC and start sending.
 * Wraps dnet_link_link_service() into a full data frame. Requires a RUN link.
 */
int dnet_engine_link_service(struct dnet_engine *e,
                             uint8_t *frame_out, size_t cap, size_t *len_out,
                             dnet_tick_t now);

/*
 * dnet_engine_link_tick - advance the active link's timers at time `now`
 * (Connect Initiate retransmission and its give-up budget). If the FSM decides
 * to (re)transmit, builds the frame into frame_out and sets *has_out = 1. The
 * PDU is the FSM's own, never a template copy. No active link -> EINVAL.
 */
int dnet_engine_link_tick(struct dnet_engine *e, dnet_tick_t now,
                          uint8_t *frame_out, size_t cap, size_t *len_out,
                          int *has_out);

/*
 * dnet_engine_link_rx - consume a received full NSP data frame at time `now`:
 * parse the routing header, decode the NSP PDU, and drive the embedded link
 * FSM. If the FSM produced a protocol reply (a data acknowledgement, or a
 * Disconnect Confirm), builds it into reply_frame and sets *has_reply = 1.
 * *event (may be NULL) receives the higher-layer event; on DNET_LINK_EV_DATA the
 * delivered payload is in e->rx_data / e->rx_datalen. On an inbound Connect
 * Initiate the peer node id from the frame is adopted as the link's remote node.
 * Returns DNET_ENGINE_OK (frame consumed or honestly dropped) or
 * DNET_ENGINE_EINVAL.
 */
int dnet_engine_link_rx(struct dnet_engine *e, dnet_tick_t now,
                        const uint8_t *frame, size_t len,
                        uint8_t *reply_frame, size_t cap, size_t *reply_len,
                        int *has_reply, enum dnet_link_event *event);

/* --- the VMS-faithful presentation surface (NCP SHOW ..., the hidden-socket
 * face) -------------------------------------------------------------------- */

/* NCP "SHOW EXECUTOR" analogue: executor node = a.n (NAME), State = on, etc. */
void dnet_engine_show_executor(const struct dnet_engine *e, FILE *out);
/* NCP "SHOW CIRCUIT" analogue: the DECnet circuit over the datalink device. */
void dnet_engine_show_circuit(const struct dnet_engine *e, FILE *out);
/* NCP "SHOW ADJACENT NODES" analogue: the live neighbour adjacency table. */
void dnet_engine_show_adjacent(const struct dnet_engine *e, FILE *out);

/* Format a DECnet address as "area.node" into buf (>= 8 bytes). Returns buf. */
const char *dnet_addr_str(uint16_t addr, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* DNET_ENGINE_H */
