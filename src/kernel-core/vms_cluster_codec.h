/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_codec.h - the OVMX VMScluster wire CODEC: pure build/parse of
 * SCA (ethertype 0x6007) frames (plan item FC-P0.6; design
 * docs/design-faithful-cluster-executive.md §3.2 "vms_cluster_codec.c", §3.9).
 *
 * WHAT THIS IS. The one place in the executive-resident cluster stack that is
 * allowed to know a byte offset. Design §3.9 coding rule 2:
 *
 *     "No raw byte offsets outside vms_cluster_codec.c; the codec exposes
 *      typed getters/setters per field. Two crashes came from body[N]
 *      arithmetic in orchestration code."
 *
 * Everything above (vms_pe, vms_scs, vms_cnxman, vms_dlm_scs, vms_mscp_*)
 * reads and writes wire fields ONLY through the accessors declared here. A
 * grep for `[0-9]\]` arithmetic on a frame buffer outside this TU is a bug.
 *
 * WHAT THIS IS NOT. No state, no allocation, no substrate calls, no time, no
 * logging. It compiles on a developer host with no kernel headers at all (that
 * is what makes rung R1 of the test ladder a sub-second loop) and into both
 * kmods unchanged.
 *
 * THE SOURCE OF TRUTH IS docs/cluster-protocol-spec.md, NEVER A CAPTURE.
 * Every offset below carries its spec section in a comment. Re-deriving an
 * offset from a pcap instead of the spec is how the campaign got three
 * mutually inconsistent field maps; don't.
 *
 * INV-6 (real data or honest omission) IS STRUCTURAL HERE, three ways:
 *   1. Reads are bounds-checked against a view with a STICKY error. A value
 *      returned from a view that has already failed is NOT data -- callers
 *      must gate on vms_wire_view_ok() before asserting any field on the wire
 *      or in a snapshot. Returning 0 from an out-of-range read is a parse
 *      failure, not a field value.
 *   2. Field accessors are CLASS-GATED by the capability bits the frame-class
 *      registry publishes. The Connection-ID pair, for instance, is GROUNDED
 *      only for the 190-byte SCS message class (spec §4(d)) and the
 *      connection-control classes (§4(g) phase 4, §4(h)(1)); asking any other
 *      frame for a Con.ID returns VMS_CODEC_E_CLASS rather than 4 bytes that
 *      merely live at that offset. The spec's own §4(d) caveat says other
 *      length classes "do not reliably match this layout"; the codec refuses
 *      instead of guessing.
 *   3. The (SYSAP, category, opcode) response table is an ALLOWLIST with no
 *      wildcard and no default (spec §4(p): "answer only (category, opcode)
 *      pairs grounded in the reference; for anything else send nothing and log
 *      it"). Answering an ungrounded pair with a full-body echo crashed two
 *      real VAXes (INCONSTATE / INVEXCEPTN).
 *
 * OFFSET CONVENTION. Every offset in this codec is ABSOLUTE FRAME offset:
 * byte 0 is the first byte of the Ethernet destination address, the SCA
 * payload starts at 14. This matches docs/cluster-protocol-spec.md's stated
 * convention. Where the spec quotes a payload-relative or SYSAP-body-relative
 * offset (its §4(g)/§4(h)/§4(j)/§4(f).1 tables do), the comment gives BOTH so
 * the translation is never done in the reader's head:
 *      abs = payload + 14 = body + 72
 */
#ifndef OVMX_VMS_CLUSTER_CODEC_H
#define OVMX_VMS_CLUSTER_CODEC_H

/*
 * Fixed-width integer vocabulary.
 *
 * This is the ONE substrate-conditional block the codec is allowed, and it
 * selects TYPE headers only -- never a facility header (no netdevice.h, no
 * mbuf.h, no skbuff). The codec deliberately does NOT include exec_kbackend.h:
 * design §3.9 rule 3 bars a substrate include from a codec/_fsm TU, and
 * exec_kbackend.h hard-errors when no kernel backend is selected, which would
 * make the host unit rung (R1) impossible. The FSM layers get their integer
 * types from exec_kbackend.h as §3.9's table allows; the codec, which must
 * build with NO kernel headers, provides its own.
 *
 * Fixed-width types come from the sanctioned single source vms_wire_types.h
 * (FC-P0.1 gate resolution: a wire TU includes that quoted kernel-core header,
 * not a bare <linux/types.h>; the substrate selection lives there only).
 */
#include "vms_wire_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Status
 * ------------------------------------------------------------------ */

typedef enum vms_codec_status {
	VMS_CODEC_OK = 0,
	VMS_CODEC_E_RANGE,   /* read/write outside the caller's buffer      */
	VMS_CODEC_E_NOTSCA,  /* ethertype is not 0x6007                     */
	VMS_CODEC_E_SHORT,   /* frame shorter than the class needs          */
	VMS_CODEC_E_CLASS,   /* field is not GROUNDED for this frame class  */
	VMS_CODEC_E_INVAL    /* caller argument                             */
} vms_codec_status_t;

const char *vms_codec_status_name(vms_codec_status_t st);

/* ------------------------------------------------------------------ *
 * §1  Bounded views and build buffers -- the typed get/put primitives
 * ------------------------------------------------------------------ */

/*
 * A read-only view over a received frame. `err` is STICKY: the first
 * out-of-range access latches it and every later read returns zero. A caller
 * that asserts a field on the wire, into a snapshot, or into a lock record
 * MUST check vms_wire_view_ok() first (INV-6: a zero from a failed view is not
 * data).
 */
typedef struct vms_wire_view {
	const uint8_t     *b;
	uint32_t           len;
	vms_codec_status_t err;
} vms_wire_view_t;

/*
 * A write view over a frame being built. Writes are position-addressed (wire
 * layouts are fixed-offset, not streaming). `len` is the high-water mark: the
 * length of the frame built so far. `err` is sticky in the same way.
 */
typedef struct vms_wire_buf {
	uint8_t           *b;
	uint32_t           cap;
	uint32_t           len;
	vms_codec_status_t err;
} vms_wire_buf_t;

void     vms_wire_view_init(vms_wire_view_t *v, const uint8_t *b, uint32_t len);
int      vms_wire_view_ok(const vms_wire_view_t *v);

uint8_t  vms_wire_get_u8(vms_wire_view_t *v, uint32_t off);
uint16_t vms_wire_get_le16(vms_wire_view_t *v, uint32_t off);
uint32_t vms_wire_get_le32(vms_wire_view_t *v, uint32_t off);
uint16_t vms_wire_get_be16(vms_wire_view_t *v, uint32_t off);
void     vms_wire_get_bytes(vms_wire_view_t *v, uint32_t off, uint32_t n,
			    uint8_t *dst);

void     vms_wire_buf_init(vms_wire_buf_t *w, uint8_t *b, uint32_t cap);
int      vms_wire_buf_ok(const vms_wire_buf_t *w);
uint32_t vms_wire_buf_len(const vms_wire_buf_t *w);

void     vms_wire_put_u8(vms_wire_buf_t *w, uint32_t off, uint8_t val);
void     vms_wire_put_le16(vms_wire_buf_t *w, uint32_t off, uint16_t val);
void     vms_wire_put_le32(vms_wire_buf_t *w, uint32_t off, uint32_t val);
void     vms_wire_put_be16(vms_wire_buf_t *w, uint32_t off, uint16_t val);
void     vms_wire_put_bytes(vms_wire_buf_t *w, uint32_t off, uint32_t n,
			    const uint8_t *src);
void     vms_wire_put_zero(vms_wire_buf_t *w, uint32_t off, uint32_t n);

/* ------------------------------------------------------------------ *
 * §2  Datalink / SCA framing constants (spec §2)
 * ------------------------------------------------------------------ */

#define VMS_SCA_ETHERTYPE        0x6007u  /* DEC SCA/LAVC, spec §2         */
#define VMS_ETH_HDR_LEN          14u      /* SCA payload starts here       */
#define VMS_ETH_ADDR_LEN         6u
#define VMS_ETH_MIN_FRAME        60u      /* runt padding target, spec §2  */

/* Absolute offsets shared by every SCA frame (spec §2, §4(a), §4(d)). */
#define VMS_OFF_ETH_DST          0u
#define VMS_OFF_ETH_SRC          6u
#define VMS_OFF_ETHERTYPE        12u
#define VMS_OFF_SCA_LEN          14u   /* payload[0]  LE u16, +2 == content */
#define VMS_OFF_DST_LAVC         16u   /* payload[2]  peer/group LOGICAL    */
#define VMS_OFF_CONNECT_FLAG     22u   /* payload[8]  constant 0x0001       */
#define VMS_OFF_SRC_LAVC         24u   /* payload[10] sender's own LOGICAL  */
#define VMS_OFF_WORD30           30u   /* payload[16] see below             */

/*
 * abs 30..31 is TWO different fields depending on the frame family, which is
 * exactly why it is exposed raw here and only interpreted through a
 * class-gated accessor:
 *   discovery family (HELLO/SOLICIT): abs 30 is the NISCA channel-verify
 *      request/response counter (a0 multicast, b2/b3/b4 directed, b6 solicit
 *      -- spec §4(a).1) and abs 31 is 0x00 (spec §4(k) payload[17]).
 *   SCS family: abs 30 is the message-type byte and abs 31 is the format
 *      constant 0x13 (spec §4(g), GROUNDED 2975/2975).
 */
#define VMS_OFF_SCS_MSGTYPE      30u   /* payload[16] */
#define VMS_OFF_SCS_FORMAT       31u   /* payload[17] */
#define VMS_OFF_SCS_RECV_ACK     32u   /* payload[18] spec §4(h)(4)        */
#define VMS_OFF_SCS_SEND_SEQ     34u   /* payload[20] mirrored at abs 44   */
#define VMS_OFF_SCS_CTRL_TYPE    60u   /* payload[46] spec §4(h)(1a)       */
#define VMS_OFF_SCS_CONID_REMOTE 64u   /* payload[50] spec §4(d), §4(g)ph4 */
#define VMS_OFF_SCS_CONID_LOCAL  68u   /* payload[54]                      */
#define VMS_OFF_SYSAP_BODY       72u   /* payload[58] == body[0], §4(j)    */

/* Discovery-family constants (spec §4(a)). */
#define VMS_OFF_DISC_PREFIX      32u   /* payload[18] const 08 00 00 80    */
#define VMS_OFF_DISC_CLASS       36u   /* payload[22] 0x05 HELLO/0x02 SOL  */
#define VMS_OFF_DISC_SUFFIX      37u   /* payload[23] const 01 00 00       */
#define VMS_DISC_CLASS_HELLO     0x05u
#define VMS_DISC_CLASS_SOLICIT   0x02u

/* SCS message-type byte values (spec §4(g) partition; labels inferred). */
#define VMS_SCS_MT_START         0x41u /* START / STACK / ACK              */
#define VMS_SCS_MT_CREDIT        0x48u /* credit-return short              */
#define VMS_SCS_MT_MSG           0x4bu /* sequenced application message    */
#define VMS_SCS_MT_SETUP         0x5bu /* connection-setup phase           */
#define VMS_SCS_MT_ALT           0x7bu /* §4(h) retransmit form of 0x5b;   */
				       /* §4(q) also sees mt 0x7b len 204  */
				       /* post-join, payload UNDECODED.    */
#define VMS_SCS_FORMAT_V13       0x13u /* GROUNDED 2975/2975, 622/622      */

/*
 * SCA connection-control message type at abs 60 (payload[46]) -- spec
 * §4(h)(1a), GROUNDED 60 frames / 16 dialogues / 0 residuals.
 */
#define VMS_SCS_CTRL_CONNECT_REQ    0u
#define VMS_SCS_CTRL_CONNECT_RSP    1u
#define VMS_SCS_CTRL_ACCEPT_REQ     2u
#define VMS_SCS_CTRL_ACCEPT_RSP     3u
#define VMS_SCS_CTRL_REJECT_REQ     4u
#define VMS_SCS_CTRL_DISCONNECT_REQ 6u
#define VMS_SCS_CTRL_APPLICATION   10u /* not a connection-control frame   */

/* ------------------------------------------------------------------ *
 * §3  Frame-class registry
 * ------------------------------------------------------------------ */

/*
 * The class taxonomy is the spec's own message-class census (§2 Table 2) plus
 * the msgtype partition (§4(g)). Each class names what the codec is entitled
 * to read out of a frame of that class; the capability bits are the INV-6
 * gate. A class is added here only when the spec grounds it -- an unrecognised
 * frame is VMS_FCLS_UNKNOWN and gets no typed accessors at all.
 */
enum vms_frame_class {
	VMS_FCLS_UNKNOWN = 0,
	VMS_FCLS_HELLO,         /* §4(a)+§4(b), SCA content 120             */
	VMS_FCLS_HELLO_PADDED,  /* §4(k), directed HELLO + zero pad         */
	VMS_FCLS_SOLICIT,       /* §4(c), SCA content 78                    */
	VMS_FCLS_SCS_START,     /* §4(g) ph2, mt 0x41 (START/STACK/ACK)     */
	VMS_FCLS_SCS_CREDIT,    /* §4(h)(3), mt 0x48, SCA content 41        */
	VMS_FCLS_SCS_CONN_CTRL, /* §4(g) ph4 + §4(h)(1)(1a)                 */
	VMS_FCLS_SCS_MSG,       /* §4(d), SCA content 190 (the VC class)    */
	VMS_FCLS_SCS_SEQ,       /* sequenced SCS msg, sub-class not grounded*/
	VMS_FCLS__COUNT
};

enum vms_frame_family {
	VMS_FFAM_NONE = 0,
	VMS_FFAM_DISCOVERY,  /* HELLO / SOLICIT share the §4(a) header      */
	VMS_FFAM_SCS         /* format-0x13 sequenced envelope              */
};

/* Capability bits: which typed accessors this class is GROUNDED for. */
#define VMS_FCAP_CHANWORD  0x01u  /* abs 30 is the channel-verify counter  */
#define VMS_FCAP_MSGTYPE   0x02u  /* abs 30/31 are msgtype + format 0x13   */
#define VMS_FCAP_SEQ       0x04u  /* abs 32/34 recv_ack / send_seq         */
#define VMS_FCAP_CONID     0x08u  /* abs 64/68 Con.ID pair (§4(d),§4(g))   */
#define VMS_FCAP_DISCNAME  0x10u  /* abs 40 length-prefixed node name      */

/*
 * The registry row. Public because the harvest items (FC-P0.7 HELLO, P1.1 VC,
 * P2.1 SCS, P3.1 CM, P4.5 DLM, P6.2 MSCP) each raise `harvest_len` for their
 * class as they teach the codec more typed fields, and the round-trip test
 * reads it back.
 */
struct vms_frame_class_info {
	uint8_t     cls;          /* enum vms_frame_class                    */
	uint8_t     family;       /* enum vms_frame_family                   */
	uint8_t     caps;         /* VMS_FCAP_*                              */
	uint16_t    min_len;      /* wire bytes needed to classify at all    */
	uint16_t    harvest_len;  /* leading bytes the codec BUILDS from     */
				  /* typed fields today (round-trip span)    */
	const char *name;         /* stable id used by fixtures and tests    */
	const char *spec;         /* the spec section that grounds the class */
};

/* Outcome of the §2 length identity: LE16(abs14) + 2 == SCA content. */
enum vms_sca_len_check {
	VMS_SCA_LEN_EXACT = 0,   /* 14 + content == wire length             */
	VMS_SCA_LEN_RUNT_PAD,    /* short frame zero-padded to 60 (spec §2)  */
	VMS_SCA_LEN_MISMATCH     /* neither -- 0 residuals in 24570 frames   */
};

struct vms_frame_info {
	uint8_t  cls;            /* enum vms_frame_class                    */
	uint8_t  family;         /* enum vms_frame_family                   */
	uint8_t  caps;           /* VMS_FCAP_* of the matched class         */
	uint8_t  len_check;      /* enum vms_sca_len_check                  */
	uint16_t sca_content;    /* LE16(abs 14) + 2                        */
	uint16_t wire_len;       /* as supplied by the caller               */
};

const struct vms_frame_class_info *vms_frame_class_lookup(uint8_t cls);
const struct vms_frame_class_info *vms_frame_class_by_name(const char *name);

/*
 * Classify a received frame. Returns VMS_CODEC_E_NOTSCA for a non-0x6007
 * frame and VMS_CODEC_E_SHORT for one too short to hold an SCA header; in
 * both cases `out` is still filled with what was determined (cls =
 * VMS_FCLS_UNKNOWN) so a caller can log honestly.
 */
vms_codec_status_t vms_frame_classify(const uint8_t *frame, uint32_t len,
				      struct vms_frame_info *out);

/* ------------------------------------------------------------------ *
 * §4  The shared SCA header -- typed parse / build
 * ------------------------------------------------------------------ */

/*
 * The span every SCA frame shares (abs 0..31): Ethernet header, SCA length
 * field, the destination and source cluster-LOGICAL LAVC addresses, the
 * connect flag, and the abs-30 word. Note abs 16 and abs 24 are LOGICAL
 * addresses (aa:00:04:00:<LE16(sysid)>), which on a directed frame are NOT
 * the Ethernet MACs at abs 0/6 -- mirroring abs 0 into abs 16 makes the peer
 * silently drop every reply (spec §4(a).0; it cost the campaign a 3-node lab
 * to find).
 */
struct vms_sca_hdr {
	uint8_t  eth_dst[VMS_ETH_ADDR_LEN];
	uint8_t  eth_src[VMS_ETH_ADDR_LEN];
	uint8_t  dst_lavc[VMS_ETH_ADDR_LEN];
	uint8_t  src_lavc[VMS_ETH_ADDR_LEN];
	uint16_t sca_len_field;  /* raw abs 14; content == this + 2         */
	uint16_t connect_flag;   /* abs 22, observed constant 0x0001        */
	uint16_t word30;         /* raw abs 30..31, LE                      */
};

/* The contiguous prefix vms_sca_hdr_build() writes, and the round-trip span. */
#define VMS_SCA_HDR_LEN 32u

vms_codec_status_t vms_sca_hdr_parse(const uint8_t *frame, uint32_t len,
				     struct vms_sca_hdr *out);
vms_codec_status_t vms_sca_hdr_build(const struct vms_sca_hdr *h,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written);

/* Content length the frame ASSERTS (abs 14 + 2), independent of wire length. */
uint16_t vms_sca_content_len(const struct vms_sca_hdr *h);

/* The §2 length identity, with the runt-padding rule. */
enum vms_sca_len_check vms_sca_len_check(uint16_t sca_content, uint32_t wire_len);

/* ------------------------------------------------------------------ *
 * §5  Class-gated field accessors
 * ------------------------------------------------------------------ *
 *
 * Each returns VMS_CODEC_E_CLASS -- and leaves *out untouched -- when the
 * frame's class does not GROUND that field. That refusal is the point: it is
 * the difference between reading a Con.ID and reading whatever four bytes
 * happen to sit at abs 64 of a HELLO.
 */

vms_codec_status_t vms_sca_chan_word(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     uint8_t *out);
vms_codec_status_t vms_sca_disc_class(const uint8_t *frame, uint32_t len,
				      const struct vms_frame_info *fi,
				      uint8_t *out);
vms_codec_status_t vms_scs_msgtype(const uint8_t *frame, uint32_t len,
				   const struct vms_frame_info *fi,
				   uint8_t *msgtype, uint8_t *format);
vms_codec_status_t vms_scs_seq(const uint8_t *frame, uint32_t len,
			       const struct vms_frame_info *fi,
			       uint16_t *recv_ack, uint16_t *send_seq);
vms_codec_status_t vms_scs_conid(const uint8_t *frame, uint32_t len,
				 const struct vms_frame_info *fi,
				 uint32_t *remote, uint32_t *local);
vms_codec_status_t vms_scs_ctrl_type(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     uint16_t *out);

/* ------------------------------------------------------------------ *
 * §6  The (SYSAP, category, opcode) response ALLOWLIST -- type only
 * ------------------------------------------------------------------ *
 *
 * Spec §4(p): "The rule is an allowlist, never a default: answer only
 * (category, opcode) pairs grounded in the reference; for anything else send
 * nothing and log it." OVMX once answered every unlisted pair with a
 * cat-0x01 full-body echo and crashed two real VAXes, because those request
 * bodies carry the PEER's live Con.IDs and cluster id.
 *
 * This item (FC-P0.6) ships the TYPE, the lookup and the table validator. The
 * grounded ROWS are data owned by the layer that speaks that SYSAP -- the CM
 * rows land with FC-P3.1, the DLM rows with FC-P4.5/P5.2. There is deliberately
 * no wildcard sentinel and no "default" row: vms_wire_allow_table_validate()
 * rejects a table that tries to invent one.
 */
enum vms_sysap_id {
	VMS_SYSAP_UNKNOWN = 0,
	VMS_SYSAP_VMS_VAXCLUSTER,   /* the connection manager's own SYSAP   */
	VMS_SYSAP_SCS_DIRECTORY,
	VMS_SYSAP_MSCP_DISK,
	VMS_SYSAP_VMS_DISK_CL_DRVR,
	VMS_SYSAP_SCA_TRANSPORT,
	VMS_SYSAP__COUNT
};

/* What this node is entitled to do with a received (sysap, cat, op). */
enum vms_wire_action {
	VMS_WIRE_ACT_NONE = 0,  /* not in the table: send NOTHING, log it   */
	VMS_WIRE_ACT_CONSUME,   /* grounded that we accept and never answer */
	VMS_WIRE_ACT_RESPOND    /* a grounded response recipe exists        */
};

/*
 * One allowlist row. `category` is the REQUEST form: bit 0x80 (the response
 * bit, spec §4(j)) must be clear. `recipe` is an opaque id the responding
 * layer maps to its own builder -- the codec never builds a response from a
 * template, and a RESPOND row with recipe 0 is rejected by the validator.
 * `spec` is mandatory: a row with no spec cite is by definition ungrounded.
 */
struct vms_wire_allow_entry {
	uint8_t     sysap;     /* enum vms_sysap_id, never UNKNOWN          */
	uint8_t     category;  /* body[8] request form (abs 80)             */
	uint8_t     opcode;    /* body[9] (abs 81)                          */
	uint8_t     action;    /* enum vms_wire_action, never NONE          */
	uint16_t    recipe;    /* layer-owned builder id (0 iff !RESPOND)   */
	const char *spec;      /* required spec cite                        */
};

struct vms_wire_allow_table {
	const struct vms_wire_allow_entry *rows;
	uint16_t                           n;
};

#define VMS_WIRE_RESPONSE_BIT 0x80u

int vms_wire_is_response(uint8_t category);
uint8_t vms_wire_response_category(uint8_t request_category);

/* NULL => not grounded => the caller must send nothing and log. */
const struct vms_wire_allow_entry *
vms_wire_allow_find(const struct vms_wire_allow_table *t,
		    uint8_t sysap, uint8_t category, uint8_t opcode);

/*
 * Structural validation of a table, run as a unit test by whichever item owns
 * the rows: no duplicate keys, no response-bit categories, no UNKNOWN sysap,
 * no NONE action, RESPOND iff recipe != 0, every row cites the spec.
 */
vms_codec_status_t
vms_wire_allow_table_validate(const struct vms_wire_allow_table *t);

/* ------------------------------------------------------------------ *
 * §7  Parser fuzz entry point
 * ------------------------------------------------------------------ *
 *
 * One call that drives every parse path in this TU over an arbitrary byte
 * string, for the host fuzz harness (tests/cluster/host/fuzz_codec_seed.c) and
 * for a libFuzzer LLVMFuzzerTestOneInput shim. It must never read outside
 * [d, d+n) and never assert a field from a failed view. Returns the class it
 * settled on so a corpus minimiser can measure coverage.
 */
uint8_t vms_cluster_codec_fuzz_one(const uint8_t *d, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_CLUSTER_CODEC_H */
