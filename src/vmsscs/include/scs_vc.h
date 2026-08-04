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
#include <stdio.h>

#include "scs_conn.h"   /* the connection state machine a broken VC drives (vms-dd5) */
#include "scs_config.h" /* struct scs_pb + enum scs_vc_state/event/action (vms-7be) */
#include "scs_cdt.h"    /* struct scs_cdt, the PB connection queue, scs_cdl_vc_loss (vms-e1a) */
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

    /*
     * vms-abc: the p. 2-31 message guarantees. See "BREAKING THE CIRCUIT"
     * below for the rules, the measurement, and the kill switch.
     */

    /* 1 once a sequenced message has been observed since init/reset, i.e. once
     * recv_seq means "the peer's last send_seq" rather than "nothing seen yet".
     * The FIRST sequenced message on a circuit can never be a gap: OVMX may
     * attach to a circuit that is already carrying traffic (scsd.c calls
     * scs_vc_init() lazily on the first sequenced frame from a peer), and
     * nothing is known about what preceded the message that anchors it. */
    int                  seq_anchored;

    unsigned long        seq_gaps;      /* sequentiality failures detected */
    int                  broken;        /* 1 = this circuit has been explicitly broken */
    int                  break_reason;  /* enum scs_vc_break_reason of the last break */
    unsigned long        breaks;        /* times this VC was broken */
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
 *
 *  6. WHEN THE ACTION IS ACTUALLY TRANSMITTED IS NOT THE MACHINE'S BUSINESS.
 *     The p. 2-14 table says a STACK received in START SENT / START RECEIVED
 *     opens the circuit AND makes the port driver issue an ACK, so
 *     scs_vc_fsm_recv() returns SCS_VC_ACT_SEND_ACK at that moment. Whether the
 *     NISCA encoding puts that ACK on the wire THEN, or waits for the peer's own
 *     round-2 frame, is a transport decision made in scsd.c -- see
 *     scs_vc_early_ack_enabled() and the ordering note above scsd_vc_settle().
 *     OVMX ships the WAIT ordering by default because that is what it emitted
 *     before vms-4071; the p. 2-14 ordering is opt-in and UNVERIFIED on the
 *     wire.
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
 * Environment OPT-IN (default OFF): OVMX_VC_EARLY_ACK=1 makes OVMX put its
 * round-2 ACK on the wire the moment the circuit reaches OPEN -- i.e. on the
 * peer's round-1 STACK, which is the literal p. 2-14 rule ("A response of
 * either ACK or STACK will advance the circuit to the OPEN state; and if the
 * response is STACK, the port driver will issue an ACK").
 *
 * WHY IT IS NOT THE DEFAULT. Pre-vms-4071 OVMX emitted its round-2 ACK only
 * after the PEER's round-2 frame arrived, giving the fresh-join interleaving
 *     OVMX r0, OVMX r1, peer r1, peer r2, OVMX r2
 * The early ordering emits the same 46 bytes one peer-frame sooner:
 *     OVMX r0, OVMX r1, peer r1, OVMX r2, peer r2
 * Same frames, same bytes, different interleaving -- and NO lab capture has yet
 * shown the reference member accepts the earlier one. The vms-4071 constraint
 * is "a fresh join must produce the same frames", so the preserved ordering
 * ships and the p. 2-14 ordering is available behind this switch for the lab
 * run that would promote it.
 */
#define SCS_VC_EARLY_ACK_ENV "OVMX_VC_EARLY_ACK"

/*
 * scs_vc_early_ack_enabled - 1 when SCS_VC_EARLY_ACK_ENV is exactly "1", else
 * 0. Read on every call (no cached decision).
 */
int scs_vc_early_ack_enabled(void);

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

/*
 * =====================================================================
 * vms-abc -- BREAKING THE CIRCUIT WHEN A MESSAGE GUARANTEE FAILS (p. 2-31)
 * =====================================================================
 *
 * THE RULE, quoted (VAXcluster Principles p. 2-31):
 *
 *   "Furthermore, if either the guarantee of message delivery or the guarantee
 *   of message sequentiality cannot be satisfied, the virtual circuit between
 *   the ports involved will be explicitly broken (if it isn't already). If this
 *   happens, then every connection supported by this virtual circuit is also
 *   broken, and the SYSAPs participating in these connections are notified of
 *   the event."
 *
 * WHAT OVMX DID BEFORE THIS ITEM: nothing. A gap in the peer's send-sequence
 * numbers advanced recv_seq to the new high-water and was credit-acked as if
 * every intervening message had arrived (scs_vc_note_recv is a plain
 * `recv_seq = max(recv_seq, peer_send_seq)`), so OVMX told the peer it had
 * received messages it had never seen. Retransmit exhaustion was a `continue`
 * in scsd.c's timer loop: OVMX stopped retransmitting an unacked message and
 * carried on with the circuit still marked OPEN. Both are the "limp on"
 * behaviour p. 2-31 forbids, and both are silent.
 *
 * THIS IS A FAIL-LOUD CHANGE. Under loss, OVMX now tears a circuit down where
 * it previously continued. `OVMX_NO_VC_BREAK=1` restores the old behaviour on
 * every path that BREAKS a circuit -- see scs_vc_break_enabled() for precisely
 * what it gates, and for the one behaviour change of this item that it
 * deliberately does NOT gate (the SYSAP notification on vms-17f's peer
 * departure path, which OVMX_NO_PEER_DEPART gates instead).
 *
 * ================== IS THE DETECTOR SAFE ON THE REAL WIRE? ================
 * A detector that fires on a healthy circuit would tear down working joins, so
 * it was MEASURED before it was shipped, not after.
 * tools/cluster/scs_seqgap_measure.py applies THE SAME RULE
 * (scs_vc_check_recv_seq's, restated in the script's docstring) to every
 * capture this project holds and counts how many times it would have fired.
 *
 *   47 pcaps, 321,599 sequenced messages examined, 506 duplicate/retransmit
 *   frames correctly NOT scored as gaps, 41 gaps -- and ALL 41 have source MAC
 *   b6:16:8a:dc:3a:53, which is OVMX's own transmit. In the RECEIVE direction,
 *   which is the only direction this detector runs in, the count is ZERO,
 *   including 2,960 sequenced messages of the golden fresh join
 *   (formation-ci1-joinwindow.pcap), 17,760 of formation-ci1.pcap and 18,881 of
 *   vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap.
 *
 * Re-derive with `python3 tools/cluster/scs_seqgap_measure.py --all` on a lab
 * host (the captures are host-only, 13 GB, not in git); the run prints
 * "TOTAL pcaps=47 sequenced messages=321599 dup/retx=506 GAPS=41" followed by a
 * GAP SOURCE CENSUS over EVERY gap -- one line, b6:16:8a:dc:3a:53 41. The pcap
 * count agrees with docs/cluster-protocol-spec.md sec 4h(4a) and with
 * `ls /data/training/vax/cluster/captures/ | grep -c '[.]pcap$'`. (An earlier revision
 * of this block said 51; that figure re-derived from nothing and was a
 * transcription slip. The census line and the pcap total were added to the
 * script by this item precisely so the claim above cannot drift again: before,
 * the script truncated its gap detail to 8 per file, so "ALL 41" was not
 * checkable from the command this comment names.)
 *
 * The 41 OVMX-sourced gaps are a real OVMX transmit-side defect -- it stamps
 * the member's continuation send_seq into its own outbound frames after a
 * START, which spec sec 4i.A says a joiner must not do -- and they are NOT
 * fixed here; this item is the receive-side guarantee only.
 * ==========================================================================
 *
 * OVMX DESIGN CHOICES (labeled per CLAUDE.md rule 8):
 *
 *  1. WHAT COUNTS AS A SEQUENTIALITY FAILURE. The book states the guarantee,
 *     never how a port driver detects a breach. OVMX uses the sequence
 *     numbering it already reproduces (spec sec 4h(4), GROUNDED: "a node holds
 *     send_seq (its own next number) and recv_seq (highest peer send_seq seen);
 *     a sequenced message stamps send_seq ... then increments send_seq") --
 *     so consecutive is the rule and a jump of more than one is a breach.
 *  2. THE ANCHOR. The first sequenced message seen on a circuit establishes the
 *     baseline and can never be a gap (see scs_vc::seq_anchored). This costs
 *     one message of detection and cannot produce a false break.
 *  3. WHAT COUNTS AS A DELIVERY FAILURE. p. 2-31 does not define one either.
 *     OVMX uses retransmit exhaustion of its own outstanding sequenced message:
 *     once the retry limit is reached, delivery has demonstrably not been
 *     achieved. The LIMIT is an OVMX number, not a VMS quantity.
 *  4. BREAKING THE VC DOES NOT DEALLOCATE THE PATH BLOCK. scs_vc_break() sets
 *     vc_state to CLOSED (p. 2-11 names no "broken" state, and a broken circuit
 *     is simply not open) and leaves the PB and its CDT queue in place. Calling
 *     scs_pb_close() -- which zeroes the PB and would dangle every CDT still
 *     queued to it -- is the caller's decision and belongs to the teardown path
 *     (vms-17f), not to the detector.
 *  5. WHAT STOPS THE SENDING AFTERWARDS IS THE **CLOSED PATH BLOCK**, NOT THE
 *     SYSAP HANDLER. scsd.c's scsd_sysap_vc_loss() clears the daemon's
 *     bound-connection flags, and an earlier revision claimed that was what
 *     kept OVMX from transmitting into a broken circuit. It is NOT. Two of the
 *     three flags are read NEGATED by the dispatch, so clearing them RE-ARMS
 *     those sends; the third (`connected`) is not read by the branch that
 *     answers the peer's connect at all -- that branch SETS it, so a replayed
 *     connect re-arms it and with it the 0x81 CM-response path. MEASURED, on
 *     one circuit broken by a real sequence gap: replaying the peer's captured
 *     SCS$DIRECTORY CONNECT-REQUEST produced another CONNECT-RESPONSE **plus a
 *     credit-return**, and replaying its captured VMS$VAXcluster connect
 *     produced two more frames and re-opened the connection. Four distinct
 *     send paths, one defect: none of them consulted the Path Block.
 *
 *     Because of note 4 the PB is still there and still says CLOSED, and that
 *     is the durable fact. It is now consulted in exactly ONE place --
 *     scsd.c's send_frame_vc(), which every SCS-layer sender is routed
 *     through, with a census of senders and two justified exemptions (VC
 *     formation frames and NISCA HELLOs, both of which must transmit on a
 *     non-OPEN circuit or no circuit could ever form). Adding a per-path guard
 *     instead of using that choke point is how this defect survived a round of
 *     review: guarding paths one at a time cannot terminate.
 */

/* Why a circuit was broken. */
enum scs_vc_break_reason {
    SCS_VC_BREAK_NONE = 0,
    SCS_VC_BREAK_SEQ_GAP = 1,  /* p. 2-31 "guarantee of message sequentiality" */
    SCS_VC_BREAK_DELIVERY = 2, /* p. 2-31 "guarantee of message delivery" */
    SCS_VC_BREAK_LOCAL = 3     /* an explicit local teardown, not a guarantee failure */
};

/* Human-readable reason, for logs and tests. Never NULL. */
const char *scs_vc_break_reason_name(enum scs_vc_break_reason r);

/*
 * THE KILL SWITCH. 0 iff OVMX_NO_VC_BREAK is set to anything other than "0".
 * Read fresh on every call (NOT cached), so a test can bracket a single call.
 *
 * WHAT IT GATES, EXACTLY (guardrail 23 -- this statement is checked by
 * tests/vmsscs/test_scs_vc.c and by tests/vmsscs/test_scsd_wire.c, which RUN the
 * switch and assert the gated behaviour is suppressed):
 *   - scs_vc_break() performs NO teardown: it does not step any connection, does
 *     not invoke any SYSAP VC-loss handler, does not change vc_state, does not
 *     mark the VC broken, and returns 0. It logs ONE line saying it was
 *     suppressed, so a run log never reads as "no circuit was ever broken" when
 *     the truth is "breaking was switched off".
 *   - the DETECTORS still run: scs_vc_check_recv_seq() still returns
 *     SCS_VC_SEQ_GAP and scsd.c still counts and logs it. The switch gates the
 *     CONSEQUENCE, not the diagnosis -- so a lab run with the switch on still
 *     tells you whether a gap occurred.
 * It gates no emitted byte directly. It is wire-visible only in the sense that
 * a circuit OVMX breaks stops carrying OVMX frames.
 *
 * WHAT IT DOES *NOT* GATE, stated because "this switch reverts my item" was
 * overclaimed here once already. scsd.c's conn_bind() installs its SYSAP
 * VC-loss handler UNCONDITIONALLY, so vms-17f's peer-departure teardown --
 * scs_pb_depart() -> scs_cdl_vc_loss(), a path this switch has nothing to do
 * with -- now reaches that handler and logs SCSD-W-SYSAPVCLOSS whether this
 * switch is set or not. That is deliberate: a departure is not a
 * message-guarantee failure, and this item's switch must not disable a sibling
 * item's p. 2-28 behaviour. The reasoning is on scsd_sysap_vc_loss() in
 * src/vmsscs/scsd.c; the not-gating is ASSERTED by
 * test_departure_notifies_the_sysaps() in tests/vmsscs/test_scsd_wire.c, which
 * also runs OVMX_NO_PEER_DEPART=1 -- the switch that DOES gate that path.
 */
#define SCS_VC_NO_BREAK_ENV "OVMX_NO_VC_BREAK"
int scs_vc_break_enabled(void);

/* What scs_vc_check_recv_seq() found. */
enum scs_vc_seq_verdict {
    SCS_VC_SEQ_ANCHOR = 0,   /* first sequenced message on this VC -- baseline, never a gap */
    SCS_VC_SEQ_IN_ORDER = 1, /* exactly recv_seq + 1 */
    SCS_VC_SEQ_DUPLICATE = 2,/* at or behind recv_seq -- a retransmit, NOT a gap */
    SCS_VC_SEQ_GAP = 3       /* ahead by more than 1: sequentiality guarantee breached */
};

/*
 * scs_vc_check_recv_seq - PURE predicate. Classify one received sequenced
 * message's send_seq against the VC's recv_seq. Does not mutate. `missing_out`
 * (optional) receives the number of messages the gap implies, 0 for every other
 * verdict. A peer_send_seq of 0 is a pure ack (a 0x48 credit-return) and is
 * classified SCS_VC_SEQ_DUPLICATE -- it carries no sequence at all.
 * Comparison is modular over 16 bits, so it is correct across wrap.
 */
enum scs_vc_seq_verdict scs_vc_check_recv_seq(const struct scs_vc *vc,
                                              uint16_t peer_send_seq,
                                              unsigned *missing_out);

/*
 * scs_vc_note_recv_checked - what scs_vc_note_recv does, plus the p. 2-31
 * sequentiality check. Returns the verdict, anchors the VC, bumps seq_gaps on a
 * gap, and then advances recv_seq exactly as scs_vc_note_recv would.
 *
 * IT DOES NOT BREAK THE CIRCUIT: the caller decides, because only the caller
 * knows which Path Block the message arrived on. scsd.c calls scs_vc_break()
 * on SCS_VC_SEQ_GAP.
 *
 * recv_seq IS advanced even on a gap, deliberately: if the break is switched
 * off the VC keeps running, and not advancing would re-report the same gap for
 * every subsequent message.
 */
enum scs_vc_seq_verdict scs_vc_note_recv_checked(struct scs_vc *vc,
                                                 uint16_t peer_send_seq,
                                                 unsigned *missing_out);

/*
 * Retransmits of ONE outstanding sequenced message before delivery is declared
 * unrecoverable (design choice 3 above -- an OVMX number). Kept equal to the
 * cap scsd.c already applied when it merely stopped retransmitting, so the
 * point at which OVMX gives up is unchanged; only what it does there changes.
 */
#define SCS_VC_DELIVERY_RETRY_LIMIT 5u

/*
 * scs_vc_delivery_failed - PURE predicate: 1 iff this VC has an outstanding
 * unacked sequenced message that has been retransmitted SCS_VC_DELIVERY_RETRY_LIMIT
 * times or more, i.e. the p. 2-31 delivery guarantee cannot be satisfied.
 * 0 for NULL, for a VC with nothing outstanding, or below the limit.
 */
int scs_vc_delivery_failed(const struct scs_vc *vc);

/*
 * scs_vc_break - EXPLICITLY BREAK THE CIRCUIT (p. 2-31).
 *
 *   1. every CDT queued to pb (the p. 2-28 per-circuit connection queue) is
 *      driven through the connection state machine with SCS_CONN_EV_VC_LOST,
 *      so each connection reaches CLOSED by a transition that is logged and
 *      that a test can walk -- not by a store;
 *   2. scs_cdl_vc_loss(pb) then invokes each connection's SYSAP VC-loss error
 *      handler (p. 2-28: "the VMS implementation of SCA stores the address of
 *      the interested SYSAP's error handler for virtual circuit loss in the CDT
 *      itself"). Handlers run AFTER step 1, so a handler that inspects its
 *      connection sees it already broken;
 *   3. pb->vc_state becomes SCS_VC_CLOSED;
 *   4. `vc` (optional, may be NULL) records the break and its reason;
 *   5. `log` (optional, may be NULL) receives one SCSD-W-VCBREAK line naming
 *      the circuit and the trigger, then one SCSD-W-VCLOSSCONN line per
 *      connection naming its CONID, its SYSAP pair and the state it was in.
 *
 * Returns the number of connections broken. Returns 0 and does nothing if pb is
 * NULL, or if the kill switch is set (see scs_vc_break_enabled).
 *
 * It does NOT call scs_pb_close() -- see design choice 4.
 */
unsigned scs_vc_break(struct scs_vc *vc, struct scs_pb *pb,
                      enum scs_vc_break_reason reason, FILE *log);

/* Human-readable names, for logging. Never NULL. */
const char *scs_vc_event_name(enum scs_vc_event ev);
const char *scs_vc_action_name(enum scs_vc_action act);

#ifdef __cplusplus
}
#endif

#endif /* SCS_VC_H */
