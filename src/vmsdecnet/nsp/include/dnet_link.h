/*
 * dnet_link.h - DECnet Phase IV NSP LOGICAL-LINK connection service
 *               (engine rung 2, rd vms-c23 / epic vms-30e).
 *
 * Rung 1 (dnet_engine.{c,h}, rd vms-449d) moves HELLO frames and drives the
 * routing ADJACENCY. The NSP transport codec (dnet_nsp.{c,h}, rd vms-6986) can
 * encode/decode individual NSP messages. This module builds the missing middle:
 * the NSP CONNECTION STATE MACHINE that establishes, runs, and tears down a
 * single logical link over an adjacency the engine has already formed.
 *
 * It is the piece that turns "we can encode a Connect Initiate" into "two nodes
 * hold an open logical link, exchange a data segment with acknowledgement, and
 * disconnect cleanly". Like the codecs and the adjacency SM it consumes, it is a
 * PURE, deterministic state machine: no socket, no thread, no allocation, no
 * wall clock, no sleep. Every entry point takes an injected monotonic tick
 * (dnet_tick_t), so the whole connection lifecycle -- including the Connect
 * Initiate retransmission timer -- is unit-testable deterministically and the
 * module serves either engine boundary (docs/design-decnet-ovmx.md sec 4:
 * "the L3-L6 VMS surface is identical either way -- only the engine boundary
 * moves"). The datalink/daemon that actually clocks this SM and puts NSP PDUs on
 * the wire is decnetd.c, via the routing-layer data frames the engine assembles.
 *
 * LAYER BOUNDARY. This module owns the NSP logical-link FSM ONLY. It consumes
 * struct dnet_nsp_msg (decoded PDUs) and produces struct dnet_nsp_msg (PDUs to
 * send); it never touches a routing header, a length prefix, an Ethernet frame,
 * or a socket. The caller (the engine) wraps an outbound PDU in the Phase IV
 * long-data-packet routing header and hands an inbound PDU up already decoded.
 *
 * RULE 1 -- "do it like VMS, or HIDE it." The VMS-visible face of this FSM is a
 * DECnet LOGICAL LINK / task-to-task connection (open / send / receive /
 * disconnect), never a raw NSP frame or a socket. The daemon presents it as the
 * connection surface an NCP/$QIO user sees.
 *
 * SCOPE (rung 2, rd vms-c23): the connection SERVICE -- link establishment
 * (Connect Initiate/Confirm exchange), the RUN data-flow state (a data segment
 * carried with its acknowledgement and simple in-order sequencing), and clean
 * teardown (Disconnect Initiate/Confirm handshake). Explicitly NOT this rung
 * (filed as children of vms-30e): SET HOST / CTERM and file transfer / DAP --
 * the LAYERED PRODUCTS that ride on top of an established logical link -- and
 * the live-VAX oracle bracket (rd vms-aac0). One active logical link per FSM
 * instance is the rung-2 scope (a multi-link port table is a later rung; OVMX
 * design choice, labelled, not a protocol limit).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md Rule 8, HARD; docs/decnet-provenance-
 * register.md). The connection choreography and every wire field derive ONLY
 * from: (a) the public DEC DNA Phase IV NSP Functional Specification (the
 * logical-link state model, the CI/CC establishment handshake, data-segment
 * sequencing + acknowledgement, and the DI/DC disconnect handshake), and (b)
 * the lab-oracle NSP capture committed as specimen #3/#4 in
 * docs/decnet-provenance-register.md sec 4.6 (a Connect Initiate and its
 * retransmit). ORACLE COVERAGE is honest: only the Connect Initiate is oracle-
 * byte-verified (in the codec); the capture never completed a handshake, so the
 * CC / data / ack / DI / DC *choreography* is SPEC-DERIVED and proven by the
 * two-endpoint round-trip in test_dnet_link.c, never against captured bytes. The
 * CI retransmit interval (~5.5 s, DNET_LINK_CI_RETRANS_SECS) and the give-up
 * count (8 retransmits, DNET_LINK_MAX_RETRANS) ARE oracle-informed: specimens #3
 * and #4 are 5.504 s apart and the register records VAX1 abandoning the Connect
 * Initiate after 8 retransmits. No VSI/HPE/DEC source or binary was
 * disassembled, decompiled, or copied.
 */
#ifndef DNET_LINK_H
#define DNET_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_nsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Injected monotonic clock, conventionally seconds (the same discipline and unit
 * as the adjacency SM's dnet_tick_t). Guarded so a translation unit that also
 * includes the routing adjacency header sees a single, identical definition.
 */
#ifndef DNET_TICK_T_DEFINED
#define DNET_TICK_T_DEFINED
typedef uint64_t dnet_tick_t;
#endif

/*
 * Logical-link connection state (DNA Phase IV NSP logical-link state model,
 * reduced to the rung-2 connection-service subset).
 *
 *   CLOSED --connect()--> CI_SENT --rx CC--> RUN --disconnect()--> DI_SENT
 *      ^                     |                 |                       |
 *      |                     |                 | rx DI (peer closed)   | rx DC
 *      |  rx CI (inbound)    |                 v  (-> send DC)         v
 *      +-- CR_RCVD --accept()/CC--> RUN      CLOSED <-----------------+
 *          |  reject()/DI --> CLOSED
 *
 *   CI_SENT also -> CLOSED if the CI is retransmitted DNET_LINK_MAX_RETRANS
 *   times with no Confirm (the peer is unreachable), matching the oracle.
 */
enum dnet_link_state {
    DNET_LINK_CLOSED = 0,   /* no link */
    DNET_LINK_CI_SENT,      /* outbound: Connect Initiate sent, awaiting Confirm */
    DNET_LINK_CR_RCVD,      /* inbound: Connect Initiate received, awaiting accept */
    DNET_LINK_RUN,          /* established: data flows */
    DNET_LINK_DI_SENT       /* local disconnect sent, awaiting Disconnect Confirm */
};

/* Event a received PDU produced, reported by dnet_link_rx to the caller so it
 * can drive the VMS-visible connection surface (connect indication, data
 * delivery, disconnect, ...). */
enum dnet_link_event {
    DNET_LINK_EV_NONE = 0,
    DNET_LINK_EV_CONNECT_IND,    /* inbound CI: a peer wants to connect (now CR_RCVD) */
    DNET_LINK_EV_CONNECT_CONF,   /* our CI was confirmed (CC in): link is now RUN */
    DNET_LINK_EV_DATA,           /* a data segment arrived (payload in the *in msg) */
    DNET_LINK_EV_ACK,            /* an acknowledgement arrived */
    DNET_LINK_EV_DISCONNECT,     /* peer disconnected (DI in): link CLOSED */
    DNET_LINK_EV_DISCONNECT_CONF /* our disconnect confirmed (DC in): link CLOSED */
};

/* Return codes (distinct namespace from the codec's DNET_NSP_E*). */
#define DNET_LINK_OK        0
#define DNET_LINK_EINVAL  (-1)   /* null argument / bad parameter */
#define DNET_LINK_ESTATE  (-2)   /* operation invalid in the current state */

/*
 * Timer / limit defaults.
 *
 * DNET_LINK_CI_RETRANS_SECS -- Connect Initiate retransmission interval.
 * ORACLE-INFORMED: register sec 4.6 specimens #3 and #4 (the CI and its
 * retransmit) are 5.504 s apart; 5 s is the whole-second default, labelled.
 *
 * DNET_LINK_MAX_RETRANS -- number of CI retransmits before the connection is
 * abandoned as unreachable. ORACLE-INFORMED: register sec 4.6 records VAX1's
 * SET HOST giving up after 8 retransmits.
 */
#define DNET_LINK_CI_RETRANS_SECS   5u
#define DNET_LINK_MAX_RETRANS       8u

/* Reason codes surfaced on the VMS connection face (DNA Phase IV NSP reasons /
 * OVMX-labelled where noted). */
#define DNET_LINK_REASON_NORMAL       0    /* normal user disconnect */
#define DNET_LINK_REASON_UNREACHABLE  38   /* no response to Connect Initiate (spec: node unreachable) */

/* 12-bit NSP data/ack sequence number mask. */
#define DNET_LINK_SEQ_MASK          0x0fffu

/*
 * A single NSP logical link. Pure state: no socket, no fd. `remote_node` is the
 * peer's DECnet address (area<<10|node) the routing layer needs to address the
 * frame; the NSP srcaddr/dstaddr are the 16-bit logical-link addresses ("ports")
 * the two ends assign. Only fields relevant to the current state are meaningful.
 */
struct dnet_link {
    enum dnet_link_state state;

    uint16_t local_addr;    /* our NSP logical-link address (our SRCADDR) */
    uint16_t remote_addr;   /* peer's NSP logical-link address (learned from CI/CC) */
    uint16_t remote_node;   /* peer DECnet node addr (area<<10|node) for routing */

    /* Negotiated connect parameters (kept for CI retransmission + reporting). */
    uint8_t  services;      /* SERVICES flow-control option byte */
    uint8_t  info;          /* INFO byte (NSP version in low 2 bits) */
    uint16_t segsize;       /* negotiated max segment size */

    /* Data subchannel sequencing (simple in-order, single subchannel). */
    uint16_t send_seq;      /* next data segment number we will assign (12-bit) */
    uint16_t recv_seq;      /* highest in-order segment number we have received */
    uint16_t send_ack;      /* highest of our segments the peer has acknowledged */

    /* Connect data (access control / session-control payload), retained so a CI
     * retransmit is byte-identical to the original. Opaque to NSP. */
    uint16_t conn_len;
    uint8_t  conn_data[DNET_NSP_MAX_DATA];

    /* CI retransmission timer (CI_SENT only). */
    dnet_tick_t ci_deadline;    /* tick at which the next CI retransmit is due */
    unsigned    ci_retransmits; /* retransmits emitted so far */

    /* Disconnect bookkeeping. */
    uint16_t disc_reason;   /* reason recorded when the link closed */

    /* Honest counters (reported on the VMS surface; never fabricated). */
    unsigned long data_sent, data_recv, acks_sent, acks_recv, ci_sent;
};

/*
 * Initialise a fresh (CLOSED) logical link. `local_addr` is our NSP logical-link
 * address (the caller/engine assigns a nonzero value; 0 is reserved for an
 * as-yet-unknown remote address). `remote_node` is the peer DECnet node address
 * (area<<10|node) the routing layer will address; pass 0 for an inbound link
 * whose peer node is learned from the received frame. `now` seeds the clock.
 * Returns DNET_LINK_OK, or DNET_LINK_EINVAL on a null argument.
 */
int dnet_link_init(struct dnet_link *lk, uint16_t local_addr,
                   uint16_t remote_node, dnet_tick_t now);

/*
 * Originate a connection (CLOSED -> CI_SENT). Builds the Connect Initiate PDU
 * into *out and arms the retransmit timer. `conn_data`/`conn_len` is the opaque
 * connect payload (session-control access control etc.; may be NULL/0).
 * `segsize`, `services`, `info` fill the CI fields (info's low 2 bits select the
 * NSP version, e.g. DNET_NSP_VER_41). Returns DNET_LINK_OK, DNET_LINK_ESTATE if
 * the link is not CLOSED, or DNET_LINK_EINVAL on a bad argument.
 */
int dnet_link_connect(struct dnet_link *lk,
                      const uint8_t *conn_data, size_t conn_len,
                      uint16_t segsize, uint8_t services, uint8_t info,
                      struct dnet_nsp_msg *out, dnet_tick_t now);

/*
 * Accept an inbound connection (CR_RCVD -> RUN). Builds the Connect Confirm PDU
 * into *out (echoing the originator's logical-link address as DSTADDR, our
 * local_addr as SRCADDR). Returns DNET_LINK_OK, DNET_LINK_ESTATE if the link is
 * not CR_RCVD, or DNET_LINK_EINVAL.
 */
int dnet_link_accept(struct dnet_link *lk, struct dnet_nsp_msg *out,
                     dnet_tick_t now);

/*
 * Send a single data segment on a RUN link. Builds a data-segment PDU into *out
 * carrying up to DNET_NSP_MAX_DATA bytes, assigns the next segment number, and
 * piggybacks the current receive acknowledgement when there is one to send.
 * Returns DNET_LINK_OK, DNET_LINK_ESTATE if the link is not RUN, or
 * DNET_LINK_EINVAL (null / oversized payload).
 */
int dnet_link_send_data(struct dnet_link *lk, const uint8_t *data, size_t len,
                        struct dnet_nsp_msg *out, dnet_tick_t now);

/*
 * Initiate a clean disconnect (RUN or CI_SENT or CR_RCVD -> DI_SENT). Builds the
 * Disconnect Initiate PDU into *out with `reason`. The link fully closes when
 * the peer's Disconnect Confirm arrives (dnet_link_rx). Returns DNET_LINK_OK,
 * DNET_LINK_ESTATE if the link is already CLOSED, or DNET_LINK_EINVAL.
 */
int dnet_link_disconnect(struct dnet_link *lk, uint16_t reason,
                         struct dnet_nsp_msg *out, dnet_tick_t now);

/*
 * Feed one decoded, inbound NSP PDU into the FSM at time `now`. Drives the state
 * machine and, when the protocol requires an immediate response (a data
 * acknowledgement for a received data segment, or a Disconnect Confirm for a
 * received Disconnect Initiate), builds that reply PDU into *reply and sets
 * *has_reply = 1. *event receives the higher-layer event the PDU produced (see
 * enum dnet_link_event) so the caller can drive the VMS connection surface. Any
 * of reply/has_reply/event may be NULL if not wanted. Returns DNET_LINK_OK, or
 * DNET_LINK_EINVAL on a null lk/in. A PDU that is invalid for the current state
 * (e.g. data before RUN, a mismatched logical-link address) is ignored honestly:
 * the call still returns OK with event NONE and no reply, never a fabricated
 * transition.
 */
int dnet_link_rx(struct dnet_link *lk, const struct dnet_nsp_msg *in,
                 dnet_tick_t now, struct dnet_nsp_msg *reply, int *has_reply,
                 enum dnet_link_event *event);

/*
 * Advance the clock to `now`. On a CI_SENT link whose retransmit deadline has
 * lapsed, rebuilds the Connect Initiate into *out (byte-identical to the
 * original), rearms the timer, and sets *has_out = 1 -- unless the retransmit
 * budget (DNET_LINK_MAX_RETRANS) is exhausted, in which case the link goes
 * CLOSED with reason UNREACHABLE and *has_out = 0. On any other state it is a
 * no-op with *has_out = 0. Returns DNET_LINK_OK or DNET_LINK_EINVAL.
 */
int dnet_link_tick(struct dnet_link *lk, dnet_tick_t now,
                   struct dnet_nsp_msg *out, int *has_out);

/* State accessors. */
static inline enum dnet_link_state dnet_link_state_of(const struct dnet_link *lk)
{
    return lk ? lk->state : DNET_LINK_CLOSED;
}
static inline int dnet_link_is_up(const struct dnet_link *lk)
{
    return lk && lk->state == DNET_LINK_RUN;
}

/* Human-readable state name for the VMS-faithful surface / logs. */
const char *dnet_link_state_name(enum dnet_link_state s);

#ifdef __cplusplus
}
#endif

#endif /* DNET_LINK_H */
