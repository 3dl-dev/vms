/*
 * scs_vc.h - SCS sequenced-message virtual-circuit (VC) engine (vms-691).
 *
 * WHY THIS EXISTS. After the phase-2 0x41 START completes (vms-21e) the real
 * VAX forms a virtual circuit and recognizes REMOTE NODE OVMX, then begins
 * streaming SCS *sequenced messages* (0x5b directory lookups, the 0x4b
 * VMS$VAXcluster connect, and the 190-byte VC/DLM class). Each such message
 * MUST be acknowledged. OVMX previously sent single frames with no sustained
 * VC state, so from the VAX's point of view its messages to OVMX were never
 * acked -> it logged "%PEA0 Excessive packet losses ... Closed Virtual
 * Circuit - REMOTE NODE OVMX" and tore the VC down. This module is the VC
 * engine that keeps the circuit alive:
 *
 *   1. SEQUENCING     - tracks send_seq/recv_seq per VC (built on the vms-21e
 *                       scs_seq_state) so OVMX stamps its own outgoing
 *                       sequenced messages and honors the [20:22]==[30:32]
 *                       send-seq mirror (spec sec 4h (4)).
 *   2. CREDIT-RETURN  - for every sequenced message received from the VAX,
 *                       emit exactly one 0x48 credit-return short acking the
 *                       received sequence (spec sec 4h (3)). This is the
 *                       1-for-1 credit that stops the packet-loss teardown.
 *   3. RETRANSMIT     - track OVMX's own last unacked sequenced message and
 *                       re-send it on a timeout if the peer has not acked it
 *                       (pure timer logic, deterministic + unit-tested).
 *
 * CLEAN-ROOM PROVENANCE (CLAUDE.md rule 8 / spec sec 0). The 0x48
 * credit-return frame this module builds is NOT a replayed template -- every
 * one of its 41 SCA bytes is positioned field-by-field from the GROUNDED
 * spec sec 4(h)(3) map, which was validated byte-exact against 622/622 real
 * 0x48 frames in formation-ci1.pcap. The build was cross-checked byte-for-byte
 * against the clean archetype frames in formation-ci1-joinwindow.pcap
 * (SCA idx 34/36, VAX1<->VAX2 credit-acks): with acked_seq=2 and
 * secondary_seq=1 the builder reproduces those real frames exactly. No VSI/HPE
 * source or binary was read.
 *
 * GROUNDED fields (spec sec 4h (3), 622/622 frames unless noted):
 *   - [0:2]   SCA length 0x0027 (=39 -> total 41)
 *   - [8:10]  connect flag 0x0001
 *   - [16]    opcode 0x48, [17] format constant 0x13
 *   - [18:20] acknowledged sequence (= OVMX's recv_seq = peer's last send_seq)
 *   - [20:22] send-seq == 0x0000 (a credit-return emits no new sequence)
 *   - [22:24] constant 0x0001
 *   - [24:26] 0x0012 = 18 = SYSGEN NISCS_LAN_OVRHD
 *   - [26:28] acknowledged-sequence mirror (== [18:20])
 *   - [34:36] acknowledged-sequence 3rd repeat (== [18:20]; 616/622)
 * INFERRED / reproduced-as-observed (spec sec 4h (3), labeled):
 *   - [30:32] secondary counter (the sender's own outstanding seq) -- filled
 *             from OVMX's send_seq; "not cleanly a single function of [18:20]".
 *   - [38:40] constant 0x0001 (598/622; the clean-archetype value).
 * All other bytes ([2:8] dst-logical, [10:16] src-logical) are GROUNDED
 * substitutions of the peer/OVMX addresses; [28:30], [32:34], [36:38], [40]
 * and the Ethernet pad to 60 are zero (observed).
 */
#ifndef SCS_VC_H
#define SCS_VC_H

#include <stddef.h>
#include <stdint.h>

#include "scs_start.h" /* struct scs_seq_state + scs_seq_* helpers (vms-21e) */

#ifdef __cplusplus
extern "C" {
#endif

#define SCS_CREDIT_SCA_LEN    41 /* total SCA content bytes of the 0x48 short (spec sec 4h) */
#define SCS_CREDIT_FRAME_LEN  60 /* 14 Eth hdr + 41 SCA = 55, padded to the 60-byte Eth min */

#define SCS_CREDIT_OPCODE     0x48 /* credit-return short (spec sec 4g/4h) */
#define SCS_CREDIT_FORMAT     0x13 /* format/version constant (GROUNDED, 622/622) */
#define SCS_NISCS_LAN_OVRHD   18   /* 0x0012 at [24:26], = SYSGEN NISCS_LAN_OVRHD (GROUNDED) */

/*
 * scs_credit_params - inputs to build one 0x48 credit-return short.
 */
struct scs_credit_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's observed Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src + SCA src-logical addr = OVMX HW MAC */
    uint8_t  peer_logical[6]; /* SCA dest-logical addr [2:8] = peer's advertised logical addr */
    uint16_t acked_seq;       /* peer's send_seq we're acking (= OVMX recv_seq) [18:20]/[26:28]/[34:36] */
    uint16_t secondary_seq;   /* [30:32] inferred: OVMX's own outstanding send_seq (reproduced) */
};

/*
 * scs_credit_build - Fill out[SCS_CREDIT_FRAME_LEN] with a complete
 * Ethernet+SCA 0x48 credit-return short per spec sec 4h(3), acking
 * p->acked_seq. Returns 0, or -1 if p or out is NULL.
 */
int scs_credit_build(const struct scs_credit_params *p,
                     uint8_t out[SCS_CREDIT_FRAME_LEN]);

/*
 * struct scs_vc - per-virtual-circuit engine state.
 *
 * Wraps the vms-21e scs_seq_state (send_seq/recv_seq) and adds the retransmit
 * bookkeeping for OVMX's own last unacked sequenced message. Times are caller-
 * supplied monotonic milliseconds so the retransmit trigger is deterministic
 * and unit-testable (no wall clock inside the pure logic).
 */
struct scs_vc {
    struct scs_seq_state seq;       /* OVMX's send_seq / recv_seq for this VC */
    int                  initialized;

    /* Retransmit of OVMX's own last outstanding (unacked) sequenced message. */
    int                  have_unacked;
    uint16_t             unacked_seq;      /* send_seq of the outstanding message */
    uint64_t             unacked_sent_ms;  /* monotonic ms when we (re)sent it */
    unsigned             retransmit_count; /* times the current message was retransmitted */

    /* Stats (observability only). */
    unsigned long        credit_returns_sent;
    unsigned long        retransmits;
};

/* Initialize a fresh VC (send_seq=1, recv_seq=0, no outstanding message). */
void scs_vc_init(struct scs_vc *vc);

/*
 * scs_vc_reset_seq - reset the VC sequence space to a fresh post-START VC:
 * send_seq=1, recv_seq=0, dropping any outstanding-unacked retransmit
 * bookkeeping. Call ONCE at START completion (the STARTDONE round-2
 * transition, vms-246). Per spec sec 4i.A the phase-2 0x41 START/config-round
 * counters are SEPARATE from the SCS VC and must NOT carry into it -- both
 * sides reset the VC to send_seq=1/recv_seq=0 when START completes, then run
 * the sec 4h directory lockstep byte-identical to a fresh formation. Without
 * this reset, recv_seq accumulated across the formation phase, so OVMX's 0x5b
 * CONNECT-RESPONSE acked a sequence the VAX never sent post-reset (recv_ack too
 * high) and the VAX rejected the SCS$DIRECTORY connect. Preserves the
 * `initialized` flag and observability stats. No-op if vc is NULL.
 */
void scs_vc_reset_seq(struct scs_vc *vc);

/*
 * scs_vc_note_recv - record a peer sequenced message's send_seq (advances
 * recv_seq high-water). Call once per received sequenced message before
 * building its credit-return. A send_seq of 0 (e.g. a 0x48 short) is a pure
 * ack that carries no new sequence and does not advance recv_seq.
 */
void scs_vc_note_recv(struct scs_vc *vc, uint16_t peer_send_seq);

/*
 * scs_vc_owes_credit - returns 1 if a received frame with this send_seq is a
 * sequenced message that must be answered by exactly one 0x48 credit-return
 * (strict 1-for-1, spec sec 4h(3)); 0 for a pure ack (send_seq==0) which is
 * never itself acked (prevents an ack storm).
 */
int scs_vc_owes_credit(uint16_t peer_send_seq);

/*
 * scs_vc_build_credit_for - convenience: build the 0x48 credit-return that
 * acks the VC's current recv_seq, filling the secondary counter from OVMX's
 * own send_seq. Bumps vc->credit_returns_sent on success. Returns 0/-1.
 */
int scs_vc_build_credit_for(struct scs_vc *vc,
                            const uint8_t dst_mac[6],
                            const uint8_t src_mac[6],
                            const uint8_t peer_logical[6],
                            uint8_t out[SCS_CREDIT_FRAME_LEN]);

/*
 * scs_vc_record_sent - remember that OVMX just sent a sequenced message with
 * sequence `seq` at monotonic `now_ms`, so it can be retransmitted if the peer
 * fails to ack it. Overwrites any previous outstanding message (OVMX keeps at
 * most one in flight in this engine) and resets the retransmit counter.
 */
void scs_vc_record_sent(struct scs_vc *vc, uint16_t seq, uint64_t now_ms);

/*
 * scs_vc_note_peer_ack - the peer acknowledged sequences up to and including
 * `peer_recv_ack` (the recv_ack/leading counter [18:20] of a frame the peer
 * sent us, or the acked-seq of its 0x48). Clears the outstanding message once
 * it is covered. Uses modular-safe "reached" comparison for 16-bit wrap.
 */
void scs_vc_note_peer_ack(struct scs_vc *vc, uint16_t peer_recv_ack);

/*
 * scs_vc_retransmit_due - returns 1 iff OVMX has an outstanding unacked
 * sequenced message whose age (now_ms - unacked_sent_ms) is >= timeout_ms.
 * Pure predicate; does not mutate state.
 */
int scs_vc_retransmit_due(const struct scs_vc *vc, uint64_t now_ms,
                          uint64_t timeout_ms);

/*
 * scs_vc_mark_retransmitted - call after re-sending the outstanding message:
 * resets the timer to now_ms and bumps the retransmit counters.
 */
void scs_vc_mark_retransmitted(struct scs_vc *vc, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SCS_VC_H */
