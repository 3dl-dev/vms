/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_cm.h - the connection-manager (CM / VMS$VAXcluster
 * SYSAP) typed codec entries (plan item FC-P3.1; design
 * docs/design-faithful-cluster-executive.md sec 3.2.1, 3.6, P3; spec
 * docs/cluster-protocol-spec.md sec 4(j)/(o)/(p)/(q)/(r)/(y)/(aa)).
 *
 * SCOPE. The 190-byte VMS$VAXcluster SYSAP body (spec sec 4(d)/4(j)): the
 * 10-byte transaction envelope (send-msg#, ack-msg#, txn, correlation
 * token, category, opcode), the transition-open fields (epoch, role slot,
 * transition class, membership bitmap), the cat-0x01 op-0x01 VOTES/
 * node-parameter fields, the model-advertisement string, and the GROUNDED
 * response recipes: the cat-0x01 0x81 echo + its per-opcode mutations
 * (spec sec 4(p)/(r)), the cat-0x06 close with OVMX's own node-parameter
 * block (sec 4(p)), the cat-0x02 op-0x0d DLM-rebuild echo (sec 4(p)), and
 * a cat-0x04 credit/commit ack builder (sec 4(u)).
 *
 * OUT OF SCOPE, on purpose. The generic SCS sequenced-message envelope
 * that WRAPS this body (abs 0-71: Ethernet, SCA header, the recv_ack/
 * send_seq/Con.ID machinery) belongs to FC-P1.1 (VC codec) and FC-P2.1
 * (SCS codec), neither of which has landed yet, and to the port
 * (vms_pe.h)/SCS (vms_scs.h) glue that fills it at send time. This file
 * used to ship a MINIMAL stand-in for that span (`struct vms_cm_link`) and
 * every recipe below built a full frame through it; FC-P3.15's body-level
 * conformance retrofit (design sec 3.2.4 ruling E1) demoted that stand-in
 * to a test-only composer -- see sec 3 below.
 *
 * THE HONESTY RULE (INV-6 + honest-os-identity-broadcast, same discipline
 * as vms_cluster_codec_hello.h). Every field this file can only OBSERVE
 * (the checksum/token at body[6:8], whose derivation is UNKNOWN per spec
 * sec 4(j)) is threaded through as caller/request-supplied data, never
 * synthesised. The response recipes are ECHO-BASED per the spec's own
 * grounded text -- they copy real received bytes, they do not invent new
 * ones. No baked capture template exists anywhere in this file (contrast
 * the strawman src/vmsscs/scs_member.c's `member_config_tmpl`, which this
 * file's own doc comments cite for FIELD PLACEMENT ONLY, never for code).
 *
 * THE ALLOWLIST (sec 6 below) is the GROUNDED (SYSAP, category, opcode)
 * DATA this item owes vms_cluster_codec.h's mechanism (FC-P0.6): every
 * RESPOND row cites the spec paragraph that grounds it, and pairs this
 * project has explicitly measured as ungrounded (cat 0x02 op 0x01/0x12,
 * the DLM lock-request/grant traffic -- see src/vmsscs/scsd.c cm_response_
 * shape's own comment) are simply ABSENT, which is behaviourally identical
 * to a CONSUME/NONE row: silence, logged by the caller.
 *
 * BODY-LEVEL SINCE FC-P3.15 (design sec 3.2.4 ruling E1). Every builder in
 * sec 5/5b below reads and writes the 132-byte SYSAP BODY ONLY (body[0] ==
 * abs 72); none of them sees or touches abs [0,72) -- the Ethernet/SCA/VC
 * span is the port's (vms_pe.h) and the SCS header is SCS's (vms_scs.h).
 * `struct vms_cm_link` and its builder, which used to lay a stand-in for
 * that span under every frame this file built, have been DEMOTED to a
 * test-only full-frame composer (`vms_frame_compose`, tests/cluster/host/) --
 * production code never calls it. body[0:8] (send/ack/txn/token) is written
 * by exactly one function, `cnxman_envelope_stamp()` (vms_cnxman_csb.h),
 * from the destination CSB's real dialogue counters; no builder here takes
 * an envelope struct or writes those eight bytes -- see each builder's own
 * doc comment for which half (the codec's echo, or the stamper) owns
 * body[4:8] on a given recipe.
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_CM_H
#define OVMX_VMS_CLUSTER_CODEC_CM_H

#include "vms_cluster_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * sec 1  Body-relative field widths and absolute offsets
 * ------------------------------------------------------------------ *
 * body[0] = SCA payload offset 58 = VMS_OFF_SYSAP_BODY (abs 72). Every
 * VMS_OFF_CM_* constant below is an ABSOLUTE frame offset, matching the
 * parent codec's convention (vms_cluster_codec.h "OFFSET CONVENTION").
 * Several offsets are deliberately reused across structs because the
 * WIRE reuses the byte range for a different meaning per opcode (spec
 * sec 4(r): body[16] is a role slot on the transition-open family but a
 * plain LE u32 barrier-step index on op 0x0b/0x0c, and the SAME byte is
 * the op-0x14 model-string length prefix under a different category).
 * Each struct below documents which opcode(s) it is GROUNDED for; there
 * is no single "the CM body" struct because the wire itself has none.
 */
#define VMS_CM_BODY_LEN     132u  /* body[0..131] = abs [72..203]         */
#define VMS_CM_FRAME_LEN    204u  /* VMS_ETH_HDR_LEN + 190 (spec sec 4(d)) */
#define VMS_CM_SCA_CONTENT  190u

/* sec 4(d): "a constant 0x0012 (=18 decimal) sits at offset 38-39...
 * byte-exact match to SYSGEN NISCS_LAN_OVRHD 18". Part of the abs [32,72)
 * sequence-number region the PORT lays down (design sec 3.2.4) -- the
 * test-only composer (tests/cluster/host/vms_frame_compose.h) is the only
 * place left in this tree that still writes it, for a specimen. */
#define VMS_OFF_CM_LINK_OVRHD     38u
#define VMS_CM_LINK_OVRHD_VAL     18u

/* The 10-byte SYSAP transaction envelope every category shares (sec 4(j)). */
#define VMS_OFF_CM_SEND_MSG   (VMS_OFF_SYSAP_BODY + 0)   /* abs 72, LE u16 */
#define VMS_OFF_CM_ACK_MSG    (VMS_OFF_SYSAP_BODY + 2)   /* abs 74, LE u16 */
#define VMS_OFF_CM_TXN        (VMS_OFF_SYSAP_BODY + 4)   /* abs 76, LE u16 */
#define VMS_OFF_CM_TOKEN      (VMS_OFF_SYSAP_BODY + 6)   /* abs 78, LE u16 */
#define VMS_OFF_CM_CATEGORY   (VMS_OFF_SYSAP_BODY + 8)   /* abs 80         */
#define VMS_OFF_CM_OPCODE     (VMS_OFF_SYSAP_BODY + 9)   /* abs 81         */

/* Transition-open / barrier family (op 0x08/0x09/0x0a/0x0b/0x0c/0x0d;
 * sec 4(p)/(r)). */
#define VMS_OFF_CM_EPOCH      (VMS_OFF_SYSAP_BODY + 12)  /* abs 84, LE u32 */
#define VMS_OFF_CM_ROLE       (VMS_OFF_SYSAP_BODY + 16)  /* abs 88         */
#define VMS_OFF_CM_CLASS      (VMS_OFF_SYSAP_BODY + 17)  /* abs 89         */
#define VMS_OFF_CM_STEP       (VMS_OFF_SYSAP_BODY + 16)  /* abs 88, LE u32 --
							    * op 0x0b/0x0c ONLY,
							    * ALIASES role+class */
#define VMS_OFF_CM_BITMAP     (VMS_OFF_SYSAP_BODY + 55)  /* abs 127, op 0x09
							    * open ONLY; width
							    * beyond one byte is
							    * UNDETERMINED (spec
							    * sec 4(p)) */

/*
 * The bitmap NEIGHBOURHOOD, body[52:60] -- the span whose emptiness is the
 * ONLY published evidence about how wide the membership bitmap really is.
 *
 * Spec sec 4(p): "body[52:55] and body[56:60] are all-zero in every specimen,
 * so the field is certainly WIDER than a byte, but its extent and endianness
 * are UNDETERMINED -- a BE u32 at body[52:56] fits the data as well as an LE
 * map based at body[55]. Do not assume 8 slots." One byte holds 8 slots while
 * the capture library already reaches CSV slot 5, so an implementation that
 * treats body[55] as the whole field is one member away from silently losing a
 * participant -- which is why FC-P3.5's barrier FSM reads this span on every
 * op-0x09 open and INSTRUMENTS a nonzero byte outside body[55] rather than
 * decoding it. A residual here is the observation that settles the width; it
 * is never a licence to guess an encoding.
 */
#define VMS_OFF_CM_BITMAP_SPAN  (VMS_OFF_SYSAP_BODY + 52) /* abs 124..131      */
#define VMS_CM_BITMAP_SPAN_LEN  8u   /* body[52:60]                            */
#define VMS_CM_BITMAP_SPAN_IDX  3u   /* index of body[55] WITHIN the span      */
#define VMS_OFF_CM_RESP_MARK  (VMS_OFF_SYSAP_BODY + 18)  /* abs 90, 0x81
							    * response marker  */
#define VMS_OFF_CM_RELAY_EPOCH (VMS_OFF_SYSAP_BODY + 20) /* abs 92, LE u32 --
							    * op 0x12 RESPONSE
							    * only: copy of the
							    * request's epoch  */

/* cat-0x01 op-0x01 cluster-parameters (VOTES) + the node-parameter block
 * (sec 4(j) VOTES table; sec 4(p) cat-0x06 close reuses the same block). */
#define VMS_OFF_CM_VOTES      (VMS_OFF_SYSAP_BODY + 22)  /* abs 94, LE u16 */
#define VMS_OFF_CM_PARAM_F1   (VMS_OFF_SYSAP_BODY + 72)  /* abs 144, LE u32,
							    * observed const 0x10*/
#define VMS_OFF_CM_PARAM_F2   (VMS_OFF_SYSAP_BODY + 76)  /* abs 148, LE u32,
							    * observed const 0x01*/
#define VMS_OFF_CM_VERSION    (VMS_OFF_SYSAP_BODY + 88)  /* abs 160, 8 ASCII
							    * e.g. "V7.3    " */
#define VMS_CM_VERSION_LEN 8u

/* cat-0x01 op-0x14 node CPU/model advertisement (sec 4(j)). */
#define VMS_OFF_CM_MODEL_LEN  (VMS_OFF_SYSAP_BODY + 16)  /* abs 88, ALIASES
							    * role/step        */
#define VMS_OFF_CM_MODEL_NAME (VMS_OFF_SYSAP_BODY + 17)  /* abs 89..        */
#define VMS_CM_MODEL_MAX 114u /* body[17..131) inclusive of the len prefix at
			       * body[16]: 132 - 17 - 1 = 114 bytes max     */

/* cat-0x02 op-0x0d DLM lock-resource rebuild record (sec 4(p) "Request
 * layout (GROUNDED)"). NOTE these offsets are NOT the cat-0x01 offsets
 * body[16]/body[18]/body[55] -- applying the cat-0x01 mutations here
 * corrupted the resource name and bugchecked two real VAXes
 * (LOCKMGRERR); that is why this family has its own accessor names. */
#define VMS_OFF_CM_DLM_L1_TAG   (VMS_OFF_SYSAP_BODY + 12) /* abs 84, LE u16,
							     * invariant 0x0001 */
#define VMS_OFF_CM_DLM_L1_TAG2  (VMS_OFF_SYSAP_BODY + 14) /* abs 86, LE u16,
							     * invariant 0x0003 */
#define VMS_OFF_CM_DLM_L1_LEN   (VMS_OFF_SYSAP_BODY + 16) /* abs 88, L1 length*/
#define VMS_OFF_CM_DLM_RESULT   (VMS_OFF_SYSAP_BODY + 34) /* abs 106, result
							     * stamp; 0xf9 on
							     * an op-0x0d
							     * RESPONSE,
							     * MANDATORY       */
#define VMS_OFF_CM_DLM_RESNAMLEN (VMS_OFF_SYSAP_BODY + 47) /* abs 119        */
#define VMS_OFF_CM_DLM_RESNAME   (VMS_OFF_SYSAP_BODY + 48) /* abs 120..      */
#define VMS_CM_DLM_RESNAME_MAX 84u /* body[48..131] = 84 bytes             */
#define VMS_CM_DLM_RESULT_OP0D 0xf9u /* the sec 4(p) MANDATORY stamp        */

/*
 * BODY-RELATIVE offsets (body[0] == abs 72), derived arithmetically from the
 * frame-absolute constants above so the two addressing schemes can never
 * drift apart. The PARSE accessors (sec 4) read a RECEIVED FRAME and keep
 * using the VMS_OFF_CM_* constants above; every ORIGINATING/RESPONSE builder
 * (sec 5/5b) writes only a 132-byte body buffer and uses these VMS_OFB_CM_*
 * ones (design sec 3.2.4 ruling E1 -- FC-P3.15's body-level conformance
 * retrofit).
 */
#define VMS_OFB_CM_CATEGORY    (VMS_OFF_CM_CATEGORY    - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_OPCODE      (VMS_OFF_CM_OPCODE      - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_EPOCH       (VMS_OFF_CM_EPOCH       - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_ROLE        (VMS_OFF_CM_ROLE        - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_CLASS       (VMS_OFF_CM_CLASS       - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_STEP        (VMS_OFF_CM_STEP        - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_BITMAP      (VMS_OFF_CM_BITMAP      - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_RESP_MARK   (VMS_OFF_CM_RESP_MARK   - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_RELAY_EPOCH (VMS_OFF_CM_RELAY_EPOCH - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_PARAM_F1    (VMS_OFF_CM_PARAM_F1    - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_PARAM_F2    (VMS_OFF_CM_PARAM_F2    - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_VERSION     (VMS_OFF_CM_VERSION     - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_TXN         (VMS_OFF_CM_TXN         - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_TOKEN       (VMS_OFF_CM_TOKEN       - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_DLM_RESULT  (VMS_OFF_CM_DLM_RESULT  - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_DLM_L1_LEN  (VMS_OFF_CM_DLM_L1_LEN  - VMS_OFF_SYSAP_BODY)
/* FC-P3.3's three joiner originations (sec 5c) need the body-relative form of
 * the VOTES word and the model-string pair as well. */
#define VMS_OFB_CM_VOTES       (VMS_OFF_CM_VOTES       - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_MODEL_LEN   (VMS_OFF_CM_MODEL_LEN   - VMS_OFF_SYSAP_BODY)
#define VMS_OFB_CM_MODEL_NAME  (VMS_OFF_CM_MODEL_NAME  - VMS_OFF_SYSAP_BODY)

/* The category/opcode values this file's recipes and accessors key on
 * (sec 4(j) table + sec 4(p)/(r); NOT the general SCS byte at abs 30,
 * which vms_cluster_codec.h already owns as VMS_OFF_SCS_MSGTYPE). */
#define VMS_CM_CAT_CONFIG      0x01u /* membership/config dialogue          */
#define VMS_CM_CAT_DLM         0x02u /* distributed lock manager traffic    */
#define VMS_CM_CAT_ACK         0x04u /* credit/commit acknowledgement       */
#define VMS_CM_CAT_MEMBERSHIP  0x06u /* the transaction-close/poll category */

#define VMS_CM_OP_PARAMS       0x01u /* cat 0x01: cluster parameters (VOTES)*/
#define VMS_CM_OP_CONFIG       0x02u /* cat 0x01: config/topology           */
#define VMS_CM_OP_COMMIT       0x03u /* cat 0x01: membership-commit txn     */
#define VMS_CM_OP_ABORT        0x04u /* cat 0x01: transition abort, role 0x50*/
#define VMS_CM_OP_LOCKRB       0x05u /* cat 0x01: lock/resource rebuild txn */
#define VMS_CM_OP_MEMBERSHIP   0x06u /* cat 0x01: post-commit MEMBERSHIP burst*/
#define VMS_CM_OP_XITION_REM   0x08u /* cat 0x01: class-0x03 transition open*/
#define VMS_CM_OP_XITION_ADD   0x09u /* cat 0x01: class-0x02 transition open*/
#define VMS_CM_OP_XITION_GO    0x0au /* cat 0x01: barrier GO, never answered*/
#define VMS_CM_OP_BARRIER      0x0bu /* cat 0x01: joiner-initiated step     */
#define VMS_CM_OP_BARRIER_REL  0x0cu /* cat 0x01: coordinator release       */
#define VMS_CM_OP_DEPART_XITION 0x0du/* cat 0x01: class-0x04 self-departure
				       * open. NOTE: cat 0x02 op 0x0d is a
				       * DIFFERENT message, the DLM rebuild
				       * record -- see VMS_CM_OP_DLM_REBUILD */
#define VMS_CM_OP_0F           0x0fu /* cat 0x01: class-0x03 extra step     */
#define VMS_CM_OP_RELAY        0x12u /* cat 0x01: coordinator's relay       */
#define VMS_CM_OP_MODEL        0x14u /* cat 0x01: node CPU/model advert     */
#define VMS_CM_OP_DLM_REBUILD  0x0du /* cat 0x02: lock-resource rebuild rec */
#define VMS_CM_OP_CLOSE        0x00u /* cat 0x06: close / recurring poll    */

#define VMS_CM_ROLE_RELAY   0x10u /* body[16] on op 0x12 (and coord's 0x81/0x0b)*/
#define VMS_CM_ROLE_COMMIT  0x20u /* body[16] on op 0x03/0x05/0x06              */
#define VMS_CM_ROLE_0F      0x30u /* body[16] on op 0x0f                        */
#define VMS_CM_ROLE_XITION  0x40u /* body[16] on op 0x08/0x09/0x0d              */
#define VMS_CM_ROLE_GO      0x60u /* body[16] on op 0x0a                        */
#define VMS_CM_ROLE_ABORT   0x50u /* body[16] on the cat-0x01 op-0x04 abort     */

#define VMS_CM_CLASS_ADD     0x02u /* body[17]: ADD a member (has the barrier) */
#define VMS_CM_CLASS_REMOVE  0x03u /* body[17]: REMOVE a failed member         */
#define VMS_CM_CLASS_DEPART  0x04u /* body[17]: self-departure (NO barrier)    */

/* ------------------------------------------------------------------ *
 * sec 2  The SYSAP transaction envelope (sec 4(j), every category)
 * ------------------------------------------------------------------ */
struct vms_cm_envelope {
	uint16_t send_msg; /* body[0:2], GROUNDED strictly monotonic per sender */
	uint16_t ack_msg;  /* body[2:4], GROUNDED: acks peer's highest send_msg */
	uint16_t txn;      /* body[4:6], per-dialogue id; opaque, echoed only   */
	uint16_t token;    /* body[6:8], correlation "checksum"; derivation is
			    * UNKNOWN (sec 4(j)) -- never computed, only echoed*/
	uint8_t  category; /* body[8], REQUEST form (bit 0x80 clear)            */
	uint8_t  opcode;   /* body[9]                                          */
};

vms_codec_status_t vms_cm_envelope_parse(const uint8_t *frame, uint32_t len,
					 const struct vms_frame_info *fi,
					 struct vms_cm_envelope *out);

/* ------------------------------------------------------------------ *
 * sec 3  The abs [0,72) span -- NOT this file's business since FC-P3.15
 *
 * `struct vms_cm_link` and its builder used to stand in here for the
 * not-yet-landed port/SCS layers, and every recipe in sec 5/5b built a full
 * 204-byte frame through it. Design sec 3.2.4 ruled that stand-in out of
 * production code: "a SYSAP that fills send_seq is the same category error
 * as a daemon that fills a lock id." Both have been DEMOTED to
 * tests/cluster/host/vms_frame_compose.h as `vms_frame_compose()`, a
 * test-only full-frame composer used ONLY to assemble a byte-exact 204-byte
 * specimen for a fixture comparison or the rung-2 simulator's pcap replay.
 * No production TU includes that header (tools/ci/cluster_core_includes_
 * gate.sh enforces kernel-core-only includes here); the abs [0,72) span is
 * the port's (vms_pe.h) and SCS's (vms_scs.h) alone, per the byte-ownership
 * table in the design's sec 3.2.4.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * sec 4  Opcode-specific body structs: parse only (a later FSM item
 * owns ORIGINATING these; this item grounds reading them and the
 * RESPONSE recipes in sec 5).
 * ------------------------------------------------------------------ */

/* Transition-open family: op 0x08 (class-0x03 REMOVE), op 0x09
 * (class-0x02 ADD, carries the bitmap), op 0x0d-in-cat-0x01 (class-0x04
 * self-departure). */
struct vms_cm_open {
	struct vms_cm_envelope env;
	uint32_t epoch;  /* body[12:16] LE u32, GROUNDED sec 4(j)/(p)         */
	uint8_t  role;   /* body[16], GROUNDED sec 4(r)                       */
	uint8_t  cls;    /* body[17], GROUNDED sec 4(r): the TRANSITION CLASS */
	uint8_t  bitmap; /* body[55], op 0x09 ONLY; GROUNDED presence + the
			  * popcount==member-count fact (sec 4(p)), but the
			  * field's full WIDTH is undetermined beyond this
			  * one byte -- do not assume 8 slots is the ceiling */
	int      has_bitmap; /* 1 iff opcode == VMS_CM_OP_XITION_ADD           */
};

vms_codec_status_t vms_cm_open_parse(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     struct vms_cm_open *out);

/*
 * vms_cm_open_bitmap_span - copy body[52:60] out of an op-0x09 transition
 * open, for the width instrumentation described at VMS_OFF_CM_BITMAP_SPAN.
 *
 * Deliberately returns BYTES, not a decoded value: the field's extent and
 * endianness are UNDETERMINED (spec sec 4(p)) and a decoder here would be a
 * guess with a cluster-breaking failure mode. The caller's job is to observe
 * that every byte but index VMS_CM_BITMAP_SPAN_IDX is zero -- as it is in
 * 54 of 54 library opens -- and to COUNT it when one is not.
 *
 * VMS_CODEC_E_CLASS unless this really is a cat-0x01 op-0x09 open: op 0x08
 * (class-0x03 REMOVE) and cat-0x01 op 0x0d (class-0x04 departure) carry no
 * bitmap at all, and reading this span from them would report residue as
 * membership.
 */
vms_codec_status_t vms_cm_open_bitmap_span(const uint8_t *frame, uint32_t len,
					   const struct vms_frame_info *fi,
					   uint8_t *out /* [VMS_CM_BITMAP_SPAN_LEN] */);

/* Barrier step (op 0x0b joiner-initiated) / release (op 0x0c, coordinator,
 * never answered) -- body[16:20] is a plain LE u32 step index here, NOT
 * the role+class byte pair (sec 4(p): "step N (LE u32) at body[16:20]"). */
struct vms_cm_barrier {
	struct vms_cm_envelope env;
	uint32_t epoch; /* body[12:16] LE u32, copied from the coordinator's
			 * transition-open (sec 4(p))                        */
	uint32_t step;  /* body[16:20] LE u32, 1..12 (sec 4(p): GROUNDED
			 * across 6 joins / 4 clusters / 3 joiner nodes)     */
};

vms_codec_status_t vms_cm_barrier_parse(const uint8_t *frame, uint32_t len,
					const struct vms_frame_info *fi,
					struct vms_cm_barrier *out);

/* cat-0x01 op-0x01 cluster-parameters: VOTES + the node-parameter block. */
struct vms_cm_params {
	struct vms_cm_envelope env;
	uint16_t votes;   /* body[22:24] LE u16, GROUNDED across four vote
			   * configurations (sec 4(j)); 0 == non-voting     */
	uint32_t param_f1; /* body[72:76], observed constant 0x10             */
	uint32_t param_f2; /* body[76:80], observed constant 0x01             */
	uint8_t  version[VMS_CM_VERSION_LEN]; /* body[88:96], 8-byte space-padded ASCII version field (e.g. V7.3) */
};

vms_codec_status_t vms_cm_params_parse(const uint8_t *frame, uint32_t len,
				       const struct vms_frame_info *fi,
				       struct vms_cm_params *out);

/* cat-0x01 op-0x14 node CPU/model advertisement. */
struct vms_cm_model {
	struct vms_cm_envelope env;
	uint8_t namelen;                 /* body[16], <= VMS_CM_MODEL_MAX      */
	uint8_t name[VMS_CM_MODEL_MAX];  /* body[17..], ASCII                  */
};

vms_codec_status_t vms_cm_model_parse(const uint8_t *frame, uint32_t len,
				      const struct vms_frame_info *fi,
				      struct vms_cm_model *out);

/* cat-0x02 op-0x0d DLM lock-resource rebuild record (sec 4(p)). */
struct vms_cm_dlm_rebuild {
	struct vms_cm_envelope env;
	uint8_t l1_len;                       /* body[16]                     */
	uint8_t resnamelen;                   /* body[47]                     */
	uint8_t resname[VMS_CM_DLM_RESNAME_MAX]; /* body[48..48+resnamelen)   */
};

vms_codec_status_t vms_cm_dlm_rebuild_parse(const uint8_t *frame, uint32_t len,
					    const struct vms_frame_info *fi,
					    struct vms_cm_dlm_rebuild *out);

/* ------------------------------------------------------------------ *
 * sec 5  Response recipes (spec sec 4(p)/(q)/(r)/(u)) -- the GROUNDED
 * deliverable of this item. Every builder takes the RECEIVED request FRAME
 * (already classified VMS_FCLS_SCS_MSG -- receive stays frame-based, design
 * sec 3.2.4: "the port delivers the whole frame... no copy, no strip") and
 * returns a VMS_CM_BODY_LEN (132-byte) BODY, body[0] == abs 72. `out_body`
 * is NOT a frame: no builder here sees or writes abs [0,72).
 *
 * body[0:8] (send/ack/txn/token) is deliberately NOT built here. Every
 * caller stamps it separately with `cnxman_envelope_stamp()`
 * (vms_cnxman_csb.h) after this builder returns -- see each builder's own
 * "STAMP" note for is_response's correct value on that recipe. A RESPONSE
 * recipe that echoes the whole request body (the echo family, the DLM
 * op-0x0d echo, the step-ack) or explicitly copies the request's txn/token
 * (the close recipe) has ALREADY put the correct echoed value at body[4:8]
 * before the stamper runs -- is_response=1 there means "leave those two
 * bytes alone", not "do nothing".
 * ------------------------------------------------------------------ */

/*
 * vms_cm_echo_response_build - the cat-0x01 0x81 echo recipe (sec 4(p)
 * "The 0x81 echo takes THREE mutations" + sec 4(r) "Response recipes by
 * opcode"). Handles every GROUNDED cat-0x01 opcode with ONE recipe,
 * because the wire itself is one recipe with per-opcode exceptions:
 *
 *   body[8]  |= 0x80                              always
 *   body[18]  = 0x01                               op in {0x03,0x05,0x08,
 *                                                   0x09,0x0d,0x12}
 *   body[18]  = ECHOED, not forced                 op == 0x0f
 *   body[55]  = 0x00                               op == 0x09 ONLY
 *   body[17]  = own_class ; body[20:24] = epoch     op == 0x12 ONLY
 *   everything else                                 echoed verbatim
 *
 * `own_class` is this node's own current transition class (sec 4(r):
 * "body[17] = the responder's own current class"), caller-supplied --
 * never invented. Returns VMS_CODEC_E_CLASS if the request's (category,
 * opcode) is not one of the six GROUNDED rows in vms_cm_allow_table().
 *
 * STAMP with is_response=1: the verbatim body copy this builder does FIRST
 * already carries the request's txn/token at body[4:8].
 */
vms_codec_status_t vms_cm_echo_response_build(const uint8_t *req_frame,
					      uint32_t req_len,
					      uint8_t own_class,
					      uint8_t *out_body, uint32_t cap,
					      uint32_t *written);

/*
 * vms_cm_close_build - the cat-0x06 close recipe (sec 4(p): "closes the
 * transaction. Carry only the (txn,checksum); send your own node-
 * parameter block, the same one carried in the op 0x01 PARAMS message").
 * `own_params` supplies OVMX's own honest node-parameter fields (never a
 * captured value): echoing the REQUEST's payload here is what bugchecked
 * a real VAX with INCONSTATE (sec 4(p)).
 *
 * STAMP with is_response=1: this builder writes the request's txn/token to
 * body[4:8] itself (it builds fresh from zero, unlike the echo family, so
 * there is no verbatim copy to rely on).
 */
struct vms_cm_node_params {
	uint32_t param_f1;  /* body[72:76] */
	uint32_t param_f2;  /* body[76:80] */
	uint8_t  version[VMS_CM_VERSION_LEN]; /* body[88:96] */
};

vms_codec_status_t vms_cm_close_build(const uint8_t *req_frame, uint32_t req_len,
				      const struct vms_cm_node_params *own_params,
				      uint8_t *out_body, uint32_t cap,
				      uint32_t *written);

/*
 * vms_cm_dlm_op0d_response_build - the cat-0x02 op-0x0d DLM rebuild echo
 * (sec 4(p): "reconstructs 1367 of 1367 real responses byte-for-byte").
 * VERBATIM echo of the 132-byte body plus exactly: body[8] |= 0x80,
 * body[34] = 0xf9 (MANDATORY, unconditional). Does NOT take the cat-0x01
 * body[18]/body[55] mutations -- sec 4(p)'s explicit warning: those offsets
 * land inside the L1 region and the 8th byte of the lock RESOURCE NAME
 * here, and applying them corrupted the name and bugchecked two real VAXes
 * with LOCKMGRERR.
 *
 * STAMP with is_response=1: the verbatim copy already carries the
 * request's txn/token.
 */
vms_codec_status_t vms_cm_dlm_op0d_response_build(const uint8_t *req_frame,
						  uint32_t req_len,
						  uint8_t *out_body, uint32_t cap,
						  uint32_t *written);

/*
 * vms_cm_body_build - wrap a body the LOCK MANAGER produced from real lock
 * state (vms_dlm_scs.h RULE A -- the DLM fills a reply buffer and never
 * sends), echoing the ANSWERED REQUEST's txn/token over it.
 *
 * The DLM's reply never writes body[0:8] (design sec 3.2.4 ruling E1: "the
 * DLM cat-02 arm... never writes body[0:8]"), so this wrapper takes the
 * request frame too, purely to read the two fields a correlated response
 * MUST carry: `req_env.txn`/`req_env.token`, written to `out_body[4:8]`.
 * `body_len` must be exactly VMS_CM_BODY_LEN -- a short body would leave
 * the tail as whatever the caller's buffer held. Asserts nothing else about
 * the DLM's content: a wrapper, never a recipe.
 *
 * STAMP with is_response=1: this builder has just written the echoed
 * txn/token itself, identically to vms_cm_close_build.
 */
vms_codec_status_t vms_cm_body_build(const uint8_t *req_frame, uint32_t req_len,
				     const uint8_t *body, uint32_t body_len,
				     uint8_t *out_body, uint32_t cap,
				     uint32_t *written);

/*
 * vms_cm_barrier_build - ORIGINATE one barrier step: the participant's
 * cat-0x01 op-0x0b request for step `step` of the transition at `epoch`
 * (spec sec 4(p): "epoch at body[12:16], step N (LE u32) at body[16:20]").
 *
 * This is the one CM message this codec ORIGINATES rather than answers, so
 * every asserted byte has to name where it comes from:
 *
 *   body[0:8]        NOT written here -- the caller's cnxman_envelope_stamp
 *                    call fills send/ack/txn/token from the real CSB
 *   body[8]/[9]      cat 0x01 / op 0x0b                      -- this frame IS one
 *   body[12:16]      the epoch, as the coordinator's open carried it
 *   body[16:20]      the step, from the FSM's own real step counter
 *   everything else  EXPLICIT ZERO
 *
 * THE ZERO TAIL IS GROUNDED, NOT LAZINESS. body[20:] varies per joiner in the
 * capture library (`SCS$DIRECTORY`, `SCS$DIR_LOOKUP`, `VMS$DISK_CL_DRVRV5.0`,
 * and in af2-established-rejoin an ENTIRELY ZERO body) and the coordinator's
 * behaviour is identical across all of them -- one real VMS joiner sends a
 * zero tail and is admitted. It is acceptable residue, not a gate, and spec
 * sec 4(p) is explicit: "An implementation should send zeros; do not reproduce
 * another implementation's uninitialised memory."
 *
 * THE TOKEN IS NOT THE STEP ORDINAL (design sec 3.2.4 ruling E1: body[6:8]
 * is CNXMAN's own dialogue state, never a frame-copied or step-derived
 * value). A prior implementation used the step ordinal as a placeholder,
 * collided with its own step-1 value, and the coordinator dropped the frame
 * -- the barrier then stalled and regressed.
 *
 * `step` must be >= 1 (spec sec 4(p): "indices 1...12, no gaps"); step 0 is
 * VMS_CODEC_E_INVAL. The 12-step LAW is the FSM's (CNXMAN_BARRIER_STEPS), not
 * the codec's: this builder will honestly emit step 13 if an FSM asks for it,
 * so a step-count regression shows up in the FSM's own instrumented mismatch
 * counter rather than being silently clamped here.
 *
 * STAMP with is_response=0: this is a genuine origination, and the
 * connection manager's own txn/token belong at body[4:8].
 */
vms_codec_status_t vms_cm_barrier_build(uint32_t epoch, uint32_t step,
					uint8_t *out_body, uint32_t cap,
					uint32_t *written);

/* ------------------------------------------------------------------ *
 * sec 5b  THE COORDINATOR'S ORIGINATIONS (FC-P3.12)
 *
 * Everything in sec 5 above ANSWERS somebody. A transition coordinator has to
 * SPEAK FIRST, and these five builders are the only way it may: the connection
 * manager never touches a body offset (design SS3.9 rule 2 -- two crashes came
 * from body[N] arithmetic in orchestration code).
 *
 * THE RULE EVERY ONE OF THEM OBEYS. Write ONLY fields whose PLACEMENT is
 * grounded in the spec, from values the caller read out of real executive
 * state; leave every other byte an EXPLICIT ZERO. Never a captured template,
 * never a mirror of an inbound frame, never a plausible-looking constant. The
 * precedent is vms_cm_barrier_build's "THE ZERO TAIL IS GROUNDED, NOT
 * LAZINESS" note above, and spec sec 4(p) states the rule outright: "An
 * implementation should send zeros; do not reproduce another implementation's
 * uninitialised memory."
 *
 * NONE of these five write body[0:8] either -- every caller stamps it with
 * cnxman_envelope_stamp() after the builder returns (design sec 3.2.4 ruling
 * E1). See each builder's own STAMP note for is_response.
 *
 * WHAT IS THEREFORE HONESTLY MISSING, AND SAID OUT LOUD. Davis p. 7-40 says a
 * Phase 1 proposal also carries the proposed quorum / computed expected votes /
 * quorum-disk votes / foundation timestamp / founder's SCSSYSTEMID / rebuild
 * type. NOT ONE of those has been isolated to an offset in any capture (spec
 * sec 4(j) "RE gaps left in sec 4j"), so this codec cannot place them and does
 * not pretend to: their bytes go out zero and FC-P3.12 counts the omission.
 * Filling them at a guessed offset is the failure that bugchecked two real
 * VAXes; omitting them is honest and recoverable. The offsets are a LAB item,
 * and when a capture pins one it is a field added here -- not a redesign.
 *
 * WHAT IS NOT HERE AT ALL. The op-0x05 lock/resource-rebuild burst, the op-0x06
 * MEMBERSHIP burst and the ORIGINATING form of the cat-0x02 op-0x0d rebuild
 * record have no builder, because their PAYLOAD field maps are not grounded --
 * the membership record's {SCSSYSTEMID, incarnation, CSID} triple (book p. 7-39)
 * has no isolated offset, and the op-0x0d L1 region body[16:34] is only ever
 * observed inbound. A builder that zero-filled those would assert an empty
 * membership list and an empty lock record, which is a fabrication with a
 * cluster-breaking failure mode -- not an honest omission.
 * ------------------------------------------------------------------ */

/*
 * vms_cm_xition_open_build - PHASE 1: the transition-open proposal.
 *
 * The OPCODE IS DERIVED FROM THE CLASS, never passed in, because sec 4(r)'s
 * census pairs them with zero residuals and a mismatch is not representable:
 *
 *   class 0x02 ADD     -> op 0x09, tag 0x0240, and it CARRIES the nodemap
 *   class 0x03 REMOVE  -> op 0x08, tag 0x0340, NO nodemap (sec 4(p))
 *   class 0x04 DEPART  -> op 0x0d, tag 0x0440, NO nodemap
 *
 * `bitmap` is the caller's membership nodemap byte and is written to body[55]
 * ONLY for class 0x02. It must be built from REAL CSBs (bit k = the member
 * holding CSID index k, sec 4(p), 54/54 opens with zero residuals); this
 * builder has no way to check that and does not try -- FC-P3.12 owns it.
 * Passing has_bitmap on a non-ADD class is VMS_CODEC_E_INVAL rather than a
 * silently-dropped field.
 *
 * STAMP with is_response=0: a genuine origination.
 */
vms_codec_status_t vms_cm_xition_open_build(uint8_t tr_class, uint32_t epoch,
					    uint8_t bitmap, int has_bitmap,
					    uint8_t *out_body, uint32_t cap,
					    uint32_t *written);

/*
 * vms_cm_go_build - PHASE 2: the barrier GO (op 0x0a), the point of no return.
 *
 * body[16:18] = (class << 8) | 0x60, i.e. the invariant 0x0260 / 0x0360 /
 * 0x0460 tags (sec 4(r)); epoch at body[12:16].
 *
 * STAMP with is_response=0, THEN call vms_cm_notification_zero_txn() (below):
 * a GO is a genuine origination -- send/ack AND the correlation token
 * (body[6:8]) are this node's real per-CSB state, "token never computed"
 * either way (design sec 3.2.4 ruling E1). The ONE exception is txn
 * (body[4:6]): sec 4(p) "Notifications carry txn=0 and are NEVER answered"
 * is a WIRE FACT about op 0x0a/0x0c specifically, not CSB dialogue state, so
 * it is the codec's job to force it back to zero AFTER the stamp -- never
 * the FSM computing an offset by hand (design SS3.9 rule 2).
 */
vms_codec_status_t vms_cm_go_build(uint8_t tr_class, uint32_t epoch,
				   uint8_t *out_body, uint32_t cap,
				   uint32_t *written);

/*
 * vms_cm_release_build - the coordinator's op-0x0c release of barrier step N.
 *
 * The exact mirror of vms_cm_barrier_build's op 0x0b: epoch at body[12:16],
 * step as an LE u32 at body[16:20] (which ALIASES the role/class byte pair, so
 * no role tag is written). Sec 4(p): "0x0c#12 is byte-identical to earlier
 * releases apart from its index."
 *
 * `step` must be >= 1. The 12-step LAW belongs to the FSM, not here: this
 * builder will honestly emit step 13 if asked, so a regression shows up in
 * FC-P3.12's own instrumented counter instead of being silently clamped.
 *
 * STAMP with is_response=0, then call vms_cm_notification_zero_txn() --
 * exactly the vms_cm_go_build recipe, for the identical reason.
 */
vms_codec_status_t vms_cm_release_build(uint32_t epoch, uint32_t step,
					uint8_t *out_body, uint32_t cap,
					uint32_t *written);

/*
 * vms_cm_notification_zero_txn - force body[4:6] (txn) to zero on a
 * notification origination (op 0x0a GO, op 0x0c RELEASE), AFTER
 * cnxman_envelope_stamp() has run.
 *
 * Sec 4(p): "Notifications carry txn=0 and are NEVER answered." This is the
 * ONE documented exception to an origination's stamp (design sec 3.2.4
 * ruling E1: an origination otherwise carries this node's real per-CSB
 * txn) -- and it belongs here, in the codec, because it is knowledge about
 * these two WIRE OPCODES, not about CSB dialogue state, so a CNXMAN TU
 * calls a named function for it rather than touching body[4:6] itself
 * (design SS3.9 rule 2: no raw wire offset outside a codec TU).
 *
 * body[6:8] (token) is UNTOUCHED: the spec places no constraint on it for
 * these two opcodes, and "token never computed" (ruling E1) means this
 * node's real per-CSB token is exactly as valid here as on any other
 * origination.
 */
void vms_cm_notification_zero_txn(uint8_t out_body[VMS_CM_BODY_LEN]);

/*
 * vms_cm_relay_build - the coordinator's op-0x12 RELAY to the other members.
 *
 * Sec 4(O.31), decoded from a real-VAX readmission: the relay sits BETWEEN the
 * joiner's op 0x02 and the coordinator's op 0x03, and it is the commit gate --
 * "the member does NOT commit the returner until it has relayed the join to the
 * other member and heard back" (the Rule of Total Connectivity, book p. 7-39).
 *
 * GROUNDED here: category 0x01 / opcode 0x12, the role slot 0x10 at body[16]
 * (sec 4(r)'s census), the transition class at body[17] and the epoch at
 * body[12:16] -- the last two because the RESPONSE recipe overwrites body[17]
 * with the responder's own class and copies body[12:16] to body[20:24], which
 * it could not do unless the request carried them there.
 *
 * NOT GROUNDED, and therefore ZERO: which system is being relayed. No capture
 * isolates a subject identity in this body. FC-P3.12 counts every relay it
 * sends with the subject omitted, so the gap is visible in the diagnostics
 * rather than papered over with a guessed offset.
 *
 * STAMP with is_response=0: a genuine origination, real txn per-dialogue.
 */
vms_codec_status_t vms_cm_relay_build(uint8_t tr_class, uint32_t epoch,
				      uint8_t *out_body, uint32_t cap,
				      uint32_t *written);

/*
 * vms_cm_commit_build - the coordinator's op-0x03 membership COMMIT request.
 *
 * Sec 4(o) step 6: "M->J cat 0x01 op 0x03 membership COMMIT request
 * (txn, cksum)" -- the message IS its transaction envelope; role slot 0x20 at
 * body[16] (sec 4(r)), class at body[17], epoch at body[12:16]. Everything
 * else zero. The subject is the peer this frame is addressed to, so unlike the
 * relay there is no missing identity here.
 *
 * STAMP with is_response=0: a genuine origination.
 */
vms_codec_status_t vms_cm_commit_build(uint8_t tr_class, uint32_t epoch,
				       uint8_t *out_body, uint32_t cap,
				       uint32_t *written);

/*
 * vms_cm_step_ack_build - the coordinator's 0x81/0x0b acknowledgement of one
 * member's barrier step.
 *
 * Sec 4(p)'s barrier table: "M->J 0x81 0x0b -- the coordinator's ack, NOT the
 * release". A participant that never got it would keep retransmitting its step;
 * a participant that mistook it for the release would run ahead of a barrier
 * whose entire purpose is that nobody advances until everybody has reported.
 *
 * THE RECIPE, and the one place two grounded readings disagree. Verbatim body
 * echo + body[8] |= 0x80 + body[16] = 0x10. That last byte is sec 4(r)'s role
 * census speaking directly: role 0x10 covers "op 0x12 AND THE COORDINATOR'S
 * 0x81/0x0b", measured over 26 captures "with zero residuals". On the REQUEST
 * body[16:20] is the LE u32 step index 1..12, so the response is overwriting it
 * with the role slot -- the two readings of that offset cannot both hold, and
 * this builder follows the MEASUREMENT rather than the inference. It is safe to
 * do so: the ack is correlated by its transaction and token, and neither a real
 * member (sec 4(p): it is "not the release") nor OVMX's own participant FSM
 * reads a step index out of it. Sec 4(r) does NOT list op 0x0b in its response-
 * recipe table, so body[18] is left ECHOED, not forced -- the same treatment
 * op 0x0f gets there.
 *
 * STAMP with is_response=1: the verbatim body copy already carries the
 * member's own txn/token to echo.
 */
vms_codec_status_t vms_cm_step_ack_build(const uint8_t *req_frame,
					 uint32_t req_len,
					 uint8_t *out_body, uint32_t cap,
					 uint32_t *written);

/*
 * vms_cm_ack_build - a category-0x04 SYSAP acknowledgement (sec 4(u)):
 * "prompt, opportunistic, cumulative, and never keyed to an opcode". No
 * payload; body[9] is written 0x00 (sec 4(p): "An implementation should
 * send zeros; do not reproduce another implementation's uninitialised
 * memory").
 *
 * STAMP with is_response=1: this recipe carries no txn/token of its own
 * (sec 4(p)/(u)) -- the zero-first pass leaves body[4:8] at zero, and that
 * is the correct value to leave untouched.
 */
vms_codec_status_t vms_cm_ack_build(uint8_t *out_body, uint32_t cap,
				    uint32_t *written);

/* ------------------------------------------------------------------ *
 * sec 5c  THE JOINER'S ORIGINATIONS (FC-P3.3)
 *
 * Spec sec 4(o) grounds the ORDER of the joiner's own three category-0x01
 * messages -- op 0x14 model, op 0x01 parameters, then (deferred until its
 * disk-client discovery finishes) op 0x02 config, "this starts admission".
 * These three builders are the only way the join FSM may speak first; they
 * obey sec 5b's rule to the letter (grounded placements only, from the
 * caller's real executive state, every other byte an explicit zero), and
 * none of them writes body[0:8] -- the caller stamps that afterwards with
 * cnxman_envelope_stamp(csb, body, 0).
 * ------------------------------------------------------------------ */

/*
 * vms_cm_model_build - cat 0x01 op 0x14, the node CPU/model advertisement.
 *
 * body[16] = `namelen`, body[17..17+namelen) = the ASCII model string, as
 * sec 4(j) row 1 grounds it ("a length-prefixed ASCII string at body[16]").
 * `namelen` 0 is legal and means this node advertises NO model string --
 * the honest answer when the executive could not read one, and never a
 * plausible-looking stand-in (INV-6). VMS_CODEC_E_RANGE above
 * VMS_CM_MODEL_MAX; the string is never truncated to fit.
 *
 * STAMP with is_response=0.
 */
vms_codec_status_t vms_cm_model_build(const uint8_t *name, uint8_t namelen,
				      uint8_t *out_body, uint32_t cap,
				      uint32_t *written);

/*
 * vms_cm_params_build - cat 0x01 op 0x01, the cluster-parameters message.
 *
 * body[22:24] = `votes`, the field sec 4(j) pinned byte-exact by controlled
 * reconfiguration across four vote values, plus the node-parameter block
 * (`own_params`, the same struct vms_cm_close_build takes) at
 * body[72:76]/[76:80]/[88:96]. Both come from the caller's real SYSGEN and
 * identity state; this builder has no defaults and bakes in no version.
 *
 * WHAT IS HONESTLY MISSING. EXPECTED_VOTES (sec 4(j) RE gap: held at 1 in
 * every capture, so no contrast exists to locate it) and LOCKDIRWT (plan row
 * FC-P3.2, lab) have no isolated offset and are therefore NOT written. Their
 * bytes go out zero along with the rest of the body. A node whose real
 * LOCKDIRWT is 0 is unharmed by that; a node whose LOCKDIRWT is nonzero
 * CANNOT advertise it and its caller must count and log the fact rather than
 * write the value at a guessed offset.
 *
 * STAMP with is_response=0.
 */
vms_codec_status_t vms_cm_params_build(uint16_t votes,
				       const struct vms_cm_node_params *own_params,
				       uint8_t *out_body, uint32_t cap,
				       uint32_t *written);

/*
 * vms_cm_config_build - cat 0x01 op 0x02, the config/topology message that
 * STARTS ADMISSION (sec 4(o) step 4).
 *
 * Category and opcode; nothing else. Sec 4(o) measured that the real
 * admission op-0x02 of vax3-2to3 frame 285 "carries an all-zero topology
 * body and is acked in 0.3 ms", so the zero body is not a shortcut -- it is
 * the observed sufficient trigger. The two spans that section calls
 * "REPLAYED, not decoded" (body[10:12] = 0x5041, twelve 0x20 spaces at
 * body[40:52]) are deliberately absent: they are not constant even for the
 * node that emitted them, so reproducing them would assert a value no
 * implementation can derive.
 *
 * STAMP with is_response=0.
 */
vms_codec_status_t vms_cm_config_build(uint8_t *out_body, uint32_t cap,
				       uint32_t *written);

/*
 * vms_cm_membership_find_sysid - INSTRUMENTATION ONLY, and the distinction
 * is the whole point of this function existing.
 *
 * A joiner is supposed to learn the CSID the cluster assigned it by matching
 * its own SCSSYSTEMID in the coordinator's membership records (design sec 3.4;
 * book p. 7-39 describes each member being described as {SCSSYSTEMID,
 * incarnation, CSID}). The op-0x06 MEMBERSHIP burst's record layout has NO
 * isolated byte offset in any capture (docs/cluster-integration-notes.md E8),
 * so that learn CANNOT be performed honestly today.
 *
 * This function does the HALF that requires no layout knowledge: it searches
 * the received 132-byte body for the caller's OWN SCSSYSTEMID -- an exact
 * match on a value the caller already owns -- and reports the body-relative
 * offset and the width (8 bytes, or the 48-bit form p. 2-16 describes) at
 * which it was found. It reads nothing else, decodes no record, and returns
 * no CSID. The offset it reports is a MEASUREMENT for the lab capture that
 * will pin the layout, not an input to any wire field.
 *
 * VMS_CODEC_E_CLASS unless this is really a cat-0x01 op-0x06; E_RANGE when
 * the sysid does not appear at all (which is itself the observation).
 */
vms_codec_status_t vms_cm_membership_find_sysid(const uint8_t *frame,
						uint32_t len,
						const struct vms_frame_info *fi,
						uint64_t sysid,
						uint32_t *out_body_off,
						uint32_t *out_width);

/* ------------------------------------------------------------------ *
 * sec 6  The GROUNDED (SYSAP, category, opcode) allowlist -- codec DATA
 * owned by this item (vms_cluster_codec.h sec 6 ships the mechanism
 * only). Recipe ids are opaque to the mechanism; this file's own
 * dispatch (sec 5) is what interprets them.
 * ------------------------------------------------------------------ */
enum vms_cm_recipe {
	VMS_CM_RECIPE_ECHO = 1,
	VMS_CM_RECIPE_CLOSE,
	VMS_CM_RECIPE_DLM_OP0D,
	/* The coordinator's 0x81/0x0b barrier-step ack (sec 5b above). A recipe
	 * of its own rather than an ECHO row, because it takes the role-slot
	 * mutation at body[16] and NOT the ECHO family's body[18] marker. */
	VMS_CM_RECIPE_STEP_ACK
};

/* The grounded rows, exposed as a table the caller (a later FSM item, or
 * a test) looks up through vms_wire_allow_find() -- never a bespoke
 * per-layer switch. */
const struct vms_wire_allow_table *vms_cm_allow_table(void);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_CM_H */
