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
    out->services = lk->services;
    out->info     = lk->info;
    out->segsize  = lk->segsize;
    /* A bare accept carries no connect data; a caller wanting to return
     * session-control accept data is a later (SET HOST/FAL) rung. */
    out->datalen  = 0;

    lk->state = DNET_LINK_RUN;
    return DNET_LINK_OK;
}

int dnet_link_send_data(struct dnet_link *lk, const uint8_t *data, size_t len,
                        struct dnet_nsp_msg *out, dnet_tick_t now)
{
    (void)now;
    if (!lk || !out || (len && !data))
        return DNET_LINK_EINVAL;
    if (len > DNET_NSP_MAX_DATA)
        return DNET_LINK_EINVAL;
    if (lk->state != DNET_LINK_RUN)
        return DNET_LINK_ESTATE;

    memset(out, 0, sizeof(*out));
    out->type    = DNET_NSP_T_DATA;
    /* BOM+EOM: this rung sends each message as a single self-contained segment
     * (segmentation across multiple segments is a later flow-control rung). */
    out->msgflg  = DNET_NSP_MSGFLG_DATA;
    out->dstaddr = lk->remote_addr;
    out->srcaddr = lk->local_addr;

    lk->send_seq = (uint16_t)((lk->send_seq + 1) & DNET_LINK_SEQ_MASK);
    if (lk->send_seq == 0)
        lk->send_seq = 1;   /* NSP data segment numbers are nonzero */
    out->segnum  = lk->send_seq;

    /* Piggyback our current receive acknowledgement if we have received data. */
    if (lk->recv_seq != 0) {
        out->has_acknum = 1;
        out->acknum = (uint16_t)(DNET_NSP_ACK_QUAL | (lk->recv_seq & DNET_LINK_SEQ_MASK));
    }

    out->datalen = (uint16_t)len;
    if (len)
        memcpy(out->data, data, len);

    lk->data_sent++;
    return DNET_LINK_OK;
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
        /* Absorb a piggybacked acknowledgement of our sent data, if present. */
        if (in->has_acknum && (in->acknum & DNET_NSP_ACK_QUAL)) {
            lk->send_ack = (uint16_t)(in->acknum & DNET_LINK_SEQ_MASK);
            lk->acks_recv++;
        }
        uint16_t seg = (uint16_t)(in->segnum & DNET_LINK_SEQ_MASK);
        uint16_t expect = (uint16_t)((lk->recv_seq + 1) & DNET_LINK_SEQ_MASK);
        if (expect == 0)
            expect = 1;
        if (seg != expect)
            return DNET_LINK_OK;      /* out-of-order/dup: no fake accept, no ack bump */
        lk->recv_seq = seg;
        lk->data_recv++;
        /* Acknowledge the segment (explicit data-ack). */
        if (reply) {
            memset(reply, 0, sizeof(*reply));
            reply->type       = DNET_NSP_T_ACK;
            reply->msgflg     = DNET_NSP_MSGFLG_ACK;
            reply->dstaddr    = lk->remote_addr;
            reply->srcaddr    = lk->local_addr;
            reply->has_acknum = 1;
            reply->acknum     = (uint16_t)(DNET_NSP_ACK_QUAL |
                                           (lk->recv_seq & DNET_LINK_SEQ_MASK));
            if (has_reply)
                *has_reply = 1;
            lk->acks_sent++;
        }
        if (event)
            *event = DNET_LINK_EV_DATA;
        return DNET_LINK_OK;
    }

    case DNET_NSP_T_ACK:
        if (lk->state != DNET_LINK_RUN)
            return DNET_LINK_OK;
        if (in->dstaddr != lk->local_addr)
            return DNET_LINK_OK;
        if (in->has_acknum && (in->acknum & DNET_NSP_ACK_QUAL)) {
            lk->send_ack = (uint16_t)(in->acknum & DNET_LINK_SEQ_MASK);
            lk->acks_recv++;
        }
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

    default:
        return DNET_LINK_OK;
    }
    (void)now;
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
