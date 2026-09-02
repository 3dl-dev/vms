/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_dlm.h - cat-0x02 (DLM) typed codec entries (plan item
 * FC-P4.5; design docs/design-faithful-cluster-executive.md §3.6, spec
 * §4(f).1 "The DLM lock-request/response body -- GROUNDED field map", spec
 * §4(p) "cat 0x02" (the op-0d rebuild-record echo recipe).
 *
 * SCOPE. All cat-0x02 traffic rides the SAME 190-byte VMS_FCLS_SCS_MSG frame
 * class as CM traffic (spec §4(f): "all DLM traffic rides the
 * VMS$VAXcluster<->VMS$VAXcluster connection"); there is no dedicated frame
 * CLASS to add, only typed field accessors over that class's SYSAP body
 * (abs 72..204, spec §4(j)). Every offset below is EITHER (a) GROUNDED --
 * cited to a specific spec table row and byte-diff method, or (b)
 * explicitly marked PROVISIONAL and NOT spec-grounded. Nothing in between:
 * the "DO NOT apply the cat-0x01 mutations here" warning in spec §4(p) and
 * the fc8540ae completion crash (below) are both proof that guessing a
 * DLM-body offset from a cat-0x01 precedent or from field forensics alone
 * is how a real VAX bugchecks.
 *
 * THE HARD LESSON THIS FILE EXISTS TO ENCODE (INV-6). An earlier build
 * (fc8540ae, see operator memory cluster-promotion-gap.md pm(15)) sent a
 * DLM completion frame carrying a PLACEHOLDER lock id (the literal
 * 0x00000001) where the master's real granted lock-id belonged. VAX1 bugchecked
 * `Fatal BUG CHECK INVLOCKID, Invalid lock id` and the whole cluster went
 * down (VAX2 followed with CNXMGRERR). Every builder in this file that
 * places a lock-id field on the wire therefore REQUIRES its caller to name
 * a real LKB/RSB-sourced value and structurally REFUSES VMS_DLM_LKID_UNSET
 * (0) -- the same "not a real lock" sentinel vms_lock.c itself never
 * assigns to an established lock (grep `lkid == 0` there). This is an
 * API-level guard, not a claim that every nonzero value passed in is real;
 * the CALLER (the FSM, FC-P4.6/FC-P5.3, O5-tier) is what must source these
 * fields from the executive's actual lock records, never a counter.
 *
 * GROUNDED vs PROVISIONAL, at a glance:
 *   - op 0x01 ENQ / op 0x07 CONVERT request+response (mode, req_lkid/PID,
 *     master_lkid, resource name, grant-vs-deny shape) -- GROUNDED, spec
 *     §4(f).1, pinned by a six-value one-variable-diff method on a live
 *     lab cluster (`vms-ac4`).
 *   - op 0x0d lock-resource rebuild record request+response -- GROUNDED,
 *     spec §4(p), the recipe reconstructs 1367/1367 real responses
 *     byte-for-byte with zero residuals.
 *   - op 0x04 / op 0x03 completion+commit -- PROVISIONAL. The published
 *     spec (docs/cluster-protocol-spec.md §5(dlm)) does NOT ground a
 *     cat-0x02 completion body at all. The field positions here are
 *     carried over from repeated field-forensics sessions recorded in
 *     operator memory cluster-promotion-gap.md (pm14-pm16) -- explicitly
 *     NOT this codec's normal source of truth (design §3.9: "never
 *     re-derive an offset from a pcap instead of the spec"). They are
 *     included, clearly marked PROVISIONAL, because FC-P5.2 is EXPECTED to
 *     re-map cat-0x02 op semantics from a fresh console-correlated capture
 *     (this item's own plan-table gate: "LAB (P5.1 may re-map op
 *     semantics; the table is data)") -- superseding this table is the
 *     plan, not a regression.
 *   - BLKAST (asynchronous block notification) and the 16-byte LKSB VALBLK
 *     (lock value block) -- NOT IMPLEMENTED. Spec §5(dlm) is explicit that
 *     the VALBLK round-trip was "not exercised (driver requested no
 *     VALBLK)" and no BLKAST wire capture exists at all in the published
 *     spec. Per INV-6 ("honest omission over a placeholder"), this codec
 *     defines no struct field and no accessor for either -- a function
 *     that always returns VMS_CODEC_E_CLASS would invite a caller to
 *     forget to check it; an absent function cannot be forgotten-and-called.
 *     A later item grounds these from a real capture before this file grows
 *     them.
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_DLM_H
#define OVMX_VMS_CLUSTER_CODEC_DLM_H

#include "vms_cluster_codec.h"
#include "vms_cluster_codec_hello.h" /* vms_cluster_lavc_sysid() -- req_csid */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * SYSAP envelope category/opcode this item reads within the DLM SYSAP
 * (spec §4(j), §4(p) "cat 0x02"). abs = body + 72.
 * ------------------------------------------------------------------ */
#define VMS_OFF_DLM_CAT           80u  /* body[8]  category: 0x02 req       */
#define VMS_OFF_DLM_OP            81u  /* body[9]  opcode                   */

#define VMS_DLM_CAT_REQUEST     0x02u  /* response = REQUEST | VMS_WIRE_RESPONSE_BIT (0x82) */

/* Opcodes -- GROUNDED (spec §4(f).1, §4(p)). */
#define VMS_DLM_OP_ENQ          0x01u  /* new-lock ENQ request              */
#define VMS_DLM_OP_CONVERT      0x07u  /* lock mode CONVERT                 */
#define VMS_DLM_OP_REBUILD      0x0du  /* join-time lock-resource rebuild rec*/

/* Opcodes -- PROVISIONAL, NOT spec-grounded; see the file doc comment. */
#define VMS_DLM_OP_COMPLETE_PROVISIONAL 0x04u
#define VMS_DLM_OP_COMMIT_PROVISIONAL   0x03u

/*
 * Lock modes -- GROUNDED, spec §4(f).1 body[30] (abs 102): a clean
 * six-value one-variable diff on the same resource, byte-for-byte the
 * documented VAXcluster Principles Table 6-1 / lckdef.h LCK$K_* encoding.
 */
enum vms_lck_mode {
	VMS_LCK_NL = 0,
	VMS_LCK_CR = 1,
	VMS_LCK_CW = 2,
	VMS_LCK_PR = 3,
	VMS_LCK_PW = 4,
	VMS_LCK_EX = 5
};

/*
 * Reserved lock-id sentinel. The executive's own real DLM (vms_lock.c)
 * never assigns lkid 0 to an established lock -- every cross-node path
 * there (`lkid == 0`, `req->req_lkid == 0`, `req->master_lkid != 0`, ...)
 * treats 0 as "not a real lock yet". This codec uses the SAME convention
 * to refuse a completion/commit build whose lock-id field looks unset
 * rather than a real LKB/RSB handle (the fc8540ae lesson, see file doc
 * comment).
 */
#define VMS_DLM_LKID_UNSET 0u

/* ------------------------------------------------------------------ *
 * op 0x01 ENQ / op 0x07 CONVERT -- GROUNDED, spec §4(f).1
 * ------------------------------------------------------------------ */
#define VMS_OFF_DLM_REQ_LKID       92u  /* body[20:24] LE u32, spec row     */
#define VMS_OFF_DLM_MASTER_LKID    96u  /* body[24:28] LE u32, spec row     */
#define VMS_OFF_DLM_MODE          102u  /* body[30]    u8,     spec row     */
#define VMS_OFF_DLM_NAME_MARKER   118u  /* body[46]    u8, const 0x03       */
#define VMS_OFF_DLM_NAME_LEN      119u  /* body[47]    u8,     spec row     */
#define VMS_OFF_DLM_NAME          120u  /* body[48..]  ASCII,  spec row     */

#define VMS_DLM_NAME_MARKER_CONST 0x03u
/* Longest observed resource name is 22 bytes ("F11B$aSYSDSK1     *" family,
 * spec §4(f).1 row 47); a generous bound, not a second grounded fact. */
#define VMS_DLM_NAME_MAX          32u

/*
 * A parsed/to-be-built ENQ (op 0x01) or CONVERT (op 0x07) REQUEST.
 * `req_pid_or_lkid` carries the DUAL meaning spec §4(f).1 grounds at
 * body[20]: a fresh ENQ's PROCESS id placeholder, or an existing lock's
 * real local lock-id on a CONVERT. The field is named for the wire's own
 * dual role rather than picking one meaning, exactly as the spec presents
 * it -- resolving which meaning applies is the FSM's job (it knows whether
 * it is originating a fresh ENQ or a CONVERT), not this codec's.
 */
struct vms_dlm_enq_request {
	uint8_t  mode;              /* LKB requested mode (CONVERT: the NEW mode) */
	uint32_t req_pid_or_lkid;   /* body[20:24], see doc comment above     */
	uint32_t master_lkid;       /* body[24:28]: the RSB's master lock-id, */
				     /* present once the lock is established   */
	uint8_t  name_len;
	uint8_t  name[VMS_DLM_NAME_MAX];
};

/*
 * The grant/deny outcome is a message-SHAPE discriminator, not a literal
 * status code (spec §4(f).1 "Completion status"): the literal VMS status
 * longword never appears in the reply body. GRANTED replaces the request's
 * PID placeholder at body[20] with the requester's real assigned lock-id
 * and omits the resource name; DENIED (SS$_NOTQUEUED) leaves body[20] as
 * the PID placeholder, clears the mode byte to 0, and echoes the name back.
 */
enum vms_dlm_enq_outcome {
	VMS_DLM_ENQ_GRANTED = 0,
	VMS_DLM_ENQ_DENIED
};

struct vms_dlm_enq_response {
	enum vms_dlm_enq_outcome outcome;
	uint32_t req_lkid;      /* GRANTED: the requester's newly assigned   */
				 /* local lock-id (body[20]); DENIED: the     */
				 /* request's PID placeholder, echoed         */
	uint32_t master_lkid;   /* body[24:28], echoed in both shapes        */
	uint8_t  granted_mode;  /* GRANTED: the mode now held; DENIED: 0     */
	uint8_t  name_len;      /* DENIED only -- GRANTED carries no name    */
	uint8_t  name[VMS_DLM_NAME_MAX];
};

/*
 * Parse a cat-0x02 request frame as an ENQ/CONVERT. `*opcode_out` receives
 * VMS_DLM_OP_ENQ or VMS_DLM_OP_CONVERT so the caller can tell them apart --
 * the wire shape is identical (spec §4(f).1: "The CONVERT 0x07 request
 * carries the new mode here", same body[30]).
 */
vms_codec_status_t vms_dlm_enq_request_parse(const uint8_t *frame, uint32_t len,
					     const struct vms_frame_info *fi,
					     uint8_t *opcode_out,
					     struct vms_dlm_enq_request *out);

/*
 * Write an ENQ/CONVERT request's DLM-specific fields into `frame` at their
 * abs offsets (body[8],[9],[20:24],[24:28],[30],[46],[47],[48..]). This
 * function does NOT build the shared SCA header (abs 0-31, use
 * vms_sca_hdr_build) or the generic SYSAP envelope's send/ack/txn fields
 * (abs 72-79, spec §4(j) -- owned by whichever item lands the CM/generic
 * envelope codec, not this DLM item); `frame` must already hold a valid
 * frame of at least `cap` >= 132 (VMS_DLM_NAME_MARKER's max reach) bytes
 * that the caller assembles those spans into separately. `opcode` selects
 * VMS_DLM_OP_ENQ or VMS_DLM_OP_CONVERT.
 */
vms_codec_status_t vms_dlm_enq_request_build(const struct vms_dlm_enq_request *req,
					     uint8_t opcode,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written);

/* Parse a cat-0x82 (response bit set) ENQ/CONVERT reply, resolving the
 * grant-vs-deny SHAPE per spec §4(f).1 (see enum vms_dlm_enq_outcome doc). */
vms_codec_status_t vms_dlm_enq_response_parse(const uint8_t *frame, uint32_t len,
					      const struct vms_frame_info *fi,
					      struct vms_dlm_enq_response *out);

/*
 * Build a GRANT reply: body[8] gets the response bit, body[20] becomes
 * `req_lkid` (the value the codec's own caller assigned as the real
 * granted lock-id -- REFUSED if VMS_DLM_LKID_UNSET, see file doc comment),
 * body[24:28] echoes master_lkid, body[30] is the granted mode. The name
 * span is left untouched (spec: grant does not echo the name).
 */
vms_codec_status_t vms_dlm_enq_response_build_grant(uint32_t req_lkid,
						    uint32_t master_lkid,
						    uint8_t granted_mode,
						    uint8_t *frame, uint32_t cap,
						    uint32_t *written);

/*
 * Build a DENY (SS$_NOTQUEUED) reply: body[20] echoes the request's PID
 * placeholder verbatim (`req_pid_echo`), body[30] is cleared to 0, and the
 * name is echoed at body[46:48+len] (spec §4(f).1).
 */
vms_codec_status_t vms_dlm_enq_response_build_deny(uint32_t req_pid_echo,
						   uint32_t master_lkid,
						   uint8_t name_len,
						   const uint8_t *name,
						   uint8_t *frame, uint32_t cap,
						   uint32_t *written);

/*
 * req_csid: "who is asking" for a DLM request. The DLM body itself carries
 * no separate CSID field (spec §4(f).1 does not ground one) -- the
 * requester's identity is the frame's own cluster-LOGICAL src address
 * (spec §4(a), already GROUNDED and already tested in
 * vms_cluster_lavc_sysid()). This is a thin composition, not a new offset:
 * it never invents a field the DLM body does not have.
 */
vms_codec_status_t vms_dlm_req_csid(const struct vms_sca_hdr *hdr, uint16_t *out);

/* ------------------------------------------------------------------ *
 * op 0x0d lock-resource rebuild record -- GROUNDED, spec §4(p) "cat 0x02"
 * (1367/1367 real responses reconstructed byte-for-byte, zero residuals).
 * ------------------------------------------------------------------ */

/* Request-layout invariants (spec §4(p), GROUNDED). */
#define VMS_OFF_DLM_REBUILD_INV1    84u /* body[12:14] LE u16, invariant 0x0001 */
#define VMS_OFF_DLM_REBUILD_INV2    86u /* body[14:16] LE u16, invariant 0x0003 */
#define VMS_DLM_REBUILD_INV1_CONST 0x0001u
#define VMS_DLM_REBUILD_INV2_CONST 0x0003u

/* The envelope fields the response recipe overwrites (spec §4(p)). */
#define VMS_OFF_DLM_SEND_MSG        72u /* body[0:2]  LE u16, own send-msg# */
#define VMS_OFF_DLM_ACK_MSG         74u /* body[2:4]  LE u16, ack of peer's */
#define VMS_OFF_DLM_RESULT_STAMP   106u /* body[34]   u8, unconditional     */

#define VMS_DLM_RESULT_STAMP_REBUILD 0xf9u /* op 0x0d reply, EVERY specimen */
/* INFERRED (spec §4(p)): op 0x01/0x07/0x15 replies use 0xfa here instead --
 * not exercised by this file (0x15 is otherwise ungrounded, 0x01/0x07 grant
 * replies are built by vms_dlm_enq_response_build_grant/_deny above, which
 * do not touch body[34] because the grant/deny shape test does not name it
 * as part of either shape). Recorded for the reader, not asserted as a
 * built value anywhere in this file. */
#define VMS_DLM_RESULT_STAMP_ENQ     0xfau

/* The whole SYSAP body of a 190-byte SCS_MSG frame (body[0:132), abs
 * 72..204) -- the full span the op-0d recipe's "memcpy 132 bytes" copies. */
#define VMS_DLM_REBUILD_ECHO_LEN    132u

struct vms_dlm_rebuild_record {
	uint8_t body[VMS_DLM_REBUILD_ECHO_LEN]; /* abs 72..204, verbatim      */
	uint8_t name_len;                        /* body[47], convenience     */
	uint8_t name[VMS_DLM_NAME_MAX];          /* body[48..], convenience   */
};

/*
 * Parse a cat-0x02 op-0x0d rebuild-record request: validates the frame is
 * VMS_FCLS_SCS_MSG carrying category 0x02 op 0x0d, the two body[12:16]
 * invariants, and lifts out the whole body (for the verbatim-echo response
 * builder below) plus the resource name as a convenience.
 */
vms_codec_status_t vms_dlm_rebuild_parse(const uint8_t *frame, uint32_t len,
					 const struct vms_frame_info *fi,
					 struct vms_dlm_rebuild_record *out);

/*
 * Build the op-0x0d response by the spec's OWN recipe, applied literally
 * (spec §4(p) explicitly warns: "DO NOT apply the cat-0x01 mutations
 * here" -- a field-by-field reconstruction of this frame mis-shifted the
 * resource name and bugchecked two real VAXes with LOCKMGRERR, specimen
 * `ovmx-760-lockmgrerr-20260730.pcap`):
 *
 *     memcpy(resp_body, req_body, 132)   verbatim echo
 *     resp[0:2]  = own_send_msg          envelope
 *     resp[2:4]  = ack_of_peer_send      envelope
 *     resp[8]   |= 0x80                  0x02 -> 0x82
 *     resp[34]   = 0xf9                  MANDATORY, unconditional
 *
 * `req` is the record `vms_dlm_rebuild_parse()` produced (its `.body` is
 * the verbatim source for the memcpy). Writes ONLY abs [72,204) of `frame`
 * -- the shared SCA header and generic envelope span [0,72) is the
 * caller's responsibility, same division as the ENQ/CONVERT builders
 * above. `*written` receives 132 (the body span written), not the frame
 * total.
 */
vms_codec_status_t
vms_dlm_rebuild_response_build(const struct vms_dlm_rebuild_record *req,
			       uint16_t own_send_msg, uint16_t ack_of_peer_send,
			       uint8_t *frame, uint32_t cap, uint32_t *written);

/* ------------------------------------------------------------------ *
 * op 0x04 / op 0x03 completion + commit -- PROVISIONAL, NOT spec-grounded.
 * See the file doc comment for why this table exists despite that, and
 * FC-P5.2 which is expected to supersede it from a fresh capture.
 * ------------------------------------------------------------------ */
#define VMS_OFF_DLM_COMPLETE_STATUS       84u /* body[12:16] LE u32, PROVISIONAL */
#define VMS_OFF_DLM_COMPLETE_MASTER_LKID  92u /* body[20:24] LE u32, PROVISIONAL */
#define VMS_OFF_DLM_COMPLETE_REQ_LKID     96u /* body[24:28] LE u32, PROVISIONAL */

/* Constant observed across the forensics sessions at body[12:16]; carries
 * no known meaning beyond "present on every completion seen" (PROVISIONAL,
 * memory cluster-promotion-gap.md pm(14)/pm(15)). */
#define VMS_DLM_COMPLETE_STATUS_CONST 0x00030001u

struct vms_dlm_completion {
	uint32_t master_lkid; /* LKB: the master's granted lock-id for this  */
			       /* lock -- MUST be sourced from a real grant   */
			       /* this node received (e.g. the req_lkid field */
			       /* of vms_dlm_enq_response after a GRANT),     */
			       /* never a counter or a constant                */
	uint32_t req_lkid;    /* LKB: this node's own local lock-id for the  */
			       /* same lock                                   */
	uint8_t  name_len;
	uint8_t  name[VMS_DLM_NAME_MAX];
};

/*
 * Build a completion (op VMS_DLM_OP_COMPLETE_PROVISIONAL) or commit (op
 * VMS_DLM_OP_COMMIT_PROVISIONAL) frame. REFUSES (VMS_CODEC_E_INVAL) if
 * `c->master_lkid` or `c->req_lkid` is VMS_DLM_LKID_UNSET (0) -- the
 * fc8540ae hard lesson: a completion referencing a lock id the master
 * never granted crashes the master with INVLOCKID, and every OVMX build
 * that has ever placed a real value at this field sourced it from a lock
 * this node was actually granted; a zero can only be an unsourced
 * placeholder. This check does not and cannot prove a nonzero value is
 * genuinely real -- that discipline belongs to the FSM caller, which must
 * read `master_lkid` off the executive's own lock state, never mint one.
 */
vms_codec_status_t vms_dlm_completion_build(const struct vms_dlm_completion *c,
					    uint8_t op,
					    uint8_t *frame, uint32_t cap,
					    uint32_t *written);

/* ------------------------------------------------------------------ *
 * The (SYSAP, category, opcode) allowlist rows this item contributes
 * (vms_cluster_codec.h §6). Only the GROUNDED ops (ENQ/CONVERT/REBUILD)
 * are listed -- the PROVISIONAL completion/commit ops are deliberately
 * NOT in this table: an allowlist row asserts "grounded in the reference"
 * (spec §4(p)), which the completion body is not.
 * ------------------------------------------------------------------ */
extern const struct vms_wire_allow_entry vms_dlm_allow_rows[];
extern const struct vms_wire_allow_table vms_dlm_allow_table;

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_DLM_H */
