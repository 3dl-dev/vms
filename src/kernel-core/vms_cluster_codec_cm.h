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
 * (SCS codec), neither of which has landed yet. This file therefore
 * ships `struct vms_cm_link`: a MINIMAL, honestly-scoped stand-in that
 * builds only the fields spec sec 4(d) itself grounds (the SCA header via
 * the already-frozen vms_sca_hdr_build, recv_ack/send_seq, the Con.ID
 * pair, the CM msgtype/format markers) and leaves every other byte of
 * that span an explicit zero -- never an invented mirror or a captured
 * template. A fuller/richer envelope (the observed recv_ack/send_seq
 * mirrors at abs 40/44/48, credit accounting, MTYPE selection) is P1.1/
 * P2.1's job and supersedes vms_cm_link once it lands; nothing here
 * asserts to be that layer.
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
 * sequence-number region vms_cm_link_build() lays down. */
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
 * sec 3  The minimal SCS-envelope stand-in around the CM body (abs
 * 0-71). See the file header "OUT OF SCOPE" note -- every field here
 * either is GROUNDED per spec sec 4(d) or is an explicit, honest zero.
 * ------------------------------------------------------------------ */
struct vms_cm_link {
	struct vms_sca_hdr hdr;   /* abs 0-31; hdr.word30 is IGNORED by the
				   * builder and overwritten with the CM
				   * msgtype/format marker (msgtype 0x4b,
				   * format 0x13, GROUNDED sec 4(g)/4(d))    */
	uint16_t recv_ack;        /* abs 32, GROUNDED sec 4(d)/(h)             */
	uint16_t send_seq;        /* abs 34, GROUNDED sec 4(d)/(h)             */
	uint32_t remote_conid;    /* abs 64, GROUNDED sec 4(d): the PEER's own
				   * Con.ID, as the sender addresses it        */
	uint32_t local_conid;     /* abs 68, GROUNDED sec 4(d): OUR own Con.ID */
};

/*
 * vms_cm_link_build - write the abs [0,72) span into a >=204-byte buffer.
 * Bakes in ONLY the discovery-independent CM format markers (connect flag
 * 0x0001, msgtype 0x4b / format 0x13, and the sec 4(d)-grounded constant
 * 18 at abs 38-39); every other byte in [36,64) and [70,72) -- the
 * counter-mirror region sec 4(d) itself calls "inferred, not independently
 * confirmed" -- is left ZERO rather than guessed. A fuller mirror belongs
 * to FC-P1.1/P2.1.
 */
vms_codec_status_t vms_cm_link_build(const struct vms_cm_link *l,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written);

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
	uint8_t  version[VMS_CM_VERSION_LEN]; /* body[88:96], e.g. "V7.3    " */
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
 * deliverable of this item. Every builder takes the RECEIVED request
 * frame (already classified VMS_FCLS_SCS_MSG) plus the caller's own
 * link/envelope counters, and returns a full VMS_CM_FRAME_LEN response.
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
 */
vms_codec_status_t vms_cm_echo_response_build(const struct vms_cm_link *l,
					      const uint8_t *req_frame,
					      uint32_t req_len,
					      const struct vms_cm_envelope *own,
					      uint8_t own_class,
					      uint8_t *out_frame, uint32_t cap,
					      uint32_t *written);

/*
 * vms_cm_close_build - the cat-0x06 close recipe (sec 4(p): "closes the
 * transaction. Carry only the (txn,checksum); send your own node-
 * parameter block, the same one carried in the op 0x01 PARAMS message").
 * `own_params` supplies OVMX's own honest node-parameter fields (never a
 * captured value): echoing the REQUEST's payload here is what bugchecked
 * a real VAX with INCONSTATE (sec 4(p)).
 */
struct vms_cm_node_params {
	uint32_t param_f1;  /* body[72:76] */
	uint32_t param_f2;  /* body[76:80] */
	uint8_t  version[VMS_CM_VERSION_LEN]; /* body[88:96] */
};

vms_codec_status_t vms_cm_close_build(const struct vms_cm_link *l,
				      const uint8_t *req_frame, uint32_t req_len,
				      const struct vms_cm_envelope *own,
				      const struct vms_cm_node_params *own_params,
				      uint8_t *out_frame, uint32_t cap,
				      uint32_t *written);

/*
 * vms_cm_dlm_op0d_response_build - the cat-0x02 op-0x0d DLM rebuild echo
 * (sec 4(p): "reconstructs 1367 of 1367 real responses byte-for-byte").
 * VERBATIM echo of the 132-byte body plus exactly: own send/ack-msg#,
 * body[8] |= 0x80, body[34] = 0xf9 (MANDATORY, unconditional). Does NOT
 * take the cat-0x01 body[18]/body[55] mutations -- sec 4(p)'s explicit
 * warning: those offsets land inside the L1 region and the 8th byte of
 * the lock RESOURCE NAME here, and applying them corrupted the name and
 * bugchecked two real VAXes with LOCKMGRERR.
 */
vms_codec_status_t vms_cm_dlm_op0d_response_build(const struct vms_cm_link *l,
						  const uint8_t *req_frame,
						  uint32_t req_len,
						  const struct vms_cm_envelope *own,
						  uint8_t *out_frame, uint32_t cap,
						  uint32_t *written);

/*
 * vms_cm_ack_build - a category-0x04 SYSAP acknowledgement (sec 4(u)):
 * "prompt, opportunistic, cumulative, and never keyed to an opcode". No
 * payload; body[9] is written 0x00 (sec 4(p): "An implementation should
 * send zeros; do not reproduce another implementation's uninitialised
 * memory").
 */
vms_codec_status_t vms_cm_ack_build(const struct vms_cm_link *l,
				    const struct vms_cm_envelope *own,
				    uint8_t *out_frame, uint32_t cap,
				    uint32_t *written);

/* ------------------------------------------------------------------ *
 * sec 6  The GROUNDED (SYSAP, category, opcode) allowlist -- codec DATA
 * owned by this item (vms_cluster_codec.h sec 6 ships the mechanism
 * only). Recipe ids are opaque to the mechanism; this file's own
 * dispatch (sec 5) is what interprets them.
 * ------------------------------------------------------------------ */
enum vms_cm_recipe {
	VMS_CM_RECIPE_ECHO = 1,
	VMS_CM_RECIPE_CLOSE,
	VMS_CM_RECIPE_DLM_OP0D
};

/* The grounded rows, exposed as a table the caller (a later FSM item, or
 * a test) looks up through vms_wire_allow_find() -- never a bespoke
 * per-layer switch. */
const struct vms_wire_allow_table *vms_cm_allow_table(void);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_CM_H */
