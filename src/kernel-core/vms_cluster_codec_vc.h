/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_vc.h - virtual-circuit formation + sequenced-message
 * envelope typed codec entries (plan item FC-P1.1; design
 * docs/design-faithful-cluster-executive.md sec 3.2.1/3.9, P1 "Virtual
 * circuits in the executive"; wire spec docs/cluster-protocol-spec.md
 * sec 4(g) phase 2 (START/STACK/ACK), 4(g) "SCS sequenced-message
 * counters" (recv_ack@32/send_seq@34), 4(h)(3) (credit-return 0x48)).
 *
 * This is the SECOND per-family harvest file layered on top of the frozen
 * vms_cluster_codec.{c,h} (FC-P0.6) and alongside vms_cluster_codec_hello.
 * {c,h} (FC-P0.7) -- same convention: the shared TU stays FROZEN, this
 * family gets its own header/source pair so P1.1 lands without touching
 * the file every other harvest item also wants to grow.
 *
 * SCOPE. Three typed parse/build pairs:
 *   1. vms_scs_start_frame  -- the phase-2 0x41 START/STACK/ACK body (spec
 *      sec 4(g) phase 2). START and STACK are the SAME 106-byte wire shape
 *      (VAXcluster Principles p.2-12: a STACK re-supplies the same
 *      identity body as the START it acknowledges) -- one builder, the
 *      caller picks config_round. The 46-byte ACK has no identity body and
 *      gets its own builder.
 *   2. vms_scs_seq_envelope -- the minimal envelope every SEQUENCED SCS
 *      message this item's scope covers (msgtype 0x4b application, 0x5b
 *      connection-setup) shares: the SCA header plus recv_ack@32/
 *      send_seq@34 (spec sec 4(g) "SCS sequenced-message counters", the
 *      grounded mechanism). Stops there -- the SYSAP body past abs 72 is
 *      owned by the harvest item that grounds that SYSAP (P2.1 SCS, P3.1
 *      CM, P4.5 DLM, P6.2 MSCP); this struct exists so the VC engine
 *      (P1.2) can stamp/read the fields it tracks without needing to
 *      understand any message's body.
 *   3. vms_scs_credit_frame -- the 41-byte 0x48 credit-return short (spec
 *      sec 4(h)(3), GROUNDED byte-exact against 622/622 real frames).
 *
 * THE HONESTY RULE (INV-6 + the honest-os-identity-broadcast /
 * honest-per-node-identity ruling this item inherits from FC-P0.7).
 * Every field that VARIES per node or per boot -- SCSSYSTEMID, node name,
 * software version, hardware type, the two live absolute-time quadwords,
 * CLUSTER_CREDITS -- is CALLER-SUPPLIED, never a baked capture constant.
 * Only bytes the spec GROUNDS as a fixed, node-independent protocol
 * constant (the opcode/format markers, NISCS_LAN_OVRHD=18, the inner
 * length identity, and the handful of "constant observed N/N frames"
 * spans that held identical across THREE distinct reconfigured node
 * identities in the grounding capture -- spec sec 4(g) phase 2's own
 * vms-cd0 SCSNODE/SCSSYSTEMID/VOTES contrast) are baked in here, exactly
 * as vms_sca_hdr_build already bakes in VMS_SCA_ETHERTYPE and
 * disc_put_format_markers bakes in the discovery sandwich. A byte whose
 * MEANING is genuinely ungrounded is never invented into a bake -- there
 * is no such span left unaccounted for in the 4(g) phase-2 table, unlike
 * HELLO's cap_span/reserved_64.
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_VC_H
#define OVMX_VMS_CLUSTER_CODEC_VC_H

#include "vms_cluster_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Shared per-frame addressing (abs 0-31 fields a caller must supply for
 * every class in this file: the two Ethernet MACs and the two cluster-
 * LOGICAL LAVC addresses, spec sec 4(a).0/4(g)). connect_flag and the
 * SCA-length field are NOT here -- they are GROUNDED protocol constants
 * for every class this file builds (connect_flag universally 0x0001;
 * the length is fully determined by which builder ran), so they are
 * baked, not threaded.
 * ------------------------------------------------------------------ */
struct vms_scs_addr {
	uint8_t dst_mac[VMS_ETH_ADDR_LEN];      /* Ethernet dst (abs 0)      */
	uint8_t src_mac[VMS_ETH_ADDR_LEN];      /* Ethernet src (abs 6)      */
	uint8_t dst_logical[VMS_ETH_ADDR_LEN];  /* SCA dst-logical (abs 16), */
						 /* the peer's LAVC addr      */
	uint8_t src_logical[VMS_ETH_ADDR_LEN];  /* SCA src-logical (abs 24), */
						 /* OWN cluster-LOGICAL addr, */
						 /* NOT the raw HW MAC        */
						 /* (vms-9f3)                 */
};

/* ------------------------------------------------------------------ *
 * START / STACK / ACK -- spec sec 4(g) phase 2, opcode 0x41
 * ------------------------------------------------------------------ */

#define VMS_SCS_START_SCA_LEN     106u  /* START/STACK content length     */
#define VMS_SCS_START_FRAME_LEN   (VMS_ETH_HDR_LEN + VMS_SCS_START_SCA_LEN)
#define VMS_SCS_START_ACK_SCA_LEN  46u  /* round-2 ACK content length     */
#define VMS_SCS_START_ACK_FRAME_LEN (VMS_ETH_HDR_LEN + VMS_SCS_START_ACK_SCA_LEN)

#define VMS_SCS_START_NODENAME_LEN 8u   /* fixed 8-byte blank-padded      */
#define VMS_SCS_START_SWVER_LEN    8u
#define VMS_SCS_START_HWTYPE_LEN   4u

#define VMS_SCS_START_ACK_ROUND    2u   /* the round-2 ACK's fixed round  */

/* SYSGEN NISCS_LAN_OVRHD, GROUNDED byte-exact at abs 38 of every START/
 * STACK/ACK/credit-return frame (spec sec 4(g)/4(h)(3)) -- a fixed
 * protocol constant, not a per-node value. */
#define VMS_NISCS_LAN_OVRHD        18u

/* abs offsets this file teaches the codec, beyond the parent's abs 30/31
 * msgtype+format and abs 32/34 recv_ack/send_seq (reused, not redefined). */
#define VMS_OFF_START_INCARN      36u  /* node-incarnation echo, 4(i).B   */
#define VMS_OFF_START_SEQ_MIRROR  44u  /* mirror of send_seq (abs 34)     */
#define VMS_OFF_START_ROUND       58u  /* config-round counter            */
#define VMS_OFF_START_SYSID       60u  /* SCSSYSTEMID, LE u16             */
#define VMS_OFF_START_SWVER       72u  /* software version, 8 ASCII       */
#define VMS_OFF_START_INCARNTIME  80u  /* THIS SYSTEM'S INCARNATION, u64  */
#define VMS_OFF_START_HWTYPE      88u  /* hardware type, 4 ASCII          */
#define VMS_OFF_START_CREDITS     95u  /* SYSGEN CLUSTER_CREDITS, 1 byte  */
#define VMS_OFF_START_NODENAME   104u  /* node name, 8 ASCII blank-padded */
#define VMS_OFF_START_MSGTIME    112u  /* frame-composition time, u64     */

/*
 * struct vms_scs_start_frame - the START/STACK/ACK body (spec sec 4g
 * phase 2). BUILD: every field below is threaded through from the
 * caller; is_ack is ignored by the builders (vms_scs_start_build always
 * emits the 106-byte identity-bearing class, vms_scs_start_build_ack
 * always emits the 46-byte class). PARSE: is_ack and total_sca_len report
 * which class was actually seen; scssystemid/software_version/
 * hardware_type/credits/node_name/incarnation_time/message_time are only
 * valid when !is_ack (the 46-byte ACK carries no identity body).
 */
struct vms_scs_start_frame {
	struct vms_scs_addr addr;
	uint8_t  is_ack;             /* PARSE-ONLY: 1 iff the 46-byte class  */
	uint16_t recv_ack;           /* abs 32: 0 during the pre-VC START    */
	uint16_t send_seq;           /* abs 34, mirrored at abs 44 (GROUNDED)*/
	uint16_t incarnation;        /* abs 36: the node-incarnation the     */
				      /* MEMBER advertised (spec 4i.B GATE),  */
				      /* echoed verbatim -- READ from the     */
				      /* peer's directed HELLO, never a       */
				      /* hard-coded 1                         */
	uint16_t config_round;       /* abs 58: 0/1 for START/STACK          */
	uint16_t scssystemid;        /* abs 60: OWN SCSSYSTEMID (valid iff   */
				      /* !is_ack)                              */
	uint8_t  software_version[VMS_SCS_START_SWVER_LEN]; /* abs 72, OWN   */
				      /* identity -- NEVER a "VMS Vx.y"       */
				      /* masquerade (authenticity INV-0)      */
	uint64_t incarnation_time;   /* abs 80: THIS SYSTEM'S INCARNATION,   */
				      /* a live VMS absolute-time quadword    */
				      /* per boot (vms-2f3) -- 0 is a         */
				      /* legitimate but dishonest-if-shipped  */
				      /* value; this codec writes exactly     */
				      /* what it is given, no template        */
				      /* fallback                              */
	uint8_t  hardware_type[VMS_SCS_START_HWTYPE_LEN];   /* abs 88, e.g.  */
				      /* "VAX " -- OWN hardware class, varies */
				      /* by substrate/architecture             */
	uint8_t  credits;            /* abs 95 (pl81): OWN SYSGEN            */
				      /* CLUSTER_CREDITS                      */
	uint8_t  node_name[VMS_SCS_START_NODENAME_LEN]; /* abs 104: OWN     */
				      /* SCSNODE, space-padded by the caller   */
				      /* (fixed encoding, distinct from        */
				      /* HELLO's length-prefixed name)         */
	uint64_t message_time;       /* abs 112: when THIS frame was         */
				      /* composed, a second live quadword,     */
				      /* distinct from incarnation_time        */
};

/*
 * vms_scs_start_build - encode a 106-byte START/STACK body from *f
 * (config_round selects which round; 0 = START, >=1 = STACK -- spec sec
 * 4(g) names both the same wire shape). Bakes ONLY the protocol-format
 * constants the phase-2 table grounds as node-independent (NISCS_LAN_OVRHD,
 * the inner-length identity, the zero/constant spans held identical across
 * three distinct reconfigured node identities); every identity/version/
 * timestamp field comes from *f.
 */
vms_codec_status_t vms_scs_start_build(const struct vms_scs_start_frame *f,
				       uint8_t *frame, uint32_t cap,
				       uint32_t *written);

/*
 * vms_scs_start_build_ack - encode the 46-byte round-2 ACK. Uses only
 * addr/recv_ack/send_seq/incarnation from *f; config_round is forced to
 * VMS_SCS_START_ACK_ROUND regardless of what *f carries.
 */
vms_codec_status_t vms_scs_start_build_ack(const struct vms_scs_start_frame *f,
					   uint8_t *frame, uint32_t cap,
					   uint32_t *written);

/*
 * vms_scs_start_parse - decode a received START/STACK/ACK (spec sec 4g
 * phase 2) into *out. Class-gated on fi->cls == VMS_FCLS_SCS_START.
 * out->is_ack is set from the frame's actual content length (106 vs 46);
 * the identity fields are left zeroed when is_ack is set.
 */
vms_codec_status_t vms_scs_start_parse(const uint8_t *frame, uint32_t len,
				       const struct vms_frame_info *fi,
				       struct vms_scs_start_frame *out);

/* ------------------------------------------------------------------ *
 * The sequenced-message envelope (spec sec 4(g) "SCS sequenced-message
 * counters" / 4(h)(4)): msgtype 0x4b (sequenced application) or 0x5b
 * (connection-setup), recv_ack@32, send_seq@34. Deliberately minimal --
 * see the file header comment for why this stops before the Con.ID pair
 * and SYSAP body.
 * ------------------------------------------------------------------ */

#define VMS_SCS_SEQ_ENVELOPE_LEN (VMS_SCA_HDR_LEN + 4u) /* abs 0-35, 36B */

struct vms_scs_seq_envelope {
	struct vms_scs_addr addr;
	uint8_t  msgtype;    /* abs 30: VMS_SCS_MT_MSG or VMS_SCS_MT_SETUP   */
	uint16_t recv_ack;   /* abs 32                                       */
	uint16_t send_seq;   /* abs 34                                       */
};

/*
 * vms_scs_seq_envelope_build - encode the 36-byte envelope prefix. Rejects
 * (VMS_CODEC_E_INVAL) an msgtype outside {VMS_SCS_MT_MSG, VMS_SCS_MT_SETUP}
 * -- this item's scope is exactly those two sequenced classes (spec sec
 * 4(g) phase 4 / 4(h)); a caller building any other class uses that
 * class's own codec entry.
 */
vms_codec_status_t
vms_scs_seq_envelope_build(const struct vms_scs_seq_envelope *e,
			   uint8_t *frame, uint32_t cap, uint32_t *written);

/*
 * vms_scs_seq_envelope_parse - decode the envelope from a received frame.
 * Class-gated on the VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ capability pair (the
 * same gate vms_scs_msgtype()/vms_scs_seq() use) and further refuses a
 * msgtype outside {0x4b, 0x5b} -- reading a 0x41 START or 0x48 credit
 * frame through this accessor is a caller bug, not an envelope.
 */
vms_codec_status_t
vms_scs_seq_envelope_parse(const uint8_t *frame, uint32_t len,
			   const struct vms_frame_info *fi,
			   struct vms_scs_seq_envelope *out);

/*
 * vms_scs_seq_envelope_fixup_len - rewrite ONLY the SCA length field (abs
 * 14) once a higher layer has appended its own content past the 36-byte
 * envelope vms_scs_seq_envelope_build() wrote. That builder's own doc
 * comment names this exact need: "the caller/higher layer MUST overwrite
 * [the placeholder] once the body is appended, exactly as
 * vms_hello_build_padded() rewrites the length field after the fact" --
 * this is that rewrite, generalised from the padded-HELLO case to any
 * envelope-plus-tail frame (FC-P1.3's `pe_vc_send_msg`/`pe_vc_send_dg`:
 * the port's own 0-35 envelope, followed by the SCS/SYSAP content the
 * caller already built at abs 56 -- spec sec 4(d)/(1b), "the envelope
 * unifies across every length class"). `total_len` is the frame's real
 * total length INCLUDING the 14-byte Ethernet header. Returns
 * VMS_CODEC_E_RANGE if `total_len` cannot fit in the SCA length field's
 * 16 bits plus the +2 identity, VMS_CODEC_E_SHORT if `cap` cannot hold
 * abs 14..15.
 */
vms_codec_status_t
vms_scs_seq_envelope_fixup_len(uint8_t *frame, uint32_t cap,
			       uint32_t total_len);

/* ------------------------------------------------------------------ *
 * abs 36..55 -- THE TRANSPORT COUNTER SPAN every sequenced message
 * carries (spec sec 4(d) "SCS sequence-number region" + sec 4(h)(4)).
 *
 * The span is NOT the SYSAP's and NOT the SCS connection's: it belongs to
 * the port pair, exactly as recv_ack/send_seq at abs 32/34 do (*VAXcluster
 * Principles* pp. 2-30/2-31). It is stamped by the entry below, in ONE
 * place, at transmit time -- and re-stamped on every retransmission, so
 * the acknowledgement a retransmit carries is never stale.
 *
 * GROUNDED over 239,981 sequenced frames (msgtype 0x4b/0x5b/0x7b) across
 * every REFERENCE capture in ~/vax/cluster/captures -- re-derive with
 *   tools/cluster/scs_counter_span_measure.py <captures> --exclude ovmx
 * (the exclusion keeps our own emissions out of the population used to
 * judge our own emissions):
 *
 *   abs 36  node-incarnation echo: 1 (76.1%) / 2 (19.4%) / 3 (4.4%);
 *           ZERO IN 0 OF 239,981 -- see "abs 36 IS THE INCARNATION" below
 *   abs 38  SYSGEN NISCS_LAN_OVRHD == 18       239932/239981; zero in 0
 *   abs 40  recv_ack mirror                    239916/239981; zero in 65
 *   abs 42  zero                               239981/239981
 *   abs 44  send_seq mirror                    239981/239981
 *   abs 46  zero                               239981/239981
 *   abs 48  recv_ack, 3rd repeat               239872/239981; zero in 109
 *   abs 50  zero                               239981/239981
 *   abs 52  constant 0x0001                    239862/239981; zero in 0
 *   abs 54  constant 0x0200                    238521/239981; zero in 0
 *
 * WHY IT IS WRITTEN AND NOT OMITTED (E63). Leaving the span zero is not an
 * honest omission, it is a poisoned frame: the all-zero shape is unobserved
 * -- abs 36/38/52/54 are zero in NOT ONE of 239,981 real frames -- and a
 * live 2-node VAX cluster ACKNOWLEDGED NOT ONE of the 8,550 sequenced frames
 * a port that zeroed the span sent over 1,604 seconds (both peers' recv_ack
 * toward us: max 0; VAX1 and VAX2 retransmitted their own SCS$DIR_LOOKUP
 * CONNECT_REQ 498 and 500 times instead). The two live values (abs 40/48)
 * are the SAME executive read that produces abs 32 -- the circuit's recv_seq
 * -- never a second, invented counter.
 *
 * abs 36 IS THE INCARNATION, NOT A MESSAGE COUNT (E66 -- this REPLACES the
 * earlier "small message count, honest floor 1" reading, which was a MODEL
 * and is now REFUTED by measurement; spec sec 4(h)(4b)'s "what selects 2 or
 * 3 is NOT decoded" RE gap is closed by it).
 *
 * abs 36 carries the SAME field on EVERY class of a circuit -- 0x41
 * START/STACK/ACK, 0x4b/0x5b/0x7b sequenced, and the 0x48 credit-return:
 * the sec 4(i).B NODE-INCARNATION ECHO, i.e. the number the PEER advertised
 * for THIS node in its directed HELLO at payload [78:80]. Re-derive:
 *
 *   - af2-firsttimer-established-20260728.pcap. VAX1 (the established
 *     member) advertises 1 -> 2 -> 3 to the joiner across its three
 *     incarnations, and the joiner stamps abs 36 = 1/2/3 IN LOCKSTEP on all
 *     three classes: 0x41 {1:3, 2:3, 3:3}, sequenced {1:11769, 2:8042,
 *     3:10658}, credit-return {1:208, 2:29, 3:23}. VAX1 itself stamps 1 on
 *     18,869 sequenced frames -- the value the joiner advertised to IT.
 *   - cd0-bootB-zk1099-join-20260728.pcap: the same 1 -> 2 walk across
 *     0x41 / sequenced / credit-return.
 *   - A "message count" cannot explain this: the value is PER-CIRCUIT and
 *     constant. In one 2-node capture the SAME node stamps 2 toward one
 *     peer and 1 toward another, in the same seconds, matching exactly what
 *     each of those peers advertised to it (6 directions, 6/6).
 *
 * WHY A CONSTANT HERE IS FATAL (E66, the wall three fixes could not move).
 * A port that echoed the incarnation on its 0x41 frames but baked 1 into
 * every sequenced frame and credit-return was, against a live 2-node VAX
 * cluster that advertised incarnation 8 to it, acknowledged ZERO times in
 * 1,500 s: 8,146 sequenced frames and 1,028 credit-returns went out at
 * abs 36 = 1, both peers' recv_ack toward it stayed 0, and both peers
 * retransmitted their own CONNECT_REQ ~430 times at a frozen send_seq. The
 * frames were otherwise BYTE-IDENTICAL to the peer's own CONNECT_REQ (only
 * MACs and the local Con.ID differed). The one earlier build a real VAX
 * ever admitted (ovmx-760-MEMBER-achieved) was admitted on a VIRGIN pod
 * where the advertised incarnation happened to BE 1 -- the constant was
 * right by luck, not by rule.
 *
 * So abs 36 is CALLER-SUPPLIED on every entry below, read from the
 * circuit's own echo (pe_vc.echo_incarnation, learned from a real directed
 * HELLO), and a 0 is REFUSED rather than written: zero appears in 0 of
 * 239,981 real frames, so a port with no incarnation to echo has nothing
 * honest to put there and must not send at all (INV-6).
 * ------------------------------------------------------------------ */
#define VMS_OFF_SCS_INCARNATION  36u  /* sec 4(i).B echo, never zero       */
#define VMS_OFF_SCS_LAN_OVRHD    38u  /* SYSGEN NISCS_LAN_OVRHD            */
#define VMS_OFF_SCS_ACK_MIRROR1  40u  /* == recv_ack (abs 32)              */
#define VMS_OFF_SCS_SPAN_ZERO1   42u
#define VMS_OFF_SCS_SEQ_MIRROR   VMS_OFF_START_SEQ_MIRROR /* abs 44        */
#define VMS_OFF_SCS_SPAN_ZERO2   46u
#define VMS_OFF_SCS_ACK_MIRROR2  48u  /* == recv_ack, 3rd repeat           */
#define VMS_OFF_SCS_SPAN_ZERO3   50u
#define VMS_OFF_SCS_SPAN_CONST1  52u
#define VMS_OFF_SCS_SPAN_CONST2  54u
#define VMS_OFF_SCS_SPAN_END     56u  /* first byte past the span          */

#define VMS_SCS_SEQ_SPAN_CONST1  0x0001u
#define VMS_SCS_SEQ_SPAN_CONST2  0x0200u

/* The smallest frame that can carry the whole span (abs 36..55). Every
 * grounded sequenced class (SCA content 58/62/66/86/94/110/190 -> wire
 * 72..204) clears it by a wide margin; the check exists so a truncated frame
 * is REFUSED rather than stamped past its end. */
#define VMS_SCS_SEQ_STAMP_MIN_LEN VMS_OFF_SCS_SPAN_END

/*
 * vms_scs_seq_stamp - write the TRANSPORT's counter span into a sequenced
 * frame some other layer built: recv_ack at abs 32, send_seq at abs 34, and
 * the whole abs 36..55 span documented above (spec sec 4(d)/4(h)(4)).
 *
 * WHY THIS EXISTS. Sequencing is a property of the VIRTUAL CIRCUIT, not of
 * the SYSAP message riding it (*VAXcluster Principles* pp. 2-30/2-31: the
 * ordered unit is the port pair, and one send_seq/recv_seq pair spans every
 * connection multiplexed on the circuit -- spec sec 4(O.14)). So the layer
 * that knows the Con.ID pair and the SYSAP body builds the frame, and the
 * port stamps the sequence position into it at the moment it goes out -- and
 * stamps it AGAIN, with the same send_seq and a FRESH recv_ack, on every
 * retransmission. Doing it any other way is how a send_seq hole appears --
 * or, as E63 measured, how every ack mirror on the wire freezes at zero.
 *
 * `incarnation` is the circuit's sec 4(i).B echo for abs 36 -- the SAME value
 * this circuit's 0x41 START/STACK/ACK carry, read from the circuit's own state
 * (pe_vc.echo_incarnation), never a constant. A zero is REFUSED
 * (VMS_CODEC_E_INVAL): it is unobserved in 239,981 reference frames, so a
 * caller with no echo has nothing honest to stamp and must not send (INV-6).
 *
 * Class-gated on VMS_FCAP_SEQ and on a msgtype in {0x4b, 0x5b, 0x7b}: a 0x41
 * START has its own builder (which writes the same abs 36 through its own
 * `incarnation` field) and a 0x48 credit-return carries send_seq == 0 by
 * GROUNDED rule (622/622) and must never be stamped with one. Returns
 * VMS_CODEC_E_CLASS for either, VMS_CODEC_E_SHORT for a frame that cannot
 * hold abs 36..55.
 */
vms_codec_status_t vms_scs_seq_stamp(uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     uint16_t recv_ack, uint16_t send_seq,
				     uint16_t incarnation);

/*
 * vms_scs_seq_mark_retransmit - re-mark an already-built sequenced frame as
 * the RETRANSMIT form: msgtype 0x4b/0x5b -> 0x7b at abs 30.
 *
 * GROUNDED as the wire's own retransmit marking: spec sec 4(h) heads the
 * directory class "(opcode 0x5b, 0x7b is its retransmit)", sec 4(h)(1b)
 * observes a real VAX dialogue where "one carries an explicit 0x7b retransmit
 * marker", and sec 4(O.19) measures a real coordinator retransmitting its
 * op-8 as "mt16 0x4b->0x7b". The frame is otherwise UNCHANGED -- same
 * send_seq, same body -- which is the whole point: a retransmit is not a new
 * message.
 *
 * Idempotent (0x7b stays 0x7b). Class-gated exactly as vms_scs_seq_stamp.
 */
vms_codec_status_t vms_scs_seq_mark_retransmit(uint8_t *frame, uint32_t len,
					       const struct vms_frame_info *fi);

/* ------------------------------------------------------------------ *
 * Credit-return short -- spec sec 4(h)(3), opcode 0x48, 41-byte content
 * ------------------------------------------------------------------ */

#define VMS_SCS_CREDIT_SCA_LEN   41u
/* 14 + 41 = 55 bytes of real content, but every real 0x48 short is padded
 * to the Ethernet minimum on the wire (spec sec 2's runt-pad rule,
 * VMS_ETH_MIN_FRAME; VMS_SCA_LEN_RUNT_PAD is exactly this case) -- the
 * builder reproduces that padding, so this is 60, not 55. */
#define VMS_SCS_CREDIT_FRAME_LEN VMS_ETH_MIN_FRAME

/*
 * struct vms_scs_credit_frame - the 0x48 credit-return short. acked_seq is
 * written at abs 32 and its two GROUNDED repeats (abs 40, 622/622; abs 48,
 * 616/622); secondary_seq at abs 44 is the spec's own INFERRED field ("not
 * cleanly a single function of [18:20]") -- callers fill it from their own
 * outstanding send_seq, reproducing the real wire's shape rather than
 * asserting a derivation the spec does not give.
 *
 * `incarnation` at abs 36 is the SAME sec 4(i).B echo the circuit's sequenced
 * frames and its 0x41 handshake carry -- a credit-return is not exempt
 * (af2-firsttimer: the joiner's credit-returns walk 1/2/3 with everything
 * else it sends). vms_scs_credit_build REFUSES a zero, for the reason
 * vms_scs_seq_stamp does.
 */
struct vms_scs_credit_frame {
	struct vms_scs_addr addr;
	uint16_t acked_seq;      /* abs 32/40/48: the peer's send_seq being  */
				  /* acknowledged                              */
	uint16_t secondary_seq;  /* abs 44: INFERRED, OWN outstanding seq    */
	uint16_t incarnation;    /* abs 36: sec 4(i).B echo, never zero      */
};

vms_codec_status_t vms_scs_credit_build(const struct vms_scs_credit_frame *c,
					uint8_t *frame, uint32_t cap,
					uint32_t *written);

vms_codec_status_t vms_scs_credit_parse(const uint8_t *frame, uint32_t len,
					const struct vms_frame_info *fi,
					struct vms_scs_credit_frame *out);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_VC_H */
