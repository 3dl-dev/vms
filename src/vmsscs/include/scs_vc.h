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

#include "scs_config.h" /* struct scs_pb + enum scs_vc_state/event/action (vms-7be) */
#include "scs_start.h"  /* struct scs_seq_state + scs_seq_* helpers (vms-21e) */

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
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical addr [10:16] (abs 24) = aa:00:04:00:<LE16(sysid)>;
                                 the cluster-LOGICAL addr, NOT the raw HW MAC (vms-9f3) */
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
                            const uint8_t src_logical[6],
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

/*
 * =====================================================================
 * vms-4071 -- THE VIRTUAL-CIRCUIT FORMATION STATE MACHINE (pp. 2-12..2-16)
 * =====================================================================
 *
 * WHY THIS EXISTS. Before this, OVMX advanced a circuit by writing the two
 * states it "knew" it had passed through, back to back, from inside the 0x41
 * receive handler:
 *
 *     scs_pb_set_vc_state(pb, SCS_VC_START_SENT);
 *     scs_pb_set_vc_state(pb, SCS_VC_START_RECEIVED);
 *
 * That is a log line, not a state machine: no response was ever CLASSIFIED, no
 * timer ran, nothing could be reissued, and formation could never be abandoned.
 * A circuit that never advances stayed in whatever state the last frame left it
 * in, forever. This module is the machine the Path Block's vc_state is supposed
 * to be driven by.
 *
 * THE RULES, quoted (VAXcluster Principles, ch. 2):
 *
 *   p. 2-14: "whenever a port driver sends a START or a STACK to some other
 *   port driver during virtual circuit formation, it starts a timer and expects
 *   a response. If the timer expires before any response is received, or if the
 *   response it receives is 'acceptable' but does not cause the circuit to
 *   advance to the next state of formation, SCA requires the port driver to
 *   reissue the START or STACK (whichever it last sent). If an 'unacceptable'
 *   response is received, SCA requires that formation of the virtual circuit be
 *   abandoned."
 *
 *   p. 2-14 (the acceptability table, implemented literally in scs_vc_fsm_recv):
 *     - START SENT: "Both START and STACK are acceptable responses."
 *         STACK -> OPEN, "and the port driver issues an ACK".
 *         START -> START RECEIVED, "causing the port driver to issue a STACK".
 *         (ACK is NOT listed -> unacceptable -> abandon.)
 *     - START RECEIVED: "considers START, STACK, and ACK to be acceptable".
 *         ACK or STACK -> OPEN; "if the response is STACK, the port driver will
 *         issue an ACK".
 *         START -> "the state of the circuit is left unchanged and the port
 *         driver issues another STACK".
 *
 *   p. 2-14 (the retry limit): "in theory a port driver would indefinitely
 *   reissue a START to which it receives no response. However, it is expected
 *   that an operating system dependent retry limit be placed on each such
 *   scenario. If that limit is reached, formation of the virtual circuit is to
 *   be abandoned."
 *
 *   p. 2-16 (the implied ACK): "the local port driver considers the circuit to
 *   be in the START RECEIVED state... But what if that ACK 'somehow gets lost'?
 *   ... what if, in the meantime, the remote node performs an operation that
 *   requires the circuit to be OPEN, and this operation results in a packet
 *   being sent to the local node? In this case, SCA states that the local node
 *   will treat that packet as an 'implied ACK' and simply mark the circuit as
 *   being OPEN."
 *
 * OVMX DESIGN CHOICES (labeled per CLAUDE.md rule 8 -- the book publishes the
 * RULES, never a byte layout or a numeric constant for any of this):
 *
 *  1. THE NISCA MAPPING. On the wire OVMX does not see packets labeled
 *     START/STACK/ACK; it sees opcode-0x41 frames carrying a config-round
 *     counter (docs/cluster-protocol-spec.md sec 4g phase 2: the round walks
 *     0 -> 0 -> 1 -> 1 -> 2 -> 2, "both nodes carry the same round", one frame
 *     per node per round). OVMX maps that six-frame dialogue onto Figure 2-7:
 *        round 0, 106-byte identity-bearing frame  -> START
 *        round 1, 106-byte identity-bearing frame  -> STACK
 *        round 2, 46-byte frame with NO identity   -> ACK
 *     The mapping is an INFERENCE, but a tightly constrained one: p. 2-12 says
 *     a STACK "again supplies the other node with a description of itself"
 *     (hence the SECOND 106-byte frame) while the ACK is a bare acknowledgment
 *     (hence the 46-byte one that carries no identity body), and Figure 2-7's
 *     six-packet, one-per-node-per-stage shape is exactly the observed
 *     0/0/1/1/2/2 sequence. It supersedes the pre-book label "START-retransmit"
 *     for round 1 in the spec doc (source-of-truth hierarchy: the SCA reference
 *     outranks our own spec notes). See scs_vc_classify_round().
 *
 *  2. THE TIMER is a caller-supplied monotonic millisecond stamp, not a timer
 *     object, so every transition is deterministic and unit-testable.
 *
 *  3. THE RETRY LIMIT is explicitly "operating system dependent" (p. 2-14), so
 *     OVMX picks its own: SCS_VC_FORMATION_RETRY_LIMIT. No VMS quantity is
 *     being claimed. `OVMX_VC_NO_RETRY_LIMIT=1` is the kill-switch: it restores
 *     the pre-vms-4071 behaviour of retrying without bound (see
 *     scs_vc_retry_limit()).
 *
 *  4. ABANDONED IS NOT A NEW STATE. p. 2-11 names four VC states and an
 *     abandoned circuit is simply not formed, so abandoning sets vc_state back
 *     to CLOSED and raises pb->fsm.abandoned to record why.
 *
 *  5. CLOSED IGNORES EVERYTHING. The p. 2-14 table classifies RESPONSES to a
 *     START or STACK the local driver has issued. A port driver that has issued
 *     nothing has no response to classify, so events in CLOSED are ignored
 *     rather than treated as unacceptable. (Figure 2-8's NODE_2, with no PB at
 *     all, likewise "discards the START from NODE_1", p. 2-13.)
 */

/* Milliseconds a START/STACK waits for a response before it is reissued
 * (p. 2-14). OVMX design choice, sized just above the ~1.4 s at which the
 * reference lab's connection manager re-issues its own round-0 START. */
#define SCS_VC_FORMATION_TIMEOUT_MS 2000u

/* Reissues of a single START/STACK before formation is abandoned (p. 2-14,
 * "operating system dependent"). OVMX design choice. 0 means "no limit". */
#define SCS_VC_FORMATION_RETRY_LIMIT 8u

/* Environment kill-switch: OVMX_VC_NO_RETRY_LIMIT=1 restores unbounded retry
 * (no abandon-on-limit), i.e. the pre-vms-4071 wire behaviour under loss. */
#define SCS_VC_NO_RETRY_LIMIT_ENV "OVMX_VC_NO_RETRY_LIMIT"

/*
 * scs_vc_retry_limit - the effective p. 2-14 retry limit. Returns 0 (unlimited)
 * when SCS_VC_NO_RETRY_LIMIT_ENV is set to "1", else
 * SCS_VC_FORMATION_RETRY_LIMIT. Read on every call (no cached decision).
 */
unsigned scs_vc_retry_limit(void);

/*
 * scs_vc_classify_round - map an observed 0x41 frame onto a formation event
 * per design choice 1 above. `is_ack` is the 46-byte no-identity class
 * (scs_start_view.is_ack); `config_round` is the [44:46] counter.
 *   is_ack            -> SCS_VC_EV_ACK
 *   round 0, 106-byte -> SCS_VC_EV_START
 *   round >=1, 106-B  -> SCS_VC_EV_STACK
 */
enum scs_vc_event scs_vc_classify_round(int is_ack, uint16_t config_round);

/*
 * scs_vc_is_circuit_packet - 1 if `opcode` (SCA payload [16], abs 30) is a
 * packet class that presupposes an OPEN virtual circuit, and therefore counts
 * as an implied ACK under p. 2-16. OVMX design choice (labeled): the SCS
 * sequenced-message classes only -- 0x4b sequenced-application, 0x5b/0x7b
 * directory, 0x48 credit-return. NOT the HELLO and NOT 0x41: p. 2-40 states
 * that START/STACK/ACK are datagrams, which by definition do not require an
 * open circuit, and a HELLO precedes formation entirely.
 */
int scs_vc_is_circuit_packet(uint8_t opcode);

/* Reset a Path Block's formation machine (CLOSED, no timer, not abandoned).
 * scs_pb_create already zeroes the PB; call this to re-arm a reused one. */
void scs_vc_fsm_init(struct scs_pb *pb);

/*
 * scs_vc_fsm_send_start - the local port driver is issuing its START (Figure
 * 2-7: "SEND 'START' ... o VC STATE = 'START SENT'"). Advances CLOSED ->
 * START SENT, records START as the last-emitted packet and arms the timer at
 * now_ms. Returns SCS_VC_ACT_SEND_START (the caller must actually transmit it),
 * or SCS_VC_ACT_NONE if pb is NULL, abandoned, or not in CLOSED.
 */
enum scs_vc_action scs_vc_fsm_send_start(struct scs_pb *pb, uint64_t now_ms);

/*
 * scs_vc_fsm_recv - apply the p. 2-14 acceptability table (and the p. 2-16
 * implied-ACK rule) to one received packet. Returns the action the caller must
 * perform: SEND_STACK, SEND_ACK, ABANDON, or NONE. The PB's vc_state, timer and
 * retry counter are updated in place.
 *
 * The ONLY unacceptable response in the whole table is an ACK arriving in
 * START SENT (p. 2-14 lists only START and STACK as acceptable there); that
 * returns SCS_VC_ACT_ABANDON, sets vc_state to CLOSED and raises fsm.abandoned.
 */
enum scs_vc_action scs_vc_fsm_recv(struct scs_pb *pb, enum scs_vc_event ev,
                                   uint64_t now_ms);

/*
 * scs_vc_fsm_timer_expired - pure predicate: 1 iff a formation timer is armed
 * on this PB and (now_ms - fsm.timer_ms) >= timeout_ms. Does not mutate.
 */
int scs_vc_fsm_timer_expired(const struct scs_pb *pb, uint64_t now_ms,
                             uint64_t timeout_ms);

/*
 * scs_vc_fsm_timeout - the formation timer expired with no response (p. 2-14).
 * Bumps the retry counter and returns SEND_START or SEND_STACK -- whichever was
 * last emitted -- with the timer re-armed at now_ms. If retry_limit is non-zero
 * and the counter reaches it, formation is ABANDONED instead: vc_state goes to
 * CLOSED, fsm.abandoned is raised and SCS_VC_ACT_ABANDON is returned. A
 * retry_limit of 0 means unlimited (the kill-switch). Returns NONE if no timer
 * is armed.
 */
enum scs_vc_action scs_vc_fsm_timeout(struct scs_pb *pb, uint64_t now_ms,
                                      unsigned retry_limit);

/* Human-readable names, for logging. Never NULL. */
const char *scs_vc_event_name(enum scs_vc_event ev);
const char *scs_vc_action_name(enum scs_vc_action act);

#ifdef __cplusplus
}
#endif

#endif /* SCS_VC_H */
