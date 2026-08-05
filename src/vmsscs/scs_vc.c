/*
 * scs_vc.c - SCS sequenced-message virtual-circuit (VC) engine (vms-691).
 * See scs_vc.h for the full clean-room provenance and the GROUNDED-vs-inferred
 * field breakdown of the 0x48 credit-return short.
 */
#include "scs_vc.h"

#include <stdlib.h>
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
    /* vms-abc: a fresh post-START VC has seen no sequenced message yet, and is
     * not broken. The CUMULATIVE counters (seq_gaps, breaks) are observability
     * and deliberately survive the reset. */
    vc->seq_anchored = 0;
    vc->broken = 0;
    vc->break_reason = (int)SCS_VC_BREAK_NONE;
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

/* ======================================================================
 * vms-4071: the VC FORMATION state machine (pp. 2-12..2-16).
 * Every rule below is quoted at its implementation site; the enclosing
 * design choices and their labeling are in scs_vc.h.
 * ====================================================================== */

unsigned scs_vc_retry_limit(void)
{
    /* Kill-switch, re-read every call so it is never a cached decision. */
    const char *env = getenv(SCS_VC_NO_RETRY_LIMIT_ENV);
    if (env != NULL && env[0] == '1' && env[1] == '\0') {
        return 0; /* unlimited: pre-vms-4071 behaviour */
    }
    return SCS_VC_FORMATION_RETRY_LIMIT;
}

int scs_vc_early_ack_enabled(void)
{
    /* Opt-in, default OFF, re-read every call so it is never a cached decision.
     * OFF preserves the pre-vms-4071 fresh-join frame interleaving; see the
     * header note on SCS_VC_EARLY_ACK_ENV. */
    const char *env = getenv(SCS_VC_EARLY_ACK_ENV);
    return (env != NULL && env[0] == '1' && env[1] == '\0') ? 1 : 0;
}

enum scs_vc_event scs_vc_classify_round(int is_ack, uint16_t config_round)
{
    /* OVMX design choice 1 in scs_vc.h: the observed 0/0/1/1/2/2 config-round
     * dialogue mapped onto Figure 2-7's START / STACK / ACK. */
    if (is_ack) {
        return SCS_VC_EV_ACK;
    }
    return (config_round == 0) ? SCS_VC_EV_START : SCS_VC_EV_STACK;
}

int scs_vc_is_circuit_packet(uint8_t opcode)
{
    /* OVMX design choice: only the SCS sequenced-message classes presuppose an
     * OPEN circuit (p. 2-16). 0x41 START/STACK/ACK and the HELLO do not. */
    switch (opcode) {
    case 0x4b: /* sequenced application message (connect + VC/DLM data) */
    case 0x5b: /* SCS$DIRECTORY lookup */
    case 0x7b: /* its retransmit class */
    case SCS_CREDIT_OPCODE: /* 0x48 credit-return short */
        return 1;
    default:
        return 0;
    }
}

void scs_vc_fsm_init(struct scs_pb *pb)
{
    if (pb == NULL) {
        return;
    }
    memset(&pb->fsm, 0, sizeof(pb->fsm));
    pb->fsm.last_emitted = SCS_VC_ACT_NONE;
    /* "When the PB is first initialized, it is marked to indicate that the
     * state of the virtual circuit is CLOSED." (p. 2-11) */
    pb->vc_state = SCS_VC_CLOSED;
}

/* Arm the p. 2-14 timer on the packet we just emitted, resetting the retry
 * counter: "whenever a port driver sends a START or a STACK ... it starts a
 * timer and expects a response." */
static void fsm_arm(struct scs_pb *pb, enum scs_vc_action emitted, uint64_t now_ms)
{
    pb->fsm.last_emitted = emitted;
    pb->fsm.timer_armed = 1;
    pb->fsm.timer_ms = now_ms;
    pb->fsm.retries = 0;
}

/* The circuit reached OPEN: no response is outstanding, so no timer runs. */
static void fsm_disarm(struct scs_pb *pb)
{
    pb->fsm.timer_armed = 0;
    pb->fsm.retries = 0;
}

/* "SCA requires that formation of the virtual circuit be abandoned." (p. 2-14)
 * An abandoned circuit is not formed, so its state is CLOSED (p. 2-11); the
 * flag records why (OVMX design choice 4 in scs_vc.h). */
static enum scs_vc_action fsm_abandon(struct scs_pb *pb)
{
    pb->vc_state = SCS_VC_CLOSED;
    pb->fsm.timer_armed = 0;
    pb->fsm.abandoned = 1;
    return SCS_VC_ACT_ABANDON;
}

enum scs_vc_action scs_vc_fsm_send_start(struct scs_pb *pb, uint64_t now_ms)
{
    if (pb == NULL || pb->fsm.abandoned || pb->vc_state != SCS_VC_CLOSED) {
        return SCS_VC_ACT_NONE;
    }
    /* Figure 2-7: "o SEND 'START' TO NODE_2's PORT DRIVER / o VC STATE =
     * 'START SENT'". */
    pb->vc_state = SCS_VC_START_SENT;
    fsm_arm(pb, SCS_VC_ACT_SEND_START, now_ms);
    return SCS_VC_ACT_SEND_START;
}

enum scs_vc_action scs_vc_fsm_recv(struct scs_pb *pb, enum scs_vc_event ev,
                                   uint64_t now_ms)
{
    if (pb == NULL || pb->fsm.abandoned) {
        return SCS_VC_ACT_NONE;
    }

    switch (pb->vc_state) {

    case SCS_VC_CLOSED:
        /* Design choice 5 (scs_vc.h): nothing has been issued, so there is no
         * response to classify. Ignore, exactly as Figure 2-8's NODE_2 "discards
         * the START from NODE_1" (p. 2-13). */
        return SCS_VC_ACT_NONE;

    case SCS_VC_START_SENT:
        /* p. 2-14: "If the port driver has just issued a START, it considers the
         * port to be in the START SENT state. Both START and STACK are
         * acceptable responses." */
        if (ev == SCS_VC_EV_STACK) {
            /* "A response of STACK advances the circuit all the way to the OPEN
             * state, and the port driver issues an ACK." */
            pb->vc_state = SCS_VC_OPEN;
            fsm_disarm(pb);
            return SCS_VC_ACT_SEND_ACK;
        }
        if (ev == SCS_VC_EV_START) {
            /* "A response of START advances the circuit only to the START
             * RECEIVED state, causing the port driver to issue a STACK." */
            pb->vc_state = SCS_VC_START_RECEIVED;
            fsm_arm(pb, SCS_VC_ACT_SEND_STACK, now_ms);
            return SCS_VC_ACT_SEND_STACK;
        }
        if (ev == SCS_VC_EV_ACK) {
            /* ACK is NOT among the acceptable responses listed for START SENT,
             * and "If an 'unacceptable' response is received, SCA requires that
             * formation of the virtual circuit be abandoned." (p. 2-14) */
            return fsm_abandon(pb);
        }
        /* SCS_VC_EV_OTHER: not a response to the START at all. The implied-ACK
         * rule is written for START RECEIVED only (p. 2-16), so this is ignored
         * rather than treated as unacceptable (design choice, scs_vc.h). */
        return SCS_VC_ACT_NONE;

    case SCS_VC_START_RECEIVED:
        /* p. 2-14: "If the port driver has just issued a STACK, it sets the
         * circuit to the START RECEIVED state and considers START, STACK, and
         * ACK to be acceptable responses." Nothing is unacceptable here. */
        if (ev == SCS_VC_EV_STACK) {
            /* "A response of either ACK or STACK will advance the circuit to the
             * OPEN state; and if the response is STACK, the port driver will
             * issue an ACK." */
            pb->vc_state = SCS_VC_OPEN;
            fsm_disarm(pb);
            return SCS_VC_ACT_SEND_ACK;
        }
        if (ev == SCS_VC_EV_ACK) {
            pb->vc_state = SCS_VC_OPEN;
            fsm_disarm(pb);
            return SCS_VC_ACT_NONE;
        }
        if (ev == SCS_VC_EV_START) {
            /* "If the response is only a START, the state of the circuit is left
             * unchanged and the port driver issues another STACK." This is the
             * p. 2-14 "acceptable but does not cause the circuit to advance"
             * case, so it is a REISSUE and counts against the retry limit. */
            unsigned limit = scs_vc_retry_limit();
            pb->fsm.retries++;
            pb->fsm.reissues++;
            pb->fsm.timer_ms = now_ms;
            pb->fsm.timer_armed = 1;
            pb->fsm.last_emitted = SCS_VC_ACT_SEND_STACK;
            if (limit != 0 && pb->fsm.retries >= limit) {
                return fsm_abandon(pb);
            }
            return SCS_VC_ACT_SEND_STACK;
        }
        /* SCS_VC_EV_OTHER -- THE IMPLIED ACK (p. 2-16): "the remote node
         * performs an operation that requires the circuit to be OPEN, and this
         * operation results in a packet being sent to the local node. In this
         * case, SCA states that the local node will treat that packet as an
         * 'implied ACK' and simply mark the circuit as being OPEN." */
        pb->vc_state = SCS_VC_OPEN;
        pb->fsm.implied_acks++;
        fsm_disarm(pb);
        return SCS_VC_ACT_NONE;

    case SCS_VC_OPEN:
    default:
        /* "each port driver simply discards the ACK it receives from the other
         * node because it already considers the virtual circuit to be OPEN."
         * (p. 2-12) */
        return SCS_VC_ACT_NONE;
    }
}

int scs_vc_fsm_timer_expired(const struct scs_pb *pb, uint64_t now_ms,
                             uint64_t timeout_ms)
{
    if (pb == NULL || !pb->fsm.timer_armed || pb->fsm.abandoned) {
        return 0;
    }
    if (now_ms < pb->fsm.timer_ms) {
        return 0; /* clock appears to run backwards -- do not fire */
    }
    return (now_ms - pb->fsm.timer_ms) >= timeout_ms;
}

enum scs_vc_action scs_vc_fsm_timeout(struct scs_pb *pb, uint64_t now_ms,
                                      unsigned retry_limit)
{
    if (pb == NULL || pb->fsm.abandoned || !pb->fsm.timer_armed) {
        return SCS_VC_ACT_NONE;
    }
    if (pb->fsm.last_emitted != SCS_VC_ACT_SEND_START &&
        pb->fsm.last_emitted != SCS_VC_ACT_SEND_STACK) {
        return SCS_VC_ACT_NONE;
    }
    /* "If the timer expires before any response is received ... SCA requires the
     * port driver to reissue the START or STACK (whichever it last sent)."
     * (p. 2-14) */
    pb->fsm.retries++;
    pb->fsm.reissues++;
    /* "it is expected that an operating system dependent retry limit be placed
     * on each such scenario. If that limit is reached, formation of the virtual
     * circuit is to be abandoned." (p. 2-14) */
    if (retry_limit != 0 && pb->fsm.retries >= retry_limit) {
        return fsm_abandon(pb);
    }
    pb->fsm.timer_ms = now_ms;
    return pb->fsm.last_emitted;
}

const char *scs_vc_event_name(enum scs_vc_event ev)
{
    switch (ev) {
    case SCS_VC_EV_START:
        return "START";
    case SCS_VC_EV_STACK:
        return "STACK";
    case SCS_VC_EV_ACK:
        return "ACK";
    case SCS_VC_EV_OTHER:
        return "circuit packet";
    default:
        return "?";
    }
}

/* ========================================================================= *
 * vms-abc -- BREAKING THE CIRCUIT WHEN A MESSAGE GUARANTEE FAILS (p. 2-31).
 *
 * See scs_vc.h for the quoted rule, the four labeled OVMX design choices, the
 * exact statement of what the kill switch gates, and the measurement showing
 * this detector fires on nothing OVMX has ever RECEIVED.
 * ========================================================================= */

const char *scs_vc_break_reason_name(enum scs_vc_break_reason r)
{
    switch (r) {
    case SCS_VC_BREAK_NONE:
        return "none";
    case SCS_VC_BREAK_SEQ_GAP:
        return "message sequentiality guarantee failed (sequence gap)";
    case SCS_VC_BREAK_DELIVERY:
        return "message delivery guarantee failed (retransmits exhausted)";
    case SCS_VC_BREAK_LOCAL:
        return "explicit local teardown";
    default:
        return "?";
    }
}

int scs_vc_break_enabled(void)
{
    /* Read fresh every call (see scs_vc.h): a cached answer could not be
     * bracketed by a test, and guardrail 23 requires the switch be RUN. */
    const char *v = getenv(SCS_VC_NO_BREAK_ENV);
    if (v == NULL || v[0] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 1;
    }
    return 0;
}

/* 1 iff 16-bit sequence `a` is strictly ahead of `b`, modular over the 16-bit
 * space (the same "reached" convention scs_vc_note_peer_ack already uses). */
static int seq_ahead(uint16_t a, uint16_t b)
{
    uint16_t d = (uint16_t)(a - b);
    return d != 0 && d < 0x8000u;
}

enum scs_vc_seq_verdict scs_vc_check_recv_seq(const struct scs_vc *vc,
                                              uint16_t peer_send_seq,
                                              unsigned *missing_out)
{
    if (missing_out != NULL) {
        *missing_out = 0;
    }
    if (vc == NULL) {
        return SCS_VC_SEQ_DUPLICATE;
    }
    /* A pure ack (a 0x48 credit-return: send_seq is 0 in 622/622 observed
     * frames, spec sec 4h(3)) carries no sequence and cannot breach
     * sequentiality. */
    if (peer_send_seq == 0) {
        return SCS_VC_SEQ_DUPLICATE;
    }
    if (!vc->seq_anchored) {
        return SCS_VC_SEQ_ANCHOR;
    }
    if (!seq_ahead(peer_send_seq, vc->seq.recv_seq)) {
        return SCS_VC_SEQ_DUPLICATE; /* a retransmit -- 506 of these in the captures */
    }
    unsigned ahead = (unsigned)(uint16_t)(peer_send_seq - vc->seq.recv_seq);
    if (ahead == 1u) {
        return SCS_VC_SEQ_IN_ORDER;
    }
    if (missing_out != NULL) {
        *missing_out = ahead - 1u;
    }
    return SCS_VC_SEQ_GAP;
}

enum scs_vc_seq_verdict scs_vc_note_recv_checked(struct scs_vc *vc,
                                                 uint16_t peer_send_seq,
                                                 unsigned *missing_out)
{
    if (missing_out != NULL) {
        *missing_out = 0;
    }
    if (vc == NULL) {
        return SCS_VC_SEQ_DUPLICATE;
    }
    enum scs_vc_seq_verdict v = scs_vc_check_recv_seq(vc, peer_send_seq, missing_out);
    if (v == SCS_VC_SEQ_GAP) {
        vc->seq_gaps++;
    }
    if (peer_send_seq != 0) {
        vc->seq_anchored = 1;
    }
    /* Advance exactly as scs_vc_note_recv() would -- including on a gap; see
     * the scs_vc.h note on why. */
    scs_vc_note_recv(vc, peer_send_seq);
    return v;
}

int scs_vc_delivery_failed(const struct scs_vc *vc)
{
    if (vc == NULL || !vc->have_unacked) {
        return 0;
    }
    return vc->retransmit_count >= SCS_VC_DELIVERY_RETRY_LIMIT;
}

unsigned scs_vc_break(struct scs_vc *vc, struct scs_pb *pb,
                      enum scs_vc_break_reason reason, FILE *log)
{
    if (pb == NULL) {
        return 0;
    }

    if (!scs_vc_break_enabled()) {
        /* THE KILL SWITCH. Nothing is torn down. One line, so a run log can
         * never read as "no circuit was ever broken" when the truth is
         * "breaking was switched off". */
        if (log != NULL) {
            fprintf(log,
                    "SCSD-W-VCBREAKOFF, %s on the circuit to"
                    " %02x:%02x:%02x:%02x:%02x:%02x -- teardown SUPPRESSED by %s;"
                    " %u connection(s) left as they were, VC state left %s\n",
                    scs_vc_break_reason_name(reason),
                    pb->remote_port_addr[0], pb->remote_port_addr[1],
                    pb->remote_port_addr[2], pb->remote_port_addr[3],
                    pb->remote_port_addr[4], pb->remote_port_addr[5],
                    SCS_VC_NO_BREAK_ENV, scs_pb_cdt_count(pb),
                    scs_vc_state_name(pb->vc_state));
            fflush(log);
        }
        return 0;
    }

    unsigned n = scs_pb_cdt_count(pb);

    if (log != NULL) {
        fprintf(log,
                "SCSD-W-VCBREAK, virtual circuit to port"
                " %02x:%02x:%02x:%02x:%02x:%02x (node %s, VC state %s) is being"
                " EXPLICITLY BROKEN: %s -- breaking %u connection(s) (p. 2-31)\n",
                pb->remote_port_addr[0], pb->remote_port_addr[1],
                pb->remote_port_addr[2], pb->remote_port_addr[3],
                pb->remote_port_addr[4], pb->remote_port_addr[5],
                (pb->sb != NULL && pb->sb->node_name[0]) ? pb->sb->node_name : "?",
                scs_vc_state_name(pb->vc_state),
                scs_vc_break_reason_name(reason), n);
    }

    /* (1) Drive every connection this circuit supports through the connection
     * state machine. p. 2-28: "SCA specifies that all CDTs corresponding to
     * connections supported by a virtual circuit be queued to the Path Block
     * corresponding to that circuit. If the circuit is broken for any reason,
     * it is then a relatively simple matter to scan this queue to determine
     * which connections have also been lost". */
    for (struct scs_cdt *c = scs_pb_first_cdt(pb); c != NULL; c = scs_cdt_next_on_pb(c)) {
        enum scs_conn_state was = scs_conn_state_of(c);
        struct scs_conn_transition t = scs_conn_fsm_step(c, SCS_CONN_EV_VC_LOST);
        if (log != NULL) {
            fprintf(log,
                    "SCSD-W-VCLOSSCONN, conid=0x%08X remote=0x%08X %s->%s:"
                    " connection broken by VC loss (%s -> %s)%s\n",
                    (unsigned)c->local_conid, (unsigned)c->remote_conid,
                    c->local_sysap[0] ? c->local_sysap : "?",
                    c->remote_sysap[0] ? c->remote_sysap : "?",
                    scs_conn_state_name(was), scs_conn_state_name(scs_conn_state_of(c)),
                    t.suppressed ? " [state NOT tracked -- OVMX_NO_CONN_FSM]" : "");
        }
    }

    /* (2) Notify the SYSAPs. p. 2-28: the VC-loss error handler lives in the
     * CDT and is supplied by the SYSAP as an argument to CONNECT and ACCEPT.
     * Run AFTER the state machine so a handler that inspects its connection
     * sees it already broken. */
    (void)scs_cdl_vc_loss(pb);

    /* (3) The circuit itself. p. 2-11 names no "broken" state; a broken circuit
     * is simply not open. The Path Block is NOT closed here -- design choice 4
     * in scs_vc.h. */
    scs_pb_set_vc_state(pb, SCS_VC_CLOSED);

    /* (4) Record it. */
    if (vc != NULL) {
        vc->broken = 1;
        vc->break_reason = (int)reason;
        vc->breaks++;
        vc->have_unacked = 0; /* nothing can be delivered on a circuit that is gone */
    }

    if (log != NULL) {
        fflush(log);
    }
    return n;
}

const char *scs_vc_action_name(enum scs_vc_action act)
{
    switch (act) {
    case SCS_VC_ACT_NONE:
        return "none";
    case SCS_VC_ACT_SEND_START:
        return "send START";
    case SCS_VC_ACT_SEND_STACK:
        return "send STACK";
    case SCS_VC_ACT_SEND_ACK:
        return "send ACK";
    case SCS_VC_ACT_ABANDON:
        return "ABANDON formation";
    default:
        return "?";
    }
}

/* vms-760: idempotent reply-sequence allocation. A retransmitted REQUEST must
 * be answered with the SAME response sequence number, consuming nothing. */
uint16_t scs_retx_reply_seq(struct scs_retx_seq *st, struct scs_seq_state *seq,
                            uint16_t req_seq)
{
    if (st == NULL || seq == NULL) {
        return 0;
    }
    if (st->valid && st->last_req == req_seq) {
        return st->last_rsp;      /* retransmit -> replay, consume nothing */
    }
    st->last_req = req_seq;
    st->last_rsp = scs_seq_advance(seq);
    st->valid = 1;
    return st->last_rsp;
}
