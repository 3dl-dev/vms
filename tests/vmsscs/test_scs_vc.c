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

int main(void)
{
    printf("test_scs_vc: SCS VC engine -- credit-return + seq/ack + retransmit (vms-691)\n");
    test_credit_byte_exact();
    test_credit_fields();
    test_seq_ack();
    test_retransmit_trigger();
    test_vc_reset_at_start_completion();
    printf("test_scs_vc: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
