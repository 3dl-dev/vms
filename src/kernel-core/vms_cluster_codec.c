// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec.c - the ONLY translation unit in the cluster stack that
 * knows a wire byte offset (plan FC-P0.6; design §3.9 rule 2).
 *
 * Read vms_cluster_codec.h first: it carries the contract, the INV-6 posture
 * and the offset convention. This file is pure: no state, no allocation, no
 * substrate call, no libc call. The two byte-move helpers below exist so the
 * codec needs neither <string.h> nor <linux/string.h> and therefore compiles
 * unchanged on the host, in vms.ko and in the NetBSD SYSKRNL kmod.
 *
 * Every offset used here is a VMS_OFF_* constant from the header, each of
 * which carries its docs/cluster-protocol-spec.md citation. Do not open-code
 * a literal offset in this file either -- the constants are the audit trail.
 */

#include "vms_cluster_codec.h"

/* ------------------------------------------------------------------ *
 * Local byte moves (no libc, no kernel string.h)
 * ------------------------------------------------------------------ */

static void codec_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static void codec_zero(uint8_t *dst, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = 0;
}

static int codec_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

const char *vms_codec_status_name(vms_codec_status_t st)
{
	switch (st) {
	case VMS_CODEC_OK:       return "OK";
	case VMS_CODEC_E_RANGE:  return "E_RANGE";
	case VMS_CODEC_E_NOTSCA: return "E_NOTSCA";
	case VMS_CODEC_E_SHORT:  return "E_SHORT";
	case VMS_CODEC_E_CLASS:  return "E_CLASS";
	case VMS_CODEC_E_INVAL:  return "E_INVAL";
	}
	return "E_?";
}

/* ------------------------------------------------------------------ *
 * §1  Bounded views
 * ------------------------------------------------------------------ */

void vms_wire_view_init(vms_wire_view_t *v, const uint8_t *b, uint32_t len)
{
	v->b = b;
	v->len = (b != (const uint8_t *)0) ? len : 0;
	v->err = (b != (const uint8_t *)0) ? VMS_CODEC_OK : VMS_CODEC_E_INVAL;
}

int vms_wire_view_ok(const vms_wire_view_t *v)
{
	return v->err == VMS_CODEC_OK;
}

/* The single bounds gate every getter goes through. */
static int view_span_ok(vms_wire_view_t *v, uint32_t off, uint32_t n)
{
	if (v->err != VMS_CODEC_OK)
		return 0;
	if (off > v->len || n > v->len - off) {
		v->err = VMS_CODEC_E_RANGE;
		return 0;
	}
	return 1;
}

uint8_t vms_wire_get_u8(vms_wire_view_t *v, uint32_t off)
{
	if (!view_span_ok(v, off, 1))
		return 0;
	return v->b[off];
}

uint16_t vms_wire_get_le16(vms_wire_view_t *v, uint32_t off)
{
	if (!view_span_ok(v, off, 2))
		return 0;
	return (uint16_t)((uint16_t)v->b[off] | ((uint16_t)v->b[off + 1] << 8));
}

uint32_t vms_wire_get_le32(vms_wire_view_t *v, uint32_t off)
{
	if (!view_span_ok(v, off, 4))
		return 0;
	return (uint32_t)v->b[off] |
	       ((uint32_t)v->b[off + 1] << 8) |
	       ((uint32_t)v->b[off + 2] << 16) |
	       ((uint32_t)v->b[off + 3] << 24);
}

uint16_t vms_wire_get_be16(vms_wire_view_t *v, uint32_t off)
{
	if (!view_span_ok(v, off, 2))
		return 0;
	return (uint16_t)(((uint16_t)v->b[off] << 8) | (uint16_t)v->b[off + 1]);
}

void vms_wire_get_bytes(vms_wire_view_t *v, uint32_t off, uint32_t n,
			uint8_t *dst)
{
	if (!view_span_ok(v, off, n)) {
		codec_zero(dst, n);   /* never hand back uninitialised memory */
		return;
	}
	codec_copy(dst, v->b + off, n);
}

/* ------------------------------------------------------------------ *
 * §1b  Build buffers
 * ------------------------------------------------------------------ */

void vms_wire_buf_init(vms_wire_buf_t *w, uint8_t *b, uint32_t cap)
{
	w->b = b;
	w->cap = (b != (uint8_t *)0) ? cap : 0;
	w->len = 0;
	w->err = (b != (uint8_t *)0) ? VMS_CODEC_OK : VMS_CODEC_E_INVAL;
}

int vms_wire_buf_ok(const vms_wire_buf_t *w)
{
	return w->err == VMS_CODEC_OK;
}

uint32_t vms_wire_buf_len(const vms_wire_buf_t *w)
{
	return w->len;
}

static int buf_span_ok(vms_wire_buf_t *w, uint32_t off, uint32_t n)
{
	if (w->err != VMS_CODEC_OK)
		return 0;
	if (off > w->cap || n > w->cap - off) {
		w->err = VMS_CODEC_E_RANGE;
		return 0;
	}
	if (off + n > w->len)
		w->len = off + n;
	return 1;
}

void vms_wire_put_u8(vms_wire_buf_t *w, uint32_t off, uint8_t val)
{
	if (!buf_span_ok(w, off, 1))
		return;
	w->b[off] = val;
}

void vms_wire_put_le16(vms_wire_buf_t *w, uint32_t off, uint16_t val)
{
	if (!buf_span_ok(w, off, 2))
		return;
	w->b[off]     = (uint8_t)(val & 0xffu);
	w->b[off + 1] = (uint8_t)((val >> 8) & 0xffu);
}

void vms_wire_put_le32(vms_wire_buf_t *w, uint32_t off, uint32_t val)
{
	if (!buf_span_ok(w, off, 4))
		return;
	w->b[off]     = (uint8_t)(val & 0xffu);
	w->b[off + 1] = (uint8_t)((val >> 8) & 0xffu);
	w->b[off + 2] = (uint8_t)((val >> 16) & 0xffu);
	w->b[off + 3] = (uint8_t)((val >> 24) & 0xffu);
}

void vms_wire_put_be16(vms_wire_buf_t *w, uint32_t off, uint16_t val)
{
	if (!buf_span_ok(w, off, 2))
		return;
	w->b[off]     = (uint8_t)((val >> 8) & 0xffu);
	w->b[off + 1] = (uint8_t)(val & 0xffu);
}

void vms_wire_put_bytes(vms_wire_buf_t *w, uint32_t off, uint32_t n,
			const uint8_t *src)
{
	if (!buf_span_ok(w, off, n))
		return;
	codec_copy(w->b + off, src, n);
}

void vms_wire_put_zero(vms_wire_buf_t *w, uint32_t off, uint32_t n)
{
	if (!buf_span_ok(w, off, n))
		return;
	codec_zero(w->b + off, n);
}

/* ------------------------------------------------------------------ *
 * §3  Frame-class registry
 * ------------------------------------------------------------------ */

/*
 * A classification rule, expressed as DATA (design §3.9: tables, not switch
 * ladders). Rules are tried in order; the first whose every enabled criterion
 * matches wins. A later harvest item adds a class by adding a row here plus
 * its typed accessors -- never by adding a branch to the classifier.
 */
#define M_DISCCLASS 0x01u  /* abs 36 equals disc_class (implies discovery)  */
#define M_MSGTYPE   0x02u  /* abs 30 is one of mt[0..mt_n)                  */
#define M_LEN_EQ    0x04u  /* SCA content equals one of len[0..len_n)       */
#define M_LEN_GT    0x08u  /* SCA content strictly greater than len_gt      */
#define M_FIELD_NE  0x10u  /* LE16 at field_off differs from field_val      */
#define M_FIELD_EQ  0x20u  /* LE16 at field_off equals field_val (the same  */
			   /* field_off/field_val pair M_FIELD_NE reads --   */
			   /* EQ and NE are never both set on one rule)      */
#define M_FMTWORD   0x40u  /* LE16 at VMS_OFF_SCS_FMT_WORD == the GROUNDED  */
			   /* constant VMS_SCS_FMT_WORD_CONST (spec          */
			   /* §4(h)(1b)) -- the SCS envelope conformance test */
			   /* block-transfer frames fail (design §3.2.7)      */
#define M_INNERLEN_SELF 0x80u /* LE16 at VMS_OFF_SCS_INNER_LEN == SCA        */
			   /* content - VMS_SCS_INNER_LEN_BIAS (44) -- the   */
			   /* inner-length self-consistency check (design    */
			   /* §3.2.7); false (never a fair-test skip) if the */
			   /* content is too short to hold the bias          */

struct frame_class_rule {
	struct vms_frame_class_info info;
	uint8_t  match;
	uint8_t  disc_class;
	uint8_t  mt_n;
	uint8_t  mt[3];
	uint8_t  len_n;
	uint16_t len[4];
	uint16_t len_gt;
	uint16_t field_off;
	uint16_t field_val;
};

/*
 * min_len is the wire length needed to APPLY the rule:
 *   discovery rows read abs 32..39 (the 08 00 00 80 / class / 01 00 00
 *   sandwich, spec §4(a)) so they need 40 bytes;
 *   SCS rows read abs 30..31 so they need VMS_SCA_HDR_LEN.
 *
 * harvest_len is the contiguous prefix vms_sca_hdr_build() reproduces from
 * typed fields today; the round-trip test asserts exactly that span and
 * NOTHING beyond it. Every later codec-harvest item raises its class's
 * harvest_len and the same test re-runs -- that number is the honest measure
 * of how much of each frame class the executive actually understands.
 */
static const struct frame_class_rule g_rules[] = {
	{ { VMS_FCLS_HELLO, VMS_FFAM_DISCOVERY,
	    VMS_FCAP_CHANWORD | VMS_FCAP_DISCNAME,
	    40, VMS_SCA_HDR_LEN, "hello", "spec §4(a),§4(b)" },
	  M_DISCCLASS | M_LEN_EQ, VMS_DISC_CLASS_HELLO,
	  0, { 0, 0, 0 }, 1, { 120, 0, 0 }, 0, 0, 0 },

	{ { VMS_FCLS_HELLO_PADDED, VMS_FFAM_DISCOVERY,
	    VMS_FCAP_CHANWORD | VMS_FCAP_DISCNAME,
	    40, VMS_SCA_HDR_LEN, "hello-padded", "spec §4(k)" },
	  M_DISCCLASS | M_LEN_GT, VMS_DISC_CLASS_HELLO,
	  0, { 0, 0, 0 }, 0, { 0, 0, 0 }, 120, 0, 0 },

	{ { VMS_FCLS_SOLICIT, VMS_FFAM_DISCOVERY,
	    VMS_FCAP_CHANWORD | VMS_FCAP_DISCNAME,
	    40, VMS_SCA_HDR_LEN, "solicit", "spec §4(c)" },
	  M_DISCCLASS | M_LEN_EQ, VMS_DISC_CLASS_SOLICIT,
	  0, { 0, 0, 0 }, 1, { 78, 0, 0 }, 0, 0, 0 },

	{ { VMS_FCLS_SCS_START, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-start", "spec §4(g) ph2" },
	  M_MSGTYPE, 0,
	  1, { VMS_SCS_MT_START, 0, 0 }, 0, { 0, 0, 0 }, 0, 0, 0 },

	{ { VMS_FCLS_SCS_CREDIT, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-credit", "spec §4(h)(3)" },
	  M_MSGTYPE | M_LEN_EQ, 0,
	  1, { VMS_SCS_MT_CREDIT, 0, 0 }, 1, { 41, 0, 0 }, 0, 0, 0 },

	/*
	 * Connection-control: spec §4(h)(1a) grounds the Con.ID pair at abs
	 * 64/68 over the 110/66/62 content classes, EXCLUDING the application
	 * value 10 at abs 60 -- so length alone is not the discriminator and
	 * the rule carries the field test too. §4(h)(1b) (vms-54f) widens this
	 * to the 58-content short class too: ops 5 REJECT_RSP, 7 DISCONNECT_RSP,
	 * 8 CREDIT_REQ, 9 CREDIT_RSP are "the envelope, the message type and the
	 * Con.ID pair, and nothing else" (inner length 14) -- the same [50:58]
	 * handle pair, never carrying ctrl_type 10, so the existing field guard
	 * still excludes application messages correctly at this length too.
	 */
	{ { VMS_FCLS_SCS_CONN_CTRL, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ | VMS_FCAP_CONID,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-conn-ctrl",
	    "spec §4(g) ph4, §4(h)(1)(1a),(1b)" },
	  M_MSGTYPE | M_LEN_EQ | M_FIELD_NE, 0,
	  3, { VMS_SCS_MT_MSG, VMS_SCS_MT_SETUP, VMS_SCS_MT_ALT },
	  4, { 110, 66, 62, 58 }, 0,
	  VMS_OFF_SCS_CTRL_TYPE, VMS_SCS_CTRL_APPLICATION },

	{ { VMS_FCLS_SCS_MSG, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ | VMS_FCAP_CONID,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-msg", "spec §4(d)" },
	  M_MSGTYPE | M_LEN_EQ, 0,
	  3, { VMS_SCS_MT_MSG, VMS_SCS_MT_SETUP, VMS_SCS_MT_ALT },
	  1, { 190, 0, 0, 0 }, 0, 0, 0 },

	/*
	 * The 94-content "application message" (op 10 APPL_MSG) shape -- spec
	 * §4(h)(1b) (vms-54f): "the envelope unifies across every length
	 * class... message type [46:48], credit [48:50], handle pair [50:58]"
	 * and identifies content 94 as the SAME op-10 MTYPE the 190-content
	 * class carries, just a shorter body. TWO callers ride this exact
	 * shape -- the MSCP$DISK command/WRITE-END class (FC-P6.2,
	 * vms_cluster_codec_mscp.c, which self-disambiguates command vs.
	 * WRITE-END by the opcode's END bit) and the SCS$DIRECTORY op-10
	 * lookup (FC-P2.1, vms_scs_ctrl_parse()/vms_scs_dir_lookup_parse(),
	 * which self-disambiguates request vs. NOT-PRESENT vs. affirmative by
	 * the name/result bytes) -- and the wire gives no field to tell those
	 * two SYSAPs apart at this layer: which SYSAP owns the frame is a
	 * property of the Con.ID pair's CONNECTION, decided above the codec,
	 * exactly as it already is for the 190-content class shared between
	 * VMS$VAXcluster (FC-P3.1) and cat-0x02 DLM (FC-P4.5). This is a
	 * SEPARATE class from VMS_FCLS_SCS_MSG (never widen that one to cover
	 * 94): vms_cluster_codec_dlm.c's dlm_class_ok() and
	 * vms_cluster_codec_cm.c key on VMS_FCLS_SCS_MSG meaning "190-content
	 * VC class" alone with no length check of their own -- folding 94 into
	 * it would hand a 94-content MSCP/directory frame to the DLM/CM
	 * parsers, which is exactly the kind of silent cross-class misread
	 * INV-6 exists to refuse.
	 */
	{ { VMS_FCLS_SCS_APPLMSG94, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ | VMS_FCAP_CONID,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-applmsg-94",
	    "spec §4(h)(1b)" },
	  M_MSGTYPE | M_LEN_EQ, 0,
	  3, { VMS_SCS_MT_MSG, VMS_SCS_MT_SETUP, VMS_SCS_MT_ALT },
	  1, { 94, 0, 0, 0 }, 0, 0, 0 },

	/*
	 * design §3.2.7 RULING (E48, item FC-P2.7, 2026-09-03): SCS dispatches
	 * an application message (MTYPE 10) to the CDT its Con.ID names, at
	 * ANY length (docs/design-mscp-direction.md §1.2, Davis p.4-13/4-15)
	 * -- length-keyed classes were a capture-census convenience. The
	 * frozen table above grants VMS_FCAP_CONID to exactly one length (94,
	 * the row just above), so the four MSCP END lengths FC-P6.2 measured
	 * (86 SCC, 90 READ, 102 ONLINE, 110 GUS) classified with no Con.ID and
	 * `vc_deliver` counted them `vc_rx_undelivered` -- E48, raised by
	 * FC-P6.5's mscp_write.c scenario.
	 *
	 * This class is the length-generic form of the SAME §4(h)(1b) envelope
	 * VMS_FCLS_SCS_APPLMSG94 already grounds: the SCS envelope conformance
	 * word (content[44:46] == 0x0004, M_FMTWORD -- the test block-transfer
	 * frames FAIL, design-mscp-direction.md "it deliberately FAILS the SCS
	 * envelope conformance test"), the application marker (content[46:48]
	 * == 10, M_FIELD_EQ on the SAME field_off/field_val CONN_CTRL's
	 * M_FIELD_NE excludes on -- so the two rules are mutually exclusive by
	 * construction, not merely by table order), and the inner-length
	 * self-consistency check (content[42:44] == content - 44,
	 * M_INNERLEN_SELF) -- all three already MEASURED on these very frames
	 * (design §3.2.7's own citation of the vms291 lab-2 mount capture and
	 * the byte-exact SCC-end reproduction).
	 *
	 * ORDER IS LOAD-BEARING. Placed AFTER VMS_FCLS_SCS_MSG (190-content)
	 * and VMS_FCLS_SCS_APPLMSG94 (94-content) so first-match-wins keeps
	 * BOTH those classes' own specimens exactly as before: OVMX's own
	 * 190-content VMS$VAXcluster/DLM traffic (vms_scs_fsm.c
	 * msg_transmit_long) ALSO stamps this same fmtword/mtype/inner-length
	 * envelope (SCS_MTYPE_APPL_MSG dispatches everything, per the ruling
	 * above), so a row ahead of VMS_FCLS_SCS_MSG would silently steal
	 * every CM/DLM frame from dlm_class_ok()/vms_cluster_codec_cm.c, which
	 * key on VMS_FCLS_SCS_MSG alone -- exactly the cross-class misread
	 * INV-6 exists to refuse. The 94-content class stays its own alias
	 * (frozen-table no-regression net); this row only ever catches a
	 * length neither of those two already claimed.
	 */
	{ { VMS_FCLS_SCS_APPLMSG, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ | VMS_FCAP_CONID,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-applmsg",
	    "design §3.2.7 (E48), spec §4(h)(1b)" },
	  M_MSGTYPE | M_FIELD_EQ | M_FMTWORD | M_INNERLEN_SELF, 0,
	  3, { VMS_SCS_MT_MSG, VMS_SCS_MT_SETUP, VMS_SCS_MT_ALT },
	  0, { 0, 0, 0, 0 }, 0,
	  VMS_OFF_SCS_CTRL_TYPE, VMS_SCS_CTRL_APPLICATION },

	/*
	 * Any other format-0x13 sequenced message. Deliberately carries NO
	 * CONID capability: spec §4(d) says the other length classes "do not
	 * reliably match this layout and are therefore left undecoded", so
	 * this codec refuses to hand out a Con.ID for them.
	 */
	{ { VMS_FCLS_SCS_SEQ, VMS_FFAM_SCS,
	    VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ,
	    VMS_SCA_HDR_LEN, VMS_SCA_HDR_LEN, "scs-seq", "spec §4(g),§4(h)" },
	  M_MSGTYPE, 0,
	  3, { VMS_SCS_MT_MSG, VMS_SCS_MT_SETUP, VMS_SCS_MT_ALT },
	  0, { 0, 0, 0, 0 }, 0, 0, 0 },
};

#define G_RULE_N ((uint8_t)(sizeof(g_rules) / sizeof(g_rules[0])))

static const struct vms_frame_class_info g_unknown_info = {
	VMS_FCLS_UNKNOWN, VMS_FFAM_NONE, 0, 0, 0, "unknown", "-"
};

const struct vms_frame_class_info *vms_frame_class_lookup(uint8_t cls)
{
	uint8_t i;

	if (cls == VMS_FCLS_UNKNOWN)
		return &g_unknown_info;
	for (i = 0; i < G_RULE_N; i++) {
		if (g_rules[i].info.cls == cls)
			return &g_rules[i].info;
	}
	return (const struct vms_frame_class_info *)0;
}

const struct vms_frame_class_info *vms_frame_class_by_name(const char *name)
{
	uint8_t i;

	if (name == (const char *)0)
		return (const struct vms_frame_class_info *)0;
	if (codec_streq(name, g_unknown_info.name))
		return &g_unknown_info;
	for (i = 0; i < G_RULE_N; i++) {
		if (codec_streq(name, g_rules[i].info.name))
			return &g_rules[i].info;
	}
	return (const struct vms_frame_class_info *)0;
}

/* ---- rule predicates: one criterion each, all bounds-checked ------- */

/*
 * The discovery family is identified by the constant sandwich spec §4(a)
 * documents around the message-class byte: abs 32..35 == 08 00 00 80 and
 * abs 37..39 == 01 00 00. Matching the sandwich (not just abs 36) is what
 * keeps an SCS frame whose abs 36 happens to hold 0x05 out of the family.
 */
static int is_discovery(vms_wire_view_t *v)
{
	static const uint8_t prefix[4] = { 0x08, 0x00, 0x00, 0x80 };
	static const uint8_t suffix[3] = { 0x01, 0x00, 0x00 };
	uint8_t got[4];
	uint8_t i;

	vms_wire_get_bytes(v, VMS_OFF_DISC_PREFIX, 4, got);
	if (!vms_wire_view_ok(v))
		return 0;
	for (i = 0; i < 4; i++) {
		if (got[i] != prefix[i])
			return 0;
	}
	vms_wire_get_bytes(v, VMS_OFF_DISC_SUFFIX, 3, got);
	for (i = 0; i < 3; i++) {
		if (got[i] != suffix[i])
			return 0;
	}
	return 1;
}

/* The SCS family is the format constant 0x13 at abs 31 (GROUNDED 2975/2975). */
static int is_scs_envelope(vms_wire_view_t *v)
{
	uint8_t fmt = vms_wire_get_u8(v, VMS_OFF_SCS_FORMAT);

	return vms_wire_view_ok(v) && fmt == VMS_SCS_FORMAT_V13;
}

static int rule_match_mt(const struct frame_class_rule *r, uint8_t mt)
{
	uint8_t i;

	for (i = 0; i < r->mt_n; i++) {
		if (r->mt[i] == mt)
			return 1;
	}
	return 0;
}

static int rule_match_len(const struct frame_class_rule *r, uint16_t content)
{
	uint8_t i;

	for (i = 0; i < r->len_n; i++) {
		if (r->len[i] == content)
			return 1;
	}
	return 0;
}

struct classify_ctx {
	vms_wire_view_t *v;
	uint16_t         content;
	uint8_t          family;   /* enum vms_frame_family, already decided */
	uint8_t          disc_cls; /* abs 36, valid iff family == DISCOVERY  */
	uint8_t          msgtype;  /* abs 30, valid iff family == SCS        */
};

static int rule_matches(const struct frame_class_rule *r,
			const struct classify_ctx *c)
{
	if (r->info.family != c->family)
		return 0;
	if (c->v->len < r->info.min_len)
		return 0;
	if ((r->match & M_DISCCLASS) && r->disc_class != c->disc_cls)
		return 0;
	if ((r->match & M_MSGTYPE) && !rule_match_mt(r, c->msgtype))
		return 0;
	if ((r->match & M_LEN_EQ) && !rule_match_len(r, c->content))
		return 0;
	if ((r->match & M_LEN_GT) && c->content <= r->len_gt)
		return 0;
	if (r->match & M_FIELD_NE) {
		uint16_t got = vms_wire_get_le16(c->v, r->field_off);

		if (!vms_wire_view_ok(c->v)) {
			/* Truncated frame: this rule cannot apply. That is a
			 * non-match, not a frame error -- clear the sticky
			 * flag so the remaining rules still get a fair test. */
			c->v->err = VMS_CODEC_OK;
			return 0;
		}
		if (got == r->field_val)
			return 0;
	}
	if (r->match & M_FIELD_EQ) {
		uint16_t got = vms_wire_get_le16(c->v, r->field_off);

		if (!vms_wire_view_ok(c->v)) {
			c->v->err = VMS_CODEC_OK;
			return 0;
		}
		if (got != r->field_val)
			return 0;
	}
	if (r->match & M_FMTWORD) {
		uint16_t got = vms_wire_get_le16(c->v, VMS_OFF_SCS_FMT_WORD);

		if (!vms_wire_view_ok(c->v)) {
			c->v->err = VMS_CODEC_OK;
			return 0;
		}
		if (got != VMS_SCS_FMT_WORD_CONST)
			return 0;
	}
	if (r->match & M_INNERLEN_SELF) {
		uint16_t got;

		if (c->content < VMS_SCS_INNER_LEN_BIAS)
			return 0;
		got = vms_wire_get_le16(c->v, VMS_OFF_SCS_INNER_LEN);
		if (!vms_wire_view_ok(c->v)) {
			c->v->err = VMS_CODEC_OK;
			return 0;
		}
		if (got != (uint16_t)(c->content - VMS_SCS_INNER_LEN_BIAS))
			return 0;
	}
	return 1;
}

/* Decide the family and read the family's discriminator byte. */
static void classify_family(struct classify_ctx *c)
{
	c->family = VMS_FFAM_NONE;
	if (is_discovery(c->v)) {
		c->family = VMS_FFAM_DISCOVERY;
		c->disc_cls = vms_wire_get_u8(c->v, VMS_OFF_DISC_CLASS);
		return;
	}
	c->v->err = VMS_CODEC_OK;   /* a non-match is not a parse failure */
	if (is_scs_envelope(c->v)) {
		c->family = VMS_FFAM_SCS;
		c->msgtype = vms_wire_get_u8(c->v, VMS_OFF_SCS_MSGTYPE);
		return;
	}
	c->v->err = VMS_CODEC_OK;
}

enum vms_sca_len_check vms_sca_len_check(uint16_t sca_content, uint32_t wire_len)
{
	/*
	 * Spec §2: LE16(abs 14) + 2 == SCA content, and 23642/24570 frames hold
	 * it exactly. Every one of the 928 residuals is a short frame the wire
	 * zero-padded to the 60-byte Ethernet minimum -- the length field
	 * reports the TRUE payload length while the frame is padded. 0
	 * unexplained residuals.
	 */
	uint32_t predicted = (uint32_t)sca_content + VMS_ETH_HDR_LEN;

	if (predicted == wire_len)
		return VMS_SCA_LEN_EXACT;
	if (predicted <= wire_len && wire_len == VMS_ETH_MIN_FRAME)
		return VMS_SCA_LEN_RUNT_PAD;
	return VMS_SCA_LEN_MISMATCH;
}

static void frame_info_reset(struct vms_frame_info *out, uint32_t len)
{
	out->cls = VMS_FCLS_UNKNOWN;
	out->family = VMS_FFAM_NONE;
	out->caps = 0;
	out->len_check = VMS_SCA_LEN_MISMATCH;
	out->sca_content = 0;
	out->wire_len = (uint16_t)((len > 0xffffu) ? 0xffffu : len);
}

vms_codec_status_t vms_frame_classify(const uint8_t *frame, uint32_t len,
				      struct vms_frame_info *out)
{
	struct classify_ctx c;
	vms_wire_view_t v;
	uint16_t ethertype;
	uint8_t i;

	if (out == (struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	frame_info_reset(out, len);
	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	ethertype = vms_wire_get_be16(&v, VMS_OFF_ETHERTYPE);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_SHORT;
	if (ethertype != VMS_SCA_ETHERTYPE)
		return VMS_CODEC_E_NOTSCA;

	c.v = &v;
	c.content = (uint16_t)(vms_wire_get_le16(&v, VMS_OFF_SCA_LEN) + 2u);
	c.disc_cls = 0;
	c.msgtype = 0;
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_SHORT;
	out->sca_content = c.content;
	out->len_check = (uint8_t)vms_sca_len_check(c.content, len);

	if (len < VMS_SCA_HDR_LEN)
		return VMS_CODEC_E_SHORT;

	classify_family(&c);
	for (i = 0; i < G_RULE_N; i++) {
		if (!rule_matches(&g_rules[i], &c))
			continue;
		out->cls = g_rules[i].info.cls;
		out->family = g_rules[i].info.family;
		out->caps = g_rules[i].info.caps;
		return VMS_CODEC_OK;
	}
	/* Unclassified is a legitimate, honest outcome -- not an error. */
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * §4  Shared SCA header
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_sca_hdr_parse(const uint8_t *frame, uint32_t len,
				     struct vms_sca_hdr *out)
{
	vms_wire_view_t v;

	if (out == (struct vms_sca_hdr *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;
	if (len < VMS_SCA_HDR_LEN)
		return VMS_CODEC_E_SHORT;
	if (vms_wire_get_be16(&v, VMS_OFF_ETHERTYPE) != VMS_SCA_ETHERTYPE)
		return VMS_CODEC_E_NOTSCA;

	vms_wire_get_bytes(&v, VMS_OFF_ETH_DST, VMS_ETH_ADDR_LEN, out->eth_dst);
	vms_wire_get_bytes(&v, VMS_OFF_ETH_SRC, VMS_ETH_ADDR_LEN, out->eth_src);
	out->sca_len_field = vms_wire_get_le16(&v, VMS_OFF_SCA_LEN);
	vms_wire_get_bytes(&v, VMS_OFF_DST_LAVC, VMS_ETH_ADDR_LEN, out->dst_lavc);
	out->connect_flag = vms_wire_get_le16(&v, VMS_OFF_CONNECT_FLAG);
	vms_wire_get_bytes(&v, VMS_OFF_SRC_LAVC, VMS_ETH_ADDR_LEN, out->src_lavc);
	out->word30 = vms_wire_get_le16(&v, VMS_OFF_WORD30);

	return v.err;
}

vms_codec_status_t vms_sca_hdr_build(const struct vms_sca_hdr *h,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written)
{
	vms_wire_buf_t w;

	if (h == (const struct vms_sca_hdr *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_bytes(&w, VMS_OFF_ETH_DST, VMS_ETH_ADDR_LEN, h->eth_dst);
	vms_wire_put_bytes(&w, VMS_OFF_ETH_SRC, VMS_ETH_ADDR_LEN, h->eth_src);
	vms_wire_put_be16(&w, VMS_OFF_ETHERTYPE, VMS_SCA_ETHERTYPE);
	vms_wire_put_le16(&w, VMS_OFF_SCA_LEN, h->sca_len_field);
	vms_wire_put_bytes(&w, VMS_OFF_DST_LAVC, VMS_ETH_ADDR_LEN, h->dst_lavc);
	vms_wire_put_le16(&w, VMS_OFF_CONNECT_FLAG, h->connect_flag);
	vms_wire_put_bytes(&w, VMS_OFF_SRC_LAVC, VMS_ETH_ADDR_LEN, h->src_lavc);
	vms_wire_put_le16(&w, VMS_OFF_WORD30, h->word30);

	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return w.err;
}

uint16_t vms_sca_content_len(const struct vms_sca_hdr *h)
{
	return (uint16_t)(h->sca_len_field + 2u);
}

/* ------------------------------------------------------------------ *
 * §5  Class-gated accessors
 * ------------------------------------------------------------------ */

/*
 * The one gate every class-gated accessor goes through. `need` is the
 * capability the field requires; a class that does not publish it gets
 * VMS_CODEC_E_CLASS and no bytes (INV-6: honest omission, never the four
 * bytes that merely happen to live at that offset).
 */
static vms_codec_status_t gate(const struct vms_frame_info *fi, uint8_t need,
			       const uint8_t *frame, uint32_t len,
			       vms_wire_view_t *v)
{
	if (fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if ((fi->caps & need) == 0)
		return VMS_CODEC_E_CLASS;
	vms_wire_view_init(v, frame, len);
	return v->err;
}

/* Same gate, keyed on the frame FAMILY rather than a capability bit. */
static vms_codec_status_t gate_family(const struct vms_frame_info *fi,
				      uint8_t family, const uint8_t *frame,
				      uint32_t len, vms_wire_view_t *v)
{
	if (fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if (fi->family != family)
		return VMS_CODEC_E_CLASS;
	vms_wire_view_init(v, frame, len);
	return v->err;
}

vms_codec_status_t vms_sca_chan_word(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     uint8_t *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st = gate(fi, VMS_FCAP_CHANWORD, frame, len, &v);
	uint8_t val;

	if (st != VMS_CODEC_OK)
		return st;
	/* Spec §4(a).1: the state lives in the LOW byte at abs 30. */
	val = vms_wire_get_u8(&v, VMS_OFF_WORD30);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*out = val;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_sca_disc_class(const uint8_t *frame, uint32_t len,
				      const struct vms_frame_info *fi,
				      uint8_t *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st = gate_family(fi, VMS_FFAM_DISCOVERY, frame, len,
					    &v);
	uint8_t val;

	if (st != VMS_CODEC_OK)
		return st;
	val = vms_wire_get_u8(&v, VMS_OFF_DISC_CLASS);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*out = val;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_msgtype(const uint8_t *frame, uint32_t len,
				   const struct vms_frame_info *fi,
				   uint8_t *msgtype, uint8_t *format)
{
	vms_wire_view_t v;
	vms_codec_status_t st = gate(fi, VMS_FCAP_MSGTYPE, frame, len, &v);
	uint8_t mt, fmt;

	if (st != VMS_CODEC_OK)
		return st;
	mt = vms_wire_get_u8(&v, VMS_OFF_SCS_MSGTYPE);
	fmt = vms_wire_get_u8(&v, VMS_OFF_SCS_FORMAT);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (msgtype != (uint8_t *)0)
		*msgtype = mt;
	if (format != (uint8_t *)0)
		*format = fmt;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_seq(const uint8_t *frame, uint32_t len,
			       const struct vms_frame_info *fi,
			       uint16_t *recv_ack, uint16_t *send_seq)
{
	vms_wire_view_t v;
	vms_codec_status_t st = gate(fi, VMS_FCAP_SEQ, frame, len, &v);
	uint16_t ack, seq;

	if (st != VMS_CODEC_OK)
		return st;
	/* Spec §4(h)(4): recv_ack at abs 32, send_seq at abs 34. */
	ack = vms_wire_get_le16(&v, VMS_OFF_SCS_RECV_ACK);
	seq = vms_wire_get_le16(&v, VMS_OFF_SCS_SEND_SEQ);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (recv_ack != (uint16_t *)0)
		*recv_ack = ack;
	if (send_seq != (uint16_t *)0)
		*send_seq = seq;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_conid(const uint8_t *frame, uint32_t len,
				 const struct vms_frame_info *fi,
				 uint32_t *remote, uint32_t *local)
{
	vms_wire_view_t v;
	vms_codec_status_t st = gate(fi, VMS_FCAP_CONID, frame, len, &v);
	uint32_t r, l;

	if (st != VMS_CODEC_OK)
		return st;
	/* Spec §4(d) abs 64/68 == §4(g) ph4 / §4(h)(1) payload [50:58]. */
	r = vms_wire_get_le32(&v, VMS_OFF_SCS_CONID_REMOTE);
	l = vms_wire_get_le32(&v, VMS_OFF_SCS_CONID_LOCAL);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (remote != (uint32_t *)0)
		*remote = r;
	if (local != (uint32_t *)0)
		*local = l;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_ctrl_type(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     uint16_t *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st = gate(fi, VMS_FCAP_CONID, frame, len, &v);
	uint16_t val;

	if (st != VMS_CODEC_OK)
		return st;
	val = vms_wire_get_le16(&v, VMS_OFF_SCS_CTRL_TYPE);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*out = val;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * §6  Response allowlist
 * ------------------------------------------------------------------ */

int vms_wire_is_response(uint8_t category)
{
	return (category & VMS_WIRE_RESPONSE_BIT) != 0;
}

uint8_t vms_wire_response_category(uint8_t request_category)
{
	return (uint8_t)(request_category | VMS_WIRE_RESPONSE_BIT);
}

const struct vms_wire_allow_entry *
vms_wire_allow_find(const struct vms_wire_allow_table *t,
		    uint8_t sysap, uint8_t category, uint8_t opcode)
{
	uint16_t i;

	if (t == (const struct vms_wire_allow_table *)0 ||
	    t->rows == (const struct vms_wire_allow_entry *)0)
		return (const struct vms_wire_allow_entry *)0;
	/*
	 * Exact triple match only. There is no wildcard row and no fallthrough
	 * default: an unlisted pair MUST produce silence plus a log line
	 * (spec §4(p)). A response-bit category never matches -- a response is
	 * correlated by (txn, checksum, opcode), not answered.
	 */
	if (vms_wire_is_response(category))
		return (const struct vms_wire_allow_entry *)0;
	for (i = 0; i < t->n; i++) {
		if (t->rows[i].sysap == sysap &&
		    t->rows[i].category == category &&
		    t->rows[i].opcode == opcode)
			return &t->rows[i];
	}
	return (const struct vms_wire_allow_entry *)0;
}

static vms_codec_status_t allow_row_validate(const struct vms_wire_allow_entry *e)
{
	if (e->sysap == VMS_SYSAP_UNKNOWN || e->sysap >= VMS_SYSAP__COUNT)
		return VMS_CODEC_E_INVAL;
	if (vms_wire_is_response(e->category))
		return VMS_CODEC_E_INVAL;
	if (e->action == VMS_WIRE_ACT_NONE)
		return VMS_CODEC_E_INVAL;
	if (e->action == VMS_WIRE_ACT_RESPOND && e->recipe == 0)
		return VMS_CODEC_E_INVAL;
	if (e->action != VMS_WIRE_ACT_RESPOND && e->recipe != 0)
		return VMS_CODEC_E_INVAL;
	if (e->spec == (const char *)0 || e->spec[0] == '\0')
		return VMS_CODEC_E_INVAL;
	return VMS_CODEC_OK;
}

vms_codec_status_t
vms_wire_allow_table_validate(const struct vms_wire_allow_table *t)
{
	uint16_t i, j;

	if (t == (const struct vms_wire_allow_table *)0)
		return VMS_CODEC_E_INVAL;
	if (t->n != 0 && t->rows == (const struct vms_wire_allow_entry *)0)
		return VMS_CODEC_E_INVAL;
	for (i = 0; i < t->n; i++) {
		vms_codec_status_t st = allow_row_validate(&t->rows[i]);

		if (st != VMS_CODEC_OK)
			return st;
		for (j = 0; j < i; j++) {
			if (t->rows[j].sysap == t->rows[i].sysap &&
			    t->rows[j].category == t->rows[i].category &&
			    t->rows[j].opcode == t->rows[i].opcode)
				return VMS_CODEC_E_INVAL;
		}
	}
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * §7  Fuzz entry point
 * ------------------------------------------------------------------ */

/* Drive every gated accessor; results are deliberately discarded. */
static void fuzz_accessors(const uint8_t *d, uint32_t n,
			   const struct vms_frame_info *fi)
{
	struct vms_sca_hdr h;
	uint32_t r, l;
	uint16_t a, s, ct;
	uint8_t b1, b2;

	(void)vms_sca_hdr_parse(d, n, &h);
	(void)vms_sca_chan_word(d, n, fi, &b1);
	(void)vms_sca_disc_class(d, n, fi, &b1);
	(void)vms_scs_msgtype(d, n, fi, &b1, &b2);
	(void)vms_scs_seq(d, n, fi, &a, &s);
	(void)vms_scs_conid(d, n, fi, &r, &l);
	(void)vms_scs_ctrl_type(d, n, fi, &ct);
}

uint8_t vms_cluster_codec_fuzz_one(const uint8_t *d, uint32_t n)
{
	struct vms_frame_info fi;
	struct vms_sca_hdr h;
	uint8_t rebuilt[VMS_SCA_HDR_LEN];
	uint32_t written = 0;

	if (d == (const uint8_t *)0)
		return VMS_FCLS_UNKNOWN;
	/* classify() reports its refusals through fi (cls == UNKNOWN); the
	 * status is not an early exit for the fuzzer, which wants every
	 * accessor driven over the same bytes regardless. */
	(void)vms_frame_classify(d, n, &fi);
	fuzz_accessors(d, n, &fi);
	/* Re-build from whatever parsed, then re-classify: build must not
	 * widen the class beyond what the header alone can justify. */
	if (vms_sca_hdr_parse(d, n, &h) == VMS_CODEC_OK) {
		struct vms_frame_info fi2;

		(void)vms_sca_hdr_build(&h, rebuilt, sizeof(rebuilt), &written);
		(void)vms_frame_classify(rebuilt, written, &fi2);
	}
	return fi.cls;
}
