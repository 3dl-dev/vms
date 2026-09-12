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
 * THE SUPERSESSION THIS FILE PROMISED, AND WHY IT IS A DIVERGENCE *REDUCTION*
 * (rd vms-fa7 / vms-002 / vms-858, from the vms-c03 capture set).
 *
 * The previous revision of this comment carried a PROVISIONAL "completion
 * 0x04 + commit 0x03" pair, lifted from field-forensics rather than from a
 * console-correlated capture, and said in as many words that re-mapping the
 * cat-0x02 op semantics from a fresh capture was "the plan, not a
 * regression". That capture now exists -- a private 2-node real OpenVMS
 * VAX 7.3 cluster, every frame correlated by `master_lkid` to the `$ENQ`
 * that drove it (tests/lab/captures/vms-c03-dlm-opcodes-20260911/,
 * GROUNDING.md) -- and it says the pair was a PHANTOM:
 *
 *     op 0x03 is $DEQ.     It is not a "commit".
 *     op 0x04 is BLKAST.   It is not a "completion".
 *     op 0x06 is the CONVERT that carries the lock VALUE BLOCK.
 *     THERE IS NO SEPARATE COMPLETION OR COMMIT OPCODE AT ALL. A real
 *     requester's grant is simply the op-0x01 cat-0x82 response; nothing
 *     follows it on the wire.
 *
 * So `struct vms_dlm_completion` and `vms_dlm_completion_build()` are GONE,
 * not renamed. Deleting them makes OVMX emit STRICTLY FEWER frame shapes
 * than before and strictly fewer than it used to invent: two frames a real
 * VMS requester never sends are no longer sendable at all, which is the
 * cheapest kind of fidelity there is (memory ovmx-never-crashes-a-peer: the
 * frames you cannot build are the ones that cannot bugcheck a peer).
 *
 * GROUNDED vs OBSERVED, at a glance:
 *   - op 0x01 ENQ / op 0x07 CONVERT request+response (mode, req_lkid/PID,
 *     master_lkid, resource name, grant-vs-deny shape) -- GROUNDED, spec
 *     §4(f).1, pinned by a six-value one-variable-diff method on a live
 *     lab cluster (`vms-ac4`).
 *   - op 0x0d lock-resource rebuild record request+response -- GROUNDED,
 *     spec §4(p), the recipe reconstructs 1367/1367 real responses
 *     byte-for-byte with zero residuals.
 *   - op 0x03 $DEQ and op 0x04 BLKAST: the two lock-id fields and the mode
 *     byte -- GROUNDED, vms-c03, each byte correlated to the driving $ENQ.
 *   - op 0x06 CONVERT-with-VALBLK: the 16-byte value block at body[36:52]
 *     -- GROUNDED, vms-c03 (the driver's own `WROTEBYVAX1XXXXX` pattern
 *     appears there verbatim on the real wire).
 *   - ONE field is OBSERVED-BUT-NOT-PINNED and is labelled so everywhere it
 *     appears: the BLKAST mode-context pair at body[30:32]. Two samples in
 *     one capture read 0x01,0x05 and a third (a different lock) read
 *     0x01,0x00, which is not a one-variable diff. It is therefore carried
 *     behind an explicit `mode_ctx_valid` opt-in, exactly like the
 *     directory hash: a caller that does not hold real executive values for
 *     it writes NOTHING there. A later capture upgrades the label.
 *
 * TWO THINGS THIS FILE STILL REFUSES TO DO, and they are not oversights:
 *   - There is NO op-0x06 BUILDER. The value block's position is grounded,
 *     but body[32:36] ahead of it varies per request in a way no capture
 *     pins, so composing a whole op-0x06 frame would mean minting those
 *     four bytes. There is an ACCESSOR (read what a peer really sent) and
 *     no builder (INV-6: nowhere to put a value nobody produced) -- the
 *     same asymmetry the directory hash already has.
 *   - There is no resource-NAME field on a BLKAST. The vms-c03 BLKAST frame
 *     does have readable ASCII at body[48] -- `F11B$aSYSDSK1` -- and it is
 *     STALE BUFFER, not a field: the resource the frame is actually about
 *     (`OVMXBLK2`) appears in the capture ONLY in the op-0x01 ENQ. A BLKAST
 *     names its lock by `master_lkid` and by nothing else. Reading body[48]
 *     there would be the purest form of the bug this file exists to
 *     prevent: a wire field that looks like data and is not.
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
/* ------------------------------------------------------------------ *
 * BODY-RELATIVE OFFSETS, and why this file needs both spellings (rd vms-1ee)
 *
 * Every DLM field below is documented in terms of `body[...]` and then given a
 * FRAME-absolute constant, because the codec was written against captured
 * frames. But the only inbound path in the executive is
 * scs_sysap_ops.message -> cnxman_vc_message(), which hands a SYSAP its OWN
 * 132 bytes and nothing below them (design sec 3.2.4). Handing a
 * frame-absolute parser a SYSAP body is exactly integration note E73: it does
 * not fail loudly, it silently refuses every real inbound message -- which is
 * how a live run lost VAX1's whole CM dialogue.
 *
 * So the offsets exist in both spellings, DERIVED from one another so they can
 * never drift, and each parser has two entry points over ONE implementation:
 *   *_parse       takes a captured FRAME (and its vms_frame_info class), and
 *                 slices the body off it;
 *   *_parse_body  takes the 132 bytes SCS really delivers.
 * The BUILDERS need no such pair: they already write into a caller's frame
 * buffer at absolute offsets and the FSM sends `txframe + VMS_OFF_SYSAP_BODY`
 * (vms_dlm_scs_fsm.c), which is the same slice from the other side.
 * ------------------------------------------------------------------ */
#define VMS_OFB_FROM_FRAME(off)   ((off) - VMS_OFF_SYSAP_BODY)

#define VMS_OFF_DLM_CAT           80u  /* body[8]  category: 0x02 req       */
#define VMS_OFF_DLM_OP            81u  /* body[9]  opcode                   */

#define VMS_DLM_CAT_REQUEST     0x02u  /* response = REQUEST | VMS_WIRE_RESPONSE_BIT (0x82) */

/*
 * Opcodes -- GROUNDED (spec §4(f).1, §4(p)).
 *
 * THE `WIREOP` SPELLING IS LOAD-BEARING (renamed by FC-P4.6). These are the
 * CAT-0x02 WIRE opcodes. The executive's lock engine has an entirely separate
 * op family with the SAME short names and DIFFERENT values -- the ioctl/xnode
 * dispatch selectors in src/kernel/vms_ioctl.h and its NetBSD twin
 * (VMS_DLM_OP_ENQ 1, _GRANT 2, _DEQ 3, _BLKAST 4, _REBUILD **5**, _DLKSRCH 6).
 * `VMS_DLM_OP_REBUILD` therefore meant 5 in one family and 0x0d in the other,
 * so any translation unit that included BOTH headers -- which is exactly what
 * the requester FSM's consumers and the FC-P4.8 glue do -- got a macro
 * redefinition with a silently different value. Two names, two numbers, one
 * tree: renamed here so the wire opcode and the dispatch selector can never be
 * confused, by a reader or by the preprocessor.
 */
#define VMS_DLM_WIREOP_ENQ          0x01u  /* new-lock ENQ request              */
#define VMS_DLM_WIREOP_CONVERT      0x07u  /* lock mode CONVERT                 */
#define VMS_DLM_WIREOP_REBUILD      0x0du  /* join-time lock-resource rebuild rec*/

/*
 * Opcodes -- GROUNDED by the vms-c03 capture set (see the file doc comment's
 * supersession note). The values that used to live at 0x03 and 0x04 in this
 * table ("commit" and "completion") were phantoms; these are what a real
 * OpenVMS VAX 7.3 cluster actually puts there.
 */
#define VMS_DLM_WIREOP_DEQ          0x03u  /* cross-node lock RELEASE ($DEQ)    */
#define VMS_DLM_WIREOP_BLKAST       0x04u  /* master -> remote holder blocking AST*/
#define VMS_DLM_WIREOP_CONVERT_VALBLK 0x06u /* CONVERT carrying the value block */

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
 * to refuse any build -- and, for the lock-id-only messages, any PARSE --
 * whose lock-id field looks unset rather than a real LKB/RSB handle (the
 * fc8540ae lesson, see file doc comment).
 *
 * The engine states the same constant in src/kernel-core/vms_dlm_proxy.h, where
 * the proxy-LKB paths refuse to post or accept it (FC-P4.4). The guard below
 * keeps the two spellings from ever disagreeing when a TU pulls in both; this
 * header stays self-contained for the pure host codec build, which includes no
 * vms_internal.h at all.
 */
#ifndef VMS_DLM_LKID_UNSET
#define VMS_DLM_LKID_UNSET 0u
#endif

/* ------------------------------------------------------------------ *
 * op 0x01 ENQ / op 0x07 CONVERT -- GROUNDED, spec §4(f).1
 * ------------------------------------------------------------------ */
#define VMS_OFF_DLM_REQ_LKID       92u  /* body[20:24] LE u32, spec row     */
#define VMS_OFF_DLM_MASTER_LKID    96u  /* body[24:28] LE u32, spec row     */
#define VMS_OFF_DLM_MODE          102u  /* body[30]    u8,     spec row     */
#define VMS_OFF_DLM_NAME_MARKER   118u  /* body[46]    u8, const 0x03       */
#define VMS_OFF_DLM_NAME_LEN      119u  /* body[47]    u8,     spec row     */
#define VMS_OFF_DLM_NAME          120u  /* body[48..]  ASCII,  spec row     */

#define VMS_OFB_DLM_CAT         VMS_OFB_FROM_FRAME(VMS_OFF_DLM_CAT)
#define VMS_OFB_DLM_OP          VMS_OFB_FROM_FRAME(VMS_OFF_DLM_OP)
#define VMS_OFB_DLM_REQ_LKID    VMS_OFB_FROM_FRAME(VMS_OFF_DLM_REQ_LKID)
#define VMS_OFB_DLM_MASTER_LKID VMS_OFB_FROM_FRAME(VMS_OFF_DLM_MASTER_LKID)
#define VMS_OFB_DLM_MODE        VMS_OFB_FROM_FRAME(VMS_OFF_DLM_MODE)
#define VMS_OFB_DLM_NAME_MARKER VMS_OFB_FROM_FRAME(VMS_OFF_DLM_NAME_MARKER)
#define VMS_OFB_DLM_NAME_LEN    VMS_OFB_FROM_FRAME(VMS_OFF_DLM_NAME_LEN)
#define VMS_OFB_DLM_NAME        VMS_OFB_FROM_FRAME(VMS_OFF_DLM_NAME)

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

	/*
	 * THE ROOT NAME'S DIRECTORY HASH, body[10:12] (see the section below).
	 *
	 * `dir_hash_valid` is 0 when this executive holds NO wire-learned hash
	 * for the name, and then the builder writes NOTHING at body[10:12] --
	 * the honest omission, never a zero passed off as a hash (INV-6). It is
	 * the FSM's job (FC-P4.6) to refuse to send a directory LOOKUP at all
	 * in that case; a request addressed to a MASTER the cluster already
	 * named needs no directory index and is sent without one.
	 *
	 * There is deliberately no standalone hash BUILDER in this codec (see
	 * the section below): the value may only ride an ENQ/CONVERT whose
	 * OTHER fields the caller has already sourced from a real LKB, and it
	 * must itself be a value `vms_lock_dlm_learn_dir_hash()` recorded on
	 * that resource block from a received frame.
	 */
	uint16_t dir_hash;
	uint8_t  dir_hash_valid;
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
 * VMS_DLM_WIREOP_ENQ or VMS_DLM_WIREOP_CONVERT so the caller can tell them apart --
 * the wire shape is identical (spec §4(f).1: "The CONVERT 0x07 request
 * carries the new mode here", same body[30]).
 */
/*
 * THE BODY ENTRIES (rd vms-1ee). Same fields, same implementation, taking the
 * 132 bytes SCS actually delivers to a SYSAP (design sec 3.2.4) instead of a
 * captured frame. There is no vms_frame_info parameter because there is no
 * frame to classify: a body that arrives on the VMS$VAXcluster connection is
 * already known to be one, and inventing header bytes to re-derive that would
 * be putting bytes on a frame nobody sent. The category/opcode checks each
 * core already performs are what reject a body that is not what it claims.
 */
vms_codec_status_t vms_dlm_enq_request_parse_body(const uint8_t *body,
						  uint32_t len,
						  uint8_t *opcode_out,
						  struct vms_dlm_enq_request *out);
vms_codec_status_t vms_dlm_enq_response_parse_body(const uint8_t *body,
						   uint32_t len,
						   struct vms_dlm_enq_response *out);
vms_codec_status_t vms_dlm_dir_hash_parse_body(const uint8_t *body, uint32_t len,
					       uint16_t *out);
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
 * VMS_DLM_WIREOP_ENQ or VMS_DLM_WIREOP_CONVERT.
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

/* ------------------------------------------------------------------ *
 * THE DIRECTORY HASH -- body[10:12] (abs 82), FC-P4.3
 *
 * The 16-bit hash of the ROOT resource name, computed by the SENDING
 * system and carried on the wire. Davis p. 6-50: a directory lookup
 * request carries the resource name AND the hash value the sender
 * derived from it, "as an optimization" because every system would
 * derive the same value, and the receiving directory node right-shifts
 * the RECEIVED value to index its own Resource Hash Table.
 *
 * WHY OVMX ONLY EVER READS IT. The hash FUNCTION is not published at the
 * bit level (docs/research-dlm-directory-algorithm.md SS3, checked over
 * Davis pp. 6-18..6-53), so computing one is both Rule-8-forbidden and
 * wrong: a mismatched value makes the directory node scan the wrong
 * chain, miss the name, and create a directory entry naming the SENDER
 * as master (p. 6-31 outcome 3). That is the campaign's 35/s grant storm
 * (memory cluster-promotion-gap). So there is a PARSER here and there is
 * deliberately NO BUILDER: a builder would be a place to put a value
 * nobody received. FC-P4.6's requester echoes the learned value through
 * the ENQ builder's own fields when it has one, and refuses to send a
 * lookup at all when it does not.
 *
 * SO FC-P4.6 DID EXACTLY THAT, AND NOTHING MORE. `struct
 * vms_dlm_enq_request` grew `dir_hash` + `dir_hash_valid`, and
 * vms_dlm_enq_request_build() writes body[10:12] ONLY when the flag is
 * set. There is still no `vms_dlm_dir_hash_build()`: the value cannot be
 * placed on the wire on its own, only as a field of a request whose lock
 * id, mode and resource name were already read out of a real LKB by
 * vms_lock.c's dlm_proxy_fill_post() -- which is also where `dir_hash`
 * itself comes from (res->hash16/res->hash_known, learned from a received
 * frame and never computed).
 *
 * OFFSET PROVENANCE -- INFERRED, pending FC-P4.2's offline confirmation.
 * The strawman daemon's op-01 builder placed a 16-bit `dir_hash` here
 * (`feat/coord-rebuild-completion:src/vmsscs/scs_member.c:852`) and a
 * real VAX accepted those registrations, which names the field but does
 * not prove it. FC-P4.2 confirms it offline from existing captures by
 * the two properties any hash field must have: constant per resource
 * name across senders and occurrences, and varying across names. Until
 * then this offset is INFERRED, and the consumer is built so that a
 * wrong offset SHOWS UP rather than corrupting anything: a learned value
 * that disagrees with a previously learned one for the same name is
 * counted (vms_lock.c `dir_hash_conflicts`), and every directory lookup
 * OVMX receives is checked against its own vector
 * (`dir_lookup_misaddressed`, vms_dlm_ldwv.h SS5).
 * ------------------------------------------------------------------ */
#define VMS_OFF_DLM_DIR_HASH      82u  /* body[10:12] LE u16, INFERRED     */
#define VMS_OFB_DLM_DIR_HASH    VMS_OFB_FROM_FRAME(VMS_OFF_DLM_DIR_HASH)

/*
 * Read the directory hash out of any cat-0x02 frame that carries it.
 * Returns VMS_CODEC_E_CLASS for a frame that is not a cat-0x02 SCS_MSG,
 * and the view's own error for a frame too short to hold the field.
 * `*out` is written only on VMS_CODEC_OK -- there is no "hash 0" fallback,
 * because "the frame did not carry one" and "the hash is 0" are different
 * facts and only one of them may be put on the wire (INV-6).
 */
vms_codec_status_t vms_dlm_dir_hash_parse(const uint8_t *frame, uint32_t len,
					  const struct vms_frame_info *fi,
					  uint16_t *out);

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
#define VMS_OFB_DLM_REBUILD_INV1 VMS_OFB_FROM_FRAME(VMS_OFF_DLM_REBUILD_INV1)
#define VMS_OFB_DLM_REBUILD_INV2 VMS_OFB_FROM_FRAME(VMS_OFF_DLM_REBUILD_INV2)
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
/* The body entry (rd vms-1ee); declared here, where the record type exists. */
vms_codec_status_t vms_dlm_rebuild_parse_body(const uint8_t *body, uint32_t len,
					      struct vms_dlm_rebuild_record *out);

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
 * op 0x03 $DEQ, op 0x04 BLKAST, op 0x06 CONVERT-with-VALBLK
 * -- GROUNDED, rd vms-fa7 / vms-002 / vms-858, capture set
 *    tests/lab/captures/vms-c03-dlm-opcodes-20260911/ (GROUNDING.md).
 *
 * ALL THREE REUSE THE ENQ HEADER POSITIONS. That is not an assumption
 * carried over from op 0x01: it is what the capture shows. Each frame's
 * body[24:28] is byte-identical to the `master_lkid` of the op-0x01 ENQ
 * that created the very lock the operation is about, and each frame's
 * body[20:24] is byte-identical to the requester handle the op-0x01
 * cat-0x82 GRANT assigned. So no new lock-id offsets are defined here --
 * defining a second spelling of body[20]/body[24] would be inventing a
 * disagreement. The offsets above ARE these offsets.
 *
 *   DEQ    (0x03) dlm-deq-20260911.pcap  f14  VAX1 -> vax2
 *          master_lkid 0x3a0004eb == the f12 ENQ for resource 'OVMXDEQ1'
 *          req_lkid    0x080001cd == the f13 GRANT's assigned handle
 *          body[30]    0x00 (NL)  -- the lock's mode as it is released
 *          NO RESOURCE NAME: body[46] does not hold the 0x03 name marker
 *          (it holds uninitialised bytes, 0x9a on this specimen and 0x00 on
 *          the second DEQ in the same capture -- which is itself the proof
 *          that it is not a field). A DEQ names its lock by lock-id.
 *
 *   BLKAST (0x04) dlm-blk2-20260911.pcap f58  vax2 -> VAX1 (master->holder)
 *          master_lkid 0x590004e3 == the f18 EX-holder ENQ for 'OVMXBLK2'
 *          req_lkid    0x0a0003af == the holder's own local handle
 *          body[30:32] OBSERVED 0x01,0x05 -- NOT PINNED, see below
 *          NO RESOURCE NAME: body[48] reads 'F11B$aSYSDSK1' and that is a
 *          STALE BUFFER, not a field (the frame's real resource,
 *          'OVMXBLK2', appears in the capture ONLY in the op-0x01 ENQ).
 *
 *          WHAT IS *NOT* CLAIMED ABOUT 0x04. The same captures contain
 *          op-0x04 frames in the OTHER direction carrying master_lkid 0
 *          (dlm-lvb3 f8/f11, dlm-blk2 f14/f17). Nothing correlates those
 *          to a lock, so this codec does not say what they are: the
 *          parser REFUSES a zero lock id rather than reporting "a BLKAST
 *          for lock 0", and the BLKAST semantics grounded here are
 *          exactly the master->holder case the capture drove and no more.
 *
 *   VALBLK (0x06) dlm-lvb3-20260911.pcap f14  VAX1 -> vax2
 *          master_lkid 0x2b000489 == the f12 ENQ for resource 'OVMXLVB3'
 *          body[36:52] == 'WROTEBYVAX1XXXXX', the exact 16 bytes the
 *          driver placed at LKSB+8 before converting EX->NL with
 *          LCK$M_VALBLK. That is the lock value block, byte for byte.
 * ------------------------------------------------------------------ */

/* body[36:52] (abs 108): the 16-byte lock value block on an op-0x06. */
#define VMS_OFF_DLM_VALBLK        108u
#define VMS_OFB_DLM_VALBLK      VMS_OFB_FROM_FRAME(VMS_OFF_DLM_VALBLK)
/* The LKSB's value block is 16 bytes (LKSB is 24; the block lives at
 * LKSB+8). Spelled locally because this header stays self-contained for the
 * pure host codec build -- vms_dlm_proxy.h states the same 16 for the
 * engine side. */
#define VMS_DLM_VALBLK_WIRE_LEN    16u

/*
 * op-0x06 CONVERT-with-VALBLK BUILD layout -- grounded vms-727 (own-lab
 * vaxlab-4, 5 real-wire captures c1..c5, byte-verified). The frame is a
 * cat-0x02 REQUEST, VAX1->master, holder converting its lock DOWN and
 * flushing the value block. Fields, by body offset (body[N] = abs 72+N):
 *
 *   body[12:14] = 0x0001, body[14:16] = 0x0002   -- op-0x06 header words,
 *       constant in every capture (distinct from the rebuild op's 0x0001/
 *       0x0003 pair; grounded per-op, not shared).
 *   body[28]    = 0x13                            -- op-0x06 request flag,
 *       constant (an ENQ request carries 0x11 here; op-0x06 carries 0x13).
 *   body[30]    = mode                            -- the mode converted TO
 *       (0x00/NL on the captured EX->NL convert-down); the same body[30]
 *       lock-mode field the ENQ/DEQ builders already write.
 *   body[32]    = SERIAL  -- a per-LOCK request serial the executive assigns
 *       at ENQ and carries through every frame for that lock (the ENQ
 *       REQUEST for the same lock shows the identical body[32:36]). INFERRED
 *       provenance: equals the low byte of the lock's ENQ request id in
 *       both captures that expose it (0x2020021b->0x1b, 0x2020021f->0x1f;
 *       2/2). Re-stamped at body[52] (front==back). Sourced from the LKB;
 *       it is NOT resource/mode/name/valblk/per-write derived (proven: it
 *       is constant across three writes to one held lock, and advances only
 *       across distinct lock instances).
 *   body[34]    = 0x01    -- the cat-0x02 REQUEST stamp at the RESULT_STAMP
 *       position (a REPLY carries 0xfa/0xf9 here; a request carries 0x01,
 *       seen identically on the ENQ request for the same lock).
 *   body[36:52] = the 16-byte value block (VMS_OFF_DLM_VALBLK, above).
 *   body[52]    = SERIAL (== body[32]); body[53] = 0x02; body[54:56] = 0x2020
 *       -- the closing bracket, constant across all five varied captures
 *       (so a stable field, NOT stale buffer).
 *   body[56:88] = uninitialised sender buffer on the real wire (VAX P1 stack
 *       addresses 0x7ff8...., inconsistent between captures) -- NOT a field.
 *       The builder ZERO-FILLS this span rather than emit our own stack: an
 *       honest omission, never a minted or leaked value.
 *
 * SAFETY: the ENQ builder does not populate body[32:36] at all, yet peers
 * accept our ENQ frames (the cross-node proof holds), so body[32:36] is not
 * receiver-correctness-critical -- a SERIAL sourced from the LKB (or zero)
 * cannot bugcheck a peer (INV: never-crash-a-peer).
 */
#define VMS_OFF_DLM_VALBLK_HDR1   84u  /* body[12:14] LE u16, const 0x0001 */
#define VMS_OFF_DLM_VALBLK_HDR2   86u  /* body[14:16] LE u16, const 0x0002 */
#define VMS_OFF_DLM_VALBLK_FLAG  100u  /* body[28]    u8,     const 0x13   */
#define VMS_OFF_DLM_VALBLK_SERIAL 104u /* body[32]    u8,  per-lock SERIAL */
#define VMS_OFF_DLM_VALBLK_REQSTAMP 106u /* body[34]  u8,     const 0x01   */
#define VMS_OFF_DLM_VALBLK_SERIAL2 124u /* body[52]   u8,  == body[32]     */
#define VMS_OFF_DLM_VALBLK_TAG2  125u  /* body[53]    u8,     const 0x02   */
#define VMS_OFF_DLM_VALBLK_PAD   126u  /* body[54:56] two bytes, const 0x20*/
#define VMS_DLM_VALBLK_HDR1_VAL   0x0001u
#define VMS_DLM_VALBLK_HDR2_VAL   0x0002u
#define VMS_DLM_VALBLK_FLAG_VAL   0x13u
#define VMS_DLM_VALBLK_REQSTAMP_VAL 0x01u
#define VMS_DLM_VALBLK_TAG2_VAL   0x02u
#define VMS_DLM_VALBLK_PAD_VAL    0x20u
#define VMS_OFB_DLM_VALBLK_SERIAL  VMS_OFB_FROM_FRAME(VMS_OFF_DLM_VALBLK_SERIAL)
/* Full op-0x06 body length on the wire (through the stale-buffer tail the
 * real sender pads to). The builder writes the grounded fields and zero-fills
 * to here. */
#define VMS_DLM_VALBLK_BODY_LEN   88u

/*
 * body[30:32] (abs 102): the BLKAST's mode-context pair.
 *
 * OBSERVED, NOT PINNED, and deliberately kept distinct from the GROUNDED
 * lock-mode byte that shares body[30] on an ENQ/CONVERT/DEQ. Two BLKAST
 * frames for the contended lock read 0x01,0x05; a third, for a different
 * (F11B$a) lock, read 0x01,0x00. Three samples across two locks is not a
 * one-variable diff, so this codec will not claim to know what the pair
 * means. It is carried, labelled, and opt-in on the builder.
 */
#define VMS_OFF_DLM_BLKAST_MODE_CTX      VMS_OFF_DLM_MODE
#define VMS_OFB_DLM_BLKAST_MODE_CTX      VMS_OFB_DLM_MODE
#define VMS_DLM_BLKAST_MODE_CTX_LEN       2u

/*
 * A cross-node lock RELEASE (op 0x03). Both lock ids are REQUIRED to be
 * real: this codec refuses VMS_DLM_LKID_UNSET in either, on the parse side
 * as well as the build side. A release naming lock 0 is not a release, and
 * a peer that acted on one would be acting on nothing.
 */
struct vms_dlm_deq {
	uint32_t req_lkid;     /* body[20:24]: our own handle, post-grant   */
	uint32_t master_lkid;  /* body[24:28]: the master's handle          */
	uint8_t  mode;         /* body[30]:    the mode being released      */
};

/*
 * A blocking AST (op 0x04), master -> the remote holder whose lock is in
 * the way. It identifies its lock by lock-id and by NOTHING else -- there
 * is no name field here, on purpose (see the section comment).
 */
struct vms_dlm_blkast {
	uint32_t req_lkid;     /* body[20:24]: the HOLDER's local handle    */
	uint32_t master_lkid;  /* body[24:28]: the blocked lock's master id */

	/* body[30:32] -- OBSERVED, NOT PINNED. On a parse these are simply
	 * the two bytes the peer sent. On a BUILD they are written ONLY when
	 * `mode_ctx_valid` is set, which a caller may set only if it holds
	 * real executive mode values for them; otherwise the builder leaves
	 * the span untouched, the same honest-omission rule the directory
	 * hash follows. */
	uint8_t  mode_ctx[VMS_DLM_BLKAST_MODE_CTX_LEN];
	uint8_t  mode_ctx_valid;
};

/*
 * The value block an op-0x06 CONVERT carries, plus the lock it belongs to.
 * Both a PARSE target (read what a peer sent) and, since vms-727, a BUILD
 * source (emit our own holder's value-block flush on a cross-node convert-
 * down). `serial` is the per-lock request serial at body[32]==body[52]
 * (see the op-0x06 BUILD layout comment above); on a parse it is the byte
 * the peer sent, on a build it is sourced from the emitting LKB.
 */
struct vms_dlm_valblk_convert {
	uint32_t req_lkid;     /* body[20:24]                               */
	uint32_t master_lkid;  /* body[24:28]                               */
	uint8_t  mode;         /* body[30]: the mode converted TO           */
	uint8_t  serial;       /* body[32]==body[52]: per-lock SERIAL (INFERRED)*/
	uint8_t  valblk[VMS_DLM_VALBLK_WIRE_LEN];  /* body[36:52]           */
};

/*
 * Parse an op-0x03 $DEQ. Refuses anything that is not a cat-0x02 request
 * with opcode 0x03, and refuses a frame whose `master_lkid` or `req_lkid`
 * is VMS_DLM_LKID_UNSET -- the capture contains real cat-0x02 frames with
 * a zero there, and "the peer sent zero" and "the peer named a lock" are
 * different facts. `*out` is written only on VMS_CODEC_OK.
 */
vms_codec_status_t vms_dlm_deq_parse_body(const uint8_t *body, uint32_t len,
					  struct vms_dlm_deq *out);
vms_codec_status_t vms_dlm_deq_parse(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     struct vms_dlm_deq *out);

/*
 * Build an op-0x03 $DEQ into `frame` at its abs offsets (the same division
 * of labour as vms_dlm_enq_request_build: this writes the DLM body span
 * only). REFUSES (VMS_CODEC_E_INVAL) if either lock id is
 * VMS_DLM_LKID_UNSET -- the fc8540ae hard lesson, unchanged by the
 * supersession: it was a lock-id field that bugchecked a real VAX with
 * INVLOCKID, and a release is a lock-id-only message, so it is ALL such
 * fields. The caller must have read both ids off the executive's own lock
 * record; this check refuses only the one value that structurally cannot
 * be a real assigned handle.
 */
vms_codec_status_t vms_dlm_deq_build(const struct vms_dlm_deq *d,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written);

/* Parse an op-0x04 BLKAST. Same lock-id refusal as the DEQ parser. */
vms_codec_status_t vms_dlm_blkast_parse_body(const uint8_t *body, uint32_t len,
					     struct vms_dlm_blkast *out);
vms_codec_status_t vms_dlm_blkast_parse(const uint8_t *frame, uint32_t len,
					const struct vms_frame_info *fi,
					struct vms_dlm_blkast *out);

/* Build an op-0x04 BLKAST. Same lock-id refusal as the DEQ builder; the
 * OBSERVED mode-context pair rides only when `b->mode_ctx_valid` is set. */
vms_codec_status_t vms_dlm_blkast_build(const struct vms_dlm_blkast *b,
					uint8_t *frame, uint32_t cap,
					uint32_t *written);

/*
 * Read the lock value block an op-0x06 CONVERT carried. Same cat/op gate and
 * same lock-id refusal as the two parsers above.
 */
vms_codec_status_t
vms_dlm_valblk_convert_parse_body(const uint8_t *body, uint32_t len,
				  struct vms_dlm_valblk_convert *out);
vms_codec_status_t
vms_dlm_valblk_convert_parse(const uint8_t *frame, uint32_t len,
			     const struct vms_frame_info *fi,
			     struct vms_dlm_valblk_convert *out);

/*
 * Build an op-0x06 CONVERT-with-VALBLK (vms-727). Refuses VMS_DLM_LKID_UNSET
 * in either lock id, exactly like the DEQ/BLKAST builders. Writes the
 * grounded op-0x06 fields (see the BUILD layout comment) from `c`, sourcing
 * the per-lock SERIAL at body[32]==body[52] from `c->serial`, and ZERO-FILLS
 * body[56:88] rather than emit sender-buffer garbage. `*written` is the full
 * VMS_DLM_VALBLK_BODY_LEN-based frame length. Never populates body[10:12]:
 * the value-block convert is routed by master_lkid, not by a directory hash.
 */
vms_codec_status_t vms_dlm_valblk_convert_build(const struct vms_dlm_valblk_convert *c,
						uint8_t *frame, uint32_t cap,
						uint32_t *written);

/* ------------------------------------------------------------------ *
 * The (SYSAP, category, opcode) allowlist rows this item contributes
 * (vms_cluster_codec.h §6). Every GROUNDED op is listed and nothing else:
 * a row asserts "grounded in the reference", so the three vms-c03 ops join
 * the table now that a real cluster's own frames ground them, and each
 * cites the capture that did it. They are CONSUME, not RESPOND: the
 * capture contains no cat-0x82 reply to any of the three -- a real VMS
 * master never answers a $DEQ, a BLKAST or a value-block convert -- and a
 * RESPOND row would be claiming a response recipe nobody has seen.
 * ------------------------------------------------------------------ */
extern const struct vms_wire_allow_entry vms_dlm_allow_rows[];
extern const struct vms_wire_allow_table vms_dlm_allow_table;

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_DLM_H */
