/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec_scs.h - SCS connection/directory typed codec entries
 * (plan item FC-P2.1; design docs/design-faithful-cluster-executive.md P2;
 * wire spec docs/cluster-protocol-spec.md sec 4(h) "SCS$DIRECTORY connect +
 * directory-lookup (0x5b) + credit-return (0x48)" and sec 4(m) "SCS
 * connection lifecycle -- the op verb set at abs 60").
 *
 * Layered on the FROZEN vms_cluster_codec.{c,h} (FC-P0.6) exactly as
 * vms_cluster_codec_hello.{c,h} (FC-P0.7) is: its own header/source pair,
 * the shared TU untouched. `vms_frame_classify()` already tells this file
 * the frame's FAMILY (VMS_FFAM_SCS) and its msgtype/seq capability; this
 * file supplies its OWN offset constants for the connection-control body
 * the same way the HELLO file supplies VMS_OFF_HELLO_* -- per design sec
 * 3.9 rule 2, no raw byte offset outside a codec TU, and this is one.
 *
 * THE MTYPE ENVELOPE (sec 4(h)(1b), GROUNDED across every SCS length class):
 * every SCS message -- the connection-control short classes here, the
 * 94-content MSCP/directory class, and the 190-content SYSAP class -- carries
 * inner-length [42:44], a constant format word 0x0004 [44:46], the message
 * TYPE (the "op" verb, sec 4(m)) [46:48], credit [48:50], and the Con.ID
 * handle pair [50:58] -- all payload-relative; add 14 for absolute. This file
 * types that envelope once, as `struct vms_scs_ctrl_frame`, and reuses it for
 * every op 0-10 length class (58/62/66/94/110 SCA content) plus the thin
 * directory-lookup semantic layer sec 4(h)(2) grounds on top of the 94-byte
 * op-10 shape.
 *
 * THE OP-VERB NAMING (sec 4(m) + the independent $SCSDEF confirmation, sec
 * "PPD, Inappropriate SCA Control Message"): OpenVMS's own shipped
 * LIB.MLB/$SCSDEF macro -- a published `$xxxDEF` definition macro, admissible
 * clean-room source under CLAUDE.md rule 8 -- names the enum CON_REQ 0 /
 * CON_RSP 1 / ACCP_REQ 2 / ACCP_RSP 3 / REJ_REQ 4 / REJ_RSP 5 / DISC_REQ 6 /
 * DISC_RSP 7 / CR_REQ 8 / CR_RSP 9 / APPL_MSG 10 / APPL_DG 11. That resolves
 * the campaign's earlier op-4/5 naming collision (vms-754/vms-a58's
 * "CONFIRM5"/"MSCP accept" readings, kept only in code-comment history) in
 * favour of REJECT_REQ/REJECT_RSP, and it is why this file adds the missing
 * VMS_SCS_CTRL_REJECT_RSP(5)/DISCONNECT_RSP(7)/CREDIT_REQ(8)/CREDIT_RSP(9)
 * constants the shared header does not yet carry -- the shared header stops
 * at 0/1/2/3/4/6/10 (FC-P0.6's own harvest), this item's job is 5/7/8/9.
 *
 * THE DIRECTORY LOOKUP (sec 4(h)(2)): riding the op-10 (APPL_MSG), 94-byte
 * SCA-content shape, the body past the Con.ID pair carries two 16-byte ASCII
 * fields -- the queried SYSAP name, then a result: all-zero in a request, the
 * literal "NOT PRESENT HERE" in a negative response, or (sec 4(h)(2) RE gap
 * (c)) an opaque per-connection descriptor in an affirmative response whose
 * INTERNAL semantics are not grounded. The same two 16-byte fields, at the
 * same offsets, are also the (destination, source) SYSAP name pair on a
 * 110-byte CONNECT_REQ/CONNECT_RESPONSE (op 0/2) -- one generic
 * "name pair" body, two different callers' readings of it, matching how
 * `struct vms_scs_ctrl_frame` names the fields generically (name1/name2) and
 * `struct vms_scs_dir_lookup` is the directory-specific reading layered on
 * top, never baked into the generic parse.
 *
 * CALLER-SUPPLIED IDENTITY, no baked capture constants (INV-6, mirroring the
 * vms_hello_build() precedent): Con.ID pair, credit, the marker/reason bytes,
 * every name/result field and the sequence counters are ALWAYS the caller's
 * values. The only bytes this file bakes in are the discovery-independent
 * format markers already GROUNDED as protocol constants -- the format word
 * 0x0004 at [44:46] -- exactly as vms_hello_build() bakes in the discovery
 * prefix/suffix and nothing else.
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_SCS_H
#define OVMX_VMS_CLUSTER_CODEC_SCS_H

#include "vms_cluster_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * The op-verb constants the shared header does not carry (sec 4(m) +
 * the $SCSDEF independent confirmation). 0/1/2/3/4/6/10 already live in
 * vms_cluster_codec.h as VMS_SCS_CTRL_*; these are the rest.
 * ------------------------------------------------------------------ */
#define VMS_SCS_CTRL_REJECT_RSP     5u  /* $SCSDEF REJ_RSP           */
#define VMS_SCS_CTRL_DISCONNECT_RSP 7u  /* $SCSDEF DISC_RSP          */
#define VMS_SCS_CTRL_CREDIT_REQ     8u  /* $SCSDEF CR_REQ            */
#define VMS_SCS_CTRL_CREDIT_RSP     9u  /* $SCSDEF CR_RSP            */

/* ------------------------------------------------------------------ *
 * The MTYPE envelope offsets this item teaches the codec (sec 4(h)(1b),
 * payload-relative -14 of each; abs = payload + 14).
 * ------------------------------------------------------------------ */
#define VMS_OFF_SCSCTRL_INCARN     36u  /* payload[22], node-incarn echo   */
/*
 * abs 38-55 (payload[24:42]) -- the "counter mirror" span sec 4(h)(4)
 * documents: a SYSGEN-tunable constant, the recv_ack/send_seq values
 * REPEATED (not independent data -- GROUNDED 100% equal to recv_ack/
 * send_seq above in every specimen this item cites), and two further
 * observed-constant words. Named here because a builder MUST write every
 * wire byte (an uninitialised span is not an honest omission, it is a
 * poisoned frame); the mirrors are DERIVED from recv_ack/send_seq (no
 * separate struct field), the three genuinely independent words are
 * caller-supplied fields below (never baked -- same discipline as
 * vms_hello_build()'s cap_span/reserved_64).
 */
#define VMS_OFF_SCSCTRL_LANOVRHD   38u  /* payload[24], SYSGEN NISCS_LAN_OVRHD */
#define VMS_OFF_SCSCTRL_ACKMIRROR1 40u  /* payload[26], == recv_ack (derived) */
#define VMS_OFF_SCSCTRL_ZERO1      42u  /* payload[28], GROUNDED zero          */
#define VMS_OFF_SCSCTRL_SEQMIRROR  44u  /* payload[30], == send_seq (derived) */
#define VMS_OFF_SCSCTRL_ZERO2      46u  /* payload[32], GROUNDED zero          */
#define VMS_OFF_SCSCTRL_ACKMIRROR2 48u  /* payload[34], == recv_ack (derived) */
#define VMS_OFF_SCSCTRL_ZERO3      50u  /* payload[36], GROUNDED zero          */
#define VMS_OFF_SCSCTRL_TAILCONST1 52u  /* payload[38], observed constant     */
#define VMS_OFF_SCSCTRL_TAILCONST2 54u  /* payload[40], observed constant     */
#define VMS_OFF_SCSCTRL_INNERLEN   56u  /* payload[42], content - 44       */
#define VMS_OFF_SCSCTRL_FMTWORD    58u  /* payload[44], GROUNDED 0x0004    */
#define VMS_OFF_SCSCTRL_CREDIT     62u  /* payload[48], CALLER-supplied    */
#define VMS_OFF_SCSCTRL_MARKER     72u  /* payload[58], 4 bytes            */
#define VMS_OFF_SCSCTRL_TAIL4      76u  /* payload[62], 66-content only    */
#define VMS_OFF_SCSCTRL_NAME1      76u  /* payload[62], 16 bytes           */
#define VMS_OFF_SCSCTRL_NAME2      92u  /* payload[78], 16 bytes           */
#define VMS_OFF_SCSCTRL_BLANK     108u  /* payload[94], 110-content only   */

#define VMS_SCSCTRL_FMTWORD_CONST 0x0004u
#define VMS_SCSCTRL_NAME_LEN        16u

/* The five GROUNDED SCA content lengths this class family occupies. */
#define VMS_SCSCTRL_LEN_SHORT      58u  /* ops 5/7/8/9: envelope only      */
#define VMS_SCSCTRL_LEN_MARKER     62u  /* ops 3/4/6: + reason/match word  */
#define VMS_SCSCTRL_LEN_ECHO       66u  /* op 1: + a 4-byte name fragment  */
#define VMS_SCSCTRL_LEN_LOOKUP     94u  /* op 10 directory: + name pair    */
#define VMS_SCSCTRL_LEN_CONNECT   110u  /* ops 0/2: + name pair + blanks   */

/*
 * The connection-control / directory body, one struct for every op 0-10
 * length class. `has_*` records which optional spans this SPECIFIC frame
 * carries -- set by vms_scs_ctrl_parse() from the wire length, and read by
 * vms_scs_ctrl_build() to decide what to emit and what SCA content length to
 * assert. Exactly one combination is valid per VMS_SCSCTRL_LEN_* (see the
 * .c file); vms_scs_ctrl_build() refuses any other.
 */
struct vms_scs_ctrl_frame {
	struct vms_sca_hdr hdr;       /* abs 0-31                          */
	uint16_t recv_ack;            /* abs 32                            */
	uint16_t send_seq;            /* abs 34                            */
	uint16_t incarn;               /* abs 36, node-incarnation echo     */
	uint16_t lan_ovrhd;             /* abs 38, SYSGEN NISCS_LAN_OVRHD,
					  * CALLER-supplied (typically 18)    */
	uint16_t tail_const1;           /* abs 52, observed-constant word,
					  * CALLER-supplied, never baked      */
	uint16_t tail_const2;           /* abs 54, observed-constant word,
					  * CALLER-supplied, never baked      */
	uint16_t inner_len;            /* abs 56                            */
	uint16_t fmt_word;              /* abs 58, GROUNDED 0x0004           */
	uint16_t op;                    /* abs 60, VMS_SCS_CTRL_* verb       */
	uint16_t credit;                /* abs 62, CALLER-supplied           */
	uint32_t conid_remote;          /* abs 64                            */
	uint32_t conid_local;           /* abs 68                            */

	uint8_t  has_marker;            /* 1 iff content >= 62               */
	uint8_t  marker[4];             /* abs 72-75: reason/matching word.
					  * Only op 6's marker[2:4] (the
					  * DISC_REQ matching flag, sec
					  * 4(h)(1b)) has grounded semantics;
					  * every other op's marker is real
					  * wire data with no asserted
					  * meaning -- caller-supplied both
					  * ways, never baked.               */

	uint8_t  has_tail4;             /* 1 iff content == 66 (op 1)        */
	uint8_t  tail4[4];               /* abs 76-79: op 1's truncated
					  * 4-byte name fragment (sec 4(h)(1),
					  * "SCS$" observed) -- no semantics
					  * asserted beyond "real bytes".     */

	uint8_t  has_names;              /* 1 iff content == 94 or 110        */
	uint8_t  name1[VMS_SCSCTRL_NAME_LEN]; /* abs 76-91: target/queried
						* SYSAP name (payload[62:78])      */
	uint8_t  name2[VMS_SCSCTRL_NAME_LEN]; /* abs 92-107: source SYSAP
						* name (110) or lookup result (94) */

	uint8_t  has_blank;              /* 1 iff content == 110              */
	/*
	 * abs 108-123 (SCA content [94:110]). FC-P2.1 named this an "observed
	 * blank trailer"; sec 4(N) IDENTIFIES it -- "GROUNDED location +
	 * census" -- as the 16-byte SCA CONNECT DATA of p. 2-25, which the
	 * initiating SYSAP supplies on a CONNECT_REQ and the target SYSAP on an
	 * ACCEPT_REQ, and which two Connection Managers use to tell each other
	 * which VMS version they are. The field name is kept (it is an ABI
	 * other FC items already compile against) but the reading is corrected
	 * here: it is SYSAP data passed through uninterpreted, not padding, and
	 * FC-P2.2 delivers it to scs_sysap_ops.connect_req as `conndata`.
	 */
	uint8_t  blank[VMS_SCSCTRL_NAME_LEN];
};

/*
 * vms_scs_ctrl_parse - decode any op 0-10 connection-control/directory frame
 * into typed fields. Requires the frame to classify as VMS_FFAM_SCS with a
 * connection-phase msgtype (0x4b/0x5b/0x7b, sec 4(m) "the msgtype phase
 * rule"); refuses (VMS_CODEC_E_CLASS) any other family. Refuses
 * (VMS_CODEC_E_SHORT) an SCA content length outside the five GROUNDED
 * classes above -- honest omission over guessing at an unrecognised shape.
 */
vms_codec_status_t vms_scs_ctrl_parse(const uint8_t *frame, uint32_t len,
				      const struct vms_frame_info *fi,
				      struct vms_scs_ctrl_frame *out);

/*
 * vms_scs_ctrl_build - encode from *f. The SCA content length (and so which
 * optional spans are written) is derived from f's has_* flags, never from a
 * caller-supplied length -- exactly one combination is legal (58/62/66/94/
 * 110); any other combination of has_* flags is VMS_CODEC_E_INVAL. Bakes in
 * ONLY the format word (abs 58, GROUNDED constant 0x0004); every other byte,
 * including the marker/reason word, comes from *f.
 */
vms_codec_status_t vms_scs_ctrl_build(const struct vms_scs_ctrl_frame *f,
				      uint8_t *frame, uint32_t cap,
				      uint32_t *written);

/* ------------------------------------------------------------------ *
 * THE BODY-LEVEL SCS HEADER (abs 56-71) -- design SS3.2.4's E1 seam
 * ------------------------------------------------------------------ *
 *
 * WHY THIS EXISTS ALONGSIDE vms_scs_ctrl_build(). The two SCS transmit
 * paths have different SHAPES, because the wire does:
 *
 *  - the connection-control verbs (ops 0-9) occupy the SHORT SCA classes
 *    58/62/66/110, which no other layer can pre-build, so SCS builds the
 *    WHOLE frame through vms_scs_ctrl_build() and hands it to the port's
 *    FRAME-level primitive (pe_vc_send_frame), which stamps the sequence;
 *  - an APPLICATION MESSAGE (MTYPE 10) rides the fixed 190-content class
 *    the design's byte-ownership table splits three ways -- port 0-55,
 *    SCS 56-71, SYSAP 72-203. There SCS owns exactly 16 bytes, and E1's
 *    ruling is that it writes exactly those and no others.
 *
 * So this pair is the 16-byte half of that table, expressed as a typed
 * build/parse like every other entry in this file, rather than as six
 * `body[n] = ...` stores in vms_scs_fsm.c -- design SS3.9 rule 2, "no raw
 * byte offset outside a codec TU", which is exactly the rule that keeps
 * FC-P2.2's FSM readable and its offsets in one place.
 *
 * The offsets are DERIVED from the frame-absolute ones above by
 * subtracting VMS_OFF_SCSCTRL_INNERLEN (= 56), not re-stated: the two
 * views of the same six fields cannot drift apart.
 */
#define VMS_SCS_HDR_LEN 16u   /* abs 56..71 inclusive */

#define VMS_OFF_SCSHDR_INNERLEN (VMS_OFF_SCSCTRL_INNERLEN - VMS_OFF_SCSCTRL_INNERLEN) /*  0 */
#define VMS_OFF_SCSHDR_FMTWORD  (VMS_OFF_SCSCTRL_FMTWORD  - VMS_OFF_SCSCTRL_INNERLEN) /*  2 */
#define VMS_OFF_SCSHDR_MTYPE    (VMS_OFF_SCS_CTRL_TYPE    - VMS_OFF_SCSCTRL_INNERLEN) /*  4 */
#define VMS_OFF_SCSHDR_CREDIT   (VMS_OFF_SCSCTRL_CREDIT   - VMS_OFF_SCSCTRL_INNERLEN) /*  6 */
#define VMS_OFF_SCSHDR_CONID_R  (VMS_OFF_SCS_CONID_REMOTE - VMS_OFF_SCSCTRL_INNERLEN) /*  8 */
#define VMS_OFF_SCSHDR_CONID_L  (VMS_OFF_SCS_CONID_LOCAL  - VMS_OFF_SCSCTRL_INNERLEN) /* 12 */

struct vms_scs_hdr {
	uint16_t    inner_len;     /* SCA content - 44 (146 for the 190 class) */
	uint16_t    mtype;         /* enum scs_mtype / VMS_SCS_CTRL_* verb     */
	uint16_t    credit;        /* the CDT's Pending Receive Credit count,
				    * READ FROM THE LEDGER (p. 2-43/2-44,
				    * spec sec 4(h)(1c)/(1g)) -- never a
				    * constant, never copied frame to frame  */
	uint32_t    conid_remote;  /* abs 64: the DESTINATION endpoint's Con.ID
				    * (the peer's, from our CDT)             */
	uint32_t    conid_local;   /* abs 68: OUR OWN Con.ID for this CDT      */
};

/*
 * vms_scs_hdr_build - write the 16 bytes of abs 56-71 at out[0..15].
 * `cap` must be at least VMS_SCS_HDR_LEN. Bakes in ONLY the GROUNDED
 * format word 0x0004 (abs 58), exactly as vms_scs_ctrl_build() does;
 * every other byte comes from *h.
 */
vms_codec_status_t vms_scs_hdr_build(const struct vms_scs_hdr *h,
				     uint8_t *out, uint32_t cap);

/*
 * vms_scs_hdr_parse - the inverse, over a body/frame slice whose byte 0 is
 * abs 56. VMS_CODEC_E_SHORT if `len` is under VMS_SCS_HDR_LEN;
 * VMS_CODEC_E_CLASS if the format word is not the GROUNDED 0x0004 (which is
 * how a caller learns it is not looking at an SCS header at all, rather than
 * being handed sixteen plausible bytes).
 */
vms_codec_status_t vms_scs_hdr_parse(const uint8_t *in, uint32_t len,
				     struct vms_scs_hdr *out);

/*
 * vms_scs_hdr_parse_frame - the same, addressed by a WHOLE received frame.
 * The abs-56 slice is taken INSIDE this TU so no caller has to know the
 * number (design SS3.9 rule 2). This is the ONE envelope every SCS length
 * class shares (sec 4(h)(1b)), so it decodes an op-0 CONNECT_REQ, an op-7
 * short and a 190-content application message identically.
 */
vms_codec_status_t vms_scs_hdr_parse_frame(const uint8_t *frame, uint32_t len,
					   struct vms_scs_hdr *out);

/*
 * vms_scs_msg_body - point `*body` at the SYSAP's own bytes of a received
 * application message (frame-absolute 72 onward, design SS3.2.4's
 * "72-203 -> the emitting FSM/role") and set `*body_len` to how many are
 * present. VMS_CODEC_E_SHORT if the frame does not reach abs 72. No copy.
 */
vms_codec_status_t vms_scs_msg_body(const uint8_t *frame, uint32_t len,
				    const uint8_t **body, uint32_t *body_len);

/*
 * vms_scs_msg_body_build - assemble the BODY-LEVEL buffer an application
 * message is handed down as (design SS3.2.4's SCS<->port seam): this layer's
 * 16-byte header at [0:16] followed by the SYSAP's bytes at [16:...], zero
 * padded to `cap`. `cap` is the caller's whole body buffer (148 for the
 * grounded 190-content class). VMS_CODEC_E_INVAL if the SYSAP's bytes would
 * not fit -- never a silent truncation.
 */
vms_codec_status_t vms_scs_msg_body_build(const struct vms_scs_hdr *h,
					  const uint8_t *sysap_body,
					  uint32_t sysap_len,
					  uint8_t *out, uint32_t cap);

/*
 * The abs-22 connect-class word every connection-control frame carries
 * (spec sec 4(m), "Connect-class at abs 22": GROUNDED 0x0001; the 0x03e8 seen
 * in some fresh-formation captures is NOT accepted by an established member).
 */
#define VMS_SCSCTRL_CONNECT_FLAG 0x0001u

/* ------------------------------------------------------------------ *
 * Directory lookup (sec 4(h)(2)) -- the semantic reading of the op-10,
 * 94-content name pair. A thin layer over vms_scs_ctrl_frame: it asserts
 * nothing the generic parse does not already carry, it only NAMES the two
 * fields for this one caller (SCS$DIR_LOOKUP).
 * ------------------------------------------------------------------ */
enum vms_scs_dir_result {
	VMS_SCS_DIR_RESULT_EMPTY = 0,    /* request: result field all-zero  */
	VMS_SCS_DIR_RESULT_NOT_PRESENT,  /* response: literal negative      */
	VMS_SCS_DIR_RESULT_AFFIRMATIVE   /* response: opaque per-connection
					   * descriptor (sec 4h RE gap (c),
					   * internal semantics NOT grounded) */
};

struct vms_scs_dir_lookup {
	uint8_t queried_name[VMS_SCSCTRL_NAME_LEN]; /* payload[62:78]      */
	uint8_t result_kind;                        /* enum vms_scs_dir_result */
	uint8_t result[VMS_SCSCTRL_NAME_LEN];       /* meaningful iff
						      * AFFIRMATIVE; payload[78:94] */
};

/*
 * vms_scs_dir_lookup_parse - read the name/result pair off an already-parsed
 * 94-content connection-control frame (f->has_names && !f->has_blank).
 * VMS_CODEC_E_CLASS if f is not that shape (in particular a 110-content
 * CONNECT frame, whose name2 is a SYSAP name, not a lookup result, and must
 * not be read through this lens).
 */
vms_codec_status_t
vms_scs_dir_lookup_parse(const struct vms_scs_ctrl_frame *f,
			 struct vms_scs_dir_lookup *out);

/*
 * vms_scs_dir_lookup_build - the inverse: stamps f->name1/name2 and
 * f->has_names from *dl (f->has_blank cleared, f->has_marker/has_tail4 left
 * as the caller set them). Every other field of *f (op, credit, Con.ID pair,
 * sequence counters, the SCA header) is untouched -- the caller sets those
 * itself, exactly as it must for vms_scs_ctrl_build() directly.
 */
vms_codec_status_t
vms_scs_dir_lookup_build(const struct vms_scs_dir_lookup *dl,
			 struct vms_scs_ctrl_frame *f);

/* The GROUNDED negative-result literal (sec 4(h)(2), 16 bytes, no padding). */
extern const uint8_t vms_scs_dir_not_present_here[VMS_SCSCTRL_NAME_LEN];

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_SCS_H */
