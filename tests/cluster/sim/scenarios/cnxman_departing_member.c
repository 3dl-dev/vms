/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_departing_member.c - rd vms-e8b's rung-R2 leg: a DEPARTING
 * member's class-0x04 membership commit, replayed from the REAL captured frame,
 * and the connection this node's answer leaves on.
 *
 * ===========================================================================
 * THE CRASH THIS REPLAYS
 * ===========================================================================
 * 2026-10-08, pod `vaxlab-3`, three nodes: two real OpenVMS VAX V7.3 systems
 * (VAX1 1025, VAX2 1026) and a booted OVMX node (OVMXE 1030). OVMX joined
 * through VAX2 -- the highest SCSSYSTEMID, rd vms-e88 -- so VAX1 was never its
 * join target. VAX1 then ran `@SYS$SYSTEM:SHUTDOWN` with `REMOVE_NODE` and
 * opened its own class-0x04 self-departure transition (spec sec 4(r)) with a
 * cat-0x01 op-0x03 COMMIT to each other member:
 *
 *   VAX1 -> VAX2    cat 01 op 03 cls 04 txn 5 tok 47903
 *   VAX2 -> VAX1    cat 81 op 03 cls 04 txn 5 tok 47903     226 us later
 *   VAX1 -> OVMXE   cat 01 op 03 cls 04 txn 9 tok 46657
 *   OVMXE-> VAX2    cat 81 op 03 cls 04 txn 9 tok 46657     <- the wrong peer
 *   VAX2 -> *       last gasp; console: Fatal BUG CHECK CNXMGRERR
 *
 * Every 0x81 this FSM built used to leave on `j->cm_conid`, the connection to
 * the member the join drives through, whoever had asked. Reproduced 2/2.
 * Exhibit: tests/lab/captures/vms-e8b-cnxmgrerr-removenode-20261008/.
 *
 * ===========================================================================
 * WHAT MAKES THIS R2 AND NOT A SECOND R1
 * ===========================================================================
 *   1. THE INBOUND FRAME IS THE REAL ONE. `cm-depart-commit-to-ovmx` is the
 *      204 captured bytes of VAX1's commit to the OVMX node, loaded through
 *      the same manifest-gated loader the R1 codec tests use. The scripted
 *      peer here is a real OpenVMS VAX V7.3's own output, byte for byte --
 *      this harness models no VAX and invents no field.
 *   2. THE ANSWER IS CHECKED AGAINST A REAL VAX'S ANSWER. The expected body is
 *      `vms_cm_echo_response_build()`, which test_codec_cm.c proves reproduces
 *      the real VAX2's `cm-depart-commit-resp-oracle` byte-for-byte over all
 *      128 cited body bytes. So "the body is right" is a real-VAX oracle, and
 *      what this file adds is WHERE it went.
 *   3. TIME IS THE SIMULATOR'S VIRTUAL CLOCK. The real VAX2 answered in 226
 *      us, driven by the frame and not by a timer, and this asserts the same:
 *      the answer is out before any armed timer is due.
 *
 * THE FALSIFIER is the second arm: the identical frame arriving on the join's
 * TARGET connection is answered on the TARGET connection. Without it, "the
 * answer went to the other member" would also pass for an implementation that
 * had simply swapped one hard-wired destination for another.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * The bed: one simulated OVMX node that holds a VMS$VAXcluster connection to
 * each of two members -- which is what a real member does (E73) and what this
 * defect needed in order to be visible at all.
 * ========================================================================== */

#define TARGET_SYSID 0x000004000102ull   /* the higher: the one asked        */
#define OTHER_SYSID  0x000004000101ull   /* the departing one                */
#define OWN_SYSID    0x000004000103ull
#define TARGET_CSID  0x00010002u
#define OTHER_CSID   0x00010001u

#define MSCP_CONID      0x4e620008u
#define TARGET_CM_CONID 0x4e620009u
#define OTHER_CM_CONID  0x4e62000bu
#define SIM_NODE        0u

/* The captured envelope of `cm-depart-commit-to-ovmx` (frame 90 of
 * m4-crashwindow.pcap): VAX1's send-msg# on the OVMX connection. The answer
 * must acknowledge exactly this and nothing else (spec sec 4(j)). */
#define REQ_SEND_MSG 441u

#define MAX_SENT 16

struct sent {
	uint8_t     body[VMS_CM_BODY_LEN];
	uint32_t    len;
	vms_conid_t conid;
};

struct bed {
	struct sim_clock       clock;
	struct vms_cluster     cl;
	struct cnxman_ops      ops;
	struct cnxman_join_ops jops;
	struct cnxman_join     j;
	struct cnxman_barrier  b;
	struct vms_csb        *target_csb;
	struct vms_csb        *other_csb;
	/* The bed's mirror of vms_cnxman.c's `cur_csb`: the CSB whose Con.ID
	 * SCS delivered the body being dispatched on. `respond` answers on
	 * this and on nothing else, exactly as cnxman_ops_respond() does. */
	struct vms_csb        *cur_csb;

	struct sent sent[MAX_SENT];
	uint32_t    n_sent;
	uint32_t    logs;
};

static struct bed g;

/* ---- the injected ops --------------------------------------------------- */

static uint32_t bed_now_ms(void *ctx)
{
	(void)ctx;
	return sim_clock_now_ms(&g.clock);
}

static void bed_arm(void *ctx, enum cnxman_timer which, uint32_t key,
		    uint32_t ms)
{
	(void)ctx;
	sim_clock_arm(&g.clock, (uint8_t)SIM_NODE, (uint8_t)which, key, ms);
}

static void bed_cancel(void *ctx, enum cnxman_timer which, uint32_t key)
{
	(void)ctx;
	sim_clock_cancel(&g.clock, (uint8_t)SIM_NODE, (uint8_t)which, key);
}

static void bed_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
	g.logs++;
}

static void bed_record(const uint8_t *body, uint32_t len, vms_conid_t conid)
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

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	if (g.cur_csb == NULL || g.cur_csb->cdt_conid == 0u)
		return -1;
	bed_record(body, len, (vms_conid_t)g.cur_csb->cdt_conid);
	return 0;
}

static int bed_send_csb(void *ctx, int32_t idx, const uint8_t *body,
			uint32_t len)
{
	struct vms_csb *csb;

	(void)ctx;
	if (idx < 0)
		return -1;
	csb = cnxman_club_csb_at(&g.cl.club, (uint32_t)idx);
	if (csb == NULL || !csb->in_use || csb->cdt_conid == 0u)
		return -1;
	bed_record(body, len, (vms_conid_t)csb->cdt_conid);
	return 0;
}

static int bed_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx;
	(void)dst;
	(void)name;
	return 0;
}

static int bed_connect(void *ctx, vms_scs_sysid_t dst,
		       const uint8_t *local_name, const uint8_t *remote_name,
		       const uint8_t *conndata, uint16_t credits,
		       vms_conid_t *out_conid)
{
	(void)ctx;
	(void)dst;
	(void)local_name;
	(void)conndata;
	(void)credits;
	if (memcmp(remote_name, cnxman_join_name_mscp_disk,
		   VMS_SCS_PROCNAME_LEN) == 0) {
		*out_conid = MSCP_CONID;
		return 0;
	}
	*out_conid = TARGET_CM_CONID;
	/* Mirror cnxman_jop_connect(): the Con.ID SCS minted goes into the
	 * destination's CSB at that instant (E77). */
	cnxman_csb_bind_connection(g.target_csb, TARGET_CM_CONID);
	return 0;
}

static int bed_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len)
{
	(void)ctx;
	if (len == VMS_CM_BODY_LEN)
		bed_record(body, len, conid);
	return 0;
}

static int bed_disconnect(void *ctx, vms_conid_t conid)
{
	(void)ctx;
	(void)conid;
	return 0;
}

static uint64_t bed_time_now(void *ctx)
{
	(void)ctx;
	return sim_clock_now_vms(&g.clock);
}

static void bed_identity(void)
{
	struct cnxman_join_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.model, "OVMX simulated node", 19);
	cfg.model_len = 19;
	cfg.model_valid = 1;
	memcpy(cfg.version, "VMX V0.6", 8);
	cfg.version_valid = 1;
	cnxman_join_set_cfg(&g.j, &cfg);
}

static void bed_init(void)
{
	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);

	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.log = bed_log;
	g.ops.respond = bed_respond;
	g.ops.send_csb = bed_send_csb;

	g.jops.dir_inquire = bed_dir_inquire;
	g.jops.connect = bed_connect;
	g.jops.send_msg = bed_send_msg;
	g.jops.disconnect = bed_disconnect;
	g.jops.time_now = bed_time_now;

	memcpy(g.cl.params.scsnode, "OVMXS0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;

	(void)cnxman_club_init(&g.cl);
	g.other_csb = cnxman_club_alloc_csb(&g.cl.club, OTHER_SYSID, 1);
	cnxman_csb_set_csid(g.other_csb, OTHER_CSID);
	g.target_csb = cnxman_club_alloc_csb(&g.cl.club, TARGET_SYSID, 1);
	cnxman_csb_set_csid(g.target_csb, TARGET_CSID);

	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);
	bed_identity();
}

/* ==========================================================================
 * The manifest-hashed specimens
 * ========================================================================== */

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_nfx;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_nfx; i++) {
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	}
	return NULL;
}

/*
 * The join holds a connection to the member it drove through; the DEPARTING
 * member's own connection is one this node already has (E73). Both are bound
 * here to the Con.IDs SCS minted, which is what makes either resolvable as a
 * destination.
 */
static void bed_connections_open(void)
{
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, TARGET_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, TARGET_SYSID, cnxman_join_name_vaxcluster,
			       1);
	cnxman_join_opened(&g.j, MSCP_CONID);
	cnxman_join_opened(&g.j, TARGET_CM_CONID);

	cnxman_csb_bind_connection(g.other_csb, OTHER_CM_CONID);
	g.other_csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

/* Deliver the SYSAP body (design sec 3.2.4) of a captured frame, as arriving on
 * `csb`'s connection -- the fact vms_cnxman.c reads off the delivering Con.ID
 * and passes as `from_csb`. */
static void feed_on(struct vms_csb *csb, vms_csid_t csid,
		    const struct vms_fixture *f)
{
	const uint8_t *body = f->bytes + VMS_OFF_SYSAP_BODY;
	uint32_t len = f->wire_len - VMS_OFF_SYSAP_BODY;
	struct vms_cm_envelope env;

	if (vms_cm_envelope_parse(body, len, &env) == VMS_CODEC_OK)
		cnxman_csb_dialogue_heard(csb, env.send_msg);
	g.cur_csb = csb;
	(void)cnxman_join_rx_body(&g.j, body, len, csid, 1,
				  (int32_t)cnxman_club_csb_index(&g.cl.club,
								 csb));
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

static const struct sent *first_on(vms_conid_t conid)
{
	uint32_t i;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].conid == conid)
			return &g.sent[i];
	}
	return NULL;
}

static uint16_t body_le16(const uint8_t *b, uint32_t off)
{
	return (uint16_t)(b[off] | ((uint16_t)b[off + 1] << 8));
}

/*
 * The answer a REAL OpenVMS VAX V7.3 gives this request, built by the shipping
 * codec -- which test_codec_cm.c proves reproduces the real VAX2's captured
 * answer byte-for-byte over all 128 cited body bytes. body[0:4] is the
 * envelope, which is per-connection and is checked separately below.
 */
static void check_body_is_the_grounded_answer(const struct sent *s,
					      const struct vms_fixture *req)
{
	uint8_t want[VMS_CM_BODY_LEN];
	uint32_t written = 0;
	uint32_t i, bad = 0;

	memset(want, 0xAA, sizeof(want));
	ct_check(vms_cm_echo_response_build(req->bytes + VMS_OFF_SYSAP_BODY,
					    req->wire_len - VMS_OFF_SYSAP_BODY,
					    0, want, sizeof(want), &written)
		 == VMS_CODEC_OK && written == VMS_CM_BODY_LEN,
		 "the grounded class-0x04 answer builds");
	for (i = 4u; i < VMS_CM_BODY_LEN; i++) {
		if (s->body[i] != want[i])
			bad++;
	}
	ct_check_eq_u32(bad, 0u,
			"every body byte from [4] up is the answer a real "
			"OpenVMS VAX V7.3 gives this request");
	ct_check_eq_u32(s->body[VMS_OFB_CM_CLASS], VMS_CM_CLASS_DEPART,
			"... class 0x04, echoed");
	ct_check_eq_u32(s->body[VMS_OFB_CM_RESP_MARK], 0x01u,
			"... body[18] = 1");
}

/* ==========================================================================
 * Arm 1: the crashing exchange
 * ========================================================================== */
static void test_the_departing_member_is_answered_on_its_own_connection(void)
{
	const struct vms_fixture *req = fixture("cm-depart-commit-to-ovmx");
	const struct sent *s;

	printf("\n-- the real VAX1 departure commit, replayed on the DEPARTING "
	       "member's connection --\n");
	bed_init();
	bed_connections_open();
	g.n_sent = 0u;

	ct_check(req != NULL, "cm-depart-commit-to-ovmx loads");
	if (req == NULL)
		return;
	feed_on(g.other_csb, OTHER_CSID, req);

	ct_check_eq_u32(n_sent_on(OTHER_CM_CONID), 1u,
			"ONE answer, on the departing member's connection");
	ct_check_eq_u32(n_sent_on(TARGET_CM_CONID), 0u,
			"and NOTHING on the join target's -- the frame that "
			"bugchecked a real VAX2 CNXMGRERR");
	s = first_on(OTHER_CM_CONID);
	ct_check(s != NULL && s->len == VMS_CM_BODY_LEN,
		 "... a 132-byte VMS$VAXcluster body");
	if (s == NULL)
		return;
	check_body_is_the_grounded_answer(s, req);

	/* The envelope is the ASKING connection's, out of its own CSB. */
	ct_check_eq_u32(body_le16(s->body, VMS_OFB_CM_SEND_MSG), 1u,
			"stamped out of THAT connection's dialogue, at 1");
	ct_check_eq_u32(body_le16(s->body, VMS_OFB_CM_ACK_MSG), REQ_SEND_MSG,
			"... acking exactly what that peer sent on it");
	ct_check_eq_u32(g.target_csb->cm_send_msg, 2u,
			"the target's dialogue counter is still only its own "
			"identity records -- the answer took no number out of "
			"it (INV-6)");
	ct_check_eq_u32(g.j.replies_offtarget, 1u,
			"the answer is counted as one to a NON-target member");
	ct_check_eq_u32(g.j.replies_unaddressed, 0u, "... and none withheld");

	/* The real VAX2 answered this in 226 us, driven by the frame. So does
	 * this node: the answer is out with no timer having been due. */
	{
		uint32_t slot = 0u;

		ct_check(sim_clock_next(&g.clock, &slot) == 0 ||
			 sim_clock_now_ms(&g.clock) == 0u,
			 "no timer had to fire for the answer to go out");
	}
}

/* ==========================================================================
 * Arm 2: THE FALSIFIER -- the same frame from the join's own target
 * ========================================================================== */
static void test_the_targets_own_commit_is_answered_on_the_target(void)
{
	const struct vms_fixture *req = fixture("cm-depart-commit-to-ovmx");

	printf("\n-- FALSIFIER: the same commit arriving on the TARGET's "
	       "connection --\n");
	bed_init();
	bed_connections_open();
	g.n_sent = 0u;

	if (req == NULL)
		return;
	feed_on(g.target_csb, TARGET_CSID, req);

	ct_check_eq_u32(n_sent_on(TARGET_CM_CONID), 1u,
			"the answer goes to the target, because the target is "
			"who asked");
	ct_check_eq_u32(n_sent_on(OTHER_CM_CONID), 0u,
			"... and nothing reaches the other member");
	ct_check_eq_u32(g.j.replies_offtarget, 0u,
			"... and nothing is counted off-target");
}

/* ==========================================================================
 * Arm 3: a request on no connection this node holds is answered NOWHERE
 * ========================================================================== */
static void test_an_unresolvable_request_is_not_answered(void)
{
	const struct vms_fixture *req = fixture("cm-depart-commit-to-ovmx");

	printf("\n-- a request the executive resolved no connection for --\n");
	bed_init();
	bed_connections_open();
	g.n_sent = 0u;

	if (req == NULL)
		return;
	g.cur_csb = NULL;
	(void)cnxman_join_rx_body(&g.j, req->bytes + VMS_OFF_SYSAP_BODY,
				  req->wire_len - VMS_OFF_SYSAP_BODY,
				  0u, 0, -1);

	ct_check_eq_u32(g.n_sent, 0u,
			"nothing is sent: there are no dialogue counters to "
			"stand behind (INV-6)");
	ct_check_eq_u32(g.j.replies_unaddressed, 1u,
			"... and the omission is counted");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("rd vms-e8b R2: a departing member's commit, and where the "
	       "answer goes\n");
	g_nfx = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				     g_fx, VMS_FIXTURE_MAX_FILES,
				     err, sizeof(err));
	if (g_nfx <= 0) {
		printf("  FAIL could not load fixtures: %s\n", err);
		return 1;
	}
	test_the_departing_member_is_answered_on_its_own_connection();
	test_the_targets_own_commit_is_answered_on_the_target();
	test_an_unresolvable_request_is_not_answered();
	printf("\n");
	return ct_summary("test_sim_cnxman_departing_member");
}
