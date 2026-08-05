/*
 * scs_disc.h - THE SCA DISCONNECT DIALOGUE ON THE WIRE (vms-591).
 *
 * The two frames Figure 2-16 draws and OVMX could not build: DISCONNECT_REQ and
 * DISCONNECT_RSP. Both are now GROUNDED on our own lab captures, and grounding
 * the RESPONSE is the part that was not supposed to be possible -- see THE
 * CORRECTION below, which overturns a standing claim in the spec.
 *
 * SOURCES (public / our own wire only -- CLAUDE.md rule 8):
 *   Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, ch. 2:
 *     - Figure 2-16 "Explicitly Breaking an SCA Connection"      pp. 2-26/2-27
 *     - the symmetry requirement                                 p. 2-26
 *     - the SIMULTANEOUS-disconnect case                         p. 2-27
 *     - the optional 16-bit disconnect reason code               p. 2-26
 *   Our own reference-lab captures, /data/training/vax/cluster/captures
 *   (47 pcaps, host-only, never committed).
 *
 * ==========================================================================
 * THE CORRECTION: DISCONNECT_RSP IS ON OUR WIRE, AND THE SPEC SAID IT WAS NOT
 * ==========================================================================
 *
 * docs/cluster-protocol-spec.md sec 4(h)(1a) carried this from vms-dd5 until
 * this item. It is quoted ONCE, inside the quarantine markers
 * tests/vmsscs/test_scs_disc_figures.py enforces, so that a reader recognises
 * what was replaced without the dead claim becoming quotable prose again:
 *
 * REFUTED-QUOTE-BEGIN
 *     "`5` and `7` DO NOT EXIST ON OUR WIRE ... Do not build a `5` or `7`
 *      frame on this section."
 * REFUTED-QUOTE-END
 *
 * THAT IS FALSE, and the reason it survived three revisions is a sampling
 * error, not a decoding error: every census of the connection-control field
 * had been restricted to the SCA length classes {62, 66, 110}, because those
 * were the classes the four FORMATION messages occupy. The two RESPONSE
 * messages are SHORTER THAN ANY OF THEM. They are 58 bytes -- the envelope,
 * the message type and the Con.ID pair, and nothing else -- so they fell
 * outside the filter that was looking for them.
 *
 * Re-measured with no length restriction at all
 * (tools/cluster/scs_disc_measure.py, all 47 captures). The pairing rule is:
 * for each connection-control frame, the FIRST frame in the reverse direction
 * whose Con.ID pair is this frame's pair with the two handles swapped.
 *
 * DISC-CENSUS-CAPTURES: 47
 * DISC-CENSUS-PAIR: 110 0 -> 66 1 n=1115 pcaps=34
 * DISC-CENSUS-PAIR: 110 2 -> 62 3 n=381 pcaps=33
 * DISC-CENSUS-PAIR: 62 4 -> 58 5 n=696 pcaps=26
 * DISC-CENSUS-PAIR: 62 6 -> 58 7 n=262 pcaps=25
 *
 * i.e. CONNECT_REQ -> CONNECT_RSP, ACCEPT_REQ -> ACCEPT_RSP, REJECT_REQ ->
 * REJECT_RSP, DISCONNECT_REQ -> DISCONNECT_RSP. So the eight SCA
 * connection-control messages are all four REQ/RSP pairs, in figure order, on
 * our own wire -- and `5` and `7` were always there.
 *
 * The VMS-origin populations of the two frames this file builds:
 *
 * DISC-CENSUS-POP: 62 6 n=220 pcaps=25
 * DISC-CENSUS-POP: 58 7 n=223 pcaps=25
 *
 * (Those counts are VMS-SOURCED ONLY. A further 42 of each are OVMX's own,
 * from the pre-vms-591 attempt docs/HANDOFF-vms-2f3.md sec 4M.24 describes,
 * and counting our own frames as evidence about VMS would be circular. The
 * measure script reports them separately and never mixes them in.)
 *
 * THIS FILE'S SCOPE IS THE DISCONNECT PAIR. `5` (REJECT_RSP) is measured above
 * and left alone; OVMX has no production REJECT caller to answer (scs_svc.h),
 * and building a frame for a dialogue nothing enters would be untested code.
 * The spec's sec 4(h)(1a) and sec 5 are UPDATED in this branch with the
 * measurement, so the next agent reads the corrected table and not the gap.
 *
 * ==========================================================================
 * THE FOUR-FRAME TEARDOWN, LITERALLY ON OUR WIRE
 * ==========================================================================
 *
 * formation-ci1.pcap raw frames 61/63/64/65 (SCA #53/#55/#56/#57) are Figure
 * 2-16 end to end, between VAX1 (Con.ID 0x63050008) and VAX2 (0x33590007):
 *
 *   #53  VAX1 -> VAX2   62 B  msgtype 6  DISCONNECT_REQ   [60:62] = 0x0000
 *   #55  VAX2 -> VAX1   58 B  msgtype 7  DISCONNECT_RSP
 *   #56  VAX2 -> VAX1   62 B  msgtype 6  DISCONNECT_REQ   [60:62] = 0x0001
 *   #57  VAX1 -> VAX2   58 B  msgtype 7  DISCONNECT_RSP
 *
 * i.e. exactly p. 2-26: NODE_1's SYSAP disconnects; NODE_2's SCS answers with
 * a DISCONNECT_RSP and tells its SYSAP; NODE_2's SYSAP "symmetrically invokes
 * the DISCONNECT service", which sends the MATCHING DISCONNECT_REQ; NODE_1
 * answers that with its own DISCONNECT_RSP and both ends reach CLOSED.
 *
 * ==========================================================================
 * THE MATCHING FLAG AT [60:62] -- NEWLY GROUNDED BY THIS ITEM
 * ==========================================================================
 *
 * spec sec 4(h)(1a) recorded [60:62] on a DISCONNECT_REQ as "a live field with
 * an undecoded meaning". It is decoded now, by a partition with ZERO residuals
 * over all 220 VMS-origin DISCONNECT_REQ frames. `rank` is the frame's index
 * within its own Con.ID-pair dialogue: rank 0 is the FIRST DISCONNECT_REQ on
 * that pair, rank 1 the MATCHING one from the other end.
 *
 * DISC-CENSUS-MATCH: rank=0 value=0x0000 n=131 pcaps=25
 * DISC-CENSUS-MATCH: rank=1 value=0x0001 n=89 pcaps=18
 * DISC-CENSUS-MATCH-RESIDUALS: 0
 *
 * No frame of either rank carries the other value; no pair carries a third
 * DISCONNECT_REQ. (131 != 89 because 42 dialogues' matching half falls outside
 * the capture window -- the initiator's half is what a capture that starts
 * mid-teardown keeps.)
 *
 * So [60:62] distinguishes "I am initiating a disconnect" from "I am matching
 * yours", which is precisely Figure 2-16's DISC SENT vs DISC MATCH. OVMX sets
 * it from scs_disc_params::matching.
 *
 * HONESTY BOUND, in the same terms sec 4(h)(1a) uses for msgtype 4: this is
 * the best reading of a decisive behavioural partition, NOT a decoded field.
 * Nothing published names it, and a peer that ignores it would be
 * indistinguishable in our data. What is measured is the partition; what is
 * inferred is the meaning.
 *
 * ==========================================================================
 * THE TEMPLATES (byte-exact replay, same posture as scs_dir.c / scs_connect.c)
 * ==========================================================================
 *
 *   DISCONNECT_REQ = formation-ci1.pcap raw frame 64 (SCA #56), VAX2 -> VAX1
 *   DISCONNECT_RSP = formation-ci1.pcap raw frame 63 (SCA #55), VAX2 -> VAX1
 *
 * Both are VAX2's frames -- the same node whose CONNECT-RESPONSE and directory
 * responses scs_connect.c and scs_dir.c already replay -- and they are two
 * adjacent frames of one teardown, so the pair is internally coherent.
 *
 * SUBSTITUTED (GROUNDED positions, all payload-relative; absolute = 14 + off):
 *   [ 2: 8]  destination logical address        (peer_logical)
 *   [10:16]  source logical address             (src_logical, spec sec 4g)
 *   [18:20]/[26:28]/[34:36]  recv_ack           (spec sec 4h(4))
 *   [20:22]/[30:32]          send_seq           (the mirror is GROUNDED)
 *   [22:24]  node-incarnation echo              (spec sec 4i.B; 0 keeps the
 *            template's fresh-contact value 1)
 *   [50:54]  remote Con.ID    [54:58]  local Con.ID
 *   [58:60]  the reason code  -- REQUEST ONLY, and its OFFSET is the LABELED
 *            OVMX design choice vms-6b3 made (scs_reason.h). Not a decoded VMS
 *            field. Every VMS DISCONNECT_REQ we hold reads 0 there.
 *   [60:62]  the matching flag -- REQUEST ONLY (see above)
 *
 * REPLAYED, ungrounded, left at the captured values and labeled as such:
 *   [ 8:10] = 0x0001, [24:26] = 0x0012 (NISCS_LAN_OVRHD), [38:42] =
 *   01 00 00 02, [44:46] = 0x0004, [48:50] = 0x0000. These are CONST across
 *   100% of the measured population in BOTH frames, which is why replaying
 *   them is safe; it is not a claim that any of them is understood.
 *
 * DERIVED, not replayed: [ 0: 2] SCA length and [42:44] inner length. The
 * template values (0x003c/0x0012 and 0x0038/0x000e) satisfy the sec 4h rule
 * inner = payload_len - 44 exactly, and the builders recompute both rather
 * than copying, so a length change cannot silently desynchronise them.
 *
 * ==========================================================================
 * KILL SWITCH: OVMX_NO_CLEAN_SHUTDOWN
 * ==========================================================================
 *
 * WIRE-VISIBLE, and this switch gates EVERY new byte. Before vms-591 OVMX
 * emitted no DISCONNECT frame of any kind, ever -- not in the dialogue, not at
 * exit. With OVMX_NO_CLEAN_SHUTDOWN=1 the builders below refuse (return -1),
 * scsd.c's emitter therefore reports SCS_SVC_EMIT_NOBUILDER exactly as it did
 * before this item, and the daemon's exit teardown does not run. That restores
 * the pre-vms-591 wire byte for byte, because the pre-vms-591 wire had none of
 * these frames on it.
 *
 * The switch does NOT suppress the connection STATE machine: with it set, a
 * received DISCONNECT_REQ still moves the CDT to DISC RECEIVED and still logs
 * the transition. That is deliberate -- the diagnostic vms-dd5 exists for must
 * keep working -- and it is stated here so nobody reads "kill switch set" as
 * "nothing happened". (OVMX_NO_CONN_FSM is the switch for the state half.)
 *
 * Read fresh on every call, NOT cached, so a test can bracket one build.
 *
 * A SECOND SWITCH ALSO REACHES THIS FRAME: vms-6b3's OVMX_NO_REASON_CODE. The
 * reason code at [58:60] is written by scs_reason_put(), which enforces it, so
 * with that switch set the DISCONNECT_REQ carries the template's own 0x0000 --
 * the value every VMS DISCONNECT_REQ we hold carries. That is deliberate: the
 * offset is a LABELED OVMX design choice, not a decoded VMS field, and it is
 * the one byte-pair in this frame OVMX sets where real VMS traffic never does,
 * so being able to turn it off independently is the control a future
 * investigation will want.
 *
 * ==========================================================================
 * THE LAB VERDICT -- READ THIS BEFORE BELIEVING ANY OF THE ABOVE
 * ==========================================================================
 *
 * Four runs on lab-2 replica `vaxlab-4`, a pod SCALED UP FOR THIS ITEM so no
 * sibling's lab was disturbed and no OVMX identity had ever been seen there.
 * Each run used a FRESH SCSSYSTEMID, because vms-2f3 sec 4M.25 shows a
 * returning identity is refused -- and a refused run would leave the kill
 * switch with nothing to act on, which is the sec 4M.26 trap this epic already
 * fell into once.
 *
 *   run  identity        switch                   verdict
 *   ---- --------------- ------------------------ -----------------------------
 *   A1   OVMXA1 / 1401   (none)                   joined; 2 DISCONNECT_REQ sent
 *   B1   OVMXB1 / 1402   OVMX_NO_CLEAN_SHUTDOWN=1 joined; ZERO sent
 *   A2   OVMXA2 / 1403   (none)                   joined; 2 sent, 20 s tail
 *   C1   OVMXC1 / 1404   OVMX_NO_REASON_CODE=1    joined; 2 sent, reason 0x0000
 *
 * All four reached SCSD-I-JOINBOUND and drove the add-member burst, so every
 * run had two open connections to tear down.
 *
 * WHAT WORKED, measured on the wire (`591-{A1,A2,B1,C1}.pcap`, decoded from the
 * pod's own br0 capture):
 *
 *   - OVMX PERFORMS ITS OWN DISCONNECT CALL. Two 62-byte message-type-6 frames
 *     per run, one per open connection, each addressed to that connection's
 *     Con.ID pair, matching flag 0 (initiating), at process exit. Before this
 *     item that count was structurally ZERO.
 *   - THE KILL SWITCH GATES THE WIRE, AND IT WAS RUN (guardrail 23). Run B1 put
 *     ZERO message-type-6 frames on the wire while reaching the same join
 *     milestones as A1/A2/C1. The switch had something to suppress and
 *     suppressed exactly it.
 *   - THE FRAME IS RIGHT. Byte-diffed against a real VAX DISCONNECT_REQ
 *     (formation-ci1.pcap): every differing payload byte falls inside a field
 *     the frame is SUPPOSED to carry differently -- logical address, the three
 *     recv_ack copies, the two send_seq copies, the incarnation, the Con.ID
 *     pair, the reason code and the matching flag. UNEXPLAINED DIFFERENCES: 0.
 *
 * WHAT DID NOT WORK, stated first and not buried:
 *
 *   >>> THE PEER NEVER ANSWERS. No message-type-7 frame arrives, ever. The A2
 *   >>> capture ran 20 s past the request; C1 ran 15 s. Not one DISCONNECT_RSP.
 *   >>> So the connection does NOT reach CLOSED, and the last bullet of this
 *   >>> item's done-condition -- "the peer CDT reaches CLOSED rather than being
 *   >>> left pending" -- IS NOT MET. Every run exits reporting
 *   >>> SCSD-W-DISCPEND, 2 connection(s) still open, which is the honest
 *   >>> outcome and is why the wait is bounded.
 *
 * AND THE PEER SAYS WHY, IN ITS OWN WORDS. VAX1's console logs
 *
 *     %PEA0, Inappropriate SCA Control Message - FLAGS/OPC/STATUS/PORT 00/22/00/DD
 *
 * EXACTLY ONCE PER DISCONNECT-SENDING RUN -- 3 messages across A1, A2 and C1,
 * and NONE for B1, which instead produced the plain "%PEA0, Port has Closed
 * Virtual Circuit - REMOTE NODE OVMX" of a node that simply vanished. The VAX
 * is not ignoring OVMX's DISCONNECT_REQ; it is RECEIVING it and REFUSING it.
 * That is a better diagnostic than silence and it is the lead this item hands
 * on. What `OPC/22` denotes is NOT known -- it is a port-level opcode, not the
 * connection-control message type at [46:48], and nothing we hold decodes it.
 * Do not guess at it.
 *
 * ONE HYPOTHESIS KILLED, with a matched control. The obvious suspect was the
 * reason code: [58:60] is a LABELED OVMX placement, and OVMX was the only node
 * on that wire ever to set it (0x0005, SYSAP_SHUTDOWN, where all 220 VMS-origin
 * DISCONNECT_REQ frames read 0). Run C1 suppressed it with
 * OVMX_NO_REASON_CODE=1, and THE SWITCH WAS CONFIRMED ENGAGED ON THE WIRE
 * before anything was concluded -- the captured frames read 0x0000 at [58:60]
 * in C1 against 0x0005 in A2. The VAX logged "Inappropriate SCA Control
 * Message" anyway. THE REASON CODE IS NOT THE CAUSE.
 *
 * WHAT WAS NOT DONE: SDA `SHOW CONNECTIONS` on VAX1 was attempted and did not
 * complete (ANALYZE/SYSTEM did not reach an SDA> prompt inside the console
 * timeout), so the peer's CDT state is inferred from its console messages and
 * from the absence of a reply, NOT read off the SDA oracle. Anyone continuing
 * this should get that reading first.
 *
 * ==========================================================================
 * WHAT THIS DOES NOT DO
 * ==========================================================================
 *
 * IT IS NOT A FIX FOR THE vms-2f3 REJOIN FAILURE and nothing here may be read
 * as claiming it is. docs/HANDOFF-vms-2f3.md sec 4M.24 already KILLED the
 * hypothesis that OVMX performing its own disconnect call unblocks the rejoin:
 * a prior attempt put a well-formed op6 on the wire and *the peer never
 * answered it*, because -- sec 4M.24's real finding -- the peer's recv_ack
 * never advances past our op9 in a refused rejoin, so from its point of view we
 * have not answered its op8 at all. Eleven hypotheses are dead on that item;
 * this is not a twelfth. What this item delivers is a CORRECT teardown, which
 * OVMX owed the protocol whether or not it moves the rejoin.
 */
#ifndef OVMX_SCS_DISC_H
#define OVMX_SCS_DISC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SCA content lengths (GROUNDED length classes, see the census above). */
#define SCS_DISC_REQ_SCA_LEN   62
#define SCS_DISC_REQ_FRAME_LEN (14 + SCS_DISC_REQ_SCA_LEN) /* 76 */
#define SCS_DISC_RSP_SCA_LEN   58
#define SCS_DISC_RSP_FRAME_LEN (14 + SCS_DISC_RSP_SCA_LEN) /* 72 */

/* Connection-control message types at payload [46:48] (spec sec 4h(1a)). */
#define SCS_DISC_MSGTYPE_REQ 6u
#define SCS_DISC_MSGTYPE_RSP 7u

/* The matching flag, payload-relative. REQUEST ONLY. */
#define SCS_DISC_MATCH_PAYLOAD_OFF 60u
#define SCS_DISC_MATCH_FRAME_OFF   (14u + SCS_DISC_MATCH_PAYLOAD_OFF)
#define SCS_DISC_MATCH_INITIAL     0x0000u /* rank 0: I am initiating */
#define SCS_DISC_MATCH_MATCHING    0x0001u /* rank 1: I am matching yours */

struct scs_disc_params {
    uint8_t  dst_mac[6];      /* Ethernet dst = peer's observed Ethernet src MAC */
    uint8_t  src_mac[6];      /* Ethernet src (abs 6) = OVMX HW MAC */
    uint8_t  src_logical[6];  /* SCA src-logical [10:16] = aa:00:04:00:<LE16(sysid)> */
    uint8_t  peer_logical[6]; /* SCA dest-logical [2:8] = peer's advertised logical addr */
    uint32_t remote_conid;    /* [50:54] the peer's handle for this connection */
    uint32_t local_conid;     /* [54:58] OVMX's own handle */
    uint16_t recv_ack;        /* [18:20]/[26:28]/[34:36] */
    uint16_t send_seq;        /* [20:22]/[30:32] */
    uint16_t incarnation;     /* [22:24]; 0 => leave the template's 1 */
    uint16_t reason;          /* [58:60] REQUEST ONLY -- vms-6b3, enum scs_reason_code */
    int      matching;        /* !=0 => [60:62] = 1 (DISC MATCH); 0 => 0 (DISC SENT) */
};

/*
 * scs_disc_enabled - 0 iff OVMX_NO_CLEAN_SHUTDOWN is set to anything other
 * than "0". Read fresh on every call (never cached).
 */
int scs_disc_enabled(void);

/*
 * scs_disc_build_request / scs_disc_build_response - build the frame into
 * `out`. Return 0 on success; -1 if p/out is NULL **or if scs_disc_enabled()
 * is 0** -- the kill switch is enforced in the builder, not at the call site,
 * so no caller can route around it.
 */
int scs_disc_build_request(const struct scs_disc_params *p,
                           uint8_t out[SCS_DISC_REQ_FRAME_LEN]);
int scs_disc_build_response(const struct scs_disc_params *p,
                            uint8_t out[SCS_DISC_RSP_FRAME_LEN]);

/*
 * scs_disc_match_get - read the matching flag out of a received frame.
 * Returns 1 and fills *out for a frame that is long enough and whose
 * connection-control message type is DISCONNECT_REQ; 0 otherwise.
 */
int scs_disc_match_get(const uint8_t *frame, size_t len, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_SCS_DISC_H */
