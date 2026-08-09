/*
 * test_scs_vc.c - unit tests for the vms-691 SCS sequenced-message VC engine:
 * the 0x48 credit-return builder (spec sec 4h(3)), the seq/ack tracking, and
 * the retransmit trigger.
 *
 * Every asserted byte value is GROUNDED (spec sec 4h(3), validated byte-exact
 * against 622/622 real 0x48 frames in formation-ci1.pcap) or a documented
 * reproduction of a real captured credit-return: real_credit_vax1 below is the
 * byte-exact 60-byte frame at SCA idx 34 of formation-ci1-joinwindow.pcap
 * (VAX1->VAX2 credit-ack, acked_seq=2, secondary=1). This is the REQUIRED
 * targeted unit test; it does NOT replace the live-wire SDA proof (the item
 * DONE condition), nor is it replaced by it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scs_vc.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  OK: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static void check_bytes(const uint8_t *got, const uint8_t *want, size_t n, const char *what)
{
    check(memcmp(got, want, n) == 0, what);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* OVMX test identity. */
static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
/* vms-9f3: OVMX's cluster-LOGICAL addr (abs 24), DISTINCT from the raw HW MAC. */
static const uint8_t ovmx_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 };
/* VAX1 logical LAVC addr (a DECnet node: Eth src == its logical addr). */
static const uint8_t vax1_log[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
/* VAX2 logical LAVC addr + its real HW MAC (differ: not DECnet-remapped). */
static const uint8_t vax2_log[6] = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 };
static const uint8_t vax2_hw[6]  = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };

/* Byte-exact real 0x48 credit-return, formation-ci1-joinwindow.pcap SCA idx 34
 * (VAX1->VAX2): acked seq 2, send-seq 0, secondary counter 1. Full 60-byte
 * Ethernet frame (SCA 41 bytes + Ethernet pad to 60). */
static const uint8_t real_credit_vax1[60] = {
    0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
    0x60, 0x07, 0x27, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
    0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x48, 0x13, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x12, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void test_credit_byte_exact(void)
{
    printf("[0x48 credit-return: byte-exact vs real VAX1 frame]\n");
    /* Reproduce SCA idx 34: VAX1 (src==logical aa:..:01:04) acks VAX2. */
    struct scs_credit_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax2_hw, 6);       /* Eth dst = VAX2 HW MAC */
    memcpy(p.src_mac, vax1_log, 6);      /* Eth src = VAX1 logical (a DECnet node) */
    memcpy(p.src_logical, vax1_log, 6);  /* SCA src-logical (abs 24) = VAX1 logical (vms-9f3) */
    memcpy(p.peer_logical, vax2_log, 6); /* dst-logical = VAX2 logical */
    p.acked_seq = 2;
    p.secondary_seq = 1;

    uint8_t out[SCS_CREDIT_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_credit_build(&p, out) == 0, "scs_credit_build succeeds");
    check_bytes(out, real_credit_vax1, sizeof(real_credit_vax1),
                "reproduces real VAX1 0x48 credit-return byte-for-byte (60 bytes)");
}

static void test_credit_fields(void)
{
    printf("[0x48 credit-return: field map (spec sec 4h(3))]\n");
    struct scs_credit_params p;
    memset(&p, 0, sizeof(p));
    memcpy(p.dst_mac, vax1_log, 6);
    memcpy(p.src_mac, ovmx_mac, 6);
    memcpy(p.src_logical, ovmx_logical, 6);
    memcpy(p.peer_logical, vax1_log, 6);
    p.acked_seq = 7;
    p.secondary_seq = 3;

    uint8_t out[SCS_CREDIT_FRAME_LEN];
    memset(out, 0xAA, sizeof(out));
    check(scs_credit_build(&p, out) == 0, "build ok (OVMX->VAX1)");

    check_bytes(out + 0, vax1_log, 6, "Ethernet dst == peer MAC");
    check_bytes(out + 6, ovmx_mac, 6, "Ethernet src == OVMX HW MAC");
    check(out[12] == 0x60 && out[13] == 0x07, "ethertype 0x6007");
    check(out[14] == 0x27 && out[15] == 0x00, "SCA length 0x0027 (total 41, GROUNDED)");
    check_bytes(out + 16, vax1_log, 6, "SCA dst-logical [2:8] == peer_logical (abs 16)");
    check(le16(out + 22) == 0x0001, "connect flag [8:10]==0x0001 (abs 22, GROUNDED)");
    check_bytes(out + 24, ovmx_logical, 6, "SCA src-logical [10:16] == cluster-LOGICAL addr, NOT HW MAC (abs 24, vms-9f3)");
    check(memcmp(out + 24, ovmx_mac, 6) != 0, "src-logical (abs 24) DISTINCT from raw HW MAC (vms-9f3)");
    check(out[30] == 0x48 && out[31] == 0x13, "opcode 0x48, format 0x13 (abs 30/31, GROUNDED)");
    check(le16(out + 32) == 7, "acked seq [18:20]==7 (abs 32, GROUNDED)");
    check(le16(out + 34) == 0, "send-seq [20:22]==0 (abs 34, GROUNDED 622/622)");
    check(le16(out + 36) == 1, "const [22:24]==0x0001 (abs 36, GROUNDED 622/622)");
    check(le16(out + 38) == 18, "NISCS_LAN_OVRHD [24:26]==18 (abs 38, GROUNDED)");
    check(le16(out + 40) == 7, "acked-seq mirror [26:28]==7 (abs 40, GROUNDED 622/622)");
    check(le16(out + 42) == 0, "[28:30] zero (abs 42)");
    check(le16(out + 44) == 3, "secondary counter [30:32]==3 (abs 44, INFERRED reproduced)");
    check(le16(out + 46) == 0, "[32:34] zero (abs 46)");
    check(le16(out + 48) == 7, "acked-seq 3rd repeat [34:36]==7 (abs 48, GROUNDED 616/622)");
    check(le16(out + 50) == 0, "[36:38] zero (abs 50)");
    check(le16(out + 52) == 1, "const [38:40]==0x0001 (abs 52, INFERRED clean value)");
    check(out[54] == 0, "[40] pad byte zero (abs 54)");
    /* Ethernet pad abs 55-59 zero. */
    check(out[55] == 0 && out[56] == 0 && out[57] == 0 && out[58] == 0 && out[59] == 0,
          "Ethernet pad to 60 is zero (abs 55-59)");

    check(scs_credit_build(NULL, out) == -1, "NULL params rejected");
    check(scs_credit_build(&p, NULL) == -1, "NULL out rejected");
}

static void test_seq_ack(void)
{
    printf("[VC seq/ack tracking]\n");
    struct scs_vc vc;
    scs_vc_init(&vc);
    check(vc.initialized && vc.seq.send_seq == 1 && vc.seq.recv_seq == 0,
          "init: send_seq=1 recv_seq=0 (fresh joiner)");
    check(vc.have_unacked == 0 && vc.credit_returns_sent == 0,
          "init: no outstanding message, no credits sent");

    scs_vc_note_recv(&vc, 4);
    check(vc.seq.recv_seq == 4, "note_recv(4) -> recv_seq=4");
    scs_vc_note_recv(&vc, 2);
    check(vc.seq.recv_seq == 4, "note_recv(2) does not regress recv_seq");
    scs_vc_note_recv(&vc, 0);
    check(vc.seq.recv_seq == 4, "note_recv(0) (pure ack) does not advance recv_seq");

    check(scs_vc_owes_credit(1) == 1, "owes_credit(1): sequenced msg -> ack owed");
    check(scs_vc_owes_credit(9) == 1, "owes_credit(9): sequenced msg -> ack owed");
    check(scs_vc_owes_credit(0) == 0, "owes_credit(0): pure ack -> no ack (no storm)");

    /* build_credit_for acks the current recv_seq and stamps our send_seq as
     * the secondary counter. */
    uint8_t out[SCS_CREDIT_FRAME_LEN];
    check(scs_vc_build_credit_for(&vc, vax1_log, ovmx_mac, ovmx_logical, vax1_log, out) == 0,
          "build_credit_for succeeds");
    check_bytes(out + 24, ovmx_logical, 6, "build_credit_for writes src-logical (abs 24), NOT HW MAC (vms-9f3)");
    check(le16(out + 32) == 4, "credit acks current recv_seq (4)");
    check(le16(out + 44) == vc.seq.send_seq, "secondary counter == OVMX send_seq");
    check(vc.credit_returns_sent == 1, "credit_returns_sent incremented");

    /* NULL-safety. */
    scs_vc_init(NULL);
    scs_vc_note_recv(NULL, 1);
    check(scs_vc_build_credit_for(NULL, vax1_log, ovmx_mac, ovmx_logical, vax1_log, out) == -1,
          "build_credit_for(NULL vc) rejected");
}

static void test_retransmit_trigger(void)
{
    printf("[retransmit trigger (deterministic ms clock)]\n");
    struct scs_vc vc;
    scs_vc_init(&vc);

    /* Nothing outstanding -> never due. */
    check(scs_vc_retransmit_due(&vc, 5000, 500) == 0, "no outstanding message -> not due");

    /* OVMX sends sequenced message #5 at t=1000ms. */
    scs_vc_record_sent(&vc, 5, 1000);
    check(vc.have_unacked && vc.unacked_seq == 5, "record_sent: seq 5 now outstanding");
    check(scs_vc_retransmit_due(&vc, 1000, 500) == 0, "at send time: not due");
    check(scs_vc_retransmit_due(&vc, 1499, 500) == 0, "before timeout (499ms): not due");
    check(scs_vc_retransmit_due(&vc, 1500, 500) == 1, "at timeout (500ms elapsed): DUE");
    check(scs_vc_retransmit_due(&vc, 1000, 500) == 0, "clock apparently backwards: not due (guarded)");

    /* A peer ack that does NOT cover seq 5 leaves it outstanding. */
    scs_vc_note_peer_ack(&vc, 4);
    check(vc.have_unacked == 1, "peer ack of 4 does not clear outstanding seq 5");
    check(scs_vc_retransmit_due(&vc, 2000, 500) == 1, "still due after partial ack");

    /* Retransmit: timer resets, counter bumps, still outstanding. */
    scs_vc_mark_retransmitted(&vc, 2000);
    check(vc.retransmit_count == 1 && vc.retransmits == 1, "mark_retransmitted bumps counters");
    check(scs_vc_retransmit_due(&vc, 2000, 500) == 0, "timer reset by retransmit: not due");
    check(scs_vc_retransmit_due(&vc, 2500, 500) == 1, "due again after another timeout");

    /* Peer finally acks seq 5 -> outstanding cleared, never due again. */
    scs_vc_note_peer_ack(&vc, 5);
    check(vc.have_unacked == 0, "peer ack of 5 clears the outstanding message");
    check(scs_vc_retransmit_due(&vc, 9000, 500) == 0, "cleared message -> not due");

    /* 16-bit wrap: outstanding seq 0xFFFF, peer acks 0x0001 -> reached. */
    scs_vc_record_sent(&vc, 0xFFFF, 100);
    scs_vc_note_peer_ack(&vc, 0x0001);
    check(vc.have_unacked == 0, "modular ack: recv_ack 1 reaches outstanding 0xFFFF (wrap)");

    /* NULL-safety. */
    check(scs_vc_retransmit_due(NULL, 1, 1) == 0, "retransmit_due(NULL) -> 0");
    scs_vc_record_sent(NULL, 1, 1);
    scs_vc_note_peer_ack(NULL, 1);
    scs_vc_mark_retransmitted(NULL, 1);
}

static void test_vc_reset_at_start_completion(void)
{
    printf("[VC reset at START completion (vms-246, spec sec 4i.A)]\n");
    struct scs_vc vc;
    scs_vc_init(&vc);

    /* Simulate the formation phase polluting the VC: several peer sequenced
     * messages advance recv_seq, OVMX advances its own send_seq, and it has an
     * outstanding-unacked message pending retransmit. This is the state that
     * left OVMX's 0x5b CONNECT-RESPONSE carrying recv_ack=4 vs the golden
     * joiner's 1. */
    scs_vc_note_recv(&vc, 4);
    (void)scs_seq_advance(&vc.seq);   /* send_seq 1 -> 2 */
    (void)scs_seq_advance(&vc.seq);   /* send_seq 2 -> 3 */
    scs_vc_record_sent(&vc, 3, 1000);
    check(vc.seq.recv_seq == 4 && vc.seq.send_seq == 3 && vc.have_unacked == 1,
          "pre-reset: recv_seq=4 send_seq=3 with an outstanding message (polluted)");

    /* THE FIX: at STARTDONE the SCS VC resets, INDEPENDENT of the START/config
     * counters -- send_seq back to 1, recv_seq back to 0, outstanding cleared. */
    scs_vc_reset_seq(&vc);
    check(vc.seq.send_seq == 1, "reset: send_seq == 1 (fresh post-START VC)");
    check(vc.seq.recv_seq == 0, "reset: recv_seq == 0 (formation-phase seq does NOT carry in)");
    check(vc.have_unacked == 0 && vc.retransmit_count == 0,
          "reset: outstanding retransmit state cleared");
    check(vc.initialized == 1, "reset: VC stays initialized");

    /* After reset, the first directory CONNECT-REQUEST (send_seq=1) brings
     * recv_seq to 1, so OVMX's CONNECT-RESPONSE now acks recv_ack=1 (golden). */
    scs_vc_note_recv(&vc, 1);
    check(vc.seq.recv_seq == 1,
          "post-reset: first directory request (send_seq=1) -> recv_seq=1 (recv_ack now 1, not 4)");

    /* Reset is independent of how large the pre-reset counters grew (sec 4i.A:
     * the member's residual send_seq, e.g. 11974, must not leak into the VC). */
    scs_vc_init(&vc);
    scs_vc_note_recv(&vc, 11974);
    scs_vc_reset_seq(&vc);
    check(vc.seq.recv_seq == 0 && vc.seq.send_seq == 1,
          "reset independent of a large residual send_seq (11974 -> recv_seq 0)");

    /* NULL-safety. */
    scs_vc_reset_seq(NULL);
    check(1, "reset_seq(NULL) is a safe no-op");
}

/* =====================================================================
 * vms-4071 -- the VIRTUAL-CIRCUIT FORMATION state machine.
 *
 * Drives every transition documented in VAXcluster Principles ch. 2:
 * Figure 2-7 (dialogue 1), Figure 2-8 (dialogue 2), the p. 2-14
 * acceptable-response table, the reissue timer, the OS-dependent retry limit,
 * the abandon paths and the p. 2-16 implied ACK.
 *
 * These are PURE-STATE tests: the machine builds no frame and touches no
 * socket, so nothing here is mocked -- the code under test is the shipped
 * code. What they do NOT prove is the DAEMON's use of the machine or anything
 * about the live wire; that needs the lab (see the item's bracketed triple).
 * ===================================================================== */

#include <stdlib.h>

#include "scs_config.h"

/* A configuration queue + one local port, freshly built for each scenario. */
struct fsm_fixture {
    struct scs_config cfg;
    struct scs_pdt    pdt;
    struct scs_pb    *pb;
};

static const uint8_t peer_port[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };

static void fsm_fixture_init(struct fsm_fixture *f)
{
    scs_config_init(&f->cfg);
    scs_pdt_init(&f->pdt, SCS_PORT_TYPE_ETHERNET, 1500);
    f->pb = scs_pb_create(&f->cfg, &f->pdt, peer_port, SCS_PORT_TYPE_ETHERNET);
}

/* p. 2-11: "When the PB is first initialized, it is marked to indicate that the
 * state of the virtual circuit is CLOSED." */
static void test_fsm_initial_state(void)
{
    printf("[fsm] initial state (p. 2-11)\n");
    struct fsm_fixture f;
    fsm_fixture_init(&f);
    check(f.pb != NULL, "path block created");
    check(f.pb->vc_state == SCS_VC_CLOSED, "fresh PB: VC state CLOSED");
    check(f.pb->fsm.timer_armed == 0, "fresh PB: no formation timer armed");
    check(f.pb->fsm.abandoned == 0, "fresh PB: formation not abandoned");

    /* Design choice 5: a driver that has issued nothing has no response to
     * classify, so events in CLOSED are ignored (not "unacceptable"). */
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 0) == SCS_VC_ACT_NONE,
          "CLOSED + START -> ignored (nothing was issued to respond to)");
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 0) == SCS_VC_ACT_NONE,
          "CLOSED + STACK -> ignored");
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_ACK, 0) == SCS_VC_ACT_NONE,
          "CLOSED + ACK -> ignored");
    check(f.pb->vc_state == SCS_VC_CLOSED, "CLOSED is unchanged by ignored events");
}

/*
 * Figure 2-7, dialogue 1: both nodes discover each other at about the same
 * time. Local: send START -> START SENT; receive START -> send STACK, START
 * RECEIVED; receive STACK -> send ACK, OPEN; receive ACK -> discarded.
 */
static void test_fsm_dialogue_1(void)
{
    printf("[fsm] Figure 2-7 dialogue 1 -- simultaneous discovery\n");
    struct fsm_fixture f;
    fsm_fixture_init(&f);

    check(scs_vc_fsm_send_start(f.pb, 1000) == SCS_VC_ACT_SEND_START,
          "d1: local port driver issues START");
    check(f.pb->vc_state == SCS_VC_START_SENT, "d1: VC STATE = START SENT");
    check(f.pb->fsm.timer_armed == 1 && f.pb->fsm.timer_ms == 1000,
          "d1: timer armed on the emitted START (p. 2-14)");

    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 1100) == SCS_VC_ACT_SEND_STACK,
          "d1: START in START SENT -> issue STACK");
    check(f.pb->vc_state == SCS_VC_START_RECEIVED, "d1: VC STATE = START RECEIVED");
    check(f.pb->fsm.timer_armed == 1 && f.pb->fsm.timer_ms == 1100,
          "d1: timer re-armed on the emitted STACK");
    check(f.pb->fsm.last_emitted == SCS_VC_ACT_SEND_STACK,
          "d1: STACK is now the packet a timeout would reissue");

    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 1200) == SCS_VC_ACT_SEND_ACK,
          "d1: STACK in START RECEIVED -> OPEN and issue ACK");
    check(f.pb->vc_state == SCS_VC_OPEN, "d1: VC STATE = OPEN");
    check(f.pb->fsm.timer_armed == 0, "d1: no timer runs on an OPEN circuit");

    /* p. 2-12: "each port driver simply discards the ACK it receives from the
     * other node because it already considers the virtual circuit to be OPEN." */
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_ACK, 1300) == SCS_VC_ACT_NONE,
          "d1: the trailing ACK is discarded on an already-OPEN circuit");
    check(f.pb->vc_state == SCS_VC_OPEN, "d1: circuit stays OPEN");
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 1400) == SCS_VC_ACT_NONE,
          "d1: a late START on an OPEN circuit is also discarded");
    check(f.pb->vc_state == SCS_VC_OPEN, "d1: circuit still OPEN");
}

/*
 * Figure 2-8, dialogue 2: NODE_1 discovers NODE_2 first, its START is discarded
 * by a node that has no PB yet, and the circuit sits in START SENT until
 * NODE_2's own START arrives. The local (NODE_1) view:
 *   send START -> START SENT ... (peer silent) ... peer START -> send STACK,
 *   START RECEIVED ... peer ACK -> OPEN with NO further emission.
 * "When NODE_1's port driver receives the ACK from NODE_2, it alters its PB to
 *  advance the state of the virtual circuit to OPEN." (p. 2-14)
 */
static void test_fsm_dialogue_2(void)
{
    printf("[fsm] Figure 2-8 dialogue 2 -- staggered discovery\n");
    struct fsm_fixture f;
    fsm_fixture_init(&f);

    check(scs_vc_fsm_send_start(f.pb, 0) == SCS_VC_ACT_SEND_START, "d2: issue START");
    check(f.pb->vc_state == SCS_VC_START_SENT, "d2: START SENT while the peer is deaf");

    /* "Meanwhile the state of the virtual circuit on NODE_1 remains START SENT."
     * (p. 2-13) -- the timer has not expired, so nothing changes. */
    check(scs_vc_fsm_timer_expired(f.pb, 500, SCS_VC_FORMATION_TIMEOUT_MS) == 0,
          "d2: timer has not expired at t+500ms");
    check(f.pb->vc_state == SCS_VC_START_SENT, "d2: still START SENT");

    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 600) == SCS_VC_ACT_SEND_STACK,
          "d2: the peer's eventual START -> issue STACK");
    check(f.pb->vc_state == SCS_VC_START_RECEIVED, "d2: START RECEIVED");

    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_ACK, 700) == SCS_VC_ACT_NONE,
          "d2: ACK in START RECEIVED -> OPEN, nothing emitted (p. 2-14)");
    check(f.pb->vc_state == SCS_VC_OPEN, "d2: VC STATE = OPEN");
    check(f.pb->fsm.timer_armed == 0, "d2: timer disarmed at OPEN");
}

/*
 * The p. 2-14 table, every cell, including the two that are only reachable in
 * a crossed/lossy dialogue.
 */
static void test_fsm_acceptable_response_table(void)
{
    printf("[fsm] p. 2-14 acceptable-response table, cell by cell\n");
    struct fsm_fixture f;

    /* START SENT + STACK -> OPEN + issue ACK ("advances the circuit all the way
     * to the OPEN state, and the port driver issues an ACK"). */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 10) == SCS_VC_ACT_SEND_ACK,
          "table: START SENT + STACK -> send ACK");
    check(f.pb->vc_state == SCS_VC_OPEN, "table: START SENT + STACK -> OPEN");

    /* START RECEIVED + bare START: "the state of the circuit is left unchanged
     * and the port driver issues another STACK." */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 10);
    check(f.pb->vc_state == SCS_VC_START_RECEIVED, "table: precondition START RECEIVED");
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 20) == SCS_VC_ACT_SEND_STACK,
          "table: START RECEIVED + START -> reissue STACK");
    check(f.pb->vc_state == SCS_VC_START_RECEIVED,
          "table: START RECEIVED + START leaves the state UNCHANGED");
    check(f.pb->fsm.retries == 1 && f.pb->fsm.reissues == 1,
          "table: an acceptable-but-non-advancing response counts as a reissue");
    check(f.pb->fsm.timer_ms == 20, "table: the reissue re-arms the timer");

    /* START SENT + ACK: not in the acceptable list -> abandon immediately. */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_ACK, 10) == SCS_VC_ACT_ABANDON,
          "table: START SENT + ACK is UNACCEPTABLE -> abandon");
    check(f.pb->fsm.abandoned == 1, "table: abandoned flag raised");
    check(f.pb->vc_state == SCS_VC_CLOSED, "table: an abandoned circuit is CLOSED");
    check(f.pb->fsm.timer_armed == 0, "table: abandoning disarms the timer");
    /* An abandoned circuit is inert: nothing revives it. */
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 20) == SCS_VC_ACT_NONE,
          "table: an abandoned circuit ignores a later STACK");
    check(scs_vc_fsm_send_start(f.pb, 30) == SCS_VC_ACT_NONE,
          "table: an abandoned circuit does not re-issue a START by itself");
    check(scs_vc_fsm_timeout(f.pb, 9999, 8) == SCS_VC_ACT_NONE,
          "table: an abandoned circuit has no timeout behaviour");
}

/*
 * p. 2-14: "If the timer expires before any response is received ... reissue the
 * START or STACK (whichever it last sent)" and "it is expected that an operating
 * system dependent retry limit be placed on each such scenario. If that limit is
 * reached, formation of the virtual circuit is to be abandoned."
 */
static void test_fsm_retry_and_abandon(void)
{
    printf("[fsm] p. 2-14 reissue timer, retry limit and abandon\n");
    struct fsm_fixture f;

    /* --- reissue of a START --- */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    check(scs_vc_fsm_timer_expired(f.pb, SCS_VC_FORMATION_TIMEOUT_MS - 1,
                                   SCS_VC_FORMATION_TIMEOUT_MS) == 0,
          "retry: timer not expired one ms early");
    check(scs_vc_fsm_timer_expired(f.pb, SCS_VC_FORMATION_TIMEOUT_MS,
                                   SCS_VC_FORMATION_TIMEOUT_MS) == 1,
          "retry: timer expired exactly at the timeout");
    check(scs_vc_fsm_timeout(f.pb, 2000, 8) == SCS_VC_ACT_SEND_START,
          "retry: expiry in START SENT reissues the START");
    check(f.pb->vc_state == SCS_VC_START_SENT, "retry: reissue does not change the state");
    check(f.pb->fsm.timer_ms == 2000, "retry: timer re-armed at the reissue");
    check(f.pb->fsm.retries == 1, "retry: retry counter is 1");

    /* --- reissue of a STACK: "whichever it last sent" --- */
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 2100); /* -> START RECEIVED, STACK emitted */
    check(f.pb->fsm.retries == 0,
          "retry: advancing to a new state resets the retry counter");
    check(scs_vc_fsm_timeout(f.pb, 4100, 8) == SCS_VC_ACT_SEND_STACK,
          "retry: expiry in START RECEIVED reissues the STACK, not the START");

    /* --- the retry limit is reached -> abandon --- */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    const unsigned limit = 3;
    uint64_t t = 0;
    enum scs_vc_action act = SCS_VC_ACT_NONE;
    unsigned reissued = 0;
    for (unsigned i = 0; i < limit; i++) {
        t += SCS_VC_FORMATION_TIMEOUT_MS;
        act = scs_vc_fsm_timeout(f.pb, t, limit);
        if (act == SCS_VC_ACT_SEND_START) {
            reissued++;
        }
    }
    check(reissued == limit - 1, "retry limit 3: exactly 2 reissues before the limit");
    check(act == SCS_VC_ACT_ABANDON, "retry limit 3: the 3rd expiry ABANDONS formation");
    check(f.pb->fsm.abandoned == 1, "retry limit: abandoned flag raised");
    check(f.pb->vc_state == SCS_VC_CLOSED, "retry limit: circuit left CLOSED");

    /* --- retry_limit == 0 means unlimited (the kill-switch path) --- */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    t = 0;
    int always_reissued = 1;
    for (unsigned i = 0; i < 100; i++) {
        t += SCS_VC_FORMATION_TIMEOUT_MS;
        if (scs_vc_fsm_timeout(f.pb, t, 0) != SCS_VC_ACT_SEND_START) {
            always_reissued = 0;
        }
    }
    check(always_reissued == 1, "retry limit 0: 100 expiries all reissue the START");
    check(f.pb->fsm.abandoned == 0, "retry limit 0: formation is never abandoned");
    check(f.pb->fsm.retries == 100, "retry limit 0: the counter still climbs");

    /* --- no timer armed (OPEN circuit) -> no timeout behaviour --- */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 10); /* -> OPEN */
    check(scs_vc_fsm_timer_expired(f.pb, 999999, SCS_VC_FORMATION_TIMEOUT_MS) == 0,
          "no timer on an OPEN circuit");
    check(scs_vc_fsm_timeout(f.pb, 999999, 8) == SCS_VC_ACT_NONE,
          "an OPEN circuit has no reissue behaviour");

    /* --- a clock that appears to run backwards must not fire the timer --- */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 10000);
    check(scs_vc_fsm_timer_expired(f.pb, 5000, SCS_VC_FORMATION_TIMEOUT_MS) == 0,
          "a backwards clock does not expire the timer");
}

/*
 * The retry limit is also what bounds the "acceptable but non-advancing"
 * reissue path -- a peer that repeats its START forever must not make the local
 * driver emit STACKs forever (p. 2-14, "each such scenario").
 */
static void test_fsm_repeated_start_hits_the_limit(void)
{
    printf("[fsm] a peer repeating START forever is bounded by the retry limit\n");
    struct fsm_fixture f;
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 1); /* -> START RECEIVED */

    enum scs_vc_action act = SCS_VC_ACT_NONE;
    unsigned stacks = 0;
    for (unsigned i = 0; i < SCS_VC_FORMATION_RETRY_LIMIT + 5; i++) {
        act = scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 100 + i);
        if (act == SCS_VC_ACT_SEND_STACK) {
            stacks++;
        }
        if (act == SCS_VC_ACT_ABANDON) {
            break;
        }
    }
    check(act == SCS_VC_ACT_ABANDON, "repeated START: formation is eventually abandoned");
    check(stacks == SCS_VC_FORMATION_RETRY_LIMIT - 1,
          "repeated START: STACK reissues stop one short of the limit");
    check(f.pb->vc_state == SCS_VC_CLOSED, "repeated START: circuit CLOSED after abandon");
}

/* p. 2-16: the implied ACK. */
static void test_fsm_implied_ack(void)
{
    printf("[fsm] p. 2-16 implied ACK\n");
    struct fsm_fixture f;
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 10); /* -> START RECEIVED, STACK issued */
    check(f.pb->vc_state == SCS_VC_START_RECEIVED,
          "implied ACK: precondition -- waiting for an ACK that got lost");

    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_OTHER, 20) == SCS_VC_ACT_NONE,
          "implied ACK: a circuit packet emits nothing");
    check(f.pb->vc_state == SCS_VC_OPEN,
          "implied ACK: a packet requiring an OPEN circuit marks it OPEN");
    check(f.pb->fsm.implied_acks == 1, "implied ACK: counted");
    check(f.pb->fsm.timer_armed == 0, "implied ACK: the STACK timer is disarmed");

    /* The rule is written for START RECEIVED ONLY. In START SENT the same
     * packet must not open the circuit (nor abandon it). */
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_OTHER, 10) == SCS_VC_ACT_NONE,
          "implied ACK: ignored in START SENT");
    check(f.pb->vc_state == SCS_VC_START_SENT,
          "implied ACK: START SENT is NOT advanced by a circuit packet");
    check(f.pb->fsm.abandoned == 0,
          "implied ACK: a circuit packet in START SENT is not 'unacceptable'");

    /* Which packets count is scs_vc_is_circuit_packet's job: the sequenced
     * message classes only. START/STACK/ACK are datagrams (p. 2-40). */
    check(scs_vc_is_circuit_packet(0x4b) == 1, "circuit packet: 0x4b sequenced app");
    check(scs_vc_is_circuit_packet(0x5b) == 1, "circuit packet: 0x5b directory");
    check(scs_vc_is_circuit_packet(0x7b) == 1, "circuit packet: 0x7b directory retx");
    check(scs_vc_is_circuit_packet(SCS_CREDIT_OPCODE) == 1,
          "circuit packet: 0x48 credit-return");
    check(scs_vc_is_circuit_packet(SCS_START_OPCODE) == 0,
          "circuit packet: 0x41 START/STACK/ACK is a DATAGRAM, not a circuit packet");
    check(scs_vc_is_circuit_packet(0x0c) == 0, "circuit packet: a HELLO is not one");
}

/* vms-694: the "peer withholds its round-2 ACK" stall detector. GROUNDED live
 * on lab-2 (2026-08-09): a returning/duplicate SCSSYSTEMID (spec sec 4(w)) makes
 * the member send round-0 START + round-1 STACK, let OVMX's circuit reach OPEN,
 * then re-issue round-0 START indefinitely without ever sending its round-2 ACK
 * -- so start_acked never latches and the join sequencer never ignites. A clean
 * identity on the same build joined with the member's round-2 ACK present and
 * ZERO post-OPEN re-STARTs. This asserts the predicate that names that stall. */
static void test_fsm_noack_stall_detect(void)
{
    printf("[fsm] vms-694 sec 4(w) 'peer withholds round-2 ACK' stall detector\n");
    struct fsm_fixture f;
    fsm_fixture_init(&f);

    /* Drive to OPEN the way a real join does: our START, peer STACK. The FSM
     * asks us to send our round-2 ACK -- but the daemon layer owns whether it
     * actually goes out (start_acked), which is what this stall is about. */
    scs_vc_fsm_send_start(f.pb, 0);
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 10);              /* -> START RECEIVED */
    check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 20) == SCS_VC_ACT_SEND_ACK,
          "noack: peer STACK opens the circuit and asks for our ACK");
    check(f.pb->vc_state == SCS_VC_OPEN, "noack: circuit OPEN on our side");
    check(f.pb->fsm.start_after_open == 0, "noack: no post-OPEN re-STARTs yet");

    /* The CLEAN control: the peer sent its round-2 ACK, so the daemon latched
     * start_acked. No stall regardless of anything else. */
    check(scs_vc_noack_stall(f.pb, /*start_acked=*/1,
                             SCS_VC_NOACK_STALL_THRESHOLD) == 0,
          "noack: a peer that sent its round-2 ACK (start_acked=1) never stalls");

    /* The STALL: our round-2 ACK never went out (start_acked=0) and the peer
     * re-issues round-0 START on the already-OPEN circuit. Each is counted and
     * classified as 'none' (p. 2-12), exactly the live 'START received -> none,
     * VC OPEN' loop. */
    check(scs_vc_noack_stall(f.pb, 0, SCS_VC_NOACK_STALL_THRESHOLD) == 0,
          "noack: below threshold with no re-STARTs -- not yet a stall");
    for (unsigned i = 1; i <= SCS_VC_NOACK_STALL_THRESHOLD; i++) {
        check(scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 30 + i) == SCS_VC_ACT_NONE,
              "noack: a round-0 START on an OPEN circuit is discarded");
        check(f.pb->vc_state == SCS_VC_OPEN, "noack: circuit stays OPEN");
    }
    check(f.pb->fsm.start_after_open == SCS_VC_NOACK_STALL_THRESHOLD,
          "noack: every post-OPEN re-START was counted");
    check(scs_vc_noack_stall(f.pb, 0, SCS_VC_NOACK_STALL_THRESHOLD) == 1,
          "noack: at threshold with start_acked=0 -- STALL detected");

    /* A peer STACK (not START) on the OPEN circuit is NOT a fresh re-START and
     * must not inflate the counter. */
    unsigned long before = f.pb->fsm.start_after_open;
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_STACK, 100);
    check(f.pb->fsm.start_after_open == before,
          "noack: a STACK on an OPEN circuit does not count as a re-START");

    /* Null / disabled-threshold safety. */
    check(scs_vc_noack_stall(NULL, 0, SCS_VC_NOACK_STALL_THRESHOLD) == 0,
          "noack: NULL pb rejected");
    check(scs_vc_noack_stall(f.pb, 0, 0) == 0,
          "noack: threshold 0 disables the detector");
}

/* The NISCA config-round -> Figure 2-7 packet-class mapping (design choice 1). */
static void test_fsm_round_classification(void)
{
    printf("[fsm] NISCA config-round -> START/STACK/ACK mapping\n");
    check(scs_vc_classify_round(0, 0) == SCS_VC_EV_START,
          "round 0, 106-byte identity frame -> START");
    check(scs_vc_classify_round(0, 1) == SCS_VC_EV_STACK,
          "round 1, 106-byte identity frame -> STACK (it re-supplies the description)");
    check(scs_vc_classify_round(1, SCS_START_ACK_ROUND) == SCS_VC_EV_ACK,
          "round 2, 46-byte no-identity frame -> ACK");
    /* is_ack wins over the round: the class is decided by the presence of an
     * identity body, which is what p. 2-12 distinguishes STACK from ACK by. */
    check(scs_vc_classify_round(1, 0) == SCS_VC_EV_ACK,
          "the 46-byte class is an ACK whatever round it carries");

    /* Driving the classifier straight into the machine reproduces the observed
     * 0/0/1/1/2/2 dialogue: OVMX emits START, then STACK on the peer's round-0,
     * then ACK on the peer's round-1. */
    struct fsm_fixture f;
    fsm_fixture_init(&f);
    check(scs_vc_fsm_send_start(f.pb, 0) == SCS_VC_ACT_SEND_START,
          "wire dialogue: OVMX round-0 START");
    check(scs_vc_fsm_recv(f.pb, scs_vc_classify_round(0, 0), 10) == SCS_VC_ACT_SEND_STACK,
          "wire dialogue: peer round-0 -> OVMX round-1 STACK");
    check(scs_vc_fsm_recv(f.pb, scs_vc_classify_round(0, 1), 20) == SCS_VC_ACT_SEND_ACK,
          "wire dialogue: peer round-1 -> OVMX round-2 ACK, circuit OPEN");
    check(f.pb->vc_state == SCS_VC_OPEN, "wire dialogue: OPEN after three OVMX frames");
    check(scs_vc_fsm_recv(f.pb, scs_vc_classify_round(1, 2), 30) == SCS_VC_ACT_NONE,
          "wire dialogue: the peer's round-2 ack is then discarded");
}

/* The OVMX_VC_NO_RETRY_LIMIT=1 kill-switch. */
static void test_fsm_kill_switch(void)
{
    printf("[fsm] OVMX_VC_NO_RETRY_LIMIT kill-switch\n");
    unsetenv(SCS_VC_NO_RETRY_LIMIT_ENV);
    check(scs_vc_retry_limit() == SCS_VC_FORMATION_RETRY_LIMIT,
          "kill-switch unset: the OS-dependent retry limit applies");

    setenv(SCS_VC_NO_RETRY_LIMIT_ENV, "1", 1);
    check(scs_vc_retry_limit() == 0, "OVMX_VC_NO_RETRY_LIMIT=1 -> unlimited retry");

    /* With the switch on, the repeated-START path never abandons. */
    struct fsm_fixture f;
    fsm_fixture_init(&f);
    scs_vc_fsm_send_start(f.pb, 0);
    scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 1);
    int all_stacks = 1;
    for (unsigned i = 0; i < SCS_VC_FORMATION_RETRY_LIMIT + 20; i++) {
        if (scs_vc_fsm_recv(f.pb, SCS_VC_EV_START, 100 + i) != SCS_VC_ACT_SEND_STACK) {
            all_stacks = 0;
        }
    }
    check(all_stacks == 1, "kill-switch on: every repeated START still reissues a STACK");
    check(f.pb->fsm.abandoned == 0, "kill-switch on: formation is never abandoned");

    setenv(SCS_VC_NO_RETRY_LIMIT_ENV, "0", 1);
    check(scs_vc_retry_limit() == SCS_VC_FORMATION_RETRY_LIMIT,
          "OVMX_VC_NO_RETRY_LIMIT=0 -> the limit applies again");
    setenv(SCS_VC_NO_RETRY_LIMIT_ENV, "yes", 1);
    check(scs_vc_retry_limit() == SCS_VC_FORMATION_RETRY_LIMIT,
          "only the exact value \"1\" arms the kill-switch");
    unsetenv(SCS_VC_NO_RETRY_LIMIT_ENV);
}

/* NULL-safety across the whole FSM surface. */
static void test_fsm_null_safety(void)
{
    printf("[fsm] NULL safety\n");
    scs_vc_fsm_init(NULL);
    check(scs_vc_fsm_send_start(NULL, 0) == SCS_VC_ACT_NONE, "send_start(NULL) -> NONE");
    check(scs_vc_fsm_recv(NULL, SCS_VC_EV_START, 0) == SCS_VC_ACT_NONE, "recv(NULL) -> NONE");
    check(scs_vc_fsm_timer_expired(NULL, 0, 1) == 0, "timer_expired(NULL) -> 0");
    check(scs_vc_fsm_timeout(NULL, 0, 1) == SCS_VC_ACT_NONE, "timeout(NULL) -> NONE");
    check(scs_vc_event_name(SCS_VC_EV_STACK) != NULL, "event_name is never NULL");
    check(scs_vc_action_name(SCS_VC_ACT_ABANDON) != NULL, "action_name is never NULL");
}

/* ==========================================================================
 * vms-abc -- THE p. 2-31 MESSAGE GUARANTEES.
 *
 * "if either the guarantee of message delivery or the guarantee of message
 * sequentiality cannot be satisfied, the virtual circuit between the ports
 * involved will be explicitly broken (if it isn't already). If this happens,
 * then every connection supported by this virtual circuit is also broken, and
 * the SYSAPs participating in these connections are notified of the event."
 * ========================================================================== */

/* A SYSAP that records that its VC-loss error handler ran, and on which CDT. */
struct vcloss_record {
    int      calls;
    uint32_t conid[4];
    int      state_when_called[4]; /* to prove the machine ran BEFORE the handler */
};

static void rec_vcloss(struct scs_cdt *cdt, void *ctx)
{
    struct vcloss_record *r = (struct vcloss_record *)ctx;
    if (r->calls < 4) {
        r->conid[r->calls] = cdt->local_conid;
        r->state_when_called[r->calls] = (int)scs_conn_state_of(cdt);
    }
    r->calls++;
}

/* One circuit (Path Block) carrying two SCS connections, both with a SYSAP
 * VC-loss handler installed -- the p. 2-28 shape scs_vc_break() must walk. */
struct two_conn_vc {
    struct scs_config cfg;
    struct scs_pdt    pdt;
    struct scs_cdl    cdl;
    struct scs_pb    *pb;
    struct scs_cdt   *a;
    struct scs_cdt   *b;
    struct scs_vc     vc;
    struct vcloss_record rec;
};

static void two_conn_vc_build(struct two_conn_vc *t)
{
    memset(t, 0, sizeof(*t));
    scs_config_init(&t->cfg);
    scs_pdt_init(&t->pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    scs_cdl_init(&t->cdl);
    scs_vc_init(&t->vc);
    t->pb = scs_pb_create(&t->cfg, &t->pdt, vax2_hw, SCS_PORT_TYPE_ETHERNET);
    scs_pb_set_vc_state(t->pb, SCS_VC_OPEN);
    t->a = scs_cdl_alloc(&t->cdl, "SCS$DIRECTORY", "SCS$DIRECTORY", t->pb);
    t->b = scs_cdl_alloc(&t->cdl, "VMS$VAXcluster", "VMS$VAXcluster", t->pb);
    scs_cdt_set_handlers(t->a, NULL, NULL, rec_vcloss, &t->rec);
    scs_cdt_set_handlers(t->b, NULL, NULL, rec_vcloss, &t->rec);
    /* Drive both to OPEN through the machine, as a formed connection is. */
    for (int i = 0; i < 2; i++) {
        struct scs_cdt *c = i ? t->b : t->a;
        (void)scs_conn_fsm_step(c, SCS_CONN_EV_SVC_CONNECT);
        (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_CONNECT_RSP);
        (void)scs_conn_fsm_step(c, SCS_CONN_EV_RCV_ACCEPT_REQ);
    }
}

/* The pure sequentiality predicate. Spec sec 4h(4) GROUNDS the rule this
 * encodes: "a node holds send_seq (its own next number) and recv_seq (highest
 * peer send_seq seen); a sequenced message stamps send_seq ... then increments
 * send_seq" -- so consecutive is the rule and a jump of >1 is a breach. */
static void test_seq_gap_verdicts(void)
{
    printf("[p2-31] the sequentiality verdicts\n");
    struct scs_vc vc;
    scs_vc_init(&vc);
    unsigned missing = 12345;

    check(scs_vc_check_recv_seq(&vc, 7, &missing) == SCS_VC_SEQ_ANCHOR,
          "the FIRST sequenced message on a VC anchors and is never a gap");
    check(missing == 0, "the anchor reports no missing messages");

    /* Anchor at 7, then walk. */
    check(scs_vc_note_recv_checked(&vc, 7, NULL) == SCS_VC_SEQ_ANCHOR, "anchored at 7");
    check(vc.seq.recv_seq == 7, "the anchor advanced recv_seq to 7");
    check(scs_vc_note_recv_checked(&vc, 8, NULL) == SCS_VC_SEQ_IN_ORDER, "8 after 7 is in order");
    check(scs_vc_note_recv_checked(&vc, 8, NULL) == SCS_VC_SEQ_DUPLICATE,
          "a repeat of 8 is a retransmit, NOT a gap");
    check(scs_vc_note_recv_checked(&vc, 5, NULL) == SCS_VC_SEQ_DUPLICATE,
          "a stale 5 is behind recv_seq, NOT a gap");
    check(scs_vc_check_recv_seq(&vc, 0, &missing) == SCS_VC_SEQ_DUPLICATE,
          "send_seq 0 is a 0x48 pure ack, never a gap (spec sec 4h(3), 622/622)");

    check(scs_vc_check_recv_seq(&vc, 10, &missing) == SCS_VC_SEQ_GAP, "8 -> 10 is a GAP");
    check(missing == 1, "8 -> 10 reports exactly 1 missing message");
    check(scs_vc_check_recv_seq(&vc, 100, &missing) == SCS_VC_SEQ_GAP, "8 -> 100 is a GAP");
    check(missing == 91, "8 -> 100 reports 91 missing messages");

    check(vc.seq_gaps == 0, "the PURE predicate does not count gaps");
    check(scs_vc_note_recv_checked(&vc, 10, &missing) == SCS_VC_SEQ_GAP, "note_recv_checked reports the gap");
    check(vc.seq_gaps == 1, "note_recv_checked counted the gap");
    check(vc.seq.recv_seq == 10, "recv_seq still advances over a gap (see scs_vc.h)");

    /* 16-bit wrap: 65535 -> 0 is not a legal sequence value, 65535 -> 1 is a
     * one-message gap, and a huge backwards value stays a duplicate. */
    struct scs_vc w;
    scs_vc_init(&w);
    (void)scs_vc_note_recv_checked(&w, 65534, NULL); /* anchor */
    check(scs_vc_note_recv_checked(&w, 65535, NULL) == SCS_VC_SEQ_IN_ORDER, "65534 -> 65535 in order");
    check(scs_vc_check_recv_seq(&w, 1, &missing) == SCS_VC_SEQ_GAP, "65535 -> 1 is a gap across wrap");
    check(missing == 1, "65535 -> 1 misses exactly one message (0)");
    check(scs_vc_check_recv_seq(&w, 60000, &missing) == SCS_VC_SEQ_DUPLICATE,
          "a value far behind across wrap is a duplicate, not a 60000-message gap");
    /* A pure ack must be rejected BEFORE the sequence comparison, not by it.
     * Near the top of the space `0` compares as AHEAD of recv_seq, so an
     * implementation that let send_seq 0 reach the comparison would score every
     * 0x48 credit-return as a gap -- and tear down a healthy circuit. */
    check(scs_vc_check_recv_seq(&w, 0, &missing) == SCS_VC_SEQ_DUPLICATE,
          "a pure ack is never a gap even at recv_seq=65535, where 0 compares AHEAD");
    struct scs_vc h;
    scs_vc_init(&h);
    (void)scs_vc_note_recv_checked(&h, 60000, NULL); /* anchor high in the space */
    check(scs_vc_note_recv_checked(&h, 0, NULL) == SCS_VC_SEQ_DUPLICATE,
          "a 0x48 pure ack at recv_seq=60000 is not a 5536-message gap");
    check(h.seq_gaps == 0, "and no gap was counted for it");
    check(h.seq.recv_seq == 60000, "and it did not advance recv_seq");

    check(scs_vc_check_recv_seq(NULL, 5, &missing) == SCS_VC_SEQ_DUPLICATE, "NULL vc is safe");
    check(scs_vc_note_recv_checked(NULL, 5, &missing) == SCS_VC_SEQ_DUPLICATE, "NULL vc is safe");
}

/* A fresh post-START VC re-anchors: sec 4i.A GROUNDS that "the post-START SCS
 * VC resets to send_seq = 1 on both sides", so the counter the reset installs
 * must not be compared against the pre-reset peer sequence. */
static void test_reset_reanchors(void)
{
    printf("[p2-31] a VC reset re-anchors\n");
    struct scs_vc vc;
    scs_vc_init(&vc);
    (void)scs_vc_note_recv_checked(&vc, 11973, NULL);
    (void)scs_vc_note_recv_checked(&vc, 11974, NULL);
    check(vc.seq_anchored == 1, "the VC is anchored after two messages");
    scs_vc_reset_seq(&vc);
    check(vc.seq_anchored == 0, "a reset drops the anchor");
    check(scs_vc_note_recv_checked(&vc, 1, NULL) == SCS_VC_SEQ_ANCHOR,
          "the first post-reset message anchors instead of scoring a 11974-message gap");
    check(vc.seq_gaps == 0, "and no gap was counted");
}

/* THE DONE CONDITION, at the mechanism level: a gap on a VC carrying TWO
 * connections breaks the VC and BOTH SYSAP handlers fire. */
static void test_break_walks_the_pb_and_notifies_both(void)
{
    printf("[p2-31] breaking a VC breaks every connection on it\n");
    struct two_conn_vc t;
    two_conn_vc_build(&t);

    check(t.pb != NULL && t.a != NULL && t.b != NULL, "the two-connection circuit was built");
    check(scs_pb_cdt_count(t.pb) == 2, "both CDTs are queued to the Path Block (p. 2-28)");
    check(scs_conn_state_of(t.a) == SCS_CONN_OPEN, "connection A is OPEN");
    check(scs_conn_state_of(t.b) == SCS_CONN_OPEN, "connection B is OPEN");
    check(t.pb->vc_state == SCS_VC_OPEN, "the circuit is OPEN");

    /* Inject the sequence gap on the VC. */
    (void)scs_vc_note_recv_checked(&t.vc, 4, NULL); /* anchor */
    (void)scs_vc_note_recv_checked(&t.vc, 5, NULL); /* in order */
    unsigned missing = 0;
    check(scs_vc_note_recv_checked(&t.vc, 9, &missing) == SCS_VC_SEQ_GAP,
          "5 -> 9 on the circuit is a sequentiality failure");
    check(missing == 3, "three messages are missing");

    unsigned broken = scs_vc_break(&t.vc, t.pb, SCS_VC_BREAK_SEQ_GAP, NULL);
    check(broken == 2, "scs_vc_break reports 2 connections broken");
    check(t.rec.calls == 2, "BOTH SYSAP VC-loss handlers fired");
    check((t.rec.conid[0] == t.a->local_conid && t.rec.conid[1] == t.b->local_conid) ||
          (t.rec.conid[0] == t.b->local_conid && t.rec.conid[1] == t.a->local_conid),
          "the two handler calls carried the two connections' CONIDs");
    check(t.rec.state_when_called[0] == (int)SCS_CONN_CLOSED &&
          t.rec.state_when_called[1] == (int)SCS_CONN_CLOSED,
          "each SYSAP saw its connection ALREADY broken when it was notified");
    check(scs_conn_state_of(t.a) == SCS_CONN_CLOSED, "connection A is CLOSED");
    check(scs_conn_state_of(t.b) == SCS_CONN_CLOSED, "connection B is CLOSED");
    check(t.pb->vc_state == SCS_VC_CLOSED, "the circuit itself is CLOSED");
    check(t.vc.broken == 1 && t.vc.breaks == 1, "the VC recorded the break");
    check(t.vc.break_reason == (int)SCS_VC_BREAK_SEQ_GAP, "and recorded the reason");
    /* Design choice 4: the Path Block is NOT closed, so the CDTs still hang off
     * it rather than dangling. */
    check(t.pb->in_use == 1, "the Path Block is left allocated (scs_pb_close is the caller's)");
    check(scs_pb_cdt_count(t.pb) == 2, "and its connection queue is intact");
}

/* The log must name the VC, the trigger, and every connection broken. */
static void test_break_log_names_vc_trigger_and_connections(void)
{
    printf("[p2-31] the break is logged with the VC, the trigger and each connection\n");
    struct two_conn_vc t;
    two_conn_vc_build(&t);

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    FILE *fp = fmemopen(buf, sizeof(buf) - 1, "w");
    check(fp != NULL, "log buffer opened");
    if (fp == NULL) {
        return;
    }
    (void)scs_vc_break(&t.vc, t.pb, SCS_VC_BREAK_DELIVERY, fp);
    fclose(fp);

    check(strstr(buf, "SCSD-W-VCBREAK") != NULL, "the break is logged");
    check(strstr(buf, "08:00:2b:78:56:b9") != NULL, "the log names the VC by its remote port address");
    check(strstr(buf, "retransmits exhausted") != NULL, "the log names the trigger");
    char want_a[64], want_b[64];
    snprintf(want_a, sizeof(want_a), "conid=0x%08X", (unsigned)t.a->local_conid);
    snprintf(want_b, sizeof(want_b), "conid=0x%08X", (unsigned)t.b->local_conid);
    check(strstr(buf, want_a) != NULL, "the log names connection A");
    check(strstr(buf, want_b) != NULL, "the log names connection B");
    check(strstr(buf, "SCS$DIRECTORY") != NULL, "the log names connection A's SYSAP");
    check(strstr(buf, "VMS$VAXcluster") != NULL, "the log names connection B's SYSAP");
    check(strstr(buf, "OPEN -> CLOSED") != NULL, "the log records the state each connection left");
}

/* The p. 2-31 DELIVERY guarantee: retransmit exhaustion. */
static void test_delivery_failure_predicate(void)
{
    printf("[p2-31] the delivery-guarantee predicate\n");
    struct scs_vc vc;
    scs_vc_init(&vc);
    check(scs_vc_delivery_failed(&vc) == 0, "an idle VC has not failed delivery");
    check(scs_vc_delivery_failed(NULL) == 0, "NULL is safe");

    scs_vc_record_sent(&vc, 42, 1000);
    check(scs_vc_delivery_failed(&vc) == 0, "a freshly sent message has not failed delivery");
    for (unsigned i = 0; i < SCS_VC_DELIVERY_RETRY_LIMIT - 1u; i++) {
        scs_vc_mark_retransmitted(&vc, 2000 + i);
        check(scs_vc_delivery_failed(&vc) == 0,
              "below the retry limit delivery has not yet failed");
    }
    scs_vc_mark_retransmitted(&vc, 9000);
    check(vc.retransmit_count == SCS_VC_DELIVERY_RETRY_LIMIT, "the retry limit is reached");
    check(scs_vc_delivery_failed(&vc) == 1,
          "AT the retry limit the delivery guarantee has failed");

    /* An ack clears the outstanding message, so delivery succeeded after all. */
    scs_vc_note_peer_ack(&vc, 42);
    check(scs_vc_delivery_failed(&vc) == 0, "an acknowledged message is not a delivery failure");
}

/*
 * THE KILL SWITCH (guardrail 23). The control runs FIRST with the switch off
 * and confirms the counter moved; only then is the switch set and the gated
 * behaviour asserted absent. Both halves use the SAME circuit shape.
 */
static void test_break_kill_switch(void)
{
    printf("[p2-31] OVMX_NO_VC_BREAK -- bracketed\n");
    (void)unsetenv(SCS_VC_NO_BREAK_ENV);
    check(scs_vc_break_enabled() == 1, "breaking is ON by default");

    /* --- CONTROL: switch OFF. The teardown must actually happen. --- */
    struct two_conn_vc on;
    two_conn_vc_build(&on);
    unsigned n_on = scs_vc_break(&on.vc, on.pb, SCS_VC_BREAK_SEQ_GAP, NULL);
    check(n_on == 2, "CONTROL: 2 connections broken with the switch off");
    check(on.rec.calls == 2, "CONTROL: 2 SYSAP handlers fired");
    check(on.pb->vc_state == SCS_VC_CLOSED, "CONTROL: the circuit is CLOSED");
    check(on.vc.broken == 1, "CONTROL: the VC is marked broken");

    /* --- GATED: switch ON. Nothing may happen. --- */
    (void)setenv(SCS_VC_NO_BREAK_ENV, "1", 1);
    check(scs_vc_break_enabled() == 0, "the switch is read fresh, not cached");
    struct two_conn_vc off;
    two_conn_vc_build(&off);
    char buf[2048];
    memset(buf, 0, sizeof(buf));
    FILE *fp = fmemopen(buf, sizeof(buf) - 1, "w");
    unsigned n_off = scs_vc_break(&off.vc, off.pb, SCS_VC_BREAK_SEQ_GAP, fp);
    if (fp != NULL) {
        fclose(fp);
    }
    check(n_off == 0, "GATED: no connection is broken");
    check(off.rec.calls == 0, "GATED: no SYSAP handler fires");
    check(scs_conn_state_of(off.a) == SCS_CONN_OPEN, "GATED: connection A is still OPEN");
    check(scs_conn_state_of(off.b) == SCS_CONN_OPEN, "GATED: connection B is still OPEN");
    check(off.pb->vc_state == SCS_VC_OPEN, "GATED: the circuit is still OPEN");
    check(off.vc.broken == 0 && off.vc.breaks == 0, "GATED: the VC is not marked broken");
    check(strstr(buf, "SCSD-W-VCBREAKOFF") != NULL,
          "GATED: the suppression is LOGGED -- a run log must not read as 'nothing was broken'");

    /* The DETECTOR is not gated: diagnosis survives the switch. */
    struct scs_vc det;
    scs_vc_init(&det);
    (void)scs_vc_note_recv_checked(&det, 1, NULL);
    check(scs_vc_note_recv_checked(&det, 5, NULL) == SCS_VC_SEQ_GAP,
          "GATED: the gap is still DETECTED with the switch on");
    check(det.seq_gaps == 1, "GATED: and still counted");

    /* Only the exact non-"0" value arms it. */
    (void)setenv(SCS_VC_NO_BREAK_ENV, "0", 1);
    check(scs_vc_break_enabled() == 1, "\"0\" does not arm the switch");
    (void)unsetenv(SCS_VC_NO_BREAK_ENV);
    check(scs_vc_break_enabled() == 1, "unset does not arm the switch");
}

/* A VC loss over a circuit with no connections, and other NULL/edge paths. */
static void test_break_edges(void)
{
    printf("[p2-31] break edges\n");
    check(scs_vc_break(NULL, NULL, SCS_VC_BREAK_LOCAL, NULL) == 0, "break(NULL pb) is 0");

    struct scs_config cfg;
    struct scs_pdt pdt;
    scs_config_init(&cfg);
    scs_pdt_init(&pdt, SCS_PORT_TYPE_ETHERNET, 4096);
    struct scs_pb *pb = scs_pb_create(&cfg, &pdt, vax2_hw, SCS_PORT_TYPE_ETHERNET);
    scs_pb_set_vc_state(pb, SCS_VC_OPEN);
    check(scs_vc_break(NULL, pb, SCS_VC_BREAK_LOCAL, NULL) == 0,
          "a circuit with no connections breaks 0 of them");
    check(pb->vc_state == SCS_VC_CLOSED, "and the circuit is still CLOSED afterwards");
    check(scs_vc_break_reason_name(SCS_VC_BREAK_NONE) != NULL, "reason_name is never NULL");
    check(scs_vc_break_reason_name((enum scs_vc_break_reason)99) != NULL,
          "reason_name of a bogus value is never NULL");
}

/* =====================================================================
 * vms-578 INTEGRATION: test functions added by worktree-760.
 * Both branches only APPENDED here, so nothing is replaced -- these are
 * carried over verbatim and registered in main() below.
 * ===================================================================== */


/* vms-2f3 sec 4M.20: a retransmitted request must be answered by REPLAYING the
 * original reply's sequence number, never by consuming a fresh one.
 *
 * REGRESSION THIS PINS: scs_reflect_credit() advanced unconditionally. That was
 * unreachable while OVMX ignored retransmissions; sec 4M.14 started answering
 * them, and the wire then showed the peer replaying op8 send_seq=12 three times
 * (run Q1B, VAX1 link) while OVMX answered 12, then 13, then 14 -- two phantom
 * messages injected into the VC stream. */
static void test_retx_reply_seq(void)
{
    printf("[retransmit-reuse of the reply sequence number -- vms-2f3 sec 4M.20]\n");

    struct scs_seq_state seq;
    scs_seq_init(&seq);                       /* send_seq starts at 1 */
    struct scs_retx_seq st;
    memset(&st, 0, sizeof(st));

    /* A fresh request consumes a sequence number. */
    uint16_t a = scs_retx_reply_seq(&st, &seq, 12);
    check(a == 1, "first reply to req seq 12 uses send_seq 1");

    /* The SAME request replayed twice must reuse it, and must not advance. */
    check(scs_retx_reply_seq(&st, &seq, 12) == a, "retransmit of req 12 replays the same send_seq");
    check(scs_retx_reply_seq(&st, &seq, 12) == a, "second retransmit replays it again");
    check(seq.send_seq == 2, "the VC sequence advanced ONCE across three answers");

    /* A genuinely new request advances again. */
    uint16_t b = scs_retx_reply_seq(&st, &seq, 13);
    check(b == 2, "a new req seq 13 consumes the next send_seq");
    check(b != a, "and it differs from the previous reply");
    check(scs_retx_reply_seq(&st, &seq, 13) == b, "its retransmit replays it");
    check(seq.send_seq == 3, "still exactly one advance per distinct request");

    /* Going back to an older request seq is treated as new -- we only remember
     * the most recent, which is what the single-outstanding-request protocol
     * needs and all that the wire evidence supports. */
    check(scs_retx_reply_seq(&st, &seq, 12) == 3, "an older req seq is not replayed from history");

    check(scs_retx_reply_seq(NULL, &seq, 1) == 0, "NULL state rejected");
    check(scs_retx_reply_seq(&st, NULL, 1) == 0, "NULL seq rejected");
}

int main(void)
{
    printf("test_scs_vc: SCS VC engine -- credit-return + seq/ack + retransmit (vms-691)\n");
    test_credit_byte_exact();
    test_credit_fields();
    test_seq_ack();
    test_retransmit_trigger();
    test_vc_reset_at_start_completion();
    printf("test_scs_vc: VC FORMATION state machine (vms-4071, pp. 2-12..2-16)\n");
    test_fsm_initial_state();
    test_fsm_dialogue_1();
    test_fsm_dialogue_2();
    test_fsm_acceptable_response_table();
    test_fsm_retry_and_abandon();
    test_fsm_repeated_start_hits_the_limit();
    test_fsm_implied_ack();
    test_fsm_noack_stall_detect();
    test_fsm_round_classification();
    test_fsm_kill_switch();
    test_fsm_null_safety();
    printf("test_scs_vc: p. 2-31 MESSAGE GUARANTEES -- breaking the VC (vms-abc)\n");
    test_seq_gap_verdicts();
    test_reset_reanchors();
    test_break_walks_the_pb_and_notifies_both();
    test_break_log_names_vc_trigger_and_connections();
    test_delivery_failure_predicate();
    test_break_kill_switch();
    test_break_edges();
    /* vms-578: worktree-760 test functions, registered here too. */
    test_retx_reply_seq();
    printf("test_scs_vc: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
