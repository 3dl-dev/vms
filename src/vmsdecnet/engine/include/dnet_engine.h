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
 * src/vmsscs/ LAVC datalink (scs_datalink.h), NOT an in-kernel AF_DECnet
 * forward-port. See docs/decnet-provenance-register.md sec 6.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The AF_PACKET raw socket is a hidden
 * mechanism, exactly as src/vmsscs/scsd.c hides its raw SCA socket behind the
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

/*
 * Phase IV LONG DATA PACKET routing header (the header that carries an NSP PDU
 * node-to-node, as distinct from the endnode-HELLO control frame). Grounded on
 * the vms-3be NSP capture (docs/decnet-provenance-register.md sec 4.6, specimen
 * #3): the 21-byte long-data header preceding that Connect Initiate is
 *   RFLG(1)=0x2e  D-reserved(2)=00 00  DSTID(6)  S-reserved(2)=00 00  SRCID(6)
 *   nextl(1)  visit(1)  svc-class(1)  proto(1).
 * OVMX builds this minimal single-hop form (no optional Phase IV padding field,
 * which the specimen carried as 0x81 -- an OVMX-labelled omission, not a
 * protocol requirement between two OVMX endnodes; multi-hop forwarding / the
 * full routing-data codec is a later routing rung). The NSP PDU (dnet_nsp)
 * follows the header. Data-link 2-byte LE length prefix precedes the header,
 * exactly as the endnode-HELLO frame carries one. */
#define DNET_RFLAG_LONG_DATA    0x2e   /* long data packet routing flags (specimen #3) */
#define DNET_DATA_LENPREFIX     2      /* data-link LE length prefix */
#define DNET_DATA_RHDR_LEN      21     /* RFLG+2+DSTID(6)+2+SRCID(6)+nextl+visit+svc+proto */
/* Byte offset of the NSP PDU within a full data frame we build/parse. */
#define DNET_DATA_NSP_OFF       (DNET_ETH_HDRLEN + DNET_DATA_LENPREFIX + DNET_DATA_RHDR_LEN)

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
 * dnet_engine_rx_frame - consume one received full Ethernet frame (including the
 * 14-byte header, exactly as AF_PACKET SOCK_RAW / scs_datalink_recv() delivers
 * it) at time `now`. Validates the 0x6003 ethertype, ignores our own multicast
 * echo (src == my_id), decodes the endnode HELLO, and drives the adjacency SM.
 *
 * Returns 1 if a HELLO was accepted and the neighbour SM advanced, 0 if the
 * frame was ignored (wrong ethertype, own echo, or not a decodable endnode
 * HELLO -- frames_dropped is bumped), or DNET_ENGINE_EINVAL on a null argument.
 * When non-NULL, *from_out receives the neighbour's Ethernet id and *state_out
 * its resulting adjacency state. Counters are updated in every case.
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
