/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_rebuilt_view_relearns_csid.c - rd vms-8a9's rung-R2 leg:
 * a member whose view of a peer was DESTROYED, and where it gets that peer's
 * CSID back.
 *
 * WHAT IS BEING REPRODUCED, AND WHERE IT WAS MEASURED.
 * tests/lab/captures/vms-b36-cnxmgrerr-20260925/, campaign arms F-3, F-9,
 * F-13, F-15 and F-17: zero VAX bugchecks, zero rd vms-4c9 loops, the joiner
 * reaches MEMBER and the real VAX admits it -- and the OTHER OVMX node's own
 * SHOW CLUSTER shows the joiner as NEW for the rest of the run. Its console:
 * "committed member count differs from the transition nodemap".
 *
 * THE CHAIN, from those arms' own transcripts. The survivor lost its
 * connection to the joiner in the blackout, gave up on the block, and p. 7-25's
 * reclaim (rd vms-dfe, #1309) freed it. The rebuilt block is NEW with
 * csid_valid 0. phase2_csb_in_nodemap() needs csid_valid to turn a CSB into a
 * nodemap bit, so the coordinator's admission commit could not name the joiner
 * on that node and SELECTED was never set for it.
 *
 * WHERE A REAL MEMBER GETS IT, byte-for-byte
 * (tests/lab/captures/vms-b36-cnxmgrerr-20260925/oracle/):
 *
 *   oracle-3node-clean.pcap  frame 266   VAX2 -> VAX1  sysid 1027 csid 00010003 idx 2
 *   oracle-3node-fault-f1... frame 1523  VAX2 -> VAX1  sysid 1027 csid 00010004 idx 3
 *
 * The coordinator sends the JOINER the full member set and sends every EXISTING
 * MEMBER exactly ONE record: the one naming the system being admitted. VAX3 was
 * removed at CSID 00010003 / CSV index 2, came back as a new incarnation, and
 * was re-admitted at 00010004 / index 3 -- and the survivor's knowledge of that
 * new CSID comes from that one record, which it answers. VMS does NOT re-run
 * the admission for the survivor, and the survivor does NOT recover the CSID
 * from anything it held before.
 *
 * WHAT MAKES THIS R2: the block is destroyed by the SHIPPING reconnect FSM over
 * the SHIPPING CSB ladder after the whole p. 7-30 window really runs out on the
 * virtual clock, the record is fed to the SHIPPING join FSM, and the membership
 * is decided by the SHIPPING phase-2 commit. Nothing is hand-set.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_phase2.h"
#include "vms_cluster_codec_cm.h"

/* The oracle's own identities and assignments. */
#define OWN_SYSID    1025ull            /* VAX1, the surviving member     */
#define OWN_CSID     0x00010001u
#define COORD_SYSID  1026ull            /* VAX2, the coordinator          */
#define COORD_CSID   0x00010002u
#define PEER_SYSID   1027ull            /* VAX3, removed and re-admitted  */
#define PEER_CSID_1  0x00010003u        /* ...at CSV index 2              */
#define PEER_CSID_2  0x00010004u        /* ...and then at index 3         */
#define SIM_NODE     0u
#define RECNXINTERVAL_SECS 20u

struct bed {
	struct sim_clock       clock;
	struct vms_cluster     cl;
	struct cnxman_ops      ops;
	struct cnxman_join_ops jops;
	struct cnxman_join     j;
	struct cnxman_barrier  b;
	struct cnxman_recnx    recnx;
	uint32_t beats;
	uint8_t  body[VMS_CM_BODY_LEN];
};

static struct bed g;

/* ---- injected ops -------------------------------------------------------- */

static uint32_t bed_now_ms(void *ctx) { (void)ctx; return sim_clock_now_ms(&g.clock); }

static void bed_arm(void *ctx, enum cnxman_timer which, uint32_t key, uint32_t ms)
{
	(void)ctx;
	sim_clock_arm(&g.clock, (uint8_t)SIM_NODE, (uint8_t)which, key, ms);
}

static void bed_cancel(void *ctx, enum cnxman_timer which, uint32_t key)
{
	(void)ctx;
	sim_clock_cancel(&g.clock, (uint8_t)SIM_NODE, (uint8_t)which, key);
}

static void bed_log(void *ctx, const char *msg) { (void)ctx; (void)msg; }

static int bed_dir_inquire(void *ctx, vms_scs_sysid_t d, const uint8_t *n)
{ (void)ctx; (void)d; (void)n; return 0; }

static int bed_connect(void *ctx, vms_scs_sysid_t d, const uint8_t *l,
		       const uint8_t *r, const uint8_t *c, uint16_t cr,
		       vms_conid_t *o)
{ (void)ctx; (void)d; (void)l; (void)r; (void)c; (void)cr; (void)o; return -1; }

static int bed_send_msg(void *ctx, vms_conid_t c, const uint8_t *b, uint32_t l)
{ (void)ctx; (void)c; (void)b; (void)l; return 0; }

static int bed_disconnect(void *ctx, vms_conid_t c) { (void)ctx; (void)c; return 0; }

static uint64_t bed_time_now(void *ctx) { (void)ctx; return sim_clock_now_vms(&g.clock); }

/* ---- the bed ------------------------------------------------------------- */

static struct vms_csb *bed_member(vms_scs_sysid_t sysid, vms_csid_t csid)
{
	struct vms_csb *csb = cnxman_club_alloc_csb(&g.cl.club, sysid, 1);

	if (csb == NULL)
		return NULL;
	cnxman_csb_set_csid(csb, csid);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	return csb;
}

/*
 * The system being admitted, as the SURVIVOR holds it: a block discovery gave
 * it, carrying the CSID an earlier record named, and NOT selected -- because
 * no transition has committed it on this node yet. That is the measured shape
 * in arms F-3/F-9/F-13/F-15/F-17, and it is the shape p. 7-25's reclaim is
 * allowed to free (a SELECTED block never is).
 */
static struct vms_csb *bed_discovered(vms_scs_sysid_t sysid, vms_csid_t csid)
{
	struct vms_csb *csb = cnxman_club_alloc_csb(&g.cl.club, sysid, 1);

	if (csb == NULL)
		return NULL;
	cnxman_csb_set_csid(csb, csid);
	return csb;
}

/*
 * Drive the join to a state in which it really dispatches a membership record
 * -- the same public sequence scenarios/cnxman_recover_after_giveup.c uses,
 * and every step of it is the shipping FSM's.
 */
static void bed_join_to_advertise(void)
{
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, COORD_SYSID, cnxman_join_name_mscp_disk, 1);
	cnxman_join_dir_result(&g.j, COORD_SYSID, cnxman_join_name_vaxcluster, 1);
	cnxman_join_opened(&g.j, 0x4e620008u);
	cnxman_join_cm_accepted(&g.j, COORD_SYSID, 0x4e62000bu);
	cnxman_join_opened(&g.j, 0x4e62000bu);
}

static void bed_init(void)
{
	struct vms_csb *local;
	struct cnxman_join_cfg cfg;

	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);

	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.log = bed_log;

	g.jops.dir_inquire = bed_dir_inquire;
	g.jops.connect = bed_connect;
	g.jops.send_msg = bed_send_msg;
	g.jops.disconnect = bed_disconnect;
	g.jops.time_now = bed_time_now;

	memcpy(g.cl.params.scsnode, "VAX1  ", 6);
	g.cl.params.scsnode_len = 4;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	g.cl.params.recnxinterval = (uint16_t)RECNXINTERVAL_SECS;

	local = cnxman_club_init(&g.cl);
	cnxman_club_learn_local_csid(&g.cl.club, OWN_CSID);
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
					       VMS_CSB_F_MEMBER));
	g.cl.state = VMS_CLUSTER_MEMBER;

	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);
	cnxman_recnx_init(&g.recnx, &g.cl, &g.ops);
	memset(&cfg, 0, sizeof(cfg));
	cnxman_join_set_cfg(&g.j, &cfg);

	(void)bed_member(COORD_SYSID, COORD_CSID);
}

/* The once-a-second beat, in vms_cnxman.c's order: reclaim, then the ladder. */
static void beat(void)
{
	vms_scs_sysid_t released[VMS_CLUB_MAX_CSB];
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];
	uint32_t n, i;

	g.clock.now_ms += CNXMAN_RECNX_ATTEMPT_MS;
	g.beats++;
	n = cnxman_club_reclaim_abandoned(&g.cl.club, released,
					  (uint32_t)VMS_CLUB_MAX_CSB);
	for (i = 0; i < n; i++)
		cnxman_join_target_released(&g.j, released[i]);
	(void)cnxman_recnx_tick(&g.recnx, recs, VMS_CLUB_MAX_CSB);
}

/*
 * ONE MEMBERSHIP RECORD, exactly as the coordinator sends an existing member:
 * a cat-0x01 op-0x05 naming the system being admitted, its assigned CSID and
 * its CSV index. Built by the SHIPPING codec from the oracle's own values.
 */
static void feed_membrec(uint32_t sysid, uint32_t csid, uint16_t index)
{
	struct vms_cm_membership_rec rec;
	uint32_t written = 0u;

	memset(&rec, 0, sizeof(rec));
	rec.sysid = sysid;
	rec.csid = csid;
	rec.index = index;
	rec.epoch = 7u;
	/* The boot quadword the oracle's own record carried for VAX3's new
	 * incarnation -- read out of oracle-3node-fault-f1.pcap frame 1523. */
	rec.boot_lo = 0x99d762dfu;
	rec.boot_hi = 0x00bc308du;
	rec.boot_valid = 1u;
	ct_check_eq_u32((uint32_t)vms_cm_membership_rec_build(&rec, g.body,
							      (uint32_t)sizeof(g.body),
							      &written),
			(uint32_t)VMS_CODEC_OK, "the record builds");
	(void)cnxman_join_rx_body(&g.j, g.body, written, COORD_CSID, 1, -1);
}

/* The coordinator's Phase 2, with the nodemap it really committed. */
static void commit_with_nodemap(uint8_t bitmap)
{
	struct cnxman_phase2_in in;
	struct cnxman_phase2_stats st;

	memset(&in, 0, sizeof(in));
	memset(&st, 0, sizeof(st));
	in.bitmap = bitmap;
	in.bitmap_valid = 1u;
	in.bitmap_popcount = (uint8_t)__builtin_popcount(bitmap);
	(void)cnxman_phase2_commit(&g.cl, &in, &st, &g.ops);
}

/* ==========================================================================
 * The run
 * ========================================================================== */

static void test_rebuilt_view_relearns_the_csid(void)
{
	struct vms_csb *peer;
	uint32_t start;

	printf("\n-- rd vms-8a9: a member's rebuilt view of a peer relearns "
	       "its CSID from the committed record --\n");
	bed_init();

	bed_join_to_advertise();

	/* The peer is a system this node holds a block for, at the oracle's
	 * FIRST assignment, with a connection but not yet committed here. */
	peer = bed_discovered(PEER_SYSID, PEER_CSID_1);
	ct_check(peer != NULL && peer->csid_valid,
		 "this node holds a block and a CSID for the peer");
	(void)cnxman_csb_dispatch(&g.cl.club, peer, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g.ops);
	cnxman_csb_bind_connection(peer, 0x4e62000au);
	(void)cnxman_recnx_connectivity_gained(&g.recnx, peer);
	ct_check_eq_u32(peer->state, (uint8_t)VMS_CNXMAN_CSB_OPEN,
			"the pair's connection is OPEN");

	/* Its connectivity goes, and the whole p. 7-30 window really runs
	 * out on the clock. */
	(void)cnxman_recnx_connectivity_lost(&g.recnx, peer, 0);
	ct_check_eq_u32(peer->state, (uint8_t)VMS_CNXMAN_CSB_WAIT,
			"the reconnect window opens");
	start = g.beats;
	while (g.beats - start < RECNXINTERVAL_SECS + 3u)
		beat();

	/*
	 * p. 7-25 HAS HAPPENED. The block is gone -- which is #1309's whole
	 * point and is NOT being undone here.
	 */
	ct_check(cnxman_club_find_sysid(&g.cl.club, PEER_SYSID) == NULL,
		 "p. 7-25: the block was deallocated (#1309), so this node "
		 "holds NO CSID for that system any more");

	/* The peer re-incarnates and the coordinator re-admits it, assigning
	 * a NEW CSID at a NEW CSV index -- and sends THIS member the one
	 * record that names it. */
	feed_membrec((uint32_t)PEER_SYSID, PEER_CSID_2, 3u);

	peer = cnxman_club_find_sysid(&g.cl.club, PEER_SYSID);
	ct_check(peer != NULL,
		 "the record gave this node a block for the system the "
		 "coordinator named");
	ct_check_eq_u32(g.j.membrecs_peer_created, 1u, "...counted as created");
	ct_check(peer != NULL && peer->csid_valid &&
		 (uint32_t)peer->csid == PEER_CSID_2,
		 "...carrying the NEW CSID the record named, never the old one");
	ct_check_eq_u32(g.j.membrecs_adopted, 0u,
			"INV-6: and it is not taken as this node's own identity");
	ct_check_eq_u32(peer == NULL ? 1u :
			(uint32_t)(peer->flags & VMS_CSB_F_SELECTED), 0u,
			"...and the record alone does NOT make it a member: "
			"only the transition does that");

	/*
	 * ...and NOW the coordinator's own nodemap can name it. Slots are the
	 * CSID's low 16 bits: this node 1, the coordinator 2, the re-admitted
	 * peer 4.
	 */
	commit_with_nodemap((uint8_t)((1u << 1) | (1u << 2) | (1u << 4)));
	peer = cnxman_club_find_sysid(&g.cl.club, PEER_SYSID);
	ct_check(peer != NULL &&
		 (peer->flags & VMS_CSB_F_SELECTED) != 0u,
		 "THE POINT: the committed transition now names the peer, and "
		 "this member selects it (p. 7-42 task 1)");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 3u,
			"...so this node's own member count is THREE, which is "
			"what its SHOW CLUSTER reads");
}

/*
 * THE CONTROL, and it is the whole defect: with the record dropped -- which is
 * what this executive did before rd vms-8a9 -- the identical commit cannot
 * name the peer and the member count stays at two.
 */
static void test_without_the_record_the_commit_cannot_name_it(void)
{
	struct vms_csb *peer;
	uint32_t start;

	printf("\n-- CONTROL: no record, and the same commit leaves the peer "
	       "NEW --\n");
	bed_init();
	bed_join_to_advertise();
	peer = bed_discovered(PEER_SYSID, PEER_CSID_1);
	(void)cnxman_csb_dispatch(&g.cl.club, peer, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g.ops);
	cnxman_csb_bind_connection(peer, 0x4e62000au);
	(void)cnxman_recnx_connectivity_gained(&g.recnx, peer);
	(void)cnxman_recnx_connectivity_lost(&g.recnx, peer, 0);
	start = g.beats;
	while (g.beats - start < RECNXINTERVAL_SECS + 3u)
		beat();
	ct_check(cnxman_club_find_sysid(&g.cl.club, PEER_SYSID) == NULL,
		 "the block was deallocated");

	/* Discovery rebuilds it the way the peer sweep would: NEW, no CSID. */
	peer = cnxman_club_alloc_csb(&g.cl.club, PEER_SYSID, 1);
	ct_check(peer != NULL && !peer->csid_valid,
		 "a rebuilt block carries no CSID");

	commit_with_nodemap((uint8_t)((1u << 1) | (1u << 2) | (1u << 4)));
	peer = cnxman_club_find_sysid(&g.cl.club, PEER_SYSID);
	ct_check(peer != NULL &&
		 (peer->flags & VMS_CSB_F_SELECTED) == 0u,
		 "the commit cannot name a CSB it has no CSID for -- this is "
		 "the measured defect");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 2u,
			"...and the member count is one short, which is the "
			"'NEW' row in the arms' SHOW CLUSTER");
}

int main(void)
{
	printf("=== cnxman_rebuilt_view_relearns_csid: rd vms-8a9 ===\n");
	test_rebuilt_view_relearns_the_csid();
	test_without_the_record_the_commit_cannot_name_it();
	return ct_summary("cnxman_rebuilt_view_relearns_csid");
}
