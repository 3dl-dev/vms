/*
 * scs_vc.c - SCS sequenced-message virtual-circuit (VC) engine (vms-691).
 * See scs_vc.h for the full clean-room provenance and the GROUNDED-vs-inferred
 * field breakdown of the 0x48 credit-return short.
 */
#include "scs_vc.h"

#include <string.h>

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

int scs_credit_build(const struct scs_credit_params *p,
                     uint8_t out[SCS_CREDIT_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }

    /* The whole frame is zero except the fields we set; this gives the [28:30],
     * [32:34], [36:38], [40] zero fields and the Ethernet pad-to-60 for free. */
    memset(out, 0, SCS_CREDIT_FRAME_LEN);

    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, p->dst_mac, 6);
    memcpy(out + 6, p->src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content begins at abs 14 (payload offset 0). Offsets below are
     * payload-relative per spec sec 4h; absolute = 14 + payload offset. */
    put_le16(out + 14 + 0, SCS_CREDIT_SCA_LEN - 2); /* [0:2] length 0x0027 (GROUNDED) */
    memcpy(out + 14 + 2, p->peer_logical, 6);        /* [2:8] dst-logical (GROUNDED subst) */
    put_le16(out + 14 + 8, 0x0001);                  /* [8:10] connect flag (GROUNDED) */
    memcpy(out + 14 + 10, p->src_logical, 6);        /* [10:16] src-logical = aa:00:04:00:<sysid>
                                                      * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */
    out[14 + 16] = SCS_CREDIT_OPCODE;                /* [16] opcode 0x48 (GROUNDED) */
    out[14 + 17] = SCS_CREDIT_FORMAT;                /* [17] format 0x13 (GROUNDED 622/622) */
    put_le16(out + 14 + 18, p->acked_seq);           /* [18:20] acked seq (GROUNDED) */
    put_le16(out + 14 + 20, 0x0000);                 /* [20:22] send-seq 0 (GROUNDED 622/622) */
    put_le16(out + 14 + 22, 0x0001);                 /* [22:24] const 0x0001 (GROUNDED 622/622) */
    put_le16(out + 14 + 24, SCS_NISCS_LAN_OVRHD);    /* [24:26] 18 = NISCS_LAN_OVRHD (GROUNDED) */
    put_le16(out + 14 + 26, p->acked_seq);           /* [26:28] acked-seq mirror (GROUNDED 622/622) */
    /* [28:30] zero (from memset) */
    put_le16(out + 14 + 30, p->secondary_seq);       /* [30:32] secondary counter (INFERRED, reproduced) */
    /* [32:34] zero (from memset) */
    put_le16(out + 14 + 34, p->acked_seq);           /* [34:36] acked-seq 3rd repeat (GROUNDED 616/622) */
    /* [36:38] zero (from memset) */
    put_le16(out + 14 + 38, 0x0001);                 /* [38:40] const 0x0001 (INFERRED, 598/622 clean value) */
    /* [40] zero pad (from memset); abs 55-59 Ethernet pad (from memset) */

    return 0;
}

/* --- VC engine state --- */

void scs_vc_init(struct scs_vc *vc)
{
    if (vc == NULL) {
        return;
    }
    memset(vc, 0, sizeof(*vc));
    scs_seq_init(&vc->seq);
    vc->initialized = 1;
}

void scs_vc_reset_seq(struct scs_vc *vc)
{
    if (vc == NULL) {
        return;
    }
    /* send_seq=1, recv_seq=0 -- the fresh post-START VC starting point
     * (spec sec 4i.A). The phase-2 START/config counters do NOT carry into
     * the SCS VC. */
    scs_seq_init(&vc->seq);
    vc->have_unacked = 0;
    vc->unacked_seq = 0;
    vc->unacked_sent_ms = 0;
    vc->retransmit_count = 0;
    vc->initialized = 1;
}

void scs_vc_note_recv(struct scs_vc *vc, uint16_t peer_send_seq)
{
    if (vc == NULL) {
        return;
    }
    /* A pure ack (send_seq==0) carries no new sequence -- do not advance. */
    if (peer_send_seq == 0) {
        return;
    }
    scs_seq_note_recv(&vc->seq, peer_send_seq);
}

int scs_vc_owes_credit(uint16_t peer_send_seq)
{
    /* Strict 1-for-1 credit: every sequenced message (send_seq != 0) is
     * answered by exactly one 0x48; a pure ack (send_seq == 0) is not, which
     * prevents an ack-for-ack storm. */
    return peer_send_seq != 0;
}

int scs_vc_build_credit_for(struct scs_vc *vc,
                            const uint8_t dst_mac[6],
                            const uint8_t src_mac[6],
                            const uint8_t src_logical[6],
                            const uint8_t peer_logical[6],
                            uint8_t out[SCS_CREDIT_FRAME_LEN])
{
    if (vc == NULL || dst_mac == NULL || src_mac == NULL ||
        src_logical == NULL || peer_logical == NULL || out == NULL) {
        return -1;
    }
    struct scs_credit_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, dst_mac, 6);
    memcpy(p.src_mac, src_mac, 6);
    memcpy(p.src_logical, src_logical, 6);
    memcpy(p.peer_logical, peer_logical, 6);
    p.acked_seq = vc->seq.recv_seq;
    /* Secondary counter [30:32] is inferred as "the sender's own outstanding
     * seq"; reproduce OVMX's current send_seq (spec sec 4h(3), labeled). */
    p.secondary_seq = vc->seq.send_seq;

    int rc = scs_credit_build(&p, out);
    if (rc == 0) {
        vc->credit_returns_sent++;
    }
    return rc;
}

void scs_vc_record_sent(struct scs_vc *vc, uint16_t seq, uint64_t now_ms)
{
    if (vc == NULL) {
        return;
    }
    vc->have_unacked = 1;
    vc->unacked_seq = seq;
    vc->unacked_sent_ms = now_ms;
    vc->retransmit_count = 0;
}

/* True if `a` has reached-or-passed `target` under 16-bit modular arithmetic
 * (handles the sequence-number wrap without a false "already acked"). */
static int seq_reached(uint16_t a, uint16_t target)
{
    return (uint16_t)(a - target) < 0x8000u;
}

void scs_vc_note_peer_ack(struct scs_vc *vc, uint16_t peer_recv_ack)
{
    if (vc == NULL || !vc->have_unacked) {
        return;
    }
    if (seq_reached(peer_recv_ack, vc->unacked_seq)) {
        vc->have_unacked = 0;
        vc->retransmit_count = 0;
    }
}

int scs_vc_retransmit_due(const struct scs_vc *vc, uint64_t now_ms,
                          uint64_t timeout_ms)
{
    if (vc == NULL || !vc->have_unacked) {
        return 0;
    }
    /* Guard against a clock that appears to run backwards. */
    if (now_ms < vc->unacked_sent_ms) {
        return 0;
    }
    return (now_ms - vc->unacked_sent_ms) >= timeout_ms;
}

void scs_vc_mark_retransmitted(struct scs_vc *vc, uint64_t now_ms)
{
    if (vc == NULL || !vc->have_unacked) {
        return;
    }
    vc->unacked_sent_ms = now_ms;
    vc->retransmit_count++;
    vc->retransmits++;
}
