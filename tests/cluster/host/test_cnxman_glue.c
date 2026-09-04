/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_glue.c - FC-P3.8's own R1: the connection manager's GLUE
 * (vms_cnxman.c), which instantiates the join/barrier/coordinator/reconnect
 * FSMs on the fork context and is NOT itself host-linkable (it names
 * exec_kbackend.h and the FC-P0.5 fork API -- the same reason vms_scs.c and
 * vms_pe.c are not, test_scs_glue_conn.c's own header note).
 *
 * TWO PROOFS, on the SAME terms test_scs_glue_conn.c already established for
 * exactly this situation:
 *
 *   1. THE ALGORITHM, against the REAL pure layers it wires together. This
 *      file drives a real `struct cnxman_barrier` through a Phase 1 OPEN and
 *      the Phase 2 GO -- the composition FC-P3.3's test_cnxman_join.c already
 *      proves the join hands off into (join -> barrier, via
 *      cnxman_join_set_barrier(), test_cnxman_join.c's own step 8) -- and
 *      reproduces, byte for byte, the ONE algorithm vms_cnxman.c adds on top
 *      of that composition: the E3 proposed-quorum preload
 *      (cnxman_glue_preload_proposed) and the membership-change diff
 *      (cnxman_notify_membership_changes). Both are SMALL, are exercised here
 *      against the real vms_cnxman_csb.c / vms_cnxman_phase2.c objects, and
 *      are then confirmed, verbatim, to be what actually shipped (proof 2).
 *
 *   2. THE BINDINGS THEMSELVES, read out of the SHIPPING vms_cnxman.c: the
 *      SYSAP registration, the join/barrier/coordinator dispatch order, the
 *      E3 preload call sites, the E31 conndata (the operator-ruled, pcap-
 *      grounded CM protocol constant, named and comment-grounded, not an
 *      invented default), the E29 close-reason routing, the $SETCLUEVT AST-queue
 *      pattern, and the CLUB/CSB query functions CLUSTER_DIAG_CSB and SHOW
 *      CLUSTER read.
 *
 * INV-6 / E30: the honest outcome this file's first case pins is that a node
 * whose local CSID is never learned completes a whole open/GO/Phase-2-commit
 * cycle and STILL reads NEW, never MEMBER -- a placeholder CSID is what
 * bugchecked a real VAX. E30 was falsified + replaced by a real-VAX capture
 * (docs/cluster-integration-notes.md): the JOIN FSM now calls
 * cnxman_club_learn_local_csid() from a real op-0x06 coordinator CSID
 * (vms_cnxman_join_fsm.c). THIS file drives the BARRIER FSM directly, in
 * isolation, and never delivers an op-0x06 through the join's path -- so its
 * own scenario still, correctly, never learns a CSID and still reads NEW.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_phase2.h"
#include "vms_cnxman_quorum.h"
#include "vms_cluster_codec_cm.h"
#include "vms_frame_compose.h"   /* test-only full-frame composer */

#define COORD_CSID 0x00010001u
#define PEER_CSID  0x00010002u
#define EPOCH      0x0000000eu

struct sent_frame {
	uint8_t  bytes[VMS_CM_FRAME_LEN];
	uint32_t len;
};

#define MAX_SENT 32

struct bed {
	struct vms_cluster    cl;
	struct cnxman_ops     ops;
	struct fake_cnx       fake;
	struct cnxman_barrier b;
	struct vms_csb       *coord_csb;

	struct sent_frame sent[MAX_SENT];
	uint32_t          n_sent;
};

static struct bed g;

/* ==========================================================================
 * The bed: a real CLUB, a real coordinator CSB with REAL votes -- so the E3
 * preload has real, non-zero CLUB state to copy, exactly as it would on a
 * node that has been up long enough for cnxman_quorum_recompute() to have
 * run at least once (design SS3.7).
 * ========================================================================== */

static int bed_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx; (void)dst;
	if (g.n_sent < MAX_SENT) {
		memset(g.sent[g.n_sent].bytes, 0, VMS_CM_FRAME_LEN);
		memcpy(g.sent[g.n_sent].bytes + VMS_OFF_SYSAP_BODY, body,
		       len > VMS_CM_BODY_LEN ? VMS_CM_BODY_LEN : len);
		g.sent[g.n_sent].len = len;
		g.n_sent++;
	}
	return 0;
}

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	return bed_send(ctx, 0u, body, len);
}

static void bed_init(void)
{
	struct vms_csb *local;

	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.ops.send = bed_send;
	g.ops.respond = bed_respond;
	g.fake.now_ms = 100000u;

	memcpy(g.cl.params.scsnode, "OVMXJ0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = 0x000004000103ull;
	g.cl.params.vaxcluster = 1;
	g.cl.params.votes = 1;
	g.cl.params.expected_votes = 2;

	local = cnxman_club_init(&g.cl);
	(void)local;
	/* E30, honest: nothing here calls cnxman_club_learn_local_csid(). */

	g.coord_csb = cnxman_club_alloc_csb(&g.cl.club, 0x000004000101ull, 1);
	cnxman_csb_set_scsnode(g.coord_csb, (const uint8_t *)"VAX1", 4);
	cnxman_csb_set_csid(g.coord_csb, COORD_CSID);
	cnxman_csb_set_params(g.coord_csb, 1u /* votes */, 2u /* expected */,
			      0u /* qdskvotes */);
	cnxman_csb_set_flags(g.coord_csb, (uint16_t)(VMS_CSB_F_SELECTED |
						     VMS_CSB_F_MEMBER));
	g.coord_csb->cm_send_msg = 0x0140u;
	g.coord_csb->cm_ack_msg = 0x0100u;
	g.coord_csb->cm_txn = 0x0009u;
	g.coord_csb->cm_token = 0x07f5u;

	/* A real, non-zero running quorum -- what a node that has already
	 * been through cnxman_quorum_recompute() at least once carries. */
	cnxman_quorum_recompute(&g.cl.club);

	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
}

/* ==========================================================================
 * Frame builders (same shape as test_cnxman_barrier.c's own; a fresh, small
 * copy rather than a shared header, because the two files' bodies diverge
 * from here -- see design sec 3.9 rule 2's own "each test file builds its
 * cases" precedent across this directory).
 * ========================================================================== */

static uint32_t mk_frame(uint8_t *f, uint8_t cat, uint8_t op)
{
	struct vms_cm_link l;
	vms_wire_buf_t w;
	uint32_t written = 0;
	static const uint8_t dmac[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };
	static const uint8_t smac[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };

	memset(f, 0, VMS_CM_FRAME_LEN);
	memset(&l, 0, sizeof(l));
	memcpy(l.hdr.eth_dst, dmac, 6);
	memcpy(l.hdr.eth_src, smac, 6);
	memcpy(l.hdr.dst_lavc, dmac, 6);
	memcpy(l.hdr.src_lavc, smac, 6);
	l.hdr.connect_flag = 0x0001;
	l.recv_ack = 0x0011;
	l.send_seq = 0x0012;
	l.remote_conid = 0x33580008u;
	l.local_conid = 0x62c50009u;
	(void)vms_frame_compose_link(&l, f, VMS_CM_FRAME_LEN, &written);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	vms_wire_put_le16(&w, VMS_OFF_CM_SEND_MSG, 0x0220);
	vms_wire_put_le16(&w, VMS_OFF_CM_ACK_MSG, 0x0140);
	vms_wire_put_le16(&w, VMS_OFF_CM_TXN, 0x0009);
	vms_wire_put_le16(&w, VMS_OFF_CM_TOKEN, 0x0abc);
	vms_wire_put_u8(&w, VMS_OFF_CM_CATEGORY, cat);
	vms_wire_put_u8(&w, VMS_OFF_CM_OPCODE, op);
	return VMS_CM_FRAME_LEN;
}

static uint32_t mk_open_add(uint8_t *f, uint32_t epoch, uint8_t bitmap)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_XITION);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_ADD);
	vms_wire_put_u8(&w, VMS_OFF_CM_BITMAP, bitmap);
	return n;
}

static uint32_t mk_go(uint8_t *f, uint32_t epoch, uint8_t cls, uint8_t role)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, role);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, cls);
	return n;
}

/* ==========================================================================
 * The E3 preload -- a byte-for-byte reproduction of vms_cnxman.c's
 * cnxman_glue_preload_proposed(), so it can be driven and asserted on here
 * against the REAL vms_cnxman_csb.c / vms_cnxman_quorum.c objects. Proof 2
 * (source scan, below) confirms the shipped file really contains this
 * algorithm; this copy is what makes it exercisable at R1.
 * ========================================================================== */
static void preload_proposed(struct vms_club *club)
{
	if (club->proposed_valid)
		return;
	if (!club->transition_active)
		return;
	club->proposed_members = club->cluster_nodes;
	club->proposed_cevotes = club->cevotes;
	club->proposed_quorum = club->quorum;
	club->proposed_qdisk_votes = cnxman_quorum_qdskvotes(club);
	club->proposed_valid = 1u;
}

/* ==========================================================================
 * 1. The choreography: OPEN -> E3 preload -> GO -> Phase 2 commit, and the
 *    node STAYS NEW throughout (E30's honest outcome).
 * ========================================================================== */
/* E73: feed the barrier the SYSAP BODY SCS delivers, addressed by the CSB the
 * transition really arrived on. */
static enum cnxman_barrier_rx glue_feed_barrier(const uint8_t *frame,
						uint32_t len)
{
	return cnxman_barrier_rx_body(&g.b, frame + VMS_OFF_SYSAP_BODY,
				      len - VMS_OFF_SYSAP_BODY, COORD_CSID, 1,
				      (int32_t)cnxman_club_csb_index(
					      &g.cl.club, g.coord_csb));
}

static void test_open_preload_go_stays_new(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;
	uint16_t cevotes_before, quorum_before;

	printf("-- OPEN -> E3 preload -> GO -> Phase 2 commit; stays NEW "
	       "(E30) --\n");
	bed_init();

	ct_check(g.cl.club.cevotes > 0u && g.cl.club.quorum > 0u,
		 "the bed's running quorum is REAL and non-zero before any "
		 "transition (what cnxman_quorum_recompute() already gave it)");
	cevotes_before = g.cl.club.cevotes;
	quorum_before = g.cl.club.quorum;

	len = mk_open_add(frame, EPOCH, 0x06u);
	ct_check_eq_u32(glue_feed_barrier(frame, len),
			CNXMAN_BARRIER_RX_CONSUMED, "Phase 1 OPEN consumed");
	ct_check_eq_u32(g.b.state, (unsigned long)CNXMAN_BARRIER_OPEN,
			"barrier state is OPEN");
	ct_check(cnxman_barrier_phase2_committed(&g.b) == 0,
		 "Phase 2 has NOT run yet -- before the GO, not after (p. 7-42)");
	ct_check_eq_u32(g.cl.club.proposed_valid, 0u,
			"nothing has preloaded the proposed cells yet");

	/* E3: the glue's own moment -- after the open, before the GO. */
	preload_proposed(&g.cl.club);
	ct_check_eq_u32(g.cl.club.proposed_valid, 1u,
			"E3: proposed_valid is set from REAL running state");
	ct_check_eq_u32(g.cl.club.proposed_cevotes, cevotes_before,
			"... proposed_cevotes is the CLUB's own real cevotes");
	ct_check_eq_u32(g.cl.club.proposed_quorum, quorum_before,
			"... proposed_quorum likewise");

	/* Idempotent: a second call before the GO changes nothing. */
	g.cl.club.proposed_cevotes = 0xffffu;
	preload_proposed(&g.cl.club);
	ct_check_eq_u32(g.cl.club.proposed_cevotes, 0xffffu,
			"a second preload before the GO is a no-op (guarded "
			"by proposed_valid, matching phase2_commit_quorum's "
			"own guard)");
	g.cl.club.proposed_cevotes = cevotes_before;   /* restore for the GO */

	len = mk_go(frame, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO);
	ct_check_eq_u32(glue_feed_barrier(frame, len),
			CNXMAN_BARRIER_RX_CONSUMED, "the GO is consumed");
	ct_check(cnxman_barrier_phase2_committed(&g.b) != 0,
		 "Phase 2 has now run (book p. 7-42)");
	ct_check_eq_u32(g.cl.club.proposed_valid, 0u,
			"phase2_commit_quorum() consumed the preload and "
			"cleared the flag (p. 7-41: ignored outside a "
			"transition)");
	ct_check_eq_u32(g.cl.club.cevotes, cevotes_before,
			"the effective cevotes survived the proposed->"
			"effective copy unchanged (nothing was fabricated "
			"in between)");

	/* E30: the honest outcome for THIS scenario -- the barrier alone,
	 * with no op-0x06 delivered through the join FSM's path. */
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"E30: the local CSID was never learned (this scenario "
			"never delivers an op-0x06 coordinator CSID through "
			"the join FSM)");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER,
		 "E30: this node reads NEW, never MEMBER -- a placeholder "
		 "CSID is what bugchecked a real VAX");
}

/* ==========================================================================
 * 2. CLUSTER_DIAG_CSB's own projections, against the SAME driven CLUB --
 *    cnxman_get_club()/cnxman_get_csb() (vms_cnxman.c) are `fork_enter ->
 *    cnxman_club_project/cnxman_csb_project -> fork_leave` and nothing else
 *    (mirroring vms_scs_snapshot()'s own shape), so the projection calls
 *    themselves are exercised here with no kernel and no wire.
 * ========================================================================== */
static void test_csb_club_projection(void)
{
	struct vms_club_view cv;
	struct vms_csb_view sv;

	printf("-- the CLUB/CSB projection CLUSTER_DIAG_CSB hands userland --\n");
	bed_init();

	cnxman_club_project(&g.cl.club, g.cl.state, &cv);
	ct_check_eq_u32(cv.local_csid_valid, 0u,
			"CLUB view: local_csid_valid is honestly clear (E30)");
	ct_check_eq_u32(cv.state, (unsigned long)VMS_CLUSTER_OFF,
			"CLUB view: state reads whatever cl->state really is");
	ct_check(cv.cevotes > 0u && cv.quorum > 0u,
		 "CLUB view: the real tracked quorum, not a fabricated zero");

	cnxman_csb_project(g.coord_csb, &sv);
	ct_check_eq_u32(sv.csid, (unsigned long)COORD_CSID,
			"CSB view: the coordinator's real learned CSID");
	ct_check_eq_u32(sv.csid_valid, 1u, "... flagged learned");
	ct_check_eq_u32(sv.votes, 1u, "CSB view: the peer's real advertised VOTES");
	ct_check_eq_u32(sv.votes_valid, 1u, "... flagged learned, not a default");
	ct_check_eq_u32(sv.peer_sysid_lo,
			(unsigned long)(0x000004000101ull & 0xffffffffu),
			"CSB view: the peer's real SCSSYSTEMID");
}

/* ==========================================================================
 * 3. Membership-change detection: the same before/after snapshot diff
 *    vms_cnxman.c's cnxman_notify_membership_changes() performs, driven here
 *    against real CSB flag transitions (cnxman_csb_set_flags/clear_flags).
 * ========================================================================== */
static void snapshot(const struct vms_club *club, uint8_t *out)
{
	uint32_t i;

	for (i = 0; i < VMS_CLUB_MAX_CSB; i++)
		out[i] = 0u;
	for (i = 0; i < club->n_csb; i++)
		out[i] = club->csb[i].in_use
			? (uint8_t)cnxman_csb_is_member(&club->csb[i]) : 0u;
}

static void test_membership_diff_detects_the_edge(void)
{
	uint8_t before[VMS_CLUB_MAX_CSB];
	uint8_t after[VMS_CLUB_MAX_CSB];
	uint32_t flips = 0, i, coord_idx;

	printf("-- the membership-diff snapshot notices a real MEMBER flip "
	       "($SETCLUEVT / %%CNXMAN's trigger) --\n");
	bed_init();

	coord_idx = cnxman_club_csb_index(&g.cl.club, g.coord_csb);
	ct_check(cnxman_csb_is_member(g.coord_csb) != 0,
		 "the bed's coordinator CSB starts a real member (bed_init "
		 "set VMS_CSB_F_MEMBER)");
	snapshot(&g.cl.club, before);

	/* A real departure: the CSB ten-state ladder / Phase 2 clearing
	 * MEMBER is what this would look like from the outside. */
	cnxman_csb_clear_flags(g.coord_csb, (uint16_t)VMS_CSB_F_MEMBER);
	snapshot(&g.cl.club, after);

	for (i = 0; i < g.cl.club.n_csb; i++) {
		if (before[i] != after[i])
			flips++;
	}
	ct_check_eq_u32(flips, 1u,
			"exactly one CSB's membership bit flipped -- the "
			"REMOVE event vms_cnxman.c's diff would fire "
			"CNXMAN_CLUEVT_REMOVE + a %%CNXMAN line for");
	ct_check_eq_u32(before[coord_idx], 1u,
			"... it was the coordinator's CSB, a MEMBER before");
	ct_check_eq_u32(after[coord_idx], 0u, "... and is not one after");
}

/* ==========================================================================
 * 4. THE BINDINGS THEMSELVES, read out of the SHIPPING vms_cnxman.c
 *
 * Same shape as test_scs_glue_conn.c's own section 6: the glue is not
 * host-linkable, but its content is readable, and this is a real, teeth-
 * bearing scan against src/kernel-core/vms_cnxman.c -- a future edit that
 * drops a binding or bakes in a constant reddens this.
 * ========================================================================== */
static char glue_src[300000];

static int read_glue(const char *name)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", OVMX_KCORE_DIR, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(glue_src, 1u, sizeof(glue_src) - 1u, f);
	fclose(f);
	glue_src[n] = '\0';
	return 0;
}

static void check_has(const char *needle, const char *what)
{
	ct_check(strstr(glue_src, needle) != NULL, what);
}

/* The other half: a line this glue must NOT contain. Some of the strongest
 * statements about it are negative -- "it does not attach a fact it cannot
 * stand behind" is not expressible as a `check_has`. */
static void check_absent(const char *needle, const char *what)
{
	ct_check(strstr(glue_src, needle) == NULL, what);
}

static void test_glue_bindings(void)
{
	printf("-- the bindings, read out of src/kernel-core/vms_cnxman.c --\n");
	if (read_glue("vms_cnxman.c") != 0) {
		ct_check(0, "could not open vms_cnxman.c");
		return;
	}

	/* Instantiation: one of each FSM, wired together. */
	check_has("cnxman_join_init(&cn->join",
		  "one struct cnxman_join is instantiated");
	check_has("cnxman_barrier_init(&cn->barrier",
		  "one struct cnxman_barrier is instantiated");
	check_has("cnxman_coord_init(&cn->coord",
		  "one struct cnxman_coord is instantiated");
	check_has("cnxman_recnx_init(&cn->recnx",
		  "one struct cnxman_recnx is instantiated");
	check_has("cnxman_join_set_barrier(&cn->join, &cn->barrier)",
		  "the join hands off to the barrier (FC-P3.3/P3.5 composition)");

	/* The SYSAP registration the whole dispatch rides on. */
	check_has("scs_sysap_listen(cl->scs, cnxman_join_name_vaxcluster",
		  "the VMS$VAXcluster SYSAP is registered with SCS");

	/* E43: the MSCP$DISK CDT-open must advance the join (join_h_mscp_opened
	 * sets j->mscp_open) AND still forward to the disk client -- this exact
	 * contiguous shape lives only in cnxman_mscp_opened. Before the fix the
	 * join's MSCP_CONNECT step never advanced on a real wire (fake-ops R1
	 * reached the FSM directly, so only the glue was broken). */
	check_has("cnxman_join_opened(&cn->join, local_conid);\n\n"
		  "\tif (dc != NULL && dc->opened != NULL)",
		  "the MSCP CDT-open advances the join too, not just the VC (E43)");

	/* Dispatch order: join, then barrier, then coordinator. */
	check_has("cnxman_join_rx_body(&cn->join",
		  "the join sees the SYSAP body first");
	check_has("cnxman_barrier_rx_body(&cn->barrier",
		  "... then the barrier on NOT_MINE/HANDOFF");
	check_has("cnxman_coord_rx_body(&cn->coord",
		  "... then the coordinator on NOT_MINE");

	/*
	 * E1's transport -- AND NOTHING ELSE since E73. The glue used to
	 * advance the destination CSB's send-msg# after a successful send,
	 * one step out of phase with the join's advance-then-stamp, so the
	 * first barrier-side body stamped after the join's last one repeated
	 * a number spec sec 4(j) grounds as strictly monotonic per sender.
	 * The assignment now lives in cnxman_envelope_originate() alone.
	 */
	check_has("scs_send_msg(cn->cl->scs, csb->cdt_conid, body,",
		  "ops.send resolves the destination CSB's real CDT and hands "
		  "off to SCS (E1)");
	check_has("scs_send_msg(cn->cl->scs, csb->cdt_conid, body,",
		  "ops.send_csb does the same, addressed by CLUB slot (E73)");
	check_absent("cnxman_csb_dialogue_sent(",
		  "E73: no transport thunk assigns a send-msg# -- exactly one "
		  "function does, before the stamp");
	check_has("cnxman_csb_dialogue_heard(csb, env.send_msg)",
		  "inbound frames update the peer's ack-msg# for the barrier/"
		  "coordinator path (the join does its own, internally)");

	/* E3. */
	check_has("club->proposed_valid = 1u",
		  "E3: the glue sets proposed_valid when it fills the "
		  "proposed cells");
	check_has("if (club->proposed_valid)",
		  "... guarded so a second fill before a GO is a no-op");
	check_has("club->proposed_cevotes = club->cevotes",
		  "... copied from the CLUB's own real running value, not "
		  "invented");

	/*
	 * E31 (operator ruling, 2026-09-03): the grounded CM protocol
	 * version/tail constant, decoded byte-verified off a real VAX's own
	 * JOINER->MEMBER CONNECT_REQ (op06-join-20260903.pcap frames 64/72),
	 * IS now present and IS what cfg.conndata carries -- this replaces
	 * the pre-ruling assertion that no such constant existed anywhere in
	 * this file (that assertion encoded the honest-but-REJECTED all-zero
	 * state, now superseded).
	 */
	check_has("0x01, 0x1b, 0x01, 0x03",
		  "E31: the operator-ruled CM protocol version quad is present, "
		  "named and comment-grounded in the pcap + the ruling");
	check_has("cfg.conndata_valid = 1u",
		  "E31: conndata is explicitly marked valid, not left the "
		  "default omission");
	check_has("memcpy(cfg.conndata, cnxman_e31_conndata",
		  "E31: conndata is the named grounded constant, not an "
		  "inline replay");
	check_has("memset(&cfg, 0, sizeof(cfg))",
		  "E31: every OTHER identity field (model/version/params/"
		  "dir_descriptor) still starts fully zeroed -- only conndata "
		  "has an operator ruling behind it");

	/*
	 * E67 -- the wall this file's source scan exists for. `vms_cnxman.c`
	 * is not host-linkable, so the ONLY way to hold its thunks to the
	 * ops contract is to read them; and this is the same class of defect
	 * E43 already cost a lab run.
	 *
	 * The contract (vms_cnxman.h, vms_cnxman_join_fsm.h SS4): an op
	 * injected into a pure FSM answers 0 = accepted, nonzero = REFUSED.
	 * Every service behind these thunks answers in SS$_, where success is
	 * 1. A thunk that returns the status verbatim reports every success
	 * as a refusal -- which failed this node's join at its first step on
	 * the live 2-node cluster (join-e66refire) while all 59 host and
	 * simulator tests stayed green, because their beds return 0.
	 */
	check_has("static int cnxman_fsm_rc(int status)",
		  "E67: the SS$_ -> 0/nonzero translation is a NAMED function, "
		  "so a new thunk has something to call");
	check_has("return status == (int)SS__NORMAL ? 0 : status;",
		  "... it translates SS$_NORMAL (which is 1) to 0, and (E70) "
		  "RETURNS the refusal instead of flattening it to -1");
	check_has("return cnxman_fsm_rc(scs_dir_lookup(",
		  "E67: dir_inquire translates (the thunk whose untranslated "
		  "SS$_NORMAL failed the join at step 2)");
	check_has("rc = cnxman_fsm_rc(scs_send_msg(cn->cl->scs, conid, body, len));",
		  "E67: the join's send_msg translates");
	/*
	 * E70 -- the OTHER half of the same wall. Three promotion messages
	 * were refused on a live cluster and the ring could only say "rc=-1",
	 * which fits five different defects. The thunk must ask SCS what it
	 * actually decided and record it, or the next re-fire is as blind as
	 * this one was.
	 */
	check_has("cnxman_note_send_refusal(cn, conid);",
		  "E70: a refused send asks SCS for its OWN reason and records "
		  "it, so the ring is not left with a flattened rc");
	check_has("scs_send_refusal(cn->cl->scs, conid, &r) != (int)SS__NORMAL",
		  "E70: ... and that reason is READ from the executive's live "
		  "CDT, never inferred from the status");
	check_has("cnxman_diag_note(cn, CNXMAN_DIAG_R_SEND_REFUSED, r.err,",
		  "E70: the SCS refusal code and the PORT's own refusal reach "
		  "the E69 ring as the two facts they are");
	/*
	 * E70, the LAST ambiguity. The port's return is many-to-one too
	 * (NOCIRCUIT and RINGFULL are both SS$_DEVOFFLINE), so a transport
	 * refusal must be followed by the PORT's OWN reason, asked of the port
	 * on the circuit to the peer the refused connection rides -- and a
	 * refusal that was NOT the port's must record the CDT's own live
	 * state, which is how a peer's DISCONNECT under an originating SYSAP
	 * becomes visible.
	 */
	check_has("if (r.port_was_refuser)",
		  "E70: the glue branches on WHICH layer refused, not on a "
		  "status it would have to interpret");
	check_has("pe_send_refusal(cn->cl->pe, peer, &p)",
		  "E70: ... and asks the PORT ITSELF, on the circuit to the "
		  "peer the CDT rides");
	check_has("cnxman_diag_port_reason(p.code), p.code,",
		  "E70: the port's cause reaches the ring as a NAMED reason "
		  "with its verbatim code, never a packed sub-code");
	check_has("cnxman_diag_port_aux(&p));",
		  "E70: ... beside the ONE live number that explains it, "
		  "mapped by the ring's own (host-tested) vocabulary");
	check_has("cnxman_diag_note(cn, CNXMAN_DIAG_R_CDT_NOT_SENDABLE,",
		  "E70: a refusal that was not the port's records the CDT's "
		  "own live state instead");

	/*
	 * E71 -- the refusal that was invisible. On join-e70refire the join
	 * went MSCP_CONNECT -> FAILED with no cause anywhere: the connect that
	 * failed transmitted nothing, so the pcap could not show it and no ring
	 * record existed. A refused connect must reach the transcript.
	 */
	check_has("cnxman_diag_note(cn, CNXMAN_DIAG_R_CONNECT_REFUSED, status,",
		  "E71: a connect SCS refused reaches the E69 ring with the "
		  "executive's own SS$_ status, not silence");
	check_absent("cnxman_note_port_refusal(cn, dst);",
		     "E71 / INV-6: it does NOT attach the port's last send "
		     "refusal, which may belong to another frame entirely -- a "
		     "refused connect leaves no CDT to ask");

	/*
	 * E71 -- and the beat that lets a released join start again. A join
	 * that stopped for a connectivity reason goes back to IDLE precisely so
	 * this sweep can ask again; gating the drive on "a peer was discovered
	 * THIS beat" would mean it never does.
	 */
	check_has("(void)cnxman_discover_peers(cn);\n\t\t/*",
		  "E71: the peer sweep runs on its own, and no longer gates "
		  "the join drive on having found something new");
	check_has("if (cnxman_join_start(&cn->join) != 0)",
		  "E71: the drive believes the FSM's own answer about whether "
		  "a join really started");

	check_has("return cnxman_fsm_rc(scs_disconnect(",
		  "E67: disconnect translates");
	check_has("return cnxman_fsm_rc(scs_send_msg(",
		  "E67: ops.send/ops.send_csb/ops.respond translate too, so an FSM that "
		  "ever tests their result gets the right answer");

	/*
	 * E67, the other half: an accepted VMS$VAXcluster connection is a
	 * DIFFERENT fact from one this node opened, and only this glue can
	 * tell them apart (an accepted connection's Con.ID is one the join
	 * never recorded). There is exactly one such connection per pair of
	 * systems and either side may open it -- measured on the reference
	 * join -- so the accept half must tell the join, or a node whose
	 * member dialled first never advertises and is never promoted.
	 */
	check_has("cnxman_join_cm_accepted(&cn->join, accepted_from, local_conid)",
		  "E67: the ACCEPT half tells the join, with the peer and the "
		  "Con.ID the accept path really learned");
	check_has("if (accepted)",
		  "... only for a connection this node accepted, never for "
		  "one it opened");

	/*
	 * E72, the FIRST offer. p. 7-23's NEW state is "The CSB has just been
	 * allocated. It can represent a newly discovered remote Connection
	 * Manager", and a remote CM opening its VMS$VAXcluster connection to
	 * this node IS that discovery. So the block is allocated BEFORE the
	 * join's acceptance policy is asked; with the calls the other way round
	 * the join refused the cluster's first membership offer for want of a
	 * CSB (`refused-no-csb` on every live run) about two seconds before the
	 * peer sweep allocated the very same block. This contiguous shape lives
	 * only in cnxman_vc_connect_req.
	 */
	check_has("csb = csb_ensure(&cn->cl->club, peer);\n\n"
		  "\t/* THE SERVER HALF",
		  "E72: an inbound CONNECT allocates the CSB BEFORE the join's "
		  "acceptance policy is asked");
	check_absent("conndata_len);\n\tif (rc != 0)\n\t\treturn rc;\n\n"
		     "\tcsb = csb_ensure(",
		     "... and the order that lost the first offer is gone");

	/* E29. */
	check_has("SCS_CLOSE_REJECTED",
		  "E29: the close reason is inspected, RAW (never an SS$_), "
		  "before this glue acts on it");
	check_has("cnxman_join_rejected(&cn->join",
		  "... routed to the join's own REJECT handling");

	/* $SETCLUEVT. */
	check_has("vms_cnxman_cluevt_set(", "the registration entry point exists");
	check_has("vms_cnxman_proc_gone(", "the process-death safety hook exists");
	check_has("exec_zalloc_atomic(sizeof(*ast))",
		  "delivery queues a real AST the same non-sleeping way "
		  "vms_lock.c's own completion-AST path does");
	check_has("vms_ast_notify_arrival(proc)",
		  "... and wakes a hibernating reader exactly like it");

	/* CLUB/CSB query -- CLUSTER_DIAG_CSB, SHOW CLUSTER, $GETSYI. */
	check_has("int cnxman_get_club(struct vms_cluster *cl, struct vms_club_view *out)",
		  "cnxman_get_club() is implemented");
	check_has("int cnxman_get_csb(struct vms_cluster *cl, uint32_t index,",
		  "cnxman_get_csb() is implemented");
	check_has("int cnxman_find_csb(struct vms_cluster *cl, vms_csid_t csid,",
		  "cnxman_find_csb() is implemented");
	check_has("cnxman_club_project(&cl->club, cl->state, out)",
		  "... a pure projection, taken under the fork mutex");
}

int main(void)
{
	printf("FC-P3.8 R1: the CNXMAN glue's algorithm + its shipped "
	       "bindings\n\n");
	test_open_preload_go_stays_new();
	printf("\n");
	test_csb_club_projection();
	printf("\n");
	test_membership_diff_detects_the_edge();
	printf("\n");
	test_glue_bindings();
	printf("\n");
	return ct_summary("test_cnxman_glue");
}
