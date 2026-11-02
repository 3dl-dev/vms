/*
 * dnet_link.c - DECnet Phase IV NSP logical-link connection service (rd vms-c23).
 *
 * See dnet_link.h for the full clean-room provenance, the layer boundary, the
 * Rule-1 rationale, and the honest oracle-coverage statement. This file is the
 * pure connection state machine: it consumes decoded NSP PDUs (struct
 * dnet_nsp_msg) and produces PDUs to send, over an injected monotonic clock. It
 * opens no socket, starts no thread, reads no wall clock.
 *
 * The establishment (CI/CC), data-flow (data segment + acknowledgement) and
 * teardown (DI/DC) choreography is SPEC-DERIVED from the public DNA Phase IV NSP
 * functional spec -- the lab capture (register sec 4.6) only ever ground-truthed
 * the Connect Initiate bytes (done in the codec, dnet_nsp.c), never a completed
 * handshake. It is proven end to end by the two-endpoint round-trip in
 * test_dnet_link.c, never asserted against captured bytes.
 */
#include "dnet_link.h"

#include <string.h>

/* The SERVICES byte OVMX offers in a Connect Confirm: bit 0 set, FCOPT none --
 * what every real VAX CI/CC in the committed captures carries. OVMX's receive
 * side protects itself with FCMOD stop/resume and NAK (rd vms-985), not with
 * request counts, so it never asks a peer to wait for a count it would grant. */
#define LINK_SERVICES_FCOPT_NONE 0x01u

/* 12-bit sequence arithmetic (modulo 4096). */
static inline uint16_t seq_inc(uint16_t s) { return (uint16_t)((s + 1u) & DNET_LINK_SEQ_MASK); }
static inline uint16_t seq_dist(uint16_t from, uint16_t to)
{
    return (uint16_t)((to - from) & DNET_LINK_SEQ_MASK);
}
/* a strictly before b (within half the sequence space) */
static inline int seq_lt(uint16_t a, uint16_t b)
{
    uint16_t d = seq_dist(a, b);
    return d != 0 && d < 2048u;
}
static inline int seq_le(uint16_t a, uint16_t b) { return a == b || seq_lt(a, b); }

const char *dnet_link_state_name(enum dnet_link_state s)
{
    switch (s) {
    case DNET_LINK_CLOSED:  return "closed";
    case DNET_LINK_CI_SENT: return "connecting";   /* CI sent, awaiting confirm */
    case DNET_LINK_CR_RCVD: return "connect received";
    case DNET_LINK_RUN:     return "run";
    case DNET_LINK_DI_SENT: return "disconnecting"; /* DI sent, awaiting confirm */
    default:                return "?";
    }
}

int dnet_link_init(struct dnet_link *lk, uint16_t local_addr,
                   uint16_t remote_node, dnet_tick_t now)
{
    if (!lk)
        return DNET_LINK_EINVAL;
    memset(lk, 0, sizeof(*lk));
    lk->state       = DNET_LINK_CLOSED;
    lk->local_addr  = local_addr;
    lk->remote_node = remote_node;
    lk->ci_deadline = now;   /* meaningful only once we enter CI_SENT */
    return DNET_LINK_OK;
}

/* Build our Connect Initiate PDU from the retained connect parameters. Used both
 * by the first connect() and by a retransmit, so the two are byte-identical. */
static void build_ci(const struct dnet_link *lk, struct dnet_nsp_msg *out)
{
    memset(out, 0, sizeof(*out));
    out->type     = DNET_NSP_T_CI;
    out->msgflg   = DNET_NSP_MSGFLG_CI;
    out->dstaddr  = 0;                 /* fresh CI: destination link not yet known */
    out->srcaddr  = lk->local_addr;
    out->services = lk->services;
    out->info     = lk->info;
    out->segsize  = lk->segsize;
    out->datalen  = lk->conn_len;
    if (lk->conn_len)
        memcpy(out->data, lk->conn_data, lk->conn_len);
}

int dnet_link_connect(struct dnet_link *lk,
                      const uint8_t *conn_data, size_t conn_len,
                      uint16_t segsize, uint8_t services, uint8_t info,
                      struct dnet_nsp_msg *out, dnet_tick_t now)
{
    if (!lk || !out)
        return DNET_LINK_EINVAL;
    if (conn_len > DNET_NSP_MAX_DATA)
        return DNET_LINK_EINVAL;
    if (lk->state != DNET_LINK_CLOSED)
        return DNET_LINK_ESTATE;
    if (lk->local_addr == 0)
        return DNET_LINK_EINVAL;   /* an originator must own a logical-link address */

    lk->services = services;
    lk->info     = info;
    lk->segsize  = segsize;
    lk->conn_len = (uint16_t)conn_len;
    if (conn_len && conn_data)
        memcpy(lk->conn_data, conn_data, conn_len);
    else
        lk->conn_len = 0;

    build_ci(lk, out);
    lk->state          = DNET_LINK_CI_SENT;
    lk->ci_retransmits = 0;
    lk->ci_deadline    = now + DNET_LINK_CI_RETRANS_SECS;
    lk->ci_sent++;
    return DNET_LINK_OK;
}

int dnet_link_accept(struct dnet_link *lk, struct dnet_nsp_msg *out,
                     dnet_tick_t now)
{
    (void)now;
    if (!lk || !out)
        return DNET_LINK_EINVAL;
    if (lk->state != DNET_LINK_CR_RCVD)
        return DNET_LINK_ESTATE;

    memset(out, 0, sizeof(*out));
    out->type     = DNET_NSP_T_CC;
    out->msgflg   = DNET_NSP_MSGFLG_CC;
    out->dstaddr  = lk->remote_addr;   /* echo the originator's logical-link addr */
    out->srcaddr  = lk->local_addr;    /* our chosen logical-link address */
    out->services = LINK_SERVICES_FCOPT_NONE;
    out->info     = lk->info;
    out->segsize  = lk->segsize;
    /* A bare accept carries no connect data; a caller wanting to return
     * session-control accept data is a later (SET HOST/FAL) rung. */
    out->datalen  = 0;

    lk->state = DNET_LINK_RUN;
    return DNET_LINK_OK;
}

int dnet_link_accept_data(struct dnet_link *lk, const uint8_t *data, size_t len,
                          struct dnet_nsp_msg *out, dnet_tick_t now)
{
    if (!lk || !out || (len && !data) || len > DNET_NSP_MAX_DATA)
        return DNET_LINK_EINVAL;
    int rc = dnet_link_accept(lk, out, now);
    if (rc != DNET_LINK_OK)
        return rc;
    if (len) memcpy(out->data, data, len);
    out->datalen = (uint16_t)len;
    return DNET_LINK_OK;
}

/* Build the data-segment PDU for transmit-queue entry `i` (0 = oldest),
 * piggybacking our current receive acknowledgement -- a VAX's data segments
 * always carry it, "ack 0" included (docs/oracle/vax-copy-fal-dap.wire.txt). */
static void build_data(const struct dnet_link *lk, unsigned i, struct dnet_nsp_msg *out)
{
    const struct dnet_link_txseg *t = &lk->txq[(lk->tx_head + i) % DNET_LINK_TXQ];
    memset(out, 0, sizeof(*out));
    out->type    = DNET_NSP_T_DATA;
    /* BOM+EOM: each message is a single self-contained segment. */
    out->msgflg  = DNET_NSP_MSGFLG_DATA;
    out->dstaddr = lk->remote_addr;
    out->srcaddr = lk->local_addr;
    out->segnum  = t->seg;
    out->has_acknum = 1;
    out->acknum  = (uint16_t)(DNET_NSP_ACK_QUAL | (lk->recv_seq & DNET_LINK_SEQ_MASK));
    out->datalen = t->len;
    if (t->len)
        memcpy(out->data, t->data, t->len);
}

/* May transmit-queue entry `i` go on the wire now, under the peer's flow
 * control? FCMOD "do not send" holds everything; with a request-count FCOPT a
 * segment's FIRST transmission also needs a count (a retransmission does not). */
static int may_send(const struct dnet_link *lk, unsigned i)
{
    if (lk->rem_stopped)
        return 0;
    if (lk->rem_fcopt == DNET_NSP_SVC_FCOPT_NONE || i < lk->tx_ever)
        return 1;
    return lk->rem_reqcnt > 0;
}

unsigned dnet_link_tx_room(const struct dnet_link *lk)
{
    if (!lk || lk->state != DNET_LINK_RUN)
        return 0;
    return DNET_LINK_TXQ - lk->tx_count;
}

/* Put entry tx_sent on the wire (first transmission or go-back-N resend). */
static void emit_next(struct dnet_link *lk, struct dnet_nsp_msg *out, dnet_tick_t now)
{
    if (lk->tx_sent >= lk->tx_ever) {
        lk->tx_ever = lk->tx_sent + 1;
        if (lk->rem_fcopt != DNET_NSP_SVC_FCOPT_NONE && lk->rem_reqcnt > 0)
            lk->rem_reqcnt--;
    } else {
        lk->data_rtx++;
    }
    build_data(lk, lk->tx_sent, out);
    if (lk->tx_sent == 0)
        lk->rtx_deadline = now + DNET_LINK_RTX_SECS;
    lk->tx_sent++;
    lk->data_sent++;
}

int dnet_link_send_data(struct dnet_link *lk, const uint8_t *data, size_t len,
                        struct dnet_nsp_msg *out, dnet_tick_t now)
{
    if (!lk || !out || (len && !data))
        return DNET_LINK_EINVAL;
    if (len > DNET_NSP_MAX_DATA)
        return DNET_LINK_EINVAL;
    if (lk->state != DNET_LINK_RUN || lk->close_pending)
        return DNET_LINK_ESTATE;
    if (lk->tx_count >= DNET_LINK_TXQ)
        return DNET_LINK_EAGAIN;

    lk->send_seq = seq_inc(lk->send_seq);
    struct dnet_link_txseg *t = &lk->txq[(lk->tx_head + lk->tx_count) % DNET_LINK_TXQ];
    t->seg = lk->send_seq;
    t->len = (uint16_t)len;
    if (len)
        memcpy(t->data, data, len);
    lk->tx_count++;

    /* It goes now only if everything before it is already on the wire (keep
     * segment order) and the peer's flow control allows it. */
    if (lk->tx_sent == lk->tx_count - 1 && may_send(lk, lk->tx_sent)) {
        emit_next(lk, out, now);
        return DNET_LINK_OK;
    }
    return DNET_LINK_QUEUED;
}

int dnet_link_close(struct dnet_link *lk, uint16_t reason, struct dnet_nsp_msg *out,
                    dnet_tick_t now)
{
    if (!lk || !out)
        return DNET_LINK_EINVAL;
    if (lk->state != DNET_LINK_RUN)
        return DNET_LINK_ESTATE;
    if (lk->tx_count == 0)
        return dnet_link_disconnect(lk, reason, out, now);
    lk->close_pending = 1;
    lk->close_reason  = reason;
    return DNET_LINK_QUEUED;
}

int dnet_link_disconnect(struct dnet_link *lk, uint16_t reason,
                         struct dnet_nsp_msg *out, dnet_tick_t now)
{
    (void)now;
    if (!lk || !out)
        return DNET_LINK_EINVAL;
    if (lk->state == DNET_LINK_CLOSED || lk->state == DNET_LINK_DI_SENT)
        return DNET_LINK_ESTATE;

    memset(out, 0, sizeof(*out));
    out->type    = DNET_NSP_T_DI;
    out->msgflg  = DNET_NSP_MSGFLG_DI;
    out->dstaddr = lk->remote_addr;
    out->srcaddr = lk->local_addr;
    out->reason  = reason;
    out->datalen = 0;

    lk->disc_reason = reason;
    lk->state = DNET_LINK_DI_SENT;
    return DNET_LINK_OK;
}

/* Build a link service carrying `flags` with other-data number `seg`. */
static void build_ls(const struct dnet_link *lk, uint8_t flags, uint16_t seg,
                     struct dnet_nsp_msg *out)
{
    memset(out, 0, sizeof(*out));
    out->type    = DNET_NSP_T_LS;
    out->msgflg  = DNET_NSP_MSGFLG_LS;
    out->dstaddr = lk->remote_addr;
    out->srcaddr = lk->local_addr;
    /* ACKNUM acks the other-data subchannel: the highest in-order other-data
     * number we have received (0 right after CC -> 0x8000). Real link state. */
    out->has_acknum = 1;
    out->acknum  = (uint16_t)(DNET_NSP_ACK_QUAL | (lk->oth_recv & DNET_LINK_SEQ_MASK));
    out->segnum  = seg;
    out->ls_flags = flags;
    out->fc_val   = 0;
    out->datalen  = 0;
}

/* Start a new link service on the other-data subchannel. */
static void start_ls(struct dnet_link *lk, uint8_t flags, struct dnet_nsp_msg *out,
                     dnet_tick_t now)
{
    lk->oth_send     = seq_inc(lk->oth_send);
    lk->ls_out       = 1;
    lk->ls_out_flags = flags;
    lk->ls_out_seg   = lk->oth_send;
    lk->ls_deadline  = now + DNET_LINK_RTX_SECS;
    lk->ls_sent++;
    build_ls(lk, flags, lk->ls_out_seg, out);
}

int dnet_link_link_service(struct dnet_link *lk, struct dnet_nsp_msg *out,
                           dnet_tick_t now)
{
    if (!lk || !out)
        return DNET_LINK_EINVAL;
    if (lk->state != DNET_LINK_RUN || lk->ls_out)
        return DNET_LINK_ESTATE;
    /* LSFLAGS 0 (no change), FCVAL 0: the link service a VAX sends right after
     * the connect handshake (docs/oracle/vax-copy-fal-dap.hex.txt). */
    start_ls(lk, DNET_NSP_LS_FCMOD_NOCHANGE, out, now);
    return DNET_LINK_OK;
}

/* Our transmit side learns the peer has everything through `num` (and, for a
 * NAK, wants num+1 onward again). */
static void data_ack(struct dnet_link *lk, uint16_t num, int nak, dnet_tick_t now)
{
    unsigned k = 0;
    while (k < lk->tx_ever &&
           seq_le(lk->txq[(lk->tx_head + k) % DNET_LINK_TXQ].seg, num))
        k++;
    if (k == 0 && num != lk->send_ack)
        return;                       /* stale or bogus: names nothing we sent */
    if (k) {
        lk->tx_head   = (lk->tx_head + k) % DNET_LINK_TXQ;
        lk->tx_count -= k;
        lk->tx_ever  -= k;
        lk->tx_sent   = (lk->tx_sent > k) ? lk->tx_sent - k : 0;
        lk->send_ack  = num;
        lk->rtx_count = 0;
        lk->rtx_deadline = now + DNET_LINK_RTX_SECS;
        lk->nak_rewound = 0;
    }
    lk->acks_recv++;
    if (nak) {
        lk->naks_recv++;
        /* Go back N: resend from the oldest -- once per NAK point. Every
         * segment that followed a lost one draws the same NAK; rewinding on
         * each would resend the window again and again. The timer still
         * covers a lost resend. */
        if (!lk->nak_rewound || lk->tx_sent >= lk->rewind_mark) {
            lk->rewind_mark = lk->tx_sent;
            lk->tx_sent = 0;
            lk->nak_rewound = 1;
        }
    }
}

/* The peer acknowledged our other-data through `num`. */
static void oth_ack(struct dnet_link *lk, uint16_t num, int nak, dnet_tick_t now)
{
    if (!lk->ls_out)
        return;
    if (seq_le(lk->ls_out_seg, num)) {
        lk->ls_out = 0;
        lk->rtx_count = 0;
        lk->acks_recv++;
    } else if (nak) {
        lk->ls_deadline = now;        /* resend it at the next poll */
    }
}

/* Absorb one ack field. `on_other` says the carrying message belongs to the
 * other-data subchannel (LS, other-data ack): there QUAL 0/1 acks other data
 * and QUAL 2/3 acks data; on a data-subchannel message it is the reverse. */
static void absorb_ack(struct dnet_link *lk, uint16_t field, int on_other, dnet_tick_t now)
{
    if (!(field & DNET_NSP_ACK_QUAL))
        return;
    uint16_t q = (uint16_t)(field & DNET_NSP_ACK_QUAL_MASK);
    uint16_t num = (uint16_t)(field & DNET_LINK_SEQ_MASK);
    int nak, cross;
    switch (q) {
    case DNET_NSP_ACK_Q_ACK:  nak = 0; cross = 0; break;
    case DNET_NSP_ACK_Q_NAK:  nak = 1; cross = 0; break;
    case DNET_NSP_ACK_Q_XACK: nak = 0; cross = 1; break;
    case DNET_NSP_ACK_Q_XNAK: nak = 1; cross = 1; break;
    default: return;                  /* reserved QUAL: ignored honestly */
    }
    if (on_other ^ cross)
        oth_ack(lk, num, nak, now);
    else
        data_ack(lk, num, nak, now);
}

static void absorb_acks(struct dnet_link *lk, const struct dnet_nsp_msg *in,
                        int on_other, dnet_tick_t now)
{
    if (in->has_acknum)
        absorb_ack(lk, in->acknum, on_other, now);
    if (in->has_ackoth)
        absorb_ack(lk, in->ackoth, on_other, now);
}

/* Receive flow control: ask the peer to stop or resume as our room moves. */
static void rx_flow_eval(struct dnet_link *lk)
{
    if (!lk->rx_fc)
        return;
    if (!lk->loc_stopped && lk->rx_room <= lk->rx_low) {
        lk->loc_stopped = 1;
        lk->ls_want = DNET_NSP_LS_FCMOD_STOP;
    } else if (lk->loc_stopped && lk->rx_room >= lk->rx_high) {
        lk->loc_stopped = 0;
        lk->ls_want = DNET_NSP_LS_FCMOD_START;
        lk->rx_nak_sent = 0;          /* a gap after the resume is NAKed anew */
    }
}

void dnet_link_rx_window(struct dnet_link *lk, unsigned cap)
{
    if (!lk)
        return;
    lk->rx_fc   = 1;
    lk->rx_room = cap;
    lk->rx_low  = cap / 2u;
    lk->rx_high = (3u * cap) / 4u;
    if (lk->rx_high <= lk->rx_low)
        lk->rx_high = lk->rx_low + 1u;
    lk->loc_stopped = 0;
}

void dnet_link_rx_room(struct dnet_link *lk, unsigned room)
{
    if (!lk || !lk->rx_fc)
        return;
    lk->rx_room = room;
    rx_flow_eval(lk);
}

static void build_data_ack(const struct dnet_link *lk, int nak, struct dnet_nsp_msg *reply)
{
    memset(reply, 0, sizeof(*reply));
    reply->type       = DNET_NSP_T_ACK;
    reply->msgflg     = DNET_NSP_MSGFLG_ACK;
    reply->dstaddr    = lk->remote_addr;
    reply->srcaddr    = lk->local_addr;
    reply->has_acknum = 1;
    reply->acknum     = (uint16_t)(DNET_NSP_ACK_QUAL | (nak ? DNET_NSP_ACK_Q_NAK : 0) |
                                   (lk->recv_seq & DNET_LINK_SEQ_MASK));
}

static void build_oth_ack(const struct dnet_link *lk, int nak, struct dnet_nsp_msg *reply)
{
    memset(reply, 0, sizeof(*reply));
    reply->type       = DNET_NSP_T_OTHACK;
    reply->msgflg     = DNET_NSP_MSGFLG_OTHACK;
    reply->dstaddr    = lk->remote_addr;
    reply->srcaddr    = lk->local_addr;
    reply->has_acknum = 1;
    reply->acknum     = (uint16_t)(DNET_NSP_ACK_QUAL | (nak ? DNET_NSP_ACK_Q_NAK : 0) |
                                   (lk->oth_recv & DNET_LINK_SEQ_MASK));
}

/* Build a Disconnect Confirm reply for a received Disconnect Initiate. */
static void build_dc(const struct dnet_link *lk, uint16_t reason,
                     struct dnet_nsp_msg *out)
{
    memset(out, 0, sizeof(*out));
    out->type    = DNET_NSP_T_DC;
    out->msgflg  = DNET_NSP_MSGFLG_DC;
    out->dstaddr = lk->remote_addr;
    out->srcaddr = lk->local_addr;
    out->reason  = reason;
    out->datalen = 0;
}

int dnet_link_rx(struct dnet_link *lk, const struct dnet_nsp_msg *in,
                 dnet_tick_t now, struct dnet_nsp_msg *reply, int *has_reply,
                 enum dnet_link_event *event)
{
    if (!lk || !in)
        return DNET_LINK_EINVAL;
    if (has_reply)
        *has_reply = 0;
    if (event)
        *event = DNET_LINK_EV_NONE;

    switch (in->type) {
    case DNET_NSP_T_CI:
        /* Inbound connect request: only meaningful on a CLOSED link. Learn the
         * originator's logical-link address and the negotiated parameters, and
         * report a connect indication; the caller decides accept vs reject. */
        if (lk->state != DNET_LINK_CLOSED)
            return DNET_LINK_OK;      /* duplicate / unexpected -- ignore honestly */
        lk->remote_addr = in->srcaddr;
        lk->services    = in->services;
        lk->rem_fcopt   = (uint8_t)(in->services & DNET_NSP_SVC_FCOPT_MASK);
        lk->info        = in->info;
        lk->segsize     = in->segsize;
        lk->conn_len    = (in->datalen > DNET_NSP_MAX_DATA)
                              ? DNET_NSP_MAX_DATA : in->datalen;
        if (lk->conn_len)
            memcpy(lk->conn_data, in->data, lk->conn_len);
        lk->state = DNET_LINK_CR_RCVD;
        if (event)
            *event = DNET_LINK_EV_CONNECT_IND;
        return DNET_LINK_OK;

    case DNET_NSP_T_CC:
        /* Confirm of our Connect Initiate: only on a CI_SENT link, and only if
         * it names our logical-link address. Learn the peer's address, run. */
        if (lk->state != DNET_LINK_CI_SENT)
            return DNET_LINK_OK;
        if (in->dstaddr != lk->local_addr)
            return DNET_LINK_OK;      /* not ours -- ignore, never fake a connect */
        lk->remote_addr = in->srcaddr;
        lk->rem_fcopt   = (uint8_t)(in->services & DNET_NSP_SVC_FCOPT_MASK);
        if (in->segsize)
            lk->segsize = in->segsize;
        lk->state = DNET_LINK_RUN;
        if (event)
            *event = DNET_LINK_EV_CONNECT_CONF;
        return DNET_LINK_OK;

    case DNET_NSP_T_DATA: {
        if (lk->state != DNET_LINK_RUN)
            return DNET_LINK_OK;
        if (in->dstaddr != lk->local_addr)
            return DNET_LINK_OK;
        /* Piggybacked acknowledgements of what we sent (rd vms-985). */
        absorb_acks(lk, in, 0, now);
        uint16_t seg = (uint16_t)(in->segnum & DNET_LINK_SEQ_MASK);
        uint16_t expect = seq_inc(lk->recv_seq);
        int nak = 0, deliver = 0;
        if (seg == expect) {
            if (lk->rx_fc && lk->rx_room == 0) {
                /* No buffer for it: discard, never acknowledge, and NAK so
                 * the peer resends it -- the way a VAX treats a segment it
                 * cannot take (S1 capture: FCMOD stop, then NAK 0x900e). */
                nak = 1;
                lk->rx_nak_sent = 1;
                lk->rx_discarded++;
                rx_flow_eval(lk);
            } else {
                lk->recv_seq = seg;
                lk->rx_nak_sent = 0;
                lk->data_recv++;
                deliver = 1;
                if (lk->rx_fc) {
                    lk->rx_room--;
                    rx_flow_eval(lk);
                }
            }
        } else if (!seq_lt(seg, expect)) {
            /* A gap: NAK it once, so the peer goes back to `expect`; the
             * segments after it that follow are simply acked at recv_seq. */
            nak = !lk->rx_nak_sent;
            lk->rx_nak_sent = 1;
        }
        /* else a duplicate (our ACK was lost): re-acknowledge what we hold. */
        if (reply) {
            build_data_ack(lk, nak, reply);
            if (has_reply)
                *has_reply = 1;
            if (nak) lk->naks_sent++; else lk->acks_sent++;
        }
        if (event && deliver)
            *event = DNET_LINK_EV_DATA;
        return DNET_LINK_OK;
    }

    case DNET_NSP_T_ACK:
        if (lk->state != DNET_LINK_RUN)
            return DNET_LINK_OK;
        if (in->dstaddr != lk->local_addr)
            return DNET_LINK_OK;
        absorb_acks(lk, in, 0, now);
        if (event)
            *event = DNET_LINK_EV_ACK;
        return DNET_LINK_OK;

    case DNET_NSP_T_DI:
        /* Peer disconnected. Confirm it (DC) and close. Accept from any non-
         * closed state (RUN, CR_RCVD, CI_SENT, or a crossed DI_SENT). */
        if (lk->state == DNET_LINK_CLOSED)
            return DNET_LINK_OK;
        if (lk->remote_addr == 0)
            lk->remote_addr = in->srcaddr;  /* learn it for the DC if unknown */
        lk->disc_reason = in->reason;
        if (reply) {
            build_dc(lk, DNET_NSP_REASON_DISC_COMPLETE, reply);
            if (has_reply)
                *has_reply = 1;
        }
        lk->state = DNET_LINK_CLOSED;
        if (event)
            *event = DNET_LINK_EV_DISCONNECT;
        return DNET_LINK_OK;

    case DNET_NSP_T_DC:
        /* Confirm of our Disconnect Initiate: the link is now fully closed. */
        if (lk->state != DNET_LINK_DI_SENT)
            return DNET_LINK_OK;
        lk->state = DNET_LINK_CLOSED;
        if (event)
            *event = DNET_LINK_EV_DISCONNECT_CONF;
        return DNET_LINK_OK;

    case DNET_NSP_T_LS: {
        /* Peer's link service on the other-data subchannel (rd vms-6165,
         * rd vms-985): acks first, then -- if it is the next other-data
         * message -- its FCMOD (stop / resume our data) and request-count
         * FCVAL, acknowledged with an other-data ack. A repeat is re-acked
         * and not applied twice. Outside RUN or for another link: ignored. */
        if (lk->state != DNET_LINK_RUN)
            return DNET_LINK_OK;
        if (in->dstaddr != lk->local_addr)
            return DNET_LINK_OK;
        absorb_acks(lk, in, 1, now);
        uint16_t seg = (uint16_t)(in->segnum & DNET_LINK_SEQ_MASK);
        int nak = 0, applied = 0;
        if (seg == seq_inc(lk->oth_recv)) {
            lk->oth_recv = seg;
            applied = 1;
            switch (in->ls_flags & DNET_NSP_LS_FCMOD_MASK) {
            case DNET_NSP_LS_FCMOD_STOP:
                lk->rem_stopped = 1;
                break;
            case DNET_NSP_LS_FCMOD_START:
                if (lk->rem_stopped) {
                    lk->rtx_deadline = now + DNET_LINK_RTX_SECS;
                    /* What we sent while it was full may be gone; the peer
                     * NAKs the gap once it sees our next segment, and that NAK
                     * must rewind us even if an earlier one already did. */
                    lk->nak_rewound = 0;
                }
                lk->rem_stopped = 0;
                break;
            default:
                break;
            }
            if ((in->ls_flags & DNET_NSP_LS_FCVAL_INT_MASK) == DNET_NSP_LS_FCVAL_INT_DATA &&
                lk->rem_fcopt != DNET_NSP_SVC_FCOPT_NONE) {
                int v = (int)lk->rem_reqcnt + (int)in->fc_val;
                if (v > 127) v = 127;
                if (v < -128) v = -128;
                lk->rem_reqcnt = (int16_t)v;
            }
        } else if (!seq_le(seg, lk->oth_recv)) {
            nak = 1;                  /* out of order on the other-data subchannel */
        }
        if (reply) {
            build_oth_ack(lk, nak, reply);
            if (has_reply)
                *has_reply = 1;
            lk->acks_sent++;
        }
        if (event && applied)
            *event = DNET_LINK_EV_LINK_SERVICE;
        return DNET_LINK_OK;
    }

    case DNET_NSP_T_OTHACK:
        /* Peer acked our other-data (a link service); a cross-subchannel field
         * in it acks data. No reply. */
        if (lk->state != DNET_LINK_RUN)
            return DNET_LINK_OK;
        if (in->dstaddr != lk->local_addr)
            return DNET_LINK_OK;
        absorb_acks(lk, in, 1, now);
        return DNET_LINK_OK;

    default:
        return DNET_LINK_OK;
    }
}

int dnet_link_poll(struct dnet_link *lk, dnet_tick_t now,
                   struct dnet_nsp_msg *out, int *has_out,
                   enum dnet_link_event *event)
{
    if (!lk || !out || !has_out)
        return DNET_LINK_EINVAL;
    *has_out = 0;
    if (event)
        *event = DNET_LINK_EV_NONE;
    if (lk->state != DNET_LINK_RUN)
        return DNET_LINK_OK;

    /* 1. The other-data subchannel: resend an unacked link service, or start
     *    the stop/resume the receive side decided on. */
    if (lk->ls_out && now >= lk->ls_deadline) {
        if (lk->rtx_count >= DNET_LINK_RTX_MAX)
            goto dead;
        lk->rtx_count++;
        lk->ls_deadline = now + DNET_LINK_RTX_SECS;
        lk->ls_sent++;
        build_ls(lk, lk->ls_out_flags, lk->ls_out_seg, out);
        *has_out = 1;
        return DNET_LINK_OK;
    }
    if (!lk->ls_out && lk->ls_want) {
        uint8_t f = lk->ls_want;
        lk->ls_want = 0;
        start_ls(lk, f, out, now);
        *has_out = 1;
        return DNET_LINK_OK;
    }

    /* 2. No ACK for the oldest segment on the wire in time: go back N. While
     *    the peer holds us stopped the timer does not run. */
    if (lk->tx_sent > 0 && !lk->rem_stopped && now >= lk->rtx_deadline) {
        if (lk->rtx_count >= DNET_LINK_RTX_MAX)
            goto dead;
        lk->rtx_count++;
        lk->tx_sent = 0;
    }

    /* 3. The next segment not on the wire, if flow control lets it go. */
    if (lk->tx_sent < lk->tx_count && may_send(lk, lk->tx_sent)) {
        emit_next(lk, out, now);
        *has_out = 1;
        return DNET_LINK_OK;
    }

    /* 4. A normal close waiting on the queue: everything is acknowledged now,
     *    so the Disconnect Initiate may go. */
    if (lk->close_pending && lk->tx_count == 0) {
        lk->close_pending = 0;
        (void)dnet_link_disconnect(lk, lk->close_reason, out, now);
        *has_out = 1;
    }
    return DNET_LINK_OK;

dead:
    /* DNET_LINK_RTX_MAX retransmits and nothing acknowledged: the peer is gone.
     * Close honestly; the owner reports the loss (never a fabricated success). */
    lk->state       = DNET_LINK_CLOSED;
    lk->disc_reason = DNET_LINK_REASON_UNREACHABLE;
    lk->tx_count = lk->tx_sent = lk->tx_ever = 0;
    lk->ls_out = 0;
    lk->ls_want = 0;
    lk->close_pending = 0;
    if (event)
        *event = DNET_LINK_EV_DISCONNECT;
    return DNET_LINK_OK;
}

int dnet_link_tick(struct dnet_link *lk, dnet_tick_t now,
                   struct dnet_nsp_msg *out, int *has_out)
{
    if (!lk)
        return DNET_LINK_EINVAL;
    if (has_out)
        *has_out = 0;
    if (lk->state != DNET_LINK_CI_SENT)
        return DNET_LINK_OK;
    if (now < lk->ci_deadline)
        return DNET_LINK_OK;

    if (lk->ci_retransmits >= DNET_LINK_MAX_RETRANS) {
        /* No Confirm after the whole retransmit budget: the peer is unreachable.
         * Close honestly (this is exactly VAX1 abandoning SET HOST after 8
         * retransmits in register sec 4.6), never a fabricated connect. */
        lk->state       = DNET_LINK_CLOSED;
        lk->disc_reason = DNET_LINK_REASON_UNREACHABLE;
        return DNET_LINK_OK;
    }
    if (out) {
        build_ci(lk, out);
        if (has_out)
            *has_out = 1;
        lk->ci_sent++;
    }
    lk->ci_retransmits++;
    lk->ci_deadline = now + DNET_LINK_CI_RETRANS_SECS;
    return DNET_LINK_OK;
}

/* ---- logical-link address allocation (rd vms-3106) ---- */

uint16_t dnet_lla_seed(uint64_t entropy)
{
    /* splitmix64 finaliser: every entropy bit reaches the low 16 bits. */
    uint64_t z = entropy + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const uint32_t span = 0x10000u - DNET_LLA_MIN;
    return (uint16_t)(DNET_LLA_MIN + (uint32_t)(z % span));
}

uint16_t dnet_lla_take(uint16_t *cursor,
                       int (*in_use)(uint16_t addr, void *ctx), void *ctx)
{
    if (!cursor)
        return 0;
    const uint32_t span = 0x10000u - DNET_LLA_MIN;
    for (uint32_t tries = 0; tries < span; tries++) {
        uint16_t c = *cursor;
        if (c < DNET_LLA_MIN)
            c = (uint16_t)DNET_LLA_MIN;
        *cursor = (c == 0xFFFFu) ? (uint16_t)DNET_LLA_MIN : (uint16_t)(c + 1u);
        if (!in_use || !in_use(c, ctx))
            return c;
    }
    return 0;
}
