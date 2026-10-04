/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_join_waits_for_the_member_count.c - rd vms-e88's rung-R2
 * leg: the real V7.3 trio C3, replayed at one simulated OVMX joiner on the
 * virtual clock.
 *
 * WHAT IS BEING REPRODUCED (tests/lab/captures/vms-e88-join-target-20260930/,
 * trio C3). VAX3 (1027) founded, VAX1 (1025) joined, and VAX2 (1026) booted
 * while every frame between it and VAX3 was dropped at the bridge. VAX2 held a
 * connection to VAX1 alone, and VAX1's own PARAMS said the cluster had TWO
 * members. VAX2 sent no membership request to anybody for five minutes. When
 * VAX3 came into reach and sent its PARAMS, VAX2 asked VAX3 -- the higher of
 * the two -- 9.8 s later.
 *
 * WHAT IS BEING CLOSED (rig arms S-1 and friends): an OVMX joiner in the same
 * position asked the one member it could see. Beside a real VAX that member is
 * the outranked OVMX node, which discards the request by design (rd vms-1ac).
 *
 * WHAT MAKES THIS R2: the two members' PARAMS are the REAL records from C3
 * (manifest-hashed fixtures), the joiner is the SHIPPING join FSM, and five
 * minutes pass on the virtual clock with the join's own watchdog firing every
 * second -- "it did not ask" is an observable over 300 real beats, not the
 * absence of one call.
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

/* The trio's own identities. */
#define VAX1_SYSID   1025ull   /* reachable: the lower member            */
#define VAX3_SYSID   1027ull   /* the founder, out of reach at first     */
#define OWN_SYSID    1026ull   /* the joiner, VAX2's place                */
#define VAX1_CONID   0x2f520009u
#define VAX3_CONID   0x3fd80008u
#define SIM_NODE     0u

struct bed {
	struct sim_clock       clock;
	struct vms_cluster     cl;
	struct cnxman_ops      ops;
	struct cnxman_join_ops jops;
	struct cnxman_join     j;
	struct cnxman_barrier  b;
	struct vms_csb        *vax1, *vax3;
	uint32_t               config_to_vax1, config_to_vax3;
	uint32_t               first_config_ms;
	uint32_t               beats;
};

static struct bed g;

/* ---- injected ops --------------------------------------------------------- */

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

static int bed_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx; (void)dst; (void)name;
	return 0;
}

static int bed_connect(void *ctx, vms_scs_sysid_t dst,
		       const uint8_t *local_name, const uint8_t *remote_name,
		       const uint8_t *conndata, uint16_t credits,
		       vms_conid_t *out_conid)
{
	(void)ctx; (void)local_name; (void)remote_name; (void)conndata;
	(void)credits;
	/* Mirror cnxman_jop_connect(): the Con.ID goes into that system's CSB
	 * at the instant SCS mints it. */
	*out_conid = dst == VAX1_SYSID ? VAX1_CONID : VAX3_CONID;
	cnxman_csb_bind_connection(cnxman_club_find_sysid(&g.cl.club, dst),
				   (uint32_t)*out_conid);
	return 0;
}

static int bed_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len)
{
	(void)ctx;
	if (len != VMS_CM_BODY_LEN || body[VMS_OFB_CM_OPCODE] != VMS_CM_OP_CONFIG ||
	    body[VMS_OFB_CM_CATEGORY] != VMS_CM_CAT_CONFIG)
		return 0;
	if (g.config_to_vax1 + g.config_to_vax3 == 0u)
		g.first_config_ms = sim_clock_now_ms(&g.clock);
	if (conid == VAX1_CONID)
		g.config_to_vax1++;
	else if (conid == VAX3_CONID)
		g.config_to_vax3++;
	return 0;
}

static int bed_send_csb(void *ctx, int32_t idx, const uint8_t *body,
			uint32_t len)
{
	struct vms_csb *c = cnxman_club_csb_at(&g.cl.club, (uint32_t)idx);

	if (c == NULL || c->cdt_conid == 0u)
		return -1;
	return bed_send_msg(ctx, (vms_conid_t)c->cdt_conid, body, len);
}

static uint64_t bed_time_now(void *ctx)
{
	(void)ctx;
	return sim_clock_now_vms(&g.clock);
}

static void bed_init(void)
{
	struct cnxman_join_cfg cfg;

	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);
	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.send_csb = bed_send_csb;

	g.jops.dir_inquire = bed_dir_inquire;
	g.jops.connect = bed_connect;
	g.jops.send_msg = bed_send_msg;
	g.jops.time_now = bed_time_now;

	memcpy(g.cl.params.scsnode, "OVMXS2", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&g.cl);

	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);
	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.model, "OVMX simulated node", 19);
	cfg.model_len = 19;
	cfg.model_valid = 1;
	cnxman_join_set_cfg(&g.j, &cfg);
}

/* ---- the manifest-hashed specimens -------------------------------------- */

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

/* The body the executive hands the SYSAP, on the CSB it arrived on. */
static void feed(const char *name, struct vms_csb *from)
{
	const struct vms_fixture *f = fixture(name);

	if (f == NULL || from == NULL)
		return;
	(void)cnxman_join_rx_body(&g.j, f->bytes + VMS_OFF_SYSAP_BODY,
				  f->wire_len - VMS_OFF_SYSAP_BODY, 0u, 0,
				  (int32_t)cnxman_club_csb_index(&g.cl.club,
								 from));
}

/* Run the virtual clock for `ms`, delivering every timer that falls due. */
static void run_for(uint32_t ms)
{
	uint64_t until = g.clock.now_ms + ms;
	struct sim_timer t;
	uint32_t slot;

	while (sim_clock_next(&g.clock, &slot) &&
	       g.clock.t[slot].due_ms <= until) {
		if (!sim_clock_fire(&g.clock, slot, &t))
			break;
		if (t.due_ms > g.clock.now_ms)
			g.clock.now_ms = t.due_ms;
		if (t.which == (uint8_t)CNXMAN_TIMER_JOIN) {
			g.beats++;
			cnxman_join_timer(&g.j);
		}
	}
	g.clock.now_ms = until;
}

static struct vms_csb *discover(vms_scs_sysid_t sysid, vms_conid_t conid)
{
	struct vms_csb *c = cnxman_club_alloc_csb(&g.cl.club, sysid, 1);

	if (c == NULL)
		return NULL;
	cnxman_csb_bind_connection(c, (uint32_t)conid);
	c->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;   /* the CDT_OPEN the glue raises */
	return c;
}

/* ---- the replay ----------------------------------------------------------- */

static void test_c3_replay(void)
{
	uint32_t asked_at;

	printf("\n-- trio C3: one of two members in reach, for five minutes --\n");
	bed_init();

	/* VAX1 is the only system in sight. */
	g.vax1 = discover(VAX1_SYSID, VAX1_CONID);
	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, VAX1_SYSID, cnxman_join_name_mscp_disk, 0);
	cnxman_join_dir_result(&g.j, VAX1_SYSID, cnxman_join_name_vaxcluster, 1);
	cnxman_join_opened(&g.j, VAX1_CONID);
	feed("cm-params-reachable-member-oracle", g.vax1);   /* "2 members" */
	ct_check_eq_u32(g.vax1->adv_members, 2u,
			"VAX1's real PARAMS says the cluster has two members");

	run_for(300000u);
	ct_check(g.beats >= 290u,
		 "five minutes of the join's own once-a-second watchdog ran");
	ct_check_eq_u32(g.config_to_vax1 + g.config_to_vax3, 0u,
			"and in all of it the joiner asked NOBODY -- the real "
			"VAX2 in exactly this position sent no request for five "
			"minutes");
	ct_check_eq_u32(g.j.admit_hold, CNXMAN_JOIN_HOLD_CONNECTIVITY,
			"held for connectivity to every member");
	ct_check_eq_u32(g.j.requests_unanswered + g.j.unheard_declines, 0u,
			"nobody was asked, so nobody was declined");

	/* VAX3 comes into reach and says what it is. */
	g.vax3 = discover(VAX3_SYSID, VAX3_CONID);
	feed("cm-params-member-oracle", g.vax3);   /* its real PARAMS, "2" */
	asked_at = sim_clock_now_ms(&g.clock);
	while (g.config_to_vax3 + g.config_to_vax1 == 0u &&
	       sim_clock_now_ms(&g.clock) - asked_at < 9800u)
		run_for(100u);

	ct_check_eq_u32(g.config_to_vax3, 1u,
			"the joiner asks VAX3 -- the higher member -- once");
	ct_check_eq_u32(g.config_to_vax1, 0u,
			"and never the member it could see all along");
	ct_check(g.first_config_ms - asked_at <= 9800u,
		 "within the 9.8 s the real VAX2 took from VAX3's PARAMS to "
		 "its request");
	ct_check(g.j.target_sysid == VAX3_SYSID, "the join now drives VAX3");
	ct_check_eq_u32(g.j.retargets, 1u,
			"by a retarget, not a decline");

	/* VAX3 takes it: its real membership COMMIT stops the clock, and
	 * nothing is re-issued to the member that was never the one to ask. */
	feed("cm-commit-req", g.vax3);
	run_for(20000u);
	ct_check_eq_u32(g.j.admit_answered, 1u,
			"VAX3's COMMIT is the answer");
	ct_check_eq_u32(g.config_to_vax1 + g.config_to_vax3, 1u,
			"and one request is all this admission ever took");
}

int main(void)
{
	char err[256];

	printf("cnxman_join_waits_for_the_member_count (rd vms-e88, rung R2)\n");
	g_nfx = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				     g_fx, VMS_FIXTURE_MAX_FILES, err,
				     sizeof(err));
	if (g_nfx < 0) {
		printf("  FAIL could not load fixtures: %s\n", err);
		return 1;
	}
	ct_check(fixture("cm-params-reachable-member-oracle") != NULL,
		 "the C3 VAX1 PARAMS specimen is present");
	ct_check(fixture("cm-params-member-oracle") != NULL,
		 "the C3 VAX3 PARAMS specimen is present");

	test_c3_replay();
	return ct_summary("cnxman_join_waits_for_the_member_count");
}
