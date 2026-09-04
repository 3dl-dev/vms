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
	return 0;
}

static int bed_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len)
{
	(void)ctx;
	if (g.fail_send)
		return -1;
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

		ct_check_eq_u32(cnxman_join_rx_frame(&g.j, g_frame, len,
						     MEMBER_CSID, 1),
				CNXMAN_JOIN_RX_CONSUMED, "op 0x03 consumed");
		ct_check_eq_u32(g.j.echoes_sent, 1u,
				"op 0x03 COMMIT answered with the 0x81 echo");
		len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_LOCKRB, 0x0021);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		ct_check_eq_u32(g.j.echoes_sent, 2u,
				"op 0x05 rebuild txn answered too");
	}

	/* Step 8: the hand-off. */
	{
		uint32_t len = mk_open_add(EPOCH, 0x0eu);

		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		ct_check_eq_u32(g.b.opens_answered, 1u,
				"the transition OPEN reached the BARRIER FSM");
		len = mk_go(EPOCH);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);

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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);

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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);

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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);

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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
	ct_check_eq_u32(g.member_csb->cm_ack_msg, 0x0050u,
			"the peer's send-msg# became our ack-msg#");
	len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0x0040);
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
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

static void test_pathlost_fails_the_join(void)
{
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_closed(&g.j, CM_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_FAILED,
			"losing the VMS$VAXcluster connection fails the join");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_PATHLOST, "... as PATHLOST");

	/* But losing the disk-client connection after the walk is not a loss
	 * of anything the join still needs. */
	bed_init();
	bed_set_identity();
	drive_to_admit();
	cnxman_join_closed(&g.j, MSCP_CONID, 0u);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_ADMIT,
			"losing MSCP$DISK AFTER the walk does not fail it");
}

static void test_connect_refusal_is_named(void)
{
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	g.fail_connect = 1;
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_FAILED,
			"a local connect refusal fails the join");
	ct_check_eq_u32(g.j.failure, CNXMAN_JOIN_FAIL_CONNECT, "... as CONNECT");
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
	ct_check_eq_u32(cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID,
					     1),
			CNXMAN_JOIN_RX_HANDOFF,
			"with no barrier installed the caller must route it");
	len = mk_go(EPOCH);
	ct_check_eq_u32(cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID,
					     1),
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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
	len = mk_cm(VMS_CM_CAT_MEMBERSHIP, VMS_CM_OP_CLOSE, 0x0060);
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
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
	ct_check_eq_u32(cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID,
					     1),
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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
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
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_COMMIT:
		len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0x0081);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_MEMBERSHIP:
		/* No coordinator CSID in this fixture: exercising the cell
		 * itself, not the CSID-learn edge (covered separately above),
		 * so it must not perturb the state this generic driver put
		 * the FSM in. */
		len = mk_membership_csid(0u, 'A');
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_CLOSE:
		len = mk_cm(VMS_CM_CAT_MEMBERSHIP, VMS_CM_OP_CLOSE, 0x0082);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_TR_OPEN:
		len = mk_open_add(EPOCH, 0x0eu);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_TR_GO:
		len = mk_go(EPOCH);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_BARRIER:
		len = mk_cm(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL, 0x0083);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_BARRIER_ACK:
		len = mk_cm((uint8_t)(VMS_CM_CAT_CONFIG | 0x80u),
			    VMS_CM_OP_BARRIER, 0x0084);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
		break;
	case CNXMAN_EV_RX_REBUILD:
		len = mk_cm(VMS_CM_CAT_DLM, VMS_CM_OP_DLM_REBUILD, 0x0085);
		(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
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
	[CNXMAN_JOIN_IDLE] = {
		[CNXMAN_EV_START] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
	},
	[CNXMAN_JOIN_DIR_ROUND] = {
		[CNXMAN_EV_DIR_RESULT] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_MSCP_CONNECT] = {
		[CNXMAN_EV_CDT_OPEN] = 1,
		[CNXMAN_EV_CM_ACCEPTED] = 1,
		[CNXMAN_EV_CDT_CLOSED] = 1,
		[CNXMAN_EV_TIMER_JOIN] = 1,
	},
	[CNXMAN_JOIN_VC_CONNECT] = {
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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
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
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
	ct_check_eq_u32(g.j.echoes_sent, 1u, "the COMMIT was echoed");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 4u,
			"... on the member's own connection");

	len = mk_go(EPOCH);
	(void)cnxman_join_rx_frame(&g.j, g_frame, len, MEMBER_CSID, 1);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_BARRIER,
			"the XITGO hands off to the barrier as usual");
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

	/* (b) a true simultaneous open: our own connect is already out. No
	 * capture grounds which side yields, so this node invents nothing --
	 * it keeps its own Con.ID and counts the collision. */
	bed_init();
	bed_set_identity();
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	cnxman_join_opened(&g.j, MSCP_CONID);
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_VC_CONNECT,
			"our own VMS$VAXcluster connect is out");
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	ct_check_eq_u32(g.j.cm_already_held, 1u,
			"a simultaneous open is counted, not adopted");
	ct_check(g.j.cm_conid == CM_CONID,
		 "... and our own Con.ID is not replaced");
	ct_check_eq_u32(n_sent_on(ACC_CM_CONID), 0u,
			"... nothing goes out on the connection we did not "
			"adopt");
}

/* ==========================================================================
 * main
 * ========================================================================== */

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
	test_pathlost_fails_the_join();
	test_connect_refusal_is_named();
	test_server_half();
	test_watchdog();
	test_handoff_without_a_barrier();
	test_unowned_frame_is_not_mine();
	test_member_dialled_connection_still_promotes();
	test_member_dialled_reaches_the_barrier();
	test_adoption_refuses_what_it_does_not_own();
	test_every_table_cell();

	return ct_summary("test_cnxman_join");
}
