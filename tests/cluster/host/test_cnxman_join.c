/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_join.c - the JOIN FSM (FC-P3.3, test-ladder rung R1).
 *
 * WHAT THIS PROVES, AND WHY EACH CASE EXISTS. This FSM's two failure modes are
 * opposite and both fatal:
 *
 *   - "we are never admitted" -- a joiner that does not DRIVE (spec sec 4(L)(1)
 *     and (2): the member times out at ~1.4 s and re-issues START forever), or
 *     that connects to a SYSAP it has not resolved (sec 4(L)'s shared-sequence
 *     deadlock, which froze a real member's recv_ack and regressed OVMX below
 *     NEW to blank status);
 *   - "we are admitted on a lie" -- a joiner that fabricates the CSID it was
 *     never told, advertises a LOCKDIRWT at a guessed offset, or replays
 *     another implementation's connect data. INV-6 exists because a
 *     placeholder identifier bugchecked a real VAX.
 *
 * So the cases below split into three groups: (1) the choreography, in order,
 * asserted against spec sec 4(L)/(o); (2) every [state][event] cell of the
 * table, populated and empty; (3) the HONEST OMISSIONS, each with a NEGATIVE
 * assertion that the value really is absent (integration notes E8, E24; plan
 * row FC-P3.2).
 *
 * Everything is injected: the clock, the SCS client surface, the MSCP peer's
 * answers and the barrier. A whole join therefore runs here in microseconds,
 * with no wire, no daemon and no boot.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_mscp.h"
#include "vms_frame_compose.h"   /* test-only full-frame composer */

/* ==========================================================================
 * The bed
 * ========================================================================== */

#define MEMBER_SYSID  0x000004000101ull   /* VAX1's LAVC-derived SCSSYSTEMID */
#define OTHER_SYSID   0x000004000102ull   /* VAX2                            */
#define OWN_SYSID     0x000004000103ull   /* this node                       */
#define MEMBER_CSID   0x00010001u
#define OTHER_CSID    0x00010002u
#define EPOCH         0x0000000eu

#define MSCP_CONID  0x4e620008u
#define CM_CONID    0x4e620009u

/* The Con.ID SCS mints for a connection the MEMBER opened and this node
 * ACCEPTED. Deliberately different from CM_CONID: an accepted connection is
 * not one this join ever recorded, which is exactly why CNXMAN_EV_CDT_OPEN
 * cannot carry the fact (E67). */
#define ACC_CM_CONID 0x4e62000au

#define MAX_SENT 64
#define MAX_INQ  16

struct sent_body {
	uint8_t     body[VMS_CM_BODY_LEN];
	uint32_t    len;
	vms_conid_t conid;
};

struct bed {
	struct vms_cluster    cl;
	struct cnxman_ops     ops;
	struct fake_cnx       fake;
	struct cnxman_join_ops jops;
	struct cnxman_join    j;
	struct cnxman_barrier b;
	struct vms_csb       *member_csb;

	/* what the FSM asked the world to do */
	struct sent_body sent[MAX_SENT];
	uint32_t         n_sent;
	uint8_t          inq[MAX_INQ][VMS_SCS_PROCNAME_LEN];
	uint32_t         n_inq;
	uint32_t         n_connect;
	uint8_t          last_local[VMS_SCS_PROCNAME_LEN];
	uint8_t          last_remote[VMS_SCS_PROCNAME_LEN];
	int              last_had_conndata;
	uint32_t         n_disconnect;
	uint32_t         n_set_dir_data;

	int      fail_connect;
	int      fail_send;
	/* WHAT the injected SCS answers when `fail_send` fires. 0 keeps the
	 * historic -1; E70 needs a real refusal code, because the executive
	 * returns one and the ring has to carry it. */
	int      fail_send_rc;
	uint64_t vms_time;
};

static struct bed g;

/* ---- the injected SCS client surface ------------------------------------ */

static int bed_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx;
	(void)dst;
	if (g.n_inq < MAX_INQ) {
		memcpy(g.inq[g.n_inq], name, VMS_SCS_PROCNAME_LEN);
		g.n_inq++;
	}
	return 0;
}

static int bed_connect(void *ctx, vms_scs_sysid_t dst,
		       const uint8_t *local_name, const uint8_t *remote_name,
		       const uint8_t *conndata, uint16_t credits,
		       vms_conid_t *out_conid)
{
	(void)ctx;
	(void)dst;
	(void)credits;
	if (g.fail_connect)
		return -1;
	g.n_connect++;
	memcpy(g.last_local, local_name, VMS_SCS_PROCNAME_LEN);
	memcpy(g.last_remote, remote_name, VMS_SCS_PROCNAME_LEN);
	g.last_had_conndata = (conndata != NULL);
	*out_conid = (memcmp(remote_name, cnxman_join_name_mscp_disk,
			     VMS_SCS_PROCNAME_LEN) == 0) ? MSCP_CONID
						       : CM_CONID;
	/* Mirror the production glue: cnxman_jop_connect() writes the Con.ID
	 * SCS minted into the destination CSB at that instant, which is what
	 * makes the block the record of the connection (book p. 7-23) and what
	 * every CSB-addressed origination resolves through. */
	if (*out_conid == CM_CONID && g.member_csb != NULL)
		g.member_csb->cdt_conid = CM_CONID;
	return 0;
}

static int bed_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len)
{
	(void)ctx;
	if (g.fail_send)
		return g.fail_send_rc != 0 ? g.fail_send_rc : -1;
	if (g.n_sent < MAX_SENT) {
		memset(g.sent[g.n_sent].body, 0, VMS_CM_BODY_LEN);
		memcpy(g.sent[g.n_sent].body, body,
		       len > VMS_CM_BODY_LEN ? VMS_CM_BODY_LEN : len);
		g.sent[g.n_sent].len = len;
		g.sent[g.n_sent].conid = conid;
		g.n_sent++;
	}
	return 0;
}

/*
 * The connection manager's OWN transport ops (`cnxman_ops`), as distinct from
 * the join's injected SCS client surface above. The barrier's twelve op-0x0b
 * steps and every 0x81 response ride these, and E73 made the participant's
 * origination CSB-addressed -- so this bed resolves the CLUB slot exactly as
 * vms_cnxman.c does and REFUSES a slot it cannot resolve, rather than
 * inventing a destination.
 */
static void bed_record_cm(const uint8_t *body, uint32_t len, vms_conid_t conid)
{
	if (g.n_sent >= MAX_SENT)
		return;
	memset(g.sent[g.n_sent].body, 0, VMS_CM_BODY_LEN);
	memcpy(g.sent[g.n_sent].body, body,
	       len > VMS_CM_BODY_LEN ? VMS_CM_BODY_LEN : len);
	g.sent[g.n_sent].len = len;
	g.sent[g.n_sent].conid = conid;
	g.n_sent++;
}

static int bed_ops_send_csb(void *ctx, int32_t csb_index, const uint8_t *body,
			    uint32_t len)
{
	struct vms_csb *csb;

	(void)ctx;
	if (csb_index < 0)
		return -1;
	csb = cnxman_club_csb_at(&g.cl.club, (uint32_t)csb_index);
	if (csb == NULL || !csb->in_use || csb->cdt_conid == 0u)
		return -1;
	bed_record_cm(body, len, (vms_conid_t)csb->cdt_conid);
	return 0;
}

/* `respond` answers on the connection the request arrived on; in this bed
 * every dispatched request comes from the member, so that is its Con.ID. */
static int bed_ops_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	if (g.member_csb == NULL || g.member_csb->cdt_conid == 0u)
		return -1;
	bed_record_cm(body, len, (vms_conid_t)g.member_csb->cdt_conid);
	return 0;
}

static int bed_disconnect(void *ctx, vms_conid_t conid)
{
	(void)ctx;
	(void)conid;
	g.n_disconnect++;
	return 0;
}

static int bed_set_dir_data(void *ctx, const uint8_t *name,
			    const uint8_t *data)
{
	(void)ctx;
	(void)name;
	(void)data;
	g.n_set_dir_data++;
	return 0;
}

static uint64_t bed_time_now(void *ctx)
{
	(void)ctx;
	return g.vms_time;
}

static void bed_init(void)
{
	struct vms_csb *other;

	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.fake.now_ms = 100000u;
	g.vms_time = 0x00bc021975280bc0ULL;

	g.jops.dir_inquire = bed_dir_inquire;
	g.jops.connect = bed_connect;
	g.jops.send_msg = bed_send_msg;
	g.jops.disconnect = bed_disconnect;
	g.jops.set_dir_data = bed_set_dir_data;
	g.jops.time_now = bed_time_now;

	memcpy(g.cl.params.scsnode, "OVMXJ0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	g.cl.params.votes = 0;        /* D-10: OVMX joins non-voting        */
	g.cl.params.lockdirwt = 0;    /* D-DLM-1                            */

	(void)cnxman_club_init(&g.cl);

	/* Two members the port has really formed circuits with. The join
	 * target is the CSB nearest the queue TAIL (book p. 7-38), so the
	 * SECOND one allocated is the one this node must ask. */
	other = cnxman_club_alloc_csb(&g.cl.club, OTHER_SYSID, 1);
	cnxman_csb_set_csid(other, OTHER_CSID);
	g.member_csb = cnxman_club_alloc_csb(&g.cl.club, MEMBER_SYSID, 1);
	cnxman_csb_set_csid(g.member_csb, MEMBER_CSID);

	g.ops.send_csb = bed_ops_send_csb;
	g.ops.respond = bed_ops_respond;

	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);
}

/* This node's own honest identity, as FC-P3.8's glue will read it out of the
 * executive. Not one byte comes from a capture: the model string names OVMX,
 * and the version is OVMX's own (spec sec 4(L)(6) measured that a real VAX
 * accepts and DISPLAYS a non-"VMS" string here). */
static void bed_set_identity(void)
{
	struct cnxman_join_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.model, "OVMX x86_64", 11);
	cfg.model_len = 11;
	cfg.model_valid = 1;
	memcpy(cfg.version, "VMX V0.6", 8);
	cfg.version_valid = 1;
	cnxman_join_set_cfg(&g.j, &cfg);
}

/* ==========================================================================
 * Building the MEMBER's frames -- through the codec's own named offsets, so
 * there is no magic number in a test either (design sec 3.9 rule 2).
 * ========================================================================== */

static uint8_t g_frame[VMS_CM_FRAME_LEN];

/*
 * FEED THE JOIN WHAT THE EXECUTIVE FEEDS IT (E73).
 *
 * The builders below compose whole 204-byte frames because that is the shape a
 * capture has; SCS hands a SYSAP the 132-byte BODY and nothing below it (design
 * sec 3.2.4). Every one of these tests used to pass the whole frame -- and
 * stayed green while the live executive's every inbound CM message was refused
 * as unparsed. This slice is the difference, so it is made once, here, and no
 * test can accidentally go back to the frame.
 *
 * `from_csb` is the CLUB slot the glue resolves from the Con.ID the message
 * really arrived on; MEMBER_SYSID's block is the one the member's connection
 * belongs to.
 */
static int32_t member_csb_index(void)
{
	return (int32_t)cnxman_club_csb_index(&g.cl.club, g.member_csb);
}

/* The same, from a NAMED peer's connection -- because since E73 this node holds
 * a real dialogue with every member and "which CSB did this arrive on" is the
 * fact that decides where a peer's own record is filed. */
static enum cnxman_join_rx join_feed_from(struct vms_csb *csb,
					  vms_csid_t from_csid, uint32_t len)
{
	const uint8_t *body = g_frame + VMS_OFF_SYSAP_BODY;
	uint32_t blen = len - VMS_OFF_SYSAP_BODY;
	struct vms_cm_envelope env;

	if (vms_cm_envelope_parse(body, blen, &env) == VMS_CODEC_OK)
		cnxman_csb_dialogue_heard(csb, env.send_msg);
	return cnxman_join_rx_body(&g.j, body, blen, from_csid, 1,
				   (int32_t)cnxman_club_csb_index(&g.cl.club,
								  csb));
}

static enum cnxman_join_rx join_feed(uint32_t len)
{
	const uint8_t *body = g_frame + VMS_OFF_SYSAP_BODY;
	uint32_t blen = len - VMS_OFF_SYSAP_BODY;
	struct vms_cm_envelope env;

	/*
	 * Mirror vms_cnxman.c: the ack side of the dialogue is recorded ONCE,
	 * by the glue, on the CSB whose Con.ID the message really arrived on,
	 * BEFORE any FSM is offered the body (E73). A bed that skipped it
	 * would be testing an FSM the executive never runs.
	 */
	if (vms_cm_envelope_parse(body, blen, &env) == VMS_CODEC_OK)
		cnxman_csb_dialogue_heard(g.member_csb, env.send_msg);

	return cnxman_join_rx_body(&g.j, body, blen, MEMBER_CSID, 1,
				   member_csb_index());
}

static uint32_t mk_cm(uint8_t cat, uint8_t op, uint16_t send_msg)
{
	struct vms_cm_link l;
	vms_wire_buf_t w;
	uint32_t written = 0;
	static const uint8_t dmac[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };
	static const uint8_t smac[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };

	memset(g_frame, 0, VMS_CM_FRAME_LEN);
	memset(&l, 0, sizeof(l));
	memcpy(l.hdr.eth_dst, dmac, 6);
	memcpy(l.hdr.eth_src, smac, 6);
	memcpy(l.hdr.dst_lavc, dmac, 6);
	memcpy(l.hdr.src_lavc, smac, 6);
	l.hdr.connect_flag = 0x0001;
	l.recv_ack = 0x0011;
	l.send_seq = 0x0012;
	l.remote_conid = CM_CONID;
	l.local_conid = 0x33580008u;
	(void)vms_frame_compose_link(&l, g_frame, VMS_CM_FRAME_LEN, &written);

	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	vms_wire_put_le16(&w, VMS_OFF_CM_SEND_MSG, send_msg);
	vms_wire_put_le16(&w, VMS_OFF_CM_ACK_MSG, 0x0002);
	vms_wire_put_le16(&w, VMS_OFF_CM_TXN, 0x0009);
	vms_wire_put_le16(&w, VMS_OFF_CM_TOKEN, 0x0abc);
	vms_wire_put_u8(&w, VMS_OFF_CM_CATEGORY, cat);
	vms_wire_put_u8(&w, VMS_OFF_CM_OPCODE, op);
	return VMS_CM_FRAME_LEN;
}

static uint32_t mk_peer_params(uint16_t votes, uint16_t send_msg)
{
	vms_wire_buf_t w;
	uint32_t n = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_PARAMS, send_msg);

	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_le16(&w, VMS_OFF_CM_VOTES, votes);
	return n;
}

static uint32_t mk_open_add(uint32_t epoch, uint8_t bitmap)
{
	vms_wire_buf_t w;
	uint32_t n = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD, 0x0030);

	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_XITION);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_ADD);
	vms_wire_put_u8(&w, VMS_OFF_CM_BITMAP, bitmap);
	return n;
}

static uint32_t mk_go(uint32_t epoch)
{
	vms_wire_buf_t w;
	uint32_t n = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO, 0x0031);

	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_GO);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_ADD);
	vms_wire_put_le16(&w, VMS_OFB_CM_TXN, 0);   /* notifications: txn 0 */
	return n;
}

/*
 * An op-0x06 MEMBERSHIP burst carrying a real coordinator CSID, in either of
 * the two byte-exact forms the real-VAX capture measured (E30,
 * tests/lab/captures/op06-join-20260903.pcap):
 *   form 'A' -- CSID at body[24:28] (VMS_OFB_CM_MEMBERSHIP_CSID_A)
 *   form 'B' -- CSID at body[36:40] (VMS_OFB_CM_MEMBERSHIP_CSID_B)
 * `csid` == 0 builds a burst with NEITHER offset carrying a shape-valid
 * value -- the "no coordinator CSID in this frame" case.
 */
static uint32_t mk_membership_csid(uint32_t csid, char form)
{
	vms_wire_buf_t w;
	uint32_t off;
	uint32_t n = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_MEMBERSHIP, 0x0040);

	if (csid == 0u)
		return n;
	off = (form == 'A') ? VMS_OFF_CM_MEMBERSHIP_CSID_A
			     : VMS_OFF_CM_MEMBERSHIP_CSID_B;
	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, off, csid);
	return n;
}

/* The coordinator's 0x81/0x0b step ACK -- not the release (spec sec 4(p)). */
static uint32_t mk_step_ack(uint32_t step, uint16_t send_msg)
{
	vms_wire_buf_t w;
	uint32_t n = mk_cm(vms_wire_response_category(VMS_CM_CAT_CONFIG),
			   VMS_CM_OP_BARRIER, send_msg);

	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, EPOCH);
	vms_wire_put_le32(&w, VMS_OFF_CM_STEP, step);
	return n;
}

/* The coordinator's op-0x0c RELEASE of step N. Never answered; carries txn 0. */
static uint32_t mk_release(uint32_t step, uint16_t send_msg)
{
	vms_wire_buf_t w;
	uint32_t n = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL, send_msg);

	vms_wire_buf_init(&w, g_frame, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, EPOCH);
	vms_wire_put_le32(&w, VMS_OFF_CM_STEP, step);
	vms_wire_put_le16(&w, VMS_OFB_CM_TXN, 0);
	return n;
}

/* One op-0x05 lock/resource-rebuild transaction (sec 4(o) rows 8-9). */
static uint32_t mk_lockrb(uint16_t send_msg)
{
	return mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_LOCKRB, send_msg);
}

/* The op-0x03 membership COMMIT -- the message a real VAX2 sent this node
 * 0.7 ms after its promotion burst on join-e72refire, and that the frame-level
 * parser refused (E73). */
static uint32_t mk_commit(uint16_t send_msg)
{
	return mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, send_msg);
}

/* ---- the MSCP peer's answers ------------------------------------------- */

static uint8_t g_mscp[256];

static uint32_t mk_scc_end(uint16_t msgid)
{
	struct vms_mscp_link l;
	struct vms_mscp_scc_end e;
	uint32_t w = 0;

	memset(g_mscp, 0, sizeof(g_mscp));
	memset(&l, 0, sizeof(l));
	(void)vms_mscp_link_build(&l,
				  VMS_MSCP_END_SCA_LEN(VMS_MSCP_SCC_END_LEN),
				  g_mscp, sizeof(g_mscp), &w);
	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = VMS_MSCP_CL_CMD_REF(VMS_MSCP_CL_SCC_CLASS, msgid);
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0);
	(void)vms_mscp_scc_end_build(&e, g_mscp, sizeof(g_mscp), &w);
	return VMS_OFF_SYSAP_BODY + w;
}

static uint32_t mk_gus_end(uint16_t msgid, uint16_t unit, unsigned major)
{
	struct vms_mscp_link l;
	struct vms_mscp_gus_end e;
	uint32_t w = 0;

	memset(g_mscp, 0, sizeof(g_mscp));
	memset(&l, 0, sizeof(l));
	(void)vms_mscp_link_build(&l,
				  VMS_MSCP_END_SCA_LEN(VMS_MSCP_GUS_END_LEN),
				  g_mscp, sizeof(g_mscp), &w);
	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = VMS_MSCP_CL_CMD_REF(VMS_MSCP_CL_GUS_CLASS, msgid);
	e.eh.hdr.unit = unit;
	e.eh.status = VMS_MSCP_STATUS(major, 0);
	e.unit_flags = 0x8000u;
	e.media_id = 0x2564105cu;
	(void)vms_mscp_gus_end_build(&e, g_mscp, sizeof(g_mscp), &w);
	return VMS_OFF_SYSAP_BODY + w;
}

/* ---- reading back what the join emitted -------------------------------- */

/* The Nth body sent on the `VMS$VAXcluster` connection. The MSCP$DISK
 * commands go through the same injected send_msg (they are 36-byte bodies on
 * the 94-content class), so an assertion about the CM dialogue has to index
 * the CM dialogue -- exactly as the two connections are distinct on the wire. */
static const struct sent_body *nth_sent(uint32_t n)
{
	uint32_t i, k = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].conid != CM_CONID)
			continue;
		if (k == n)
			return &g.sent[i];
		k++;
	}
	return NULL;
}

static uint32_t n_cm_sent(void)
{
	uint32_t i, k = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].conid == CM_CONID)
			k++;
	}
	return k;
}

/* The same two readers, on ANY connection -- because when the member wins the
 * connect race the CM dialogue runs on the Con.ID SCS minted for the ACCEPTED
 * connection, not on the one this join would have opened (E67). */
static const struct sent_body *nth_sent_on(vms_conid_t conid, uint32_t n)
{
	uint32_t i, k = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].conid != conid)
			continue;
		if (k == n)
			return &g.sent[i];
		k++;
	}
	return NULL;
}

static uint32_t n_sent_on(vms_conid_t conid)
{
	uint32_t i, k = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].conid == conid)
			k++;
	}
	return k;
}

static int sent_on_is(vms_conid_t conid, uint32_t n, uint8_t cat, uint8_t op)
{
	const struct sent_body *b = nth_sent_on(conid, n);

	return b != NULL && b->len == VMS_CM_BODY_LEN &&
	       b->body[VMS_OFB_CM_CATEGORY] == cat &&
	       b->body[VMS_OFB_CM_OPCODE] == op;
}

static uint16_t sent_on_le16(vms_conid_t conid, uint32_t n, uint32_t off)
{
	const struct sent_body *b = nth_sent_on(conid, n);

	if (b == NULL)
		return 0xffffu;
	return (uint16_t)(b->body[off] | ((uint16_t)b->body[off + 1] << 8));
}

static int sent_is(uint32_t n, uint8_t cat, uint8_t op)
{
	const struct sent_body *s = nth_sent(n);

	return s != NULL && s->len == VMS_CM_BODY_LEN &&
	       s->body[VMS_OFB_CM_CATEGORY] == cat &&
	       s->body[VMS_OFB_CM_OPCODE] == op;
}

static uint16_t sent_le16(uint32_t n, uint32_t off)
{
	const struct sent_body *s = nth_sent(n);

	if (s == NULL)
		return 0xffffu;
	return (uint16_t)(s->body[off] | ((uint16_t)s->body[off + 1] << 8));
}

/* ==========================================================================
 * 1. The choreography, in the order spec sec 4(L)/sec 4(o) grounds
 * ========================================================================== */

/* Drive from IDLE to ADMIT with two served units enumerated. Returns the
 * number of MSCP commands the walk emitted. */
static void drive_to_admit(void)
{
	uint32_t len;

	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	cnxman_join_opened(&g.j, MSCP_CONID);
	cnxman_join_opened(&g.j, CM_CONID);

	len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);

	len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u, VMS_MSCP_ST_AVAILABLE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 1u), 2u,
			 VMS_MSCP_ST_AVAILABLE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 2u), 3u,
			 VMS_MSCP_ST_OFFLINE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
}

static void test_reference_sequence(void)
{
	printf("\n-- the reference joiner drive sequence (spec sec 4(L)/(o)) --\n");
	bed_init();
	bed_set_identity();

	ct_check_eq_u32(cnxman_join_start(&g.j) == 0, 1u, "CLUSTER_START drives");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_DIR_ROUND,
			"step 1/2: our OWN SCS$DIRECTORY round is open");
	ct_check_eq_u32(g.n_inq, 2u,
			"step 2: both SYSAP names looked up BEFORE any connect");
	ct_check(memcmp(g.inq[0], cnxman_join_name_mscp_disk,
			VMS_SCS_PROCNAME_LEN) == 0,
		 "MSCP$DISK is looked up");
	ct_check(memcmp(g.inq[1], cnxman_join_name_vaxcluster,
			VMS_SCS_PROCNAME_LEN) == 0,
		 "VMS$VAXcluster is looked up");
	ct_check_eq_u32(g.n_connect, 0u,
			"NOTHING is connected before a name is resolved "
			"(sec 4(L) shared-sequence deadlock)");
	/* Book p. 7-38: the joiner picks the CSB nearest the CLUB queue tail. */
	ct_check(g.j.target_sysid == MEMBER_SYSID,
		 "the join target is the CSB nearest the queue tail (p. 7-38)");

	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	ct_check_eq_u32(g.n_connect, 0u,
			"one HIT is not both: still no connect");
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MSCP_CONNECT,
			"step 3: MSCP$DISK connect issued after its HIT");
	ct_check(memcmp(g.last_local, cnxman_join_name_disk_cl_drvr,
			VMS_SCS_PROCNAME_LEN) == 0,
		 "... as VMS$DISK_CL_DRVR -> MSCP$DISK (sec 4(L)(c))");

	cnxman_join_opened(&g.j, MSCP_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"step 4: VMS$VAXcluster connect issued next");
	ct_check(memcmp(g.last_remote, cnxman_join_name_vaxcluster,
			VMS_SCS_PROCNAME_LEN) == 0,
		 "... VMS$VAXcluster -> VMS$VAXcluster, JOINER->MEMBER");

	cnxman_join_opened(&g.j, CM_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADVERTISE,
			"step 5: the VC is up and the burst went out");
	ct_check(sent_is(0, VMS_CM_CAT_CONFIG, VMS_CM_OP_MODEL),
		 "burst #1 is cat 0x01 op 0x14 (model), sec 4(o) row 1");
	ct_check(sent_is(1, VMS_CM_CAT_CONFIG, VMS_CM_OP_PARAMS),
		 "burst #2 is cat 0x01 op 0x01 (parameters), sec 4(o) row 2");
	ct_check_eq_u32(sent_le16(1, VMS_OFB_CM_VOTES), 0u,
			"PARAMS carries the REAL configured VOTES");
	ct_check_eq_u32(g.j.config_sent, 0u,
			"op 0x02 is NOT in the initial burst (sec 4(o))");
	ct_check_eq_u32(g.j.mscp_cmds_sent, 1u,
			"the disk walk starts in the gap before op 0x02");

	/* The walk: SCC x2 then the NEXT-UNIT walk to its OFFLINE terminator. */
	{
		uint32_t len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);

		cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
		ct_check_eq_u32(g.j.mscp_cmds_sent, 2u,
				"SET CONTROLLER CHARACTERISTICS, twice");
		len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
		cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
		ct_check_eq_u32(g.j.mscp_cmds_sent, 3u,
				"then the first GET UNIT STATUS");

		len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u,
				 VMS_MSCP_ST_AVAILABLE);
		cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
		len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 1u), 2u,
				 VMS_MSCP_ST_AVAILABLE);
		cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
		ct_check_eq_u32(g.j.units_found, 2u,
				"two served units enumerated from the PEER's "
				"own answers");
		ct_check_eq_u32(g.j.config_sent, 0u,
				"op 0x02 still deferred while the walk runs");

		len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 2u), 3u,
				 VMS_MSCP_ST_OFFLINE);
		cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	}
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"step 6: the OFFLINE terminator releases op 0x02");
	ct_check_eq_u32(g.j.config_sent, 1u, "op 0x02 sent exactly once");
	ct_check(sent_is(2, VMS_CM_CAT_CONFIG, VMS_CM_OP_CONFIG),
		 "the third CM body is cat 0x01 op 0x02 (sec 4(o) row 4)");

	/* Step 7: the member drives, and this node answers. */
	{
		uint32_t len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT,
				     0x0020);

		ct_check_eq_u32(join_feed(len),
				CNXMAN_JOIN_RX_CONSUMED, "op 0x03 consumed");
		ct_check_eq_u32(g.j.echoes_sent, 1u,
				"op 0x03 COMMIT answered with the 0x81 echo");
		len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_LOCKRB, 0x0021);
		(void)join_feed(len);
		ct_check_eq_u32(g.j.echoes_sent, 2u,
				"op 0x05 rebuild txn answered too");
	}

	/* Step 8: the hand-off. */
	{
		uint32_t len = mk_open_add(EPOCH, 0x0eu);

		(void)join_feed(len);
		ct_check_eq_u32(g.b.opens_answered, 1u,
				"the transition OPEN reached the BARRIER FSM");
		len = mk_go(EPOCH);
		(void)join_feed(len);
	}
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_BARRIER,
			"XITGO hands the wire to the barrier");
	ct_check(cnxman_join_handed_off(&g.j) != 0, "handed_off() agrees");
	ct_check(cnxman_barrier_phase2_committed(&g.b) != 0,
		 "the barrier committed Phase 2 (book p. 7-42)");
	ct_check_eq_u32(g.j.ignored_events, 0u,
			"not one event of the reference sequence was ignored");
}

/* ==========================================================================
 * 2. THE HONEST OMISSIONS -- negative assertions, the anti-LARP core
 * ========================================================================== */

/*
 * E30 (falsified + replaced by a real-VAX capture,
 * tests/lab/captures/op06-join-20260903.pcap): a MEMBERSHIP burst with NO
 * shape-valid coordinator CSID at either measured offset teaches this node
 * nothing. Still answered (the allowlist's CONSUME row), still honest.
 */
static void test_csid_no_coordinator_seen_stays_new(void)
{
	uint32_t len;

	printf("\n-- E30: no coordinator CSID in the burst -> stays NEW --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();

	len = mk_membership_csid(0u, 'A');
	(void)join_feed(len);

	ct_check_eq_u32(g.j.membership_records, 1u, "the burst was received");
	ct_check_eq_u32(g.j.csid_unpinned, 1u,
			"... and counted as a CSID that could NOT be learned");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"no coordinator CSID seen yet: the node stays NEW");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"THE CLUB'S LOCAL CSID IS STILL UNLEARNED (INV-6)");
	ct_check_eq_u32(g.cl.club.local_csid, 0u,
			"... and its value was not written either");
	ct_check_eq_u32(g.j.acks_sent, 1u,
			"the burst IS answered -- with the grounded cat-0x04 "
			"ack, never a 0x81 (allowlist CONSUME row)");
	ct_check(sent_is(n_cm_sent() - 1u, VMS_CM_CAT_ACK, 0x00u),
		 "... and that answer really is a cat-0x04 body");
}

/*
 * E30, byte-exact vectors from the real-VAX capture. VAX1's own record
 * (CSID 0x00010001, SCSSYSTEMID 1025, generation 1) taught the *coordinator's*
 * identity on the wire; this node's own SCSSYSTEMID (1027, the capture's
 * VAX3) combines with the WIRE-LEARNED generation to compute OVMX's own
 * CSID -- never the coordinator's value, never a copy.
 */
static void test_csid_wire_learned_form_a(void)
{
	uint32_t len;

	printf("\n-- E30: form A (body[24:28]), capture-exact 0x00010001 -> "
	       "generation 1 --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();
	g.cl.params.scssystemid = 1027ull; /* the capture's VAX3 sysid */

	len = mk_membership_csid(0x00010001u, 'A'); /* VAX1's own CSID */
	(void)join_feed(len);

	ct_check_eq_u32(g.j.csid_unpinned, 0u,
			"a shape-valid coordinator CSID WAS found");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER,
			"[ADMIT][CSID_LEARNED] -> MEMBER");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 1u, "the CLUB learned it");
	ct_check_eq_u32(g.cl.club.local_csid, 0x00010003u,
			"(1 << 16) | (1027 & 0x3ff) = 0x00010003 -- computed, "
			"not copied from the coordinator's 0x00010001");
}

/* The same mechanism through the OTHER measured offset (body[36:40]), with
 * the capture's other real value (VAX3's own re-asserted CSID). */
static void test_csid_wire_learned_form_b(void)
{
	uint32_t len;

	printf("\n-- E30: form B (body[36:40]), capture-exact 0x00010003 --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();
	g.cl.params.scssystemid = 1027ull;

	len = mk_membership_csid(0x00010003u, 'B');
	(void)join_feed(len);

	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER,
			"form B also fires the CSID cell");
	ct_check_eq_u32(g.cl.club.local_csid, 0x00010003u,
			"same computed CSID via the other offset");
}

/*
 * The generation is READ FROM THE WIRE, never hardcoded to the capture's
 * observed 1: a coordinator CSID whose generation is 7 makes this node
 * compute a CSID carrying 7, not 1.
 */
static void test_csid_generation_never_fabricated(void)
{
	uint32_t len;

	printf("\n-- E30: the generation is wire-learned, not baked in --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();
	g.cl.params.scssystemid = 1027ull;

	len = mk_membership_csid(0x00070005u, 'A'); /* generation 7, shape-valid */
	(void)join_feed(len);

	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER, "still fires");
	ct_check_eq_u32(g.cl.club.local_csid, 0x00070003u,
			"(7 << 16) | (1027 & 0x3ff): the generation tracked the "
			"wire value, not a constant 1");
}

/* The mechanism exists and the table cell is real -- exercised directly so
 * the edge itself is proven independent of the membership-burst path above. */
static void test_csid_learned_edge_exists(void)
{
	printf("\n-- the CSID edge is real, exercised directly --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();

	cnxman_join_csid_learned(&g.j, 0x00010003u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER,
			"[ADMIT][CSID_LEARNED] -> MEMBER");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 1u, "the CLUB learned it");
	ct_check_eq_u32(g.cl.club.local_csid, 0x00010003u, "... with the value");
}

static void test_lockdirwt_is_not_advertised(void)
{
	printf("\n-- FC-P3.2: LOCKDIRWT has no pinned offset, and we say so --\n");
	bed_init();
	bed_set_identity();
	g.cl.params.lockdirwt = 1;    /* a node that WANTS directory duty */
	drive_to_admit();

	ct_check(g.j.lockdirwt_unpinned >= 1u,
		 "every PARAMS counts the unpinned LOCKDIRWT field");
	ct_check_eq_u32(g.j.lockdirwt_unrepresentable, 1u,
			"a NONZERO LOCKDIRWT is reported as unrepresentable");
	ct_check(strstr(g.fake.last_log, "LOCKDIRWT") != NULL ||
		 g.fake.logs > 0u,
		 "... and logged on the console");

	/* And the zero case: honest, but still counted, because the bytes
	 * agreeing with the truth is a coincidence and not a placement. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	ct_check_eq_u32(g.j.lockdirwt_unrepresentable, 0u,
			"LOCKDIRWT 0 is representable (as an omitted field)");
	ct_check(g.j.lockdirwt_unpinned >= 1u,
		 "... and the omission is STILL counted");
}

static void test_no_invented_connect_data_or_descriptor(void)
{
	printf("\n-- E24 / sec 4(N): no replayed connect data, no invented "
	       "descriptor --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();

	ct_check_eq_u32(g.j.conndata_omitted, 1u,
			"with none configured, the 16-byte version field is "
			"OMITTED and counted");
	ct_check_eq_u32(g.last_had_conndata, 0,
			"... and SCS really was handed NULL, not a template");
	ct_check_eq_u32(g.j.dir_descriptor_omitted, 1u,
			"the VMS$VAXcluster directory descriptor is omitted");
	ct_check_eq_u32(g.n_set_dir_data, 0u,
			"... set_dir_data was NOT called with a guess");

	/* Supplied by the glue: then, and only then, it goes out. */
	{
		struct cnxman_join_cfg cfg;

		bed_init();
		memset(&cfg, 0, sizeof(cfg));
		cfg.conndata_valid = 1;
		cfg.dir_descriptor_valid = 1;
		cnxman_join_set_cfg(&g.j, &cfg);
		drive_to_admit();
		ct_check_eq_u32(g.j.conndata_omitted, 0u,
				"a CONFIGURED connect data is not an omission");
		ct_check_eq_u32(g.last_had_conndata, 1,
				"... and reaches SCS");
		ct_check_eq_u32(g.n_set_dir_data, 1u,
				"a CONFIGURED descriptor is declared");
	}
}

static void test_identity_omissions_are_counted(void)
{
	printf("\n-- an unconfigured identity is an omission, not a default --\n");
	bed_init();
	/* No bed_set_identity(): the glue supplied nothing. */
	drive_to_admit();

	ct_check_eq_u32(g.j.model_omitted, 1u, "no model string: counted");
	ct_check(g.j.version_omitted >= 1u, "no version string: counted");
	ct_check(g.j.node_params_omitted >= 1u,
		 "no node-parameter block: counted");
	ct_check(sent_is(0, VMS_CM_CAT_CONFIG, VMS_CM_OP_MODEL),
		 "op 0x14 still goes out ...");
	ct_check_eq_u32(nth_sent(0)->body[VMS_OFB_CM_MODEL_LEN], 0u,
			"... advertising NO model rather than a fake one");
	ct_check_eq_u32(g.j.target_level_unpinned, 1u,
			"the protocol/ECO selection rule is counted unpinned");
	ct_check_eq_u32(g.j.member_count_ungated, 1u,
			"the p. 7-37 member-count precondition is counted "
			"ungated (never faked into a gate)");
}

/* ==========================================================================
 * 3. The envelope really comes from the CSB
 * ========================================================================== */

static void test_envelope_is_csb_state(void)
{
	uint32_t len;

	printf("\n-- body[0:8] is the CSB's real dialogue state (sec 4(j)) --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();

	ct_check_eq_u32(sent_le16(0, 0u), 1u,
			"the FIRST VC message carries send-msg# 1");
	ct_check_eq_u32(sent_le16(1, 0u), 2u, "the second carries 2");
	ct_check_eq_u32(sent_le16(2, 0u), 3u, "the third carries 3");
	ct_check_eq_u32(g.member_csb->cm_send_msg, 3u,
			"the CSB itself holds the same count");

	/* ack-msg# is a MAXIMUM over what really arrived, so a retransmit
	 * that repeats a lower number cannot walk it back. */
	len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0x0050);
	(void)join_feed(len);
	ct_check_eq_u32(g.member_csb->cm_ack_msg, 0x0050u,
			"the peer's send-msg# became our ack-msg#");
	len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0x0040);
	(void)join_feed(len);
	ct_check_eq_u32(g.member_csb->cm_ack_msg, 0x0050u,
			"a retransmit at a LOWER number does not walk it back");
	ct_check_eq_u32(sent_le16(n_cm_sent() - 1u, 2u), 0x0050u,
			"and the echo we sent carries that ack-msg#");
}

/* ==========================================================================
 * 4. Refusals -- each honest, named, and terminal where it must be
 * ========================================================================== */

static void test_no_target_refuses(void)
{
	printf("\n-- refusals --\n");
	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.jops.dir_inquire = bed_dir_inquire;
	g.jops.connect = bed_connect;
	g.jops.send_msg = bed_send_msg;
	g.cl.params.scssystemid = OWN_SYSID;
	(void)cnxman_club_init(&g.cl);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);

	ct_check(cnxman_join_start(&g.j) != 0,
		 "a CLUB with no remote CSB refuses to start a join");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_NO_TARGET,
			"... naming the reason");
	ct_check_eq_u32(g.n_inq, 0u, "and nothing went on the wire");
}

static void test_vaxcluster_absent_is_fatal_mscp_absent_is_not(void)
{
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       0);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_FAILED,
			"NOT PRESENT HERE for VMS$VAXcluster fails the join");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_ABSENT, "... as ABSENT");

	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       0);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"a member that serves no disks is a REAL "
			"configuration: skip the walk, keep joining");
	ct_check_eq_u32(g.j.mscp_absent, 1u, "... counted");
	cnxman_join_opened(&g.j, CM_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"with no walk to wait for, op 0x02 goes at once");
	ct_check_eq_u32(g.j.mscp_cmds_sent, 0u, "and no MSCP command was sent");
}

static void test_reject_is_terminal(void)
{
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_rejected(&g.j, CM_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_FAILED,
			"a peer REJECT (p. 2-25 version gate) is terminal");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_REJECTED,
			"... named as the identity gate, not a retry");
}

/*
 * E71. Losing the VMS$VAXcluster connection is NOT a verdict (p. 7-30: "do not
 * presume that the remote system has left ... simply because the local
 * Connection Manager has lost contact"). The join names it, drops the Con.ID it
 * no longer holds, and goes back to needing a connection -- where the member's
 * own re-offer (p. 7-24 REACCEPT) is still adoptable.
 */
static void test_pathlost_keeps_the_join_alive(void)
{
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_closed(&g.j, CM_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"losing the VMS$VAXcluster connection does NOT fail the "
			"join: it needs the connection back");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_PATHLOST, "... as PATHLOST");
	ct_check_eq_u32(g.j.cm_lost, 1u, "... counted");
	ct_check_eq_u32(g.j.cm_conid, 0u,
			"... and this node stops claiming a connection it does "
			"not hold, which is what makes a re-offer adoptable");
	ct_check_eq_u32(g.j.cm_open, 0u, "... and does not call it open");

	/* p. 7-24 REACCEPT: the member dials us. The live E69/E70 runs both
	 * ended with exactly this frame being dropped into [FAILED]. */
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_adopted, 1u,
			"the member's re-offer is ADOPTED, not counted as "
			"already-held");
	ct_check_eq_u32(g.j.cm_already_held, 0u, "... never as already-held");
	cnxman_join_opened(&g.j, ACC_CM_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"... and the drive resumes to admission on it");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_NONE,
			"... with the stop reason cleared, because connectivity "
			"really came back");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL),
		 "the sec 4(o) burst is made AGAIN on the new connection: "
		 "MODEL");
	ct_check(sent_on_is(ACC_CM_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS), "... PARAMS");
	ct_check(sent_on_is(ACC_CM_CONID, 2, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_CONFIG), "... and op-0x02");

	/* Nothing about this recovery asserts membership. */
	ct_check_eq_u32(cnxman_join_handed_off(&g.j), 0,
			"INV-6: a reconnect is a CONNECTION, never a "
			"membership -- this node is still not a member");
	ct_check_eq_u32(cnxman_club_local(&g.cl.club)->csid_valid, 0u,
			"... and this node's own CSID was NOT learned by "
			"reconnecting: only a real op-0x06 teaches it");

	/* But losing the disk-client connection after the walk is not a loss
	 * of anything the join still needs. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_closed(&g.j, MSCP_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"losing MSCP$DISK AFTER the walk does not fail it");
}

/*
 * E71. A connect this node could not put on the wire refused nothing to
 * nobody: no connect data reached a peer, so it is not p. 2-25's version gate
 * and it is not terminal. It is named, counted, and retried on the beat
 * (p. 7-30's "attempt once a second").
 */
static void test_connect_refusal_is_named_and_retried(void)
{
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	g.fail_connect = 1;
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"a local connect refusal does NOT fail the join");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_CONNECT, "... as CONNECT");
	ct_check_eq_u32(g.j.mscp_connect_refused, 1u,
			"the refused MSCP$DISK connect is counted, and stepped "
			"over: it is not a membership prerequisite");
	ct_check_eq_u32(g.j.cm_connect_refused, 1u,
			"... and so is the refused VMS$VAXcluster connect");
	ct_check_eq_u32(g.j.cm_conid, 0u,
			"this node claims no Con.ID it was never given");

	/* The beat retries it, and this time the port takes it. */
	g.fail_connect = 0;
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.cm_reattempts, 1u,
			"the once-a-second beat re-issues the connect (p. 7-30)");
	ct_check(memcmp(g.last_remote, cnxman_join_name_vaxcluster,
			VMS_SCS_PROCNAME_LEN) == 0,
		 "... really issuing the VMS$VAXcluster connect");
	cnxman_join_opened(&g.j, CM_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"... and the drive goes on from there");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_NONE,
			"... with the stop reason cleared");
}

/* ==========================================================================
 * 5. The server half -- total connectivity (spec sec 4(y), p. 7-11)
 * ========================================================================== */

static void test_server_half(void)
{
	static const uint8_t cd[VMS_SCS_PROCNAME_LEN] = {
		0x01, 0x1b, 0x01, 0x03, 0, 0, 0, 0, 0, 0, 0, 0x08, 0, 0, 0x06, 0
	};

	printf("\n-- the server half: every member connects to every member --\n");
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);

	ct_check_eq_u32(cnxman_join_connect_req(&g.j, OTHER_SYSID, 0x1234u,
						cd, sizeof(cd)) == 0, 1u,
			"a member's inbound connect is ACCEPTED mid-join");
	ct_check_eq_u32(g.j.inbound_accepted, 1u, "... and counted");
	ct_check(cnxman_join_connect_req(&g.j, 0x0000040009f9ull, 0x1235u,
					 NULL, 0) != 0,
		 "a system with no CSB is refused, not admitted blind");
	ct_check_eq_u32(g.j.inbound_refused, 1u, "... and that is counted too");
}

/* ==========================================================================
 * 6. The watchdog instruments and repeats; it never abandons
 * ========================================================================== */

static void test_watchdog(void)
{
	printf("\n-- the watchdog: p. 2-51's repeat, never a timeout --\n");
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	ct_check_eq_u32(g.n_inq, 2u, "two inquiries out");

	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_DIR_ROUND,
			"a slow directory answer does NOT abandon the join");
	ct_check_eq_u32(g.j.slow_steps, 1u, "... it is counted");
	ct_check_eq_u32(g.n_inq, 4u, "... and the poll REPEATS (p. 2-51)");
	ct_check_eq_u32(g.j.lookups_reissued, 1u, "... counted as a re-issue");

	/* In every other state it counts and logs only. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"a slow admission is instrumented, not timed out");
	ct_check(g.j.slow_steps >= 1u, "... and counted");
}

/* ==========================================================================
 * 7. The hand-off contract, both wirings
 * ========================================================================== */

static void test_handoff_without_a_barrier(void)
{
	uint32_t len;

	printf("\n-- hand-off: forwarded when installed, handed back when not --\n");
	bed_init();
	bed_set_identity();
	cnxman_join_set_barrier(&g.j, NULL);
	drive_to_admit();

	len = mk_open_add(EPOCH, 0x0eu);
	ct_check_eq_u32(join_feed(len),
			CNXMAN_JOIN_RX_HANDOFF,
			"with no barrier installed the caller must route it");
	len = mk_go(EPOCH);
	ct_check_eq_u32(join_feed(len),
			CNXMAN_JOIN_RX_HANDOFF, "... including the GO");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_BARRIER,
			"and the join still records the hand-off point");
	ct_check_eq_u32(g.j.handoffs, 0u, "nothing was double-delivered");

	/* The barrier owns the transition family; the join keeps its own
	 * steady-state obligations from BARRIER onward. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	len = mk_go(EPOCH);
	(void)join_feed(len);
	len = mk_cm(VMS_CM_CAT_MEMBERSHIP, VMS_CM_OP_CLOSE, 0x0060);
	(void)join_feed(len);
	ct_check_eq_u32(g.j.closes_answered, 1u,
			"the cat-0x06 close is still answered during a "
			"transition (a recurring member poll, sec 4(q))");
}

static void test_unowned_frame_is_not_mine(void)
{
	uint32_t len = 0;

	bed_init();
	bed_set_identity();
	drive_to_admit();
	/* cat 0x02 op 0x01 -- the steady-state DLM traffic, measured as
	 * ungrounded and deliberately absent from the allowlist. */
	len = mk_cm(VMS_CM_CAT_DLM, 0x01u, 0x0070);
	ct_check_eq_u32(join_feed(len),
			CNXMAN_JOIN_RX_NOT_MINE,
			"an ungrounded pair is routed on, never answered");
	ct_check_eq_u32(g.j.echoes_sent, 0u,
			"... and no response was emitted for it");
}

/* ==========================================================================
 * 8. THE TABLE: every populated cell fires, every empty cell is counted
 *
 * The table IS this machine's specification, so it is walked exhaustively
 * rather than sampled. `expect[state][event]` is the ONE place a cell's
 * existence is asserted, and a cell added to the FSM without a cell added
 * here fails this test.
 * ========================================================================== */

/* Put the FSM into `state` with the bed freshly initialised. */
static void drive_to_state(enum cnxman_join_state s)
{
	uint32_t len;

	bed_init();
	bed_set_identity();
	if (s == CNXMAN_JOIN_IDLE)
		return;

	(void)cnxman_join_start(&g.j);
	if (s == CNXMAN_JOIN_DIR_ROUND)
		return;

	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	if (s == CNXMAN_JOIN_MSCP_CONNECT)
		return;

	cnxman_join_opened(&g.j, MSCP_CONID);
	if (s == CNXMAN_JOIN_VC_CONNECT)
		return;

	cnxman_join_opened(&g.j, CM_CONID);
	if (s == CNXMAN_JOIN_ADVERTISE)
		return;

	len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u, VMS_MSCP_ST_OFFLINE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	if (s == CNXMAN_JOIN_ADMIT)
		return;

	if (s == CNXMAN_JOIN_FAILED) {
		cnxman_join_rejected(&g.j, CM_CONID, 0u);
		return;
	}

	len = mk_go(EPOCH);
	(void)join_feed(len);
	if (s == CNXMAN_JOIN_BARRIER)
		return;

	cnxman_join_csid_learned(&g.j, 0x00010003u);
}

/* Fire `ev` at the FSM in whatever state it is in. Returns 0 if the event was
 * deliverable at all (every one below is). */
static void fire(enum cnxman_event ev)
{
	uint32_t len;

	switch (ev) {
	case CNXMAN_EV_START:
		(void)cnxman_join_start(&g.j);
		break;
	case CNXMAN_EV_CDT_OPEN:
		cnxman_join_opened(&g.j, g.j.state ==
				   (uint8_t)CNXMAN_JOIN_MSCP_CONNECT
				   ? MSCP_CONID : CM_CONID);
		break;
	case CNXMAN_EV_CDT_CLOSED:
		cnxman_join_closed(&g.j,
				   g.j.state ==
				   (uint8_t)CNXMAN_JOIN_MSCP_CONNECT
				   ? MSCP_CONID : CM_CONID, 0u);
		break;
	case CNXMAN_EV_DIR_RESULT:
		cnxman_join_dir_result(&g.j, MEMBER_SYSID,
				       cnxman_join_name_vaxcluster, 1);
		break;
	case CNXMAN_EV_MSCP_END:
		len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
		cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
		break;
	case CNXMAN_EV_TIMER_JOIN:
		cnxman_join_timer(&g.j);
		break;
	case CNXMAN_EV_CSID_LEARNED:
		cnxman_join_csid_learned(&g.j, 0x00010003u);
		break;
	case CNXMAN_EV_CM_ACCEPTED:
		cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
		break;
	case CNXMAN_EV_RX_CONFIG:
		len = mk_peer_params(1u, 0x0080);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_COMMIT:
		len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0x0081);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_MEMBERSHIP:
		/* No coordinator CSID in this fixture: exercising the cell
		 * itself, not the CSID-learn edge (covered separately above),
		 * so it must not perturb the state this generic driver put
		 * the FSM in. */
		len = mk_membership_csid(0u, 'A');
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_CLOSE:
		len = mk_cm(VMS_CM_CAT_MEMBERSHIP, VMS_CM_OP_CLOSE, 0x0082);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_TR_OPEN:
		len = mk_open_add(EPOCH, 0x0eu);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_TR_GO:
		len = mk_go(EPOCH);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_BARRIER:
		len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL, 0x0083);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_BARRIER_ACK:
		len = mk_cm((uint8_t)(VMS_CM_CAT_CONFIG | 0x80u),
			    VMS_CM_OP_BARRIER, 0x0084);
		(void)join_feed(len);
		break;
	case CNXMAN_EV_RX_REBUILD:
		len = mk_cm(VMS_CM_CAT_DLM, VMS_CM_OP_DLM_REBUILD, 0x0085);
		(void)join_feed(len);
		break;
	default:
		break;
	}
}

/* The events this FSM's entry points can actually deliver. The four the shared
 * vocabulary carries for OTHER tables (RX_TR_REQUEST, RX_TR_RELAY, RX_TR_ACK,
 * SHUTDOWN) have no join entry point and are not walked here. */
static const enum cnxman_event walked[] = {
	CNXMAN_EV_START, CNXMAN_EV_CDT_OPEN, CNXMAN_EV_CDT_CLOSED,
	CNXMAN_EV_DIR_RESULT, CNXMAN_EV_MSCP_END, CNXMAN_EV_TIMER_JOIN,
	CNXMAN_EV_CSID_LEARNED, CNXMAN_EV_RX_CONFIG, CNXMAN_EV_RX_COMMIT,
	CNXMAN_EV_RX_MEMBERSHIP, CNXMAN_EV_RX_CLOSE, CNXMAN_EV_RX_TR_OPEN,
	CNXMAN_EV_RX_TR_GO, CNXMAN_EV_RX_BARRIER, CNXMAN_EV_RX_BARRIER_ACK,
	CNXMAN_EV_RX_REBUILD, CNXMAN_EV_CM_ACCEPTED
};

/* 1 = this cell is POPULATED in vms_cnxman_join_fsm.c's table. Kept here as
 * the test's own independent statement of the specification: a handler added
 * without a 1 here (or a 1 here without a handler) reds this test. */
static const uint8_t expect[CNXMAN_JOIN_STATE__COUNT][CNXMAN_EV__COUNT] = {
	/* E73: [RX_CONFIG] is populated in every state a member's connection
	 * can be OPEN in, because this node advertises to every such member on
	 * the beat and every member reciprocates -- including before
	 * CLUSTER_START. */
	[CNXMAN_JOIN_IDLE] = {
		[CNXMAN_EV_START] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_RX_CONFIG] = 1,
	},
	[CNXMAN_JOIN_DIR_ROUND] = {
		[CNXMAN_EV_DIR_RESULT] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_MSCP_CONNECT] = {
		[CNXMAN_EV_CDT_OPEN] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_VC_CONNECT] = {
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_CDT_OPEN] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_ADVERTISE] = {
		[CNXMAN_EV_MSCP_END] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_RX_COMMIT] = 1,
		[CNXMAN_EV_RX_MEMBERSHIP] = 1,
		[CNXMAN_EV_RX_CLOSE] = 1,
		[CNXMAN_EV_RX_TR_OPEN] = 1,
		[CNXMAN_EV_RX_TR_GO] = 1,
		[CNXMAN_EV_RX_REBUILD] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_ADMIT] = {
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_RX_COMMIT] = 1,
		[CNXMAN_EV_RX_MEMBERSHIP] = 1,
		[CNXMAN_EV_RX_CLOSE] = 1,
		[CNXMAN_EV_RX_TR_OPEN] = 1,
		[CNXMAN_EV_RX_TR_GO] = 1,
		[CNXMAN_EV_RX_REBUILD] = 1,
		[CNXMAN_EV_CSID_LEARNED] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_BARRIER] = {
		[CNXMAN_EV_RX_TR_OPEN] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_RX_TR_GO] = 1,
		[CNXMAN_EV_RX_BARRIER] = 1,
		[CNXMAN_EV_RX_BARRIER_ACK] = 1,
		[CNXMAN_EV_RX_REBUILD] = 1,
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_RX_COMMIT] = 1,
		[CNXMAN_EV_RX_MEMBERSHIP] = 1,
		[CNXMAN_EV_RX_CLOSE] = 1,
		[CNXMAN_EV_CSID_LEARNED] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
	},
	[CNXMAN_JOIN_MEMBER] = {
		[CNXMAN_EV_RX_TR_OPEN] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_RX_TR_GO] = 1,
		[CNXMAN_EV_RX_BARRIER] = 1,
		[CNXMAN_EV_RX_BARRIER_ACK] = 1,
		[CNXMAN_EV_RX_REBUILD] = 1,
		[CNXMAN_EV_RX_CONFIG] = 1,
		[CNXMAN_EV_RX_COMMIT] = 1,
		[CNXMAN_EV_RX_MEMBERSHIP] = 1,
		[CNXMAN_EV_RX_CLOSE] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
	},
	[CNXMAN_JOIN_FAILED] = { 0 },
};

static void test_every_table_cell(void)
{
	unsigned st, k;
	unsigned populated = 0, empty = 0;
	int all_ok = 1;

	printf("\n-- the table: every [state][event] cell, populated and "
	       "empty --\n");

	for (st = 0; st < (unsigned)CNXMAN_JOIN_STATE__COUNT; st++) {
		for (k = 0; k < sizeof(walked) / sizeof(walked[0]); k++) {
			enum cnxman_event ev = walked[k];
			uint32_t before;
			int want = expect[st][ev];

			drive_to_state((enum cnxman_join_state)st);
			if (g.j.state != (uint8_t)st) {
				printf("  FAIL could not reach state %s\n",
				       cnxman_join_state_name(
					       (enum cnxman_join_state)st));
				all_ok = 0;
				continue;
			}
			before = g.j.ignored_events;
			fire(ev);

			if (want) {
				populated++;
				if (g.j.ignored_events != before) {
					printf("  FAIL [%s][%u] is populated "
					       "but the event was IGNORED\n",
					       cnxman_join_state_name(
						       (enum cnxman_join_state)st),
					       (unsigned)ev);
					all_ok = 0;
				}
			} else {
				empty++;
				if (g.j.ignored_events == before) {
					printf("  FAIL [%s][%u] is empty but "
					       "the event was NOT counted as "
					       "ignored\n",
					       cnxman_join_state_name(
						       (enum cnxman_join_state)st),
					       (unsigned)ev);
					all_ok = 0;
				}
			}
		}
	}
	ct_check(all_ok, "every table cell behaves as the specification says");
	printf("     (%u populated edges exercised, %u empty cells proved "
	       "ignored-and-counted)\n", populated, empty);
	ct_check(populated >= 50u,
		 "the walk really covered the whole populated table");
}

/* ==========================================================================
 * The disk-client readback (E64): a READ of the leg this join really holds
 *
 * The MSCP class driver's own sweep asks this before opening its own
 * `MSCP$DISK` connection, so that OVMX presents exactly ONE
 * `VMS$DISK_CL_DRVR` -> `MSCP$DISK` connection per member -- what every
 * reference joiner does. It must therefore answer from the live `mscp_conid`
 * and NEVER from the target selection alone, or the class driver would decline
 * a leg nobody ever opened and this node would MSCP-serve nothing.
 * ========================================================================== */
static void test_disk_client_readback(void)
{
	printf("\n-- the join reports the disk-client leg it really holds --\n");
	bed_init();
	bed_set_identity();

	ct_check_eq_u32((unsigned long)cnxman_join_holds_disk_client(
				&g.j, MEMBER_SYSID),
			0u, "before CLUSTER_START it holds nothing");

	(void)cnxman_join_start(&g.j);
	ct_check_eq_u32((unsigned long)cnxman_join_holds_disk_client(
				&g.j, MEMBER_SYSID),
			0u, "a SELECTED target is not a HELD connection");

	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MSCP_CONNECT,
			"the MSCP$DISK connect went out");
	ct_check(cnxman_join_holds_disk_client(&g.j, MEMBER_SYSID) != 0,
		 "... and NOW the join holds that member's disk-client leg");
	ct_check_eq_u32((unsigned long)cnxman_join_holds_disk_client(
				&g.j, MEMBER_SYSID + 7u),
			0u, "it holds nothing for any OTHER system");
	ct_check_eq_u32((unsigned long)cnxman_join_holds_disk_client(&g.j, 0u),
			0u, "and system 0 is not a member");
	ct_check_eq_u32((unsigned long)cnxman_join_holds_disk_client(NULL,
								     MEMBER_SYSID),
			0u, "a NULL join holds nothing");
}


/* ==========================================================================
 * E67: THE MEMBER MAY OPEN THE CONNECTION, AND THIS NODE STILL ADVERTISES
 *
 * The wall this locks: on join-e66refire (2026-09-04, the live 2-node VAX
 * cluster) both VAXes opened their own VMS$VAXcluster connection to this
 * node, each sent its cat-0x01 op-0x01 on it, and this node answered NOTHING
 * for the remaining 1600 s of the run -- because the join only ever
 * advertised on a connection IT had opened.
 *
 * The reference join (vax3-2to3-established-join-20260730) settles what a
 * real joiner does: VAX3 OPENED the connection to VAX1 (t+29.825) and
 * ACCEPTED the one VAX2 opened to it (t+30.367), and on BOTH it emitted its
 * own op-0x14 MODEL (send-msg# 1) and op-0x01 PARAMS (send-msg# 2) as
 * originations. One connection per pair; whichever side dialled.
 * ========================================================================== */

/* Drive to ADMIT with the MEMBER having opened the CM connection. */
static void drive_to_admit_member_dialled(void)
{
	uint32_t len;

	(void)cnxman_join_start(&g.j);
	/* The member's connect arrives while our own directory round is still
	 * outstanding -- the ordering the live run showed. */
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	cnxman_join_opened(&g.j, MSCP_CONID);

	len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u, VMS_MSCP_ST_AVAILABLE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 1u), 3u,
			 VMS_MSCP_ST_OFFLINE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
}

static void test_member_dialled_connection_still_promotes(void)
{
	uint32_t len;

	printf("\n-- E67: the MEMBER opened the VC and this node still "
	       "advertises --\n");
	bed_init();
	bed_set_identity();

	(void)cnxman_join_start(&g.j);
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_adopted, 1u,
			"the member's own connection is adopted as OURS");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_DIR_ROUND,
			"... without skipping the directory round: adopting a "
			"connection is not resolving a name");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"... and nothing is emitted on it yet (sec 4(L)'s "
			"lookup-before-connect is untouched)");

	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MSCP_CONNECT,
			"the drive still runs step 3 in its measured order");

	cnxman_join_opened(&g.j, MSCP_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADVERTISE,
			"step 4 is already satisfied by the adopted "
			"connection, so step 5 runs at once");
	ct_check_eq_u32(g.n_connect, 1u,
			"exactly ONE connect was issued -- MSCP$DISK; no "
			"SECOND VMS$VAXcluster connection to the same pair");

	/* THE WALL: the burst really goes out, and on the member's Con.ID. */
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 2u,
			"the MODEL+PARAMS burst went out on the member's own "
			"connection");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL),
		 "burst #1 is cat 0x01 op 0x14 (sec 4(o) row 1)");
	ct_check(sent_on_is(ACC_CM_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS),
		 "burst #2 is cat 0x01 op 0x01 (sec 4(o) row 2)");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 0, 0u), 1u,
			"an ORIGINATION: send-msg# 1, as the reference joiner "
			"sends on the connection VAX2 opened to it");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 1, 0u), 2u,
			"... then 2");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 0, VMS_OFB_CM_TXN), 0u,
			"an origination carries no transaction");
	ct_check_eq_u32(g.j.send_failures, 0u,
			"and not one send was refused (the E67 root cause was "
			"a glue thunk reporting SS$_NORMAL as a refusal)");

	/* ... and the member-driven tail runs on that same connection. The
	 * disk walk is untouched: SET CONTROLLER twice, then the NEXT-UNIT
	 * walk to the peer's own Unit-Offline terminator. */
	len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u, VMS_MSCP_ST_OFFLINE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"the disk walk's own terminator starts admission");
	ct_check(sent_on_is(ACC_CM_CONID, 2, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_CONFIG),
		 "op-0x02 CONFIG goes out on the member's connection too");

	len = mk_peer_params(2u, 0x0007);
	(void)join_feed(len);
	ct_check_eq_u32(g.member_csb->votes, 2u,
			"the member's reciprocated PARAMS reached the CSB: the "
			"dialogue is alive on the accepted connection");
}

static void test_member_dialled_reaches_the_barrier(void)
{
	uint32_t len;

	printf("\n-- E67: ... and the whole promotion runs from there --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit_member_dialled();
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT, "admission started");
	ct_check_eq_u32(g.j.model_sent, 1u, "one MODEL");
	ct_check_eq_u32(g.j.params_sent, 1u, "one PARAMS");
	ct_check_eq_u32(g.j.config_sent, 1u, "one CONFIG");

	/* The member's op-0x03 COMMIT is answered with the grounded echo -- on
	 * the adopted connection, which is what proves the join really owns
	 * it and not merely recorded it. */
	len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0x0081);
	(void)join_feed(len);
	ct_check_eq_u32(g.j.echoes_sent, 1u, "the COMMIT was echoed");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 4u,
			"... on the member's own connection");

	len = mk_go(EPOCH);
	(void)join_feed(len);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_BARRIER,
			"the XITGO hands off to the barrier as usual");
}

/* ==========================================================================
 * E68: the LIVE-RUN sequence the beds used to mask
 *
 * On the live 2-node VAX cluster (join-e67refire, 2026-09-04) the member
 * REJECTED this node's MSCP$DISK connect 0.2 ms after it went out. The join
 * failed the whole drive on that (PATHLOST), [FAILED] is an empty row, and the
 * two members' own VMS$VAXcluster connections 1.2 s later were counted and
 * dropped -- zero CM frames from this node in 1600 s. Every case below is a
 * fact from that pcap, replayed in its measured order.
 * ========================================================================== */

/* Drive to the instant the member's answer to our disk-client connect is due:
 * both names resolved, our MSCP$DISK connect out, nothing else yet. */
static void drive_to_mscp_connect(void)
{
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
}

static void test_disk_client_refusal_does_not_stop_the_join(void)
{
	printf("\n-- E68: a refused MSCP$DISK connect is not a failed join --\n");

	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MSCP_CONNECT,
			"our disk-client connect is out (live t+14.9372)");

	/* The member's REJECT_REQUEST, 0.2 ms later on the real wire. */
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);
	ct_check(g.j.state != CNXMAN_JOIN_FAILED,
		 "the member refusing our disk-client connect does NOT fail "
		 "the join");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_NONE,
			"... and names no failure at all");
	ct_check_eq_u32(g.j.mscp_rejected, 1u, "... it is counted");
	ct_check_eq_u32(g.j.mscp_conid, 0u,
			"... this node no longer claims a disk-client "
			"connection it does not have");
	ct_check_eq_u32(cnxman_join_holds_disk_client(&g.j, MEMBER_SYSID), 0,
			"... and says so to the disk class driver");

	/* And the drive carries on to step 4 rather than stopping. */
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"the drive goes on to the VMS$VAXcluster step");
	ct_check(memcmp(g.last_remote, cnxman_join_name_vaxcluster,
			VMS_SCS_PROCNAME_LEN) == 0,
		 "... by really issuing that connect");

	cnxman_join_opened(&g.j, CM_CONID);
	ct_check_eq_u32(g.j.model_sent, 1u, "the MODEL goes out (sec 4(o) row 1)");
	ct_check_eq_u32(g.j.params_sent, 1u, "then PARAMS (row 2)");
	ct_check(sent_on_is(CM_CONID, 0, VMS_CM_CAT_CONFIG, VMS_CM_OP_MODEL),
		 "... cat 0x01 op 0x14 first");
	ct_check(sent_on_is(CM_CONID, 1, VMS_CM_CAT_CONFIG, VMS_CM_OP_PARAMS),
		 "... then cat 0x01 op 0x01");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"with no walk to run, admission starts at once");
	ct_check_eq_u32(g.j.mscp_cmds_sent, 0u,
			"and not one MSCP command was sent on a connection we "
			"do not hold");
}

static void test_disk_client_loss_does_not_stop_the_join(void)
{
	/* The same fact arriving as a plain close rather than a REJECT: on the
	 * real wire the disk-client CDT belongs to the VMS$DISK_CL_DRVR SYSAP,
	 * whose glue reports every close (rejection included) through
	 * cnxman_join_closed(). Both entry points must behave alike. */
	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	cnxman_join_closed(&g.j, MSCP_CONID, 0u);
	ct_check(g.j.state != CNXMAN_JOIN_FAILED,
		 "losing the disk-client connection BEFORE the walk does not "
		 "fail the join either");
	ct_check_eq_u32(g.j.mscp_lost, 1u, "... counted as a loss, not a reject");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"... and the drive still reaches step 4");
}

static void test_refusal_then_member_dialled_still_promotes(void)
{
	/* The exact live ordering, with the member winning the connect race:
	 * its VMS$VAXcluster connection is adopted while our disk-client
	 * connect is still out, and THEN the disk-client refusal arrives. The
	 * burst must go out on the adopted Con.ID. This is the case that
	 * produced ZERO frames before E68. */
	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_adopted, 1u,
			"the member's own VMS$VAXcluster connection is adopted");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"... and nothing has gone out on it yet");

	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);
	ct_check_eq_u32(g.j.mscp_rejected, 1u, "the disk-client refusal arrives");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"... and the join advertises on the adopted connection "
			"instead of stopping");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL),
		 "MODEL on the member's own Con.ID");
	ct_check(sent_on_is(ACC_CM_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS),
		 "... then PARAMS");
	ct_check(sent_on_is(ACC_CM_CONID, 2, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_CONFIG),
		 "... then the op-0x02 that starts admission");
	ct_check_eq_u32(n_sent_on(CM_CONID), 0u,
			"and no second VMS$VAXcluster connection was opened");
	/* ... and the PARAMS body carries this node's REAL SYSGEN votes (the bed
	 * boots VOTES=0, D-10's non-voting joiner), never a default. */
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 1, VMS_OFB_CM_VOTES),
			g.cl.params.votes,
			"VOTES is this node's real SYSGEN value, not a default");
}

static void test_vaxcluster_refusal_is_still_terminal(void)
{
	/* The over-correction control: E68 must not have made the p. 2-25
	 * version gate survivable. A REJECT of the VMS$VAXcluster connection
	 * is still a verdict on this node's identity. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_rejected(&g.j, CM_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_FAILED,
			"a VMS$VAXcluster REJECT is STILL terminal");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_REJECTED, "... as REJECTED");
	ct_check_eq_u32(g.j.mscp_rejected, 0u,
			"... and is not miscounted as a disk-client refusal");

	/* A Con.ID this join never opened is still nobody's business here. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_rejected(&g.j, ACC_CM_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"a reject naming a Con.ID this join never opened is "
			"ignored");
	ct_check_eq_u32(g.j.ignored_events, 1u, "... and counted");
}

/* ==========================================================================
 * E71 -- JOIN RESILIENCE: the live join-e70refire transcript, replayed
 *
 * That run's whole failure is five records long, and every case below is one
 * fact from it:
 *
 *   t+14.472  DIR_RESULT -> MSCP_CONNECT
 *   t+14.473  cdt-closed rc=3 (the member REJECTED our disk client)
 *   t+14.473  MSCP_CONNECT -> FAILED, "connect refused locally"
 *   t+16.398  the member opens ITS OWN VMS$VAXcluster connection to us
 *   t+16.399  ... dropped into the empty [FAILED] row (ignored_events)
 *
 * The middle record is the defect: with no adopted connection to fall back on,
 * the MSCP reject led straight into a VMS$VAXcluster connect this node's own
 * SCS refused, and that refusal ended the join two seconds before the cluster
 * offered it membership.
 * ========================================================================== */

/* Park the target's CSB where the ladder parks a connection whose p. 7-30
 * reconnect window has run out -- by driving the ladder itself, not by writing
 * the field, so this test fails if that verdict ever stops being DISCONNECT. */
static void member_csb_reconnect_window_expires(void)
{
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONNECT_SENT, &g.ops);
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONN_OPEN, &g.ops);
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONN_LOST, &g.ops);
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_RECNX_EXPIRED, &g.ops);
}

static void test_e70_sequence_reaches_the_membership_offer(void)
{
	printf("\n-- E71: the live E70 sequence, and the offer it dropped --\n");

	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	g.fail_connect = 1;                       /* the local SCS says no    */
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u); /* rc=3, t+14.473         */

	ct_check(g.j.state != CNXMAN_JOIN_FAILED,
		 "the E70 pair -- a rejected disk client AND a locally refused "
		 "VMS$VAXcluster connect -- does not end the join");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"... it leaves the join needing its connection");
	ct_check_eq_u32(g.j.mscp_rejected, 1u, "the reject is counted");
	ct_check_eq_u32(g.j.cm_connect_refused, 1u,
			"... and so is the local refusal");

	/* t+16.398: the genuine membership connection, 1.9 s later. */
	g.fake.now_ms += 1925u;
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	cnxman_join_opened(&g.j, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_adopted, 1u,
			"the member's own connection is ADOPTED, not dropped "
			"into an empty [FAILED] row");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"... and this node advertises itself at last");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 3u,
			"three CM frames leave this node -- the run that "
			"produced ZERO in 1600 s");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL), "MODEL first");
	ct_check(sent_on_is(ACC_CM_CONID, 2, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_CONFIG), "... op-0x02 last");
	ct_check_eq_u32(cnxman_join_handed_off(&g.j), 0,
			"INV-6: reaching ADMIT is not being a member, and this "
			"node still claims nothing");
}

/*
 * INV-6, the anti-LARP half of resilience: a join that keeps trying must never
 * drift toward looking joined. Thirty beats with every connect refused.
 */
static void test_retrying_never_fabricates_a_join(void)
{
	uint32_t i;

	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	g.fail_connect = 1;
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);

	for (i = 0; i < 30u; i++) {
		g.fake.now_ms += 1000u;
		cnxman_join_timer(&g.j);
	}
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"thirty refused beats leave the join exactly where it "
			"was: still trying");
	ct_check_eq_u32(g.j.cm_reattempts, 30u, "... one attempt per beat");
	ct_check_eq_u32(g.j.cm_connect_refused, 31u,
			"... every one of them counted as refused");
	ct_check_eq_u32(g.n_sent, 0u, "not one frame was invented");
	ct_check_eq_u32(cnxman_join_handed_off(&g.j), 0,
			"this node is NOT a member");
	ct_check_eq_u32(cnxman_club_local(&g.cl.club)->csid_valid, 0u,
			"... this node's own CSID was never learned");
	ct_check_eq_u32(cnxman_club_recount_members(&g.cl.club), 0u,
			"... and the CLUB counts no members");
	ct_check_eq_u32(g.j.cm_open, 0u,
			"... and it never claims an open connection");
}

/*
 * The HONEST END. The bound is not this FSM's: it is the CSB's p. 7-30
 * reconnect timeout period, and when the ladder gives that connection up
 * (vms_cnxman_csb.c's csb_give_up -> DISCONNECT) the join reads that verdict
 * rather than keeping a hope of its own.
 */
static void test_expired_reconnect_window_ends_the_attempt_honestly(void)
{
	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	g.fail_connect = 1;
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT, "waiting");

	member_csb_reconnect_window_expires();
	g.fake.now_ms += 1000u;
	cnxman_join_timer(&g.j);

	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_TIMEOUT,
			"the expired reconnect window is named as the reason");
	ct_check_eq_u32(g.j.connect_windows_expired, 1u, "... and counted");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_IDLE,
			"the attempt is RELEASED -- not wedged in a terminal "
			"row where no later discovery could restart it");
	ct_check_eq_u32(cnxman_join_handed_off(&g.j), 0,
			"INV-6: an honest timeout is not a membership");
	ct_check_eq_u32(g.j.cm_conid, 0u, "... and holds no connection");

	/* And a genuinely new attempt is possible -- through a member the
	 * executive has NOT given up on. */
	g.fail_connect = 0;
	ct_check(cnxman_join_start(&g.j) == 0,
		 "a new join can be started once there is somewhere to start "
		 "one");
	ct_check_eq_u32(g.j.target_sysid == OTHER_SYSID, 1u,
			"... and it does NOT re-select the CSB the executive "
			"gave up on");
}

/*
 * The executive's own reconnect apparatus opened a connection this FSM had no
 * way of hearing about (its CDT_OPEN named a Con.ID the join was not holding).
 * p. 7-23 puts that Con.ID on the CSB, so the beat READS it there.
 */
static void test_beat_adopts_the_connection_the_executive_holds(void)
{
	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	g.fail_connect = 1;
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT, "waiting");

	/* What the glue does on a reconnect it issued: the Con.ID on the CSB,
	 * and the ladder's own OPEN when the CDT comes up. */
	g.member_csb->cdt_conid = ACC_CM_CONID;
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONNECT_SENT, &g.ops);
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONN_OPEN, &g.ops);

	g.fake.now_ms += 1000u;
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.cm_resynced, 1u,
			"the beat takes the Con.ID off the CSB");
	ct_check_eq_u32(g.j.cm_conid, ACC_CM_CONID, "... that exact one");
	ct_check_eq_u32(g.j.cm_reattempts, 0u,
			"... instead of opening a SECOND connection to a pair "
			"that may hold only one");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"... and, the CSB being OPEN, the drive resumes on it");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL),
		 "... with the burst going out on the adopted connection");
}

/*
 * The burst mask, not the lifetime counters. After a reconnect the new
 * connection has carried nothing, however much the old one carried.
 */
static void test_reoffer_is_per_connection_not_per_lifetime(void)
{
	bed_init();
	bed_set_identity();
	drive_to_admit();
	ct_check_eq_u32(g.j.model_sent, 1u, "the burst went out once");

	/* The connection goes, and the member re-offers -- but SCS refuses
	 * every send on the new one. */
	cnxman_join_closed(&g.j, CM_CONID, 0u);
	g.fail_send = 1;
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	cnxman_join_opened(&g.j, ACC_CM_CONID);
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"nothing got through on the new connection");
	ct_check_eq_u32(g.j.model_sent, 1u,
			"... so the LIFETIME count is still the old one's");

	g.fail_send = 0;
	g.fake.now_ms += 1000u;
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.burst_reoffers, 1u,
			"the beat re-offers the burst on the connection this "
			"node holds NOW -- a lifetime counter would have said "
			"'already advertised' and gone silent for good");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL), "MODEL really went out on it");
	ct_check_eq_u32(g.j.model_sent, 2u, "... and the lifetime count moved");
}

/*
 * A start with nowhere to start is VMS's "waiting to form or join an OpenVMS
 * Cluster", which is a REPEATED question -- not one attempt with a terminal
 * answer.
 */
static void test_a_start_with_no_target_is_deferred_not_terminal(void)
{
	bed_init();
	bed_set_identity();
	member_csb_reconnect_window_expires();
	/* Both remote CSBs given up on: nothing joinable at this instant. */
	(void)cnxman_csb_dispatch(&g.cl.club, cnxman_club_csb_at(&g.cl.club, 1u),
				  CNXMAN_CSB_EV_CONNECT_SENT, &g.ops);
	(void)cnxman_csb_dispatch(&g.cl.club, cnxman_club_csb_at(&g.cl.club, 1u),
				  CNXMAN_CSB_EV_DISCONNECT, &g.ops);

	ct_check(cnxman_join_start(&g.j) != 0, "no join starts");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_IDLE,
			"... and the FSM stays in IDLE, where the next beat can "
			"ask again -- never in a terminal row");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_NO_TARGET,
			"... with the reason named");
	ct_check_eq_u32(g.j.starts_deferred, 1u, "... and counted");
	ct_check_eq_u32(g.n_inq, 0u, "nothing went on the wire");

	/* A member the executive has not given up on appears. */
	(void)cnxman_club_alloc_csb(&g.cl.club, 0x000004000104ull, 1);
	ct_check(cnxman_join_start(&g.j) == 0,
		 "and the very next start goes, because nothing was wedged");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_DIR_ROUND, "... into DIR ROUND");
}

/*
 * E70 -- A REFUSED BURST IS RE-OFFERED ON THE JOIN'S OWN BEAT.
 *
 * THE LIVE WALL (join-e69, 2026-09-04). The member dialled first, this node
 * adopted its connection, the drive reached step 5 and built all three
 * originations -- and SCS refused every one of them. The join then sat waiting
 * for an answer to messages that had never left the node until the circuit
 * died under it. Nothing on the wire could show it: the frames did not exist.
 *
 * p. 2-51's rule is the one this FSM already applies to the directory round --
 * the poller REPEATS -- and it is what makes a transient refusal (a spent port
 * send window, a full unacked ring) survivable. The two halves asserted here:
 * a message that was REFUSED is offered again, and a message that really WENT
 * is never offered twice.
 */
static void test_a_refused_burst_is_reoffered(void)
{
	printf("\n-- E70: SCS refused the burst; the watchdog re-offers it "
	       "--\n");
	bed_init();
	bed_set_identity();
	g.fail_send = 1;
	g.fail_send_rc = 2692;   /* the executive's own "cannot carry this" */

	drive_to_admit_member_dialled();
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"the drive reached admission (the live shape)");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"but NOTHING left the node: SCS refused every send");
	/* Three CM originations plus the four MSCP disk-walk commands this bed
	 * drives -- every send this join made was refused. */
	ct_check_eq_u32(g.j.send_failures, 7u,
			"MODEL, PARAMS and CONFIG were each refused (with the "
			"walk's four commands)");
	ct_check_eq_u32(g.j.model_sent + g.j.params_sent + g.j.config_sent, 0u,
			"... so not one of them counts as sent");
	ct_check(g.j.state != CNXMAN_JOIN_FAILED,
		 "a refused send does not fail the join");

	/* The refusal clears -- a port window that filled and drained again,
	 * which is what several of SCS's refusals really are. */
	g.fail_send = 0;
	cnxman_join_timer(&g.j);

	ct_check_eq_u32(g.j.burst_reoffers, 1u,
			"the watchdog re-offered the burst exactly once");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 3u,
			"and all three originations reached SCS this time");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL),
		 "in the measured order: cat 0x01 op 0x14 first (sec 4(o) "
		 "row 1)");
	ct_check(sent_on_is(ACC_CM_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS),
		 "... then op 0x01 (row 2)");
	ct_check(sent_on_is(ACC_CM_CONID, 2, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_CONFIG),
		 "... then op 0x02, which is due because the disk walk "
		 "finished (row 6)");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 0, 0u), 4u,
			"a re-offer is a NEW origination with its own "
			"send-msg#: the refused ones burned 1..3, so this is "
			"4 -- a gap, never a repeat (spec sec 4(j))");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"and the join is still in ADMIT, driving");

	/* NOTHING IS SENT TWICE. A second tick has nothing left to re-offer. */
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 3u,
			"a message that really went is never offered again");
	ct_check_eq_u32(g.j.burst_reoffers, 1u,
			"... and the re-offer counter does not move");
}

/*
 * The other half of the same rule: while the refusal PERSISTS the join keeps
 * offering (it never gives up on its own -- p. 2-51: nothing expires), and
 * with no VMS$VAXcluster connection it offers nothing at all, because there is
 * no connection to originate on.
 */
static void test_reoffer_is_bounded_by_the_connection(void)
{
	printf("\n-- E70: re-offering needs an open connection, and repeats "
	       "--\n");
	bed_init();
	bed_set_identity();
	g.fail_send = 1;
	drive_to_admit_member_dialled();

	cnxman_join_timer(&g.j);
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.burst_reoffers, 2u,
			"a persistent refusal is re-offered on every beat");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"... and still nothing goes, honestly");
	ct_check(g.j.send_failures > 3u, "every attempt is counted");

	/* The connection goes: the join fails on the close, and a watchdog
	 * that fired instead would have nothing to offer. */
	bed_init();
	bed_set_identity();
	g.fail_send = 1;
	drive_to_admit_member_dialled();
	g.j.cm_open = 0u;   /* what cnxman_join_closed() sets on a lost CM */
	{
		uint32_t before = g.j.send_failures;

		cnxman_join_timer(&g.j);
		ct_check_eq_u32(g.j.burst_reoffers, 0u,
				"with no open connection nothing is re-offered");
		ct_check_eq_u32(g.j.send_failures, before,
				"... and no send is even attempted");
	}
}

static void test_adoption_refuses_what_it_does_not_own(void)
{
	printf("\n-- E67 negative controls: what is NOT adopted --\n");

	/* (a) another member's connection. Total connectivity requires taking
	 * it; it is not this join's dialogue. */
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	cnxman_join_cm_accepted(&g.j, OTHER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_adopted, 0u,
			"a connection from a member this join is not driving "
			"through is NOT adopted");
	ct_check_eq_u32(g.j.cm_other_member, 1u, "... it is counted");
	ct_check_eq_u32(g.j.cm_open, 0u, "... and we still have no VC");
	ct_check_eq_u32(g.n_sent, 0u, "... and nothing was sent on it");

	/* (b) A TRUE SIMULTANEOUS OPEN: this join already holds a connection to
	 * this member that is really OPEN. There is one per pair and no capture
	 * grounds which side yields, so this node invents nothing -- it keeps
	 * the connection it HAS and counts the collision. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	ct_check_eq_u32(g.j.cm_open, 1u, "our own VMS$VAXcluster VC is OPEN");
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_already_held, 1u,
			"a simultaneous open is counted, not adopted");
	ct_check(g.j.cm_conid == CM_CONID,
		 "... and the OPEN connection we hold is not replaced");
	ct_check_eq_u32(g.j.cm_superseded, 0u,
			"... nothing was superseded: both connections exist");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"... nothing goes out on the connection we did not "
			"adopt");
}

/* ==========================================================================
 * E72 -- A CON.ID THIS JOIN HOLDS IS NOT A CONNECTION THIS NODE HAS
 *
 * THE WALL, live (join-e71refire, 2026-09-04). The E71 resilience work kept the
 * join alive through the transient, and the cluster then did exactly what
 * p. 7-24 REACCEPT describes: the member opened its OWN VMS$VAXcluster
 * connection to this node and SCS brought it up. The join was holding the
 * Con.ID of its own connect -- which had never reached OPEN -- so it counted
 * the member's genuine, open membership connection as a simultaneous open and
 * dropped it; the CDT_OPEN that followed named a Con.ID it was not holding and
 * was ignored; and the transcript then reads one thing for the whole rest of
 * the run: [VC CONNECT], TIMER_JOIN, 1471 times, with ZERO cat-0x01 frames on
 * the wire and the member's CSB for this node still at votes 0.
 *
 * Both halves of the fix are asserted here, and both are reads of executive
 * state: the connection this node HAS beats the connect it merely ISSUED, and
 * the CSB's own p. 7-24 OPEN is acted on every beat, not only on the beat that
 * changes a Con.ID.
 * ========================================================================== */

static void test_e72_members_open_connection_supersedes_our_connect(void)
{
	uint32_t len;

	printf("\n-- E72: the member's OPEN connection beats our un-opened "
	       "connect --\n");
	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);   /* the live t+14.473 */

	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"our own VMS$VAXcluster connect is out");
	ct_check(g.j.cm_conid == CM_CONID, "... and this join holds its Con.ID");
	ct_check_eq_u32(g.j.cm_open, 0u, "... but it has NOT reached OPEN");

	/* t+16.4: the member opens its own and SCS brings it up -- the glue's
	 * two calls, in the order cnxman_vc_opened() makes them. */
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_adopted, 1u,
			"the member's OPEN connection is ADOPTED, not counted "
			"as a simultaneous open");
	ct_check_eq_u32(g.j.cm_superseded, 1u,
			"... and the connect it supersedes is counted");
	ct_check(g.j.cm_conid == ACC_CM_CONID, "... it is now THE connection");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"nothing is emitted on the accept alone: the OPEN is "
			"what confirms the connection is sendable");

	cnxman_join_opened(&g.j, ACC_CM_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"the CDT_OPEN drives [VC CONNECT] -> ADVERTISE, and with "
			"no disk walk to run, on to admission");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 3u,
			"three cat-0x01 originations reach SCS -- the run that "
			"produced ZERO in 1471 beats");
	ct_check(sent_on_is(ACC_CM_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL), "MODEL first (sec 4(o) row 1)");
	ct_check(sent_on_is(ACC_CM_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS), "... then PARAMS (row 2)");
	ct_check(sent_on_is(ACC_CM_CONID, 2, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_CONFIG), "... then op-0x02 (row 6)");
	ct_check_eq_u32(g.j.send_failures, 0u,
			"and every one of them was TAKEN by SCS (E69's three "
			"scs-refused emits were on a connection whose open had "
			"been dropped)");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 1, VMS_OFB_CM_VOTES),
			g.cl.params.votes,
			"PARAMS carries this node's REAL SYSGEN VOTES (INV-6)");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 0, 0u), 1u,
			"the envelope is the target CSB's own dialogue: "
			"send-msg# 1 ...");
	ct_check_eq_u32(sent_on_le16(ACC_CM_CONID, 2, 0u), 3u, "... then 3");
	ct_check_eq_u32(n_sent_on(CM_CONID), 0u,
			"and NOTHING went out on the connect that never opened");

	/* INV-6: reaching ADMIT is not being a member. Only the cluster's own
	 * op-0x06, carrying a real coordinator CSID, promotes this node. */
	ct_check_eq_u32(cnxman_join_handed_off(&g.j), 0,
			"this node claims no membership yet");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"... and no CSID has been learned");

	g.cl.params.scssystemid = 1027ull;   /* the capture's VAX3 */
	len = mk_membership_csid(0x00010001u, 'A');
	(void)join_feed(len);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER,
			"the cluster's own membership record promotes it: "
			"[ADMIT][CSID_LEARNED] -> MEMBER");
	ct_check_eq_u32(g.cl.club.local_csid, 0x00010003u,
			"... with the CSID computed from the WIRE-learned "
			"generation and this node's real SCSSYSTEMID");
}

static void test_e72_beat_advertises_on_the_open_it_missed(void)
{
	printf("\n-- E72: the beat advertises on the CSB's OPEN, not on a "
	       "Con.ID change --\n");
	bed_init();
	bed_set_identity();
	drive_to_mscp_connect();
	cnxman_join_rejected(&g.j, MSCP_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT, "waiting for the VC");

	/* What the glue records for a connect THIS JOIN issued: the Con.ID goes
	 * on the CSB at the instant SCS mints it (cnxman_jop_connect). */
	g.member_csb->cdt_conid = CM_CONID;
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONNECT_SENT, &g.ops);

	/* A beat with the connection still being made changes nothing. */
	g.fake.now_ms += 1000u;
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"a CSB that is not OPEN advertises nothing");
	ct_check_eq_u32(g.n_sent, 0u, "... and not one frame is invented");

	/* The connection really comes up -- and its CDT_OPEN never reaches this
	 * FSM (on the live run it named a Con.ID the join was not holding at
	 * the instant it was raised). The Con.ID does NOT move, which is the
	 * case the old beat could not see. */
	(void)cnxman_csb_dispatch(&g.cl.club, g.member_csb,
				  CNXMAN_CSB_EV_CONN_OPEN, &g.ops);
	g.fake.now_ms += 1000u;
	cnxman_join_timer(&g.j);

	ct_check_eq_u32(g.j.cm_resynced, 0u,
			"the Con.ID did not change: this is the OPEN the join "
			"missed, not a connection it did not know about");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"the beat reads p. 7-24 OPEN off the CSB and the drive "
			"resumes -- instead of 1471 silent beats");
	ct_check_eq_u32(n_sent_on(CM_CONID), 3u,
			"MODEL, PARAMS and op-0x02 went out on it");
	ct_check(sent_on_is(CM_CONID, 0, VMS_CM_CAT_CONFIG, VMS_CM_OP_MODEL),
		 "... in the measured order");
	ct_check_eq_u32(g.j.cm_reattempts, 0u,
			"... and no SECOND connection was opened to a pair that "
			"holds one");

	/* A further beat re-offers nothing: everything really went. */
	g.fake.now_ms += 1000u;
	cnxman_join_timer(&g.j);
	ct_check_eq_u32(n_sent_on(CM_CONID), 3u,
			"a message that really went is never sent twice");
	ct_check_eq_u32(g.j.burst_reoffers, 0u, "... and nothing was re-offered");
}

/* ==========================================================================
 * main
 * ========================================================================== */

/* ==========================================================================
 * E73. THE COMPLETION: from the promotion burst through to MEMBER
 *
 * Every assertion below is the reference join decoded frame by frame from
 * ~/vax/cluster/captures/vax3-2to3-established-join-20260730.pcap (spec
 * sec 4(o) rows 5-10, sec 4(p), sec 4(q)) -- and every one of them was
 * UNREACHABLE before E73, because the executive hands a SYSAP its own 132
 * bytes and this FSM's receive entry classified a 204-byte frame.
 * ========================================================================== */

/* Walk the CM bodies this node really sent and return 0 iff every send-msg# is
 * strictly greater than the one before it (spec sec 4(j), 17 539/17 541). */
static int send_msg_strictly_monotonic(uint32_t *out_first_repeat)
{
	uint32_t i, k = 0;
	uint32_t prev = 0;
	int seen = 0;

	for (i = 0; i < g.n_sent; i++) {
		uint16_t n;

		if (g.sent[i].conid != CM_CONID ||
		    g.sent[i].len != VMS_CM_BODY_LEN)
			continue;
		n = (uint16_t)(g.sent[i].body[VMS_OFB_CM_SEND_MSG] |
			       ((uint16_t)g.sent[i].body[VMS_OFB_CM_SEND_MSG + 1]
				<< 8));
		if (seen && n <= prev) {
			if (out_first_repeat != NULL)
				*out_first_repeat = k;
			return -1;
		}
		prev = n;
		seen = 1;
		k++;
	}
	return 0;
}

/*
 * THE SEAM ASSERTION. A SYSAP is handed its own bytes and nothing below them
 * (design sec 3.2.4). Feeding this FSM a whole frame is not a stricter test of
 * the same thing -- it is a DIFFERENT input that the executive never produces,
 * and believing it was is what let every host test stay green while the live
 * join lost VAX2's membership COMMIT.
 */
static void test_e73_the_executive_delivers_a_body(void)
{
	uint32_t len;

	printf("\n-- E73: the SYSAP is handed a BODY, and that is what parses --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT, "the join is in [ADMIT]");

	/* What SCS really delivers: 132 bytes, body[0] first. */
	len = mk_commit(0x0002);
	ct_check_eq_u32(join_feed(len), CNXMAN_JOIN_RX_CONSUMED,
			"the 132-byte op-0x03 COMMIT the executive delivers is "
			"CONSUMED");
	ct_check_eq_u32(g.j.echoes_sent, 1u,
			"... and answered with the grounded 0x81 echo -- the "
			"exact message join-e72refire dropped as `unparsed "
			"aux=0x84`");

	/* And the shape that is NOT a SYSAP body: a whole 204-byte frame. It
	 * must be refused, not silently mis-parsed from an offset that happens
	 * to land inside somebody else's header. */
	ct_check_eq_u32(cnxman_join_rx_body(&g.j, g_frame, VMS_CM_FRAME_LEN,
					    MEMBER_CSID, 1, member_csb_index()),
			CNXMAN_JOIN_RX_NOT_MINE,
			"a whole 204-byte FRAME is long enough to parse -- and "
			"parses as the frame's own abs [0,132), i.e. the "
			"Ethernet/SCA/SCS span, whose byte 8 is a MAC digit and "
			"no CM category: NOT_MINE, honestly");
	ct_check_eq_u32(g.j.echoes_sent, 1u,
			"... so nothing was answered on the strength of it "
			"(no second echo)");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"... and the join did not move");
}

/*
 * The whole post-ADMIT completion, in the reference's own order, on one
 * connection: COMMIT -> rebuild txns -> MEMBERSHIP burst -> transition open ->
 * GO -> twelve barrier steps -> the transition is over.
 */
static void test_post_admit_drive_to_member(void)
{
	uint32_t len, step;
	uint16_t peer_msg = 0x0002;
	uint32_t first_repeat = 0xffffffffu;

	printf("\n-- E73: the post-ADMIT completion, to MEMBER (sec 4(o)/(p)/(q)) --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();
	ct_check(sent_is(0, VMS_CM_CAT_CONFIG, VMS_CM_OP_MODEL) &&
		 sent_is(1, VMS_CM_CAT_CONFIG, VMS_CM_OP_PARAMS) &&
		 sent_is(2, VMS_CM_CAT_CONFIG, VMS_CM_OP_CONFIG),
		 "the promotion burst is out (sec 4(o) rows 1-2 and 4)");

	/* Row 6: the member's COMMIT, answered with the 0x81 echo (row 7). */
	len = mk_commit(++peer_msg);
	(void)join_feed(len);
	ct_check_eq_u32(g.j.echoes_sent, 1u, "op-0x03 COMMIT -> 0x81/0x03");
	ct_check(sent_is(3, vms_wire_response_category(VMS_CM_CAT_CONFIG),
			 VMS_CM_OP_COMMIT),
		 "  ... and that is what went out");

	/* Rows 8-9: the lock/resource rebuild transactions. */
	len = mk_lockrb(++peer_msg);
	(void)join_feed(len);
	len = mk_lockrb(++peer_msg);
	(void)join_feed(len);
	ct_check_eq_u32(g.j.echoes_sent, 3u,
			"each op-0x05 rebuild txn -> its own 0x81/0x05");

	/* Row 10: the op-0x06 MEMBERSHIP burst. It is answered with the
	 * opportunistic cat-0x04 ack, and it is the ONLY thing in this whole
	 * dialogue that can make this node a member. */
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"this node is still NEW: no op-0x06 has named a real "
			"generation yet");
	len = mk_membership_csid(MEMBER_CSID, 'A');
	(void)join_feed(len);
	ct_check_eq_u32(g.j.acks_sent, 1u,
			"op-0x06 -> the grounded cat-0x04 ack (sec 4(u))");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 1u,
			"... and the WIRE-LEARNED generation gave this node a "
			"CSID (E30)");
	ct_check_eq_u32(g.cl.club.local_csid,
			(unsigned long)((MEMBER_CSID & 0xffff0000u) |
					(OWN_SYSID & 0x3ffu)),
			"  == (the coordinator's own generation << 16) | our "
			"REAL SCSSYSTEMID -- never copied, never templated");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER,
			"the join reads MEMBER only now");

	/* sec 4(p): the transition open is Phase 1 and is ACKNOWLEDGED. */
	len = mk_open_add(EPOCH, 0x0eu);
	(void)join_feed(len);
	ct_check_eq_u32(g.b.opens_answered, 1u,
			"the op-0x09 open reached the barrier and was echoed");
	ct_check_eq_u32(g.b.bitmap_popcount, 3u,
			"  its bitmap 0x0e is the POST-transition member count "
			"(3), read not assumed");
	ct_check_eq_u32(g.b.coordinator_csb, (unsigned long)member_csb_index(),
			"  the coordinator is addressed by the CSB its "
			"transition arrived on (E73), not by a CSID this node "
			"cannot learn");

	/* The GO is Phase 2's point of no return and is NEVER answered. */
	len = mk_go(EPOCH);
	(void)join_feed(len);
	ct_check(cnxman_barrier_phase2_committed(&g.b) != 0,
		 "the op-0x0a GO commits Phase 2 (book p. 7-42)");
	ct_check_eq_u32(g.b.state, (unsigned long)CNXMAN_BARRIER_STEP,
			"and starts the barrier at step 1");
	ct_check_eq_u32(g.b.steps_sent, 1u,
			"step 1's op-0x0b really went out -- the send the "
			"CSID-addressed path could never make");

	/* Twelve steps: ack, then release, then the next step. */
	for (step = 1; step <= CNXMAN_BARRIER_STEPS; step++) {
		char what[96];

		len = mk_step_ack(step, ++peer_msg);
		(void)join_feed(len);
		snprintf(what, sizeof(what),
			 "  step %u: the 0x81/0x0b ack does NOT advance it",
			 (unsigned)step);
		ct_check_eq_u32(g.b.steps_sent, step, what);

		len = mk_release(step, ++peer_msg);
		(void)join_feed(len);
		snprintf(what, sizeof(what),
			 "  step %u: the op-0x0c release does", (unsigned)step);
		ct_check_eq_u32(g.b.steps_sent,
				step < CNXMAN_BARRIER_STEPS ? step + 1u : step,
				what);
	}
	ct_check_eq_u32(g.b.steps_sent, CNXMAN_BARRIER_STEPS,
			"exactly TWELVE op-0x0b requests, never a thirteenth "
			"(sec 4(p), 30 of 30 captures)");
	ct_check_eq_u32(g.b.state, (unsigned long)CNXMAN_BARRIER_COMPLETE,
			"release #12 completes the transition -- the count is "
			"the only termination signal");
	ct_check_eq_u32(g.b.transitions_completed, 1u,
			"one transition, completed");

	/* sec 4(j): every body this node originated carried its own number. */
	ct_check(send_msg_strictly_monotonic(&first_repeat) == 0,
		 "every send-msg# this node put on that connection is "
		 "STRICTLY greater than the last -- the join's dialogue and "
		 "the barrier's share one CSB and must share one counter");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_MEMBER,
			"and this node is a MEMBER at the end of it");
}

/*
 * THE ANTI-FABRICATION ASSERTION, and the reason this item exists. Run the
 * SAME completion with an op-0x06 that carries no shape-valid CSID: the
 * transition still completes (a joiner that ignores the barrier breaks the
 * cluster, sec 4(p)) and this node is still NOT a member.
 */
static void test_member_only_on_a_real_op06_csid(void)
{
	uint32_t len, step;
	uint16_t peer_msg = 0x0002;

	printf("\n-- E73: no op-0x06 CSID, no membership -- however far the "
	       "transition gets --\n");
	bed_init();
	bed_set_identity();
	drive_to_admit();

	len = mk_commit(++peer_msg);
	(void)join_feed(len);
	len = mk_lockrb(++peer_msg);
	(void)join_feed(len);

	/* A burst with NEITHER measured offset carrying a shape-valid CSID. */
	len = mk_membership_csid(0u, 'A');
	(void)join_feed(len);
	ct_check_eq_u32(g.j.membership_records, 1u, "the burst arrived");
	ct_check_eq_u32(g.j.csid_unpinned, 1u,
			"... and is COUNTED as one from which no CSID could be "
			"read");
	ct_check_eq_u32(g.j.acks_sent, 1u,
			"it is still acked -- an unanswered obligation strands "
			"the coordinator");

	len = mk_open_add(EPOCH, 0x0eu);
	(void)join_feed(len);
	len = mk_go(EPOCH);
	(void)join_feed(len);
	for (step = 1; step <= CNXMAN_BARRIER_STEPS; step++) {
		len = mk_step_ack(step, ++peer_msg);
		(void)join_feed(len);
		len = mk_release(step, ++peer_msg);
		(void)join_feed(len);
	}

	ct_check_eq_u32(g.b.state, (unsigned long)CNXMAN_BARRIER_COMPLETE,
			"the barrier ran to completion: this node met every "
			"obligation the cluster gated on");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"AND THE LOCAL CSID IS STILL UNLEARNED -- twelve "
			"completed steps do not make a member (INV-6)");
	ct_check(g.j.state != CNXMAN_JOIN_MEMBER,
			"the join does NOT read MEMBER");
	ct_check_eq_u32(cnxman_club_local(&g.cl.club)->csid_valid, 0u,
			"and the local CSB carries no assigned identity");
}

/* ==========================================================================
 * E73 part A: the identity exchange is PER-PEER
 * ========================================================================== */

/* Put a peer's CSB in the state the executive puts it in when its
 * VMS$VAXcluster connection is really up: a Con.ID it minted, and p. 7-24's
 * OPEN. Nothing here asserts membership -- only connectivity. */
static void bed_peer_connected(struct vms_csb *csb, vms_conid_t conid)
{
	csb->cdt_conid = (uint32_t)conid;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

static void test_per_peer_identity_exchange(void)
{
	struct vms_csb *other;
	const vms_conid_t OTHER_CONID = 0x4e62000fu;

	printf("\n-- E73 part A: every member hears what this node IS --\n");
	bed_init();
	bed_set_identity();

	other = cnxman_club_find_sysid(&g.cl.club, OTHER_SYSID);
	ct_check(other != NULL, "the other member has a CSB");
	if (other == NULL)
		return;

	/*
	 * THE E72 SHAPE, exactly: a member opens its VMS$VAXcluster connection
	 * to this node BEFORE CLUSTER_START. On the live run that connection
	 * landed in [IDLE], whose table has no CDT_OPEN cell, and VAX1 -- the
	 * node CLUSTER_NODES is read from -- never heard a word from us.
	 */
	bed_peer_connected(other, OTHER_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_IDLE, "no join has started");

	cnxman_join_advertise_peers(&g.j);
	ct_check_eq_u32(n_sent_on(OTHER_CONID), 2u,
			"the beat advertises to it ANYWAY -- the identity pair "
			"is a connection manager's per-CSB obligation, not a "
			"step of a join");
	ct_check(sent_on_is(OTHER_CONID, 0, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_MODEL),
		 "  #1 is cat 0x01 op 0x14 (model), as J->VAX1 at t+29.8253");
	ct_check(sent_on_is(OTHER_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS),
		 "  #2 is cat 0x01 op 0x01 (parameters)");
	ct_check_eq_u32(sent_on_le16(OTHER_CONID, 0, VMS_OFB_CM_SEND_MSG), 1u,
			"  and this peer's dialogue starts at send-msg# 1 "
			"(sec 4(j)), independently of any other peer's");
	ct_check_eq_u32(sent_on_le16(OTHER_CONID, 1, VMS_OFB_CM_SEND_MSG), 2u,
			"  the second carries 2");
	ct_check_eq_u32(sent_on_le16(OTHER_CONID, 1, VMS_OFB_CM_VOTES),
			(unsigned long)g.cl.params.votes,
			"  PARAMS carries this node's REAL configured VOTES");

	/* Idempotent: this connection has already carried both. */
	cnxman_join_advertise_peers(&g.j);
	ct_check_eq_u32(n_sent_on(OTHER_CONID), 2u,
			"a second beat says nothing again -- the record is per "
			"(peer, connection), not a lifetime counter");

	/* A connection that CHANGED has carried nothing. */
	bed_peer_connected(other, OTHER_CONID + 1u);
	cnxman_join_advertise_peers(&g.j);
	ct_check_eq_u32(n_sent_on(OTHER_CONID + 1u), 2u,
			"a NEW Con.ID to the same member is a new dialogue and "
			"is advertised on again (E71's per-connection rule)");
}

static void test_per_peer_skips_what_is_not_connected(void)
{
	struct vms_csb *other;

	printf("\n-- E73 part A: a peer with no OPEN connection is skipped, "
	       "not queued --\n");
	bed_init();
	bed_set_identity();
	other = cnxman_club_find_sysid(&g.cl.club, OTHER_SYSID);
	if (other == NULL)
		return;

	/* A Con.ID but no OPEN: the executive has not said it is connected. */
	other->cdt_conid = 0x4e620020u;
	other->state = (uint8_t)VMS_CNXMAN_CSB_CONNECT;
	cnxman_join_advertise_peers(&g.j);
	ct_check_eq_u32(g.n_sent, 0u,
			"a CSB the ladder does not call OPEN gets nothing -- "
			"p. 7-24's OPEN is the executive's own record of "
			"connectivity and this reads it");
	ct_check_eq_u32(g.j.peers_advertised, 0u, "and nothing is counted");

	/* OPEN but no Con.ID: nothing to send on, and no invented one. */
	other->cdt_conid = 0u;
	other->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	cnxman_join_advertise_peers(&g.j);
	ct_check_eq_u32(g.n_sent, 0u,
			"a CSB with no Con.ID gets nothing either: 0 is 'no "
			"connection', never 'connection zero'");
}

static void test_per_peer_covers_every_member(void)
{
	struct vms_csb *other;
	const vms_conid_t OTHER_CONID = 0x4e620031u;

	printf("\n-- E73 part A: BOTH members, each on its own VC (the "
	       "reference did) --\n");
	bed_init();
	bed_set_identity();
	other = cnxman_club_find_sysid(&g.cl.club, OTHER_SYSID);
	if (other == NULL)
		return;

	/* The join drives its own target and sends it the burst... */
	drive_to_admit();
	ct_check_eq_u32(n_sent_on(CM_CONID), 3u,
			"the target member got MODEL, PARAMS and CONFIG");

	/* ... and the beat covers the OTHER member, which the join never
	 * spoke to. That is the whole of part (A). */
	bed_peer_connected(other, OTHER_CONID);
	cnxman_join_advertise_peers(&g.j);
	ct_check_eq_u32(n_sent_on(OTHER_CONID), 2u,
			"the other member gets MODEL and PARAMS too");
	ct_check(sent_on_is(OTHER_CONID, 1, VMS_CM_CAT_CONFIG,
			    VMS_CM_OP_PARAMS),
		 "  its PARAMS is a real record it can put in its CSB for us");
	ct_check_eq_u32(n_sent_on(OTHER_CONID), 2u,
			"  and NOT op-0x02: admission is single-coordinator "
			"(sec 4(o): a non-coordinator SILENTLY DISCARDS it)");
	ct_check_eq_u32(g.j.peer_adverts_sent, 2u,
			"both records are counted as peer advertisements");
	ct_check_eq_u32(n_sent_on(CM_CONID), 3u,
			"and the target's dialogue is untouched -- one mask, "
			"so nothing is said twice");
}

/*
 * INV-6 ON THE NEW PATH. The per-peer beat is the one thing in this FSM that
 * runs unconditionally, on every CSB, whether or not a join is in flight -- so
 * it is the one place a "make it look joined" shortcut would be invisible.
 * What it may do is say what this node IS. What it may not do is touch one bit
 * of membership.
 */
static void test_per_peer_beat_asserts_no_membership(void)
{
	struct vms_csb *other;
	uint32_t i;

	printf("\n-- E73: the per-peer beat says what this node IS and nothing "
	       "else (INV-6) --\n");
	bed_init();
	bed_set_identity();
	other = cnxman_club_find_sysid(&g.cl.club, OTHER_SYSID);
	if (other == NULL)
		return;
	bed_peer_connected(other, 0x4e620040u);
	bed_peer_connected(g.member_csb, CM_CONID);

	for (i = 0; i < 200u; i++)
		cnxman_join_advertise_peers(&g.j);

	ct_check_eq_u32(g.n_sent, 4u,
			"200 beats, two peers, FOUR records -- the identity "
			"pair each, once per connection");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"the local CSID is STILL unlearned: only a real op-0x06 "
			"can name a generation");
	ct_check_eq_u32(cnxman_club_local(&g.cl.club)->csid_valid, 0u,
			"and the local CSB carries no assigned identity");
	ct_check_eq_u32(g.cl.club.transition_active, 0u,
			"no transition was invented");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_IDLE,
			"the join did not move -- advertising is not joining");
	ct_check_eq_u32(other->votes, 0u,
			"and the PEER's own CSB gained no parameters from it: "
			"this beat SENDS, it does not record anything about "
			"anybody else");
	ct_check_eq_u32(g.j.peer_adverts_sent, 4u,
			"exactly what was really transmitted is counted");
}

/*
 * A member's own PARAMS is filed under THAT MEMBER, in any state -- including
 * before CLUSTER_START, which is when VAX1's arrived on the live cluster.
 */
static void test_peer_params_land_in_the_senders_own_csb(void)
{
	struct vms_csb *other;
	uint32_t len;

	printf("\n-- E73: a member's VOTES go into ITS OWN CSB, whoever this "
	       "join is driving --\n");
	bed_init();
	bed_set_identity();
	other = cnxman_club_find_sysid(&g.cl.club, OTHER_SYSID);
	if (other == NULL)
		return;

	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_IDLE,
			"no join has started -- the state VAX1's op-0x01 "
			"arrived in on join-e72refire");
	len = mk_peer_params(2u, 1u);
	(void)join_feed_from(other, OTHER_CSID, len);
	ct_check_eq_u32(other->votes, 2u,
			"the SENDER's CSB got the VOTES it really advertised");
	ct_check_eq_u32(other->params_valid, 1u,
			"... and is marked as holding a real record");
	ct_check_eq_u32(g.member_csb->votes, 0u,
			"and the join's TARGET gained nothing -- a record is "
			"filed under whoever sent it, never under whoever this "
			"join happens to be driving");
	ct_check_eq_u32(g.member_csb->params_valid, 0u,
			"    (still no record at all for the target)");
	ct_check_eq_u32(other->cm_ack_msg, 1u,
			"the sender's own ack side advanced too -- one CSB, one "
			"dialogue");

	/* And the same record on the join's target lands there instead. */
	len = mk_peer_params(1u, 1u);
	(void)join_feed(len);
	ct_check_eq_u32(g.member_csb->votes, 1u,
			"the target's own record lands on the target");
	ct_check_eq_u32(other->votes, 2u, "and does not disturb the other");
}

int main(void)
{
	printf("test_cnxman_join: the join FSM (FC-P3.3, rung R1)\n");

	test_reference_sequence();
	test_disk_client_readback();
	test_csid_no_coordinator_seen_stays_new();
	test_csid_wire_learned_form_a();
	test_csid_wire_learned_form_b();
	test_csid_generation_never_fabricated();
	test_csid_learned_edge_exists();
	test_lockdirwt_is_not_advertised();
	test_no_invented_connect_data_or_descriptor();
	test_identity_omissions_are_counted();
	test_envelope_is_csb_state();
	test_no_target_refuses();
	test_vaxcluster_absent_is_fatal_mscp_absent_is_not();
	test_reject_is_terminal();
	test_pathlost_keeps_the_join_alive();
	test_connect_refusal_is_named_and_retried();
	test_server_half();
	test_watchdog();
	test_handoff_without_a_barrier();
	test_unowned_frame_is_not_mine();
	test_member_dialled_connection_still_promotes();
	test_member_dialled_reaches_the_barrier();
	test_a_refused_burst_is_reoffered();
	test_reoffer_is_bounded_by_the_connection();
	test_adoption_refuses_what_it_does_not_own();
	test_e72_members_open_connection_supersedes_our_connect();
	test_e72_beat_advertises_on_the_open_it_missed();
	test_disk_client_refusal_does_not_stop_the_join();
	test_disk_client_loss_does_not_stop_the_join();
	test_refusal_then_member_dialled_still_promotes();
	test_vaxcluster_refusal_is_still_terminal();
	test_e70_sequence_reaches_the_membership_offer();
	test_retrying_never_fabricates_a_join();
	test_expired_reconnect_window_ends_the_attempt_honestly();
	test_beat_adopts_the_connection_the_executive_holds();
	test_reoffer_is_per_connection_not_per_lifetime();
	test_a_start_with_no_target_is_deferred_not_terminal();
	test_every_table_cell();
	test_e73_the_executive_delivers_a_body();
	test_post_admit_drive_to_member();
	test_member_only_on_a_real_op06_csid();
	test_per_peer_identity_exchange();
	test_per_peer_skips_what_is_not_connected();
	test_per_peer_covers_every_member();
	test_per_peer_beat_asserts_no_membership();
	test_peer_params_land_in_the_senders_own_csb();

	return ct_summary("test_cnxman_join");
}
