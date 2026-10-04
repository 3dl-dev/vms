/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_joiner_frozen_across_the_go.c - rd vms-eb3's rung-R2 leg:
 * the real V7.3 oracle F6, replayed at one simulated OVMX joiner on the
 * virtual clock.
 *
 * WHAT IS BEING REPRODUCED (tests/lab/captures/vms-eb3-joiner-freeze-20260930/,
 * experiment F6). Three real OpenVMS VAX V7.3 nodes; VAX3 (1027) founded,
 * VAX1 joined, and VAX2 (1026) was SIGSTOPped the instant it sent its Phase-1
 * answer (cat-0x81 op-0x09, send-msg# 94) to the coordinator's transition OPEN.
 * The coordinator's GO (cat-0x01 op-0x0a, send-msg# 266) was dropped at VAX2's
 * tap, and VAX2 was thawed 14 s later. The connection was re-established; VAX2
 * advertised ack 265 in its accept (conndata[12:14] = 09 01), VAX3 RE-SENT the
 * GO with its original number 266 on the new connection, and VAX2's barrier
 * step 1 went out as send-msg# 95, ack 266 -- the dialogue CARRIED. No new
 * identity, no second request, no bugcheck.
 *
 * WHAT IS BEING CLOSED (stall-rig arm P-3): OVMX in the same position aborted
 * the transition, restarted its dialogue at 1/0 on the re-established
 * connection and re-introduced itself, and the real VAX bugchecked CNXMGRERR.
 *
 * WHAT MAKES THIS R2: every frame the coordinator sends here is the REAL frame
 * from F6 (manifest-hashed fixtures); the Phase-1 answer is compared with the
 * real joiner's, byte for byte; the reconnect window is RUN by the shipping
 * vms_cnxman_recnx_fsm.c over the shipping CSB ladder on the virtual clock, and
 * the dialogue it carries is read back through the shipping connect-data
 * builder and the shipping barrier's own step 1.
 *
 * WHAT THE HARNESS OWNS: vms_cnxman.c's receive order (the envelope's send-msg#
 * is HEARD, then its ack taken, then the body routed -- cnxman_vc_route) and its
 * reconnect action (bind the minted Con.ID through the carrying binder). Both
 * live in the glue TU, which names exec_kbackend.h and is not host-linkable;
 * the close path's decision to leave the participant's transition standing is
 * pinned by test_cnxman_glue's wiring scan.
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
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cluster_codec_cm.h"

#define COORD_SYSID  1027ull       /* VAX3, the coordinator             */
#define OWN_SYSID    1026ull       /* VAX2's place: the frozen joiner   */
#define CONID_OLD    0x641b000au   /* the joiner's end before the freeze */
#define CONID_NEW    0x641e000au   /* ...and after the re-establishment */
#define SIM_NODE     0u
#define RECNXINTERVAL_SECS 20u
#define FREEZE_MS    14000u
#define MAX_SENT     32u

/* The joiner's own position at the freeze, read off F6: it had SENT 93
 * messages to VAX3 before its Phase-1 answer, and TAKEN VAX3's 263. */
#define ORACLE_SENT_BEFORE  93u
#define ORACLE_TAKEN_BEFORE 263u

static const uint8_t CD_HEAD[4] = { 0x01, 0x1b, 0x01, 0x03 };
static const uint8_t CD_TAIL[5] = { 0x08, 0x00, 0x00, 0x06, 0x00 };

struct sent {
	uint8_t body[VMS_CM_BODY_LEN];
};

struct bed {
	struct sim_clock      clock;
	struct vms_cluster    cl;
	struct cnxman_ops     ops;
	struct cnxman_recnx   recnx;
	struct cnxman_barrier b;
	struct vms_csb       *coord;
	struct sent           sent[MAX_SENT];
	uint32_t              n_sent;
	uint32_t              reconnects;
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

static void bed_log(void *ctx, const char *msg)
{
	(void)ctx; (void)msg;
}

static void bed_record(const uint8_t *body, uint32_t len)
{
	if (g.n_sent >= MAX_SENT)
		return;
	memset(g.sent[g.n_sent].body, 0, VMS_CM_BODY_LEN);
	memcpy(g.sent[g.n_sent].body, body,
	       len < VMS_CM_BODY_LEN ? len : VMS_CM_BODY_LEN);
	g.n_sent++;
}

static int bed_send_csb(void *ctx, int32_t idx, const uint8_t *body,
			uint32_t len)
{
	(void)ctx; (void)idx;
	bed_record(body, len);
	return 0;
}

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	bed_record(body, len);
	return 0;
}

static void bed_init(void)
{
	uint32_t i;

	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);
	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.log = bed_log;
	g.ops.send_csb = bed_send_csb;
	g.ops.respond = bed_respond;

	memcpy(g.cl.params.scsnode, "OVMXS2", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	g.cl.params.recnxinterval = (uint16_t)RECNXINTERVAL_SECS;
	(void)cnxman_club_init(&g.cl);
	cnxman_recnx_init(&g.recnx, &g.cl, &g.ops);
	cnxman_barrier_init(&g.b, &g.cl, &g.ops);

	/* The coordinator's block, where the wire stood at the freeze: OPEN on
	 * the old connection, not yet SELECTED (this node is being admitted,
	 * Phase 2 has not run), 93 sent to it and its 263 taken. */
	g.coord = cnxman_club_alloc_csb(&g.cl.club, COORD_SYSID, 1);
	if (g.coord == NULL)
		return;
	g.coord->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	cnxman_csb_bind_connection(g.coord, CONID_OLD);
	for (i = 0; i < ORACLE_SENT_BEFORE; i++)
		cnxman_csb_dialogue_sent(g.coord);
	cnxman_csb_dialogue_heard(g.coord, (uint16_t)ORACLE_TAKEN_BEFORE);
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

static uint16_t le16_at(const uint8_t *p, uint32_t off)
{
	return (uint16_t)(p[off] | (p[off + 1u] << 8));
}

/* vms_cnxman.c's receive order for one frame from the coordinator: the
 * envelope's send-msg# is HEARD, its ack TAKEN, then the body is routed. */
static void deliver(const char *name)
{
	const struct vms_fixture *f = fixture(name);
	const uint8_t *body;
	uint32_t len;

	if (f == NULL || g.coord == NULL)
		return;
	body = f->bytes + VMS_OFF_SYSAP_BODY;
	len = f->wire_len - VMS_OFF_SYSAP_BODY;
	cnxman_csb_dialogue_heard(g.coord, le16_at(body, VMS_OFB_CM_SEND_MSG));
	cnxman_csb_dialogue_acked(g.coord, le16_at(body, VMS_OFB_CM_ACK_MSG));
	(void)cnxman_barrier_rx_body(&g.b, body, len, 0u, 0,
				     (int32_t)cnxman_club_csb_index(&g.cl.club,
								    g.coord));
}

/* One second of vms_cnxman.c's beat, reduced to the reconnect action. */
static void beat(void)
{
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];
	uint32_t n, i;

	g.clock.now_ms += CNXMAN_RECNX_ATTEMPT_MS;
	n = cnxman_recnx_tick(&g.recnx, recs, VMS_CLUB_MAX_CSB);
	for (i = 0; i < n; i++) {
		if (recs[i].action != (uint8_t)CNXMAN_CSB_ACT_RECONNECT)
			continue;
		g.reconnects++;
		if (g.reconnects == 1u)
			cnxman_csb_bind_reconnect(g.coord, CONID_NEW);
	}
}

/* The frozen seconds, then the ladder's beats until a reconnect is bound. */
static void freeze_and_reconnect(void)
{
	uint32_t i;

	(void)cnxman_csb_dispatch(&g.cl.club, g.coord, CNXMAN_CSB_EV_CONN_LOST,
				  &g.ops);
	g.clock.now_ms += FREEZE_MS;
	for (i = 0; i < 64u && g.reconnects == 0u; i++)
		beat();
}

static void conndata_now(uint8_t out[VMS_CM_CONNDATA_LEN])
{
	struct vms_cm_conndata_in in;

	memset(&in, 0, sizeof(in));
	in.peer_ack_msg = cnxman_csb_dialogue_ack(g.coord);
	(void)vms_cm_conndata_build(&in, CD_HEAD, 4u, CD_TAIL, 5u, out,
				    VMS_CM_CONNDATA_LEN);
}

static int sent_is(uint32_t i, uint8_t cat, uint8_t op)
{
	return i < g.n_sent && g.sent[i].body[VMS_OFB_CM_CATEGORY] == cat &&
	       g.sent[i].body[VMS_OFB_CM_OPCODE] == op;
}

/* ---- the replay ----------------------------------------------------------- */

static void check_phase1_answer(void)
{
	const struct vms_fixture *want = fixture("cm-eb3-f6-phase1-answer");

	ct_check(sent_is(0u, 0x81u, VMS_CM_OP_XITION_ADD),
		 "Phase 1 is answered with cat-0x81 op-0x09");
	ct_check(want != NULL && g.n_sent >= 1u &&
		 memcmp(g.sent[0].body, want->bytes + VMS_OFF_SYSAP_BODY,
			VMS_CM_BODY_LEN) == 0,
		 "...BYTE FOR BYTE the real joiner's answer (F6: send-msg# 94, "
		 "ack 264, the echo)");
}

static void check_carried(void)
{
	uint8_t cd[VMS_CM_CONNDATA_LEN];

	ct_check(g.reconnects >= 1u, "the ladder re-established the connection");
	ct_check_eq_u32(g.coord->cdt_conid, CONID_NEW, "on a new Con.ID");
	ct_check_eq_u32(g.coord->cm_dialogues_carried, 1u,
			"with the dialogue CARRIED -- before Phase 2, because "
			"this node answered the transition's Phase 1");
	ct_check_eq_u32(g.coord->cm_send_msg, 94u,
			"the send side stands at 94, the real joiner's");
	ct_check_eq_u32(g.coord->cm_ack_msg, 265u,
			"and the ack at 265, the last the real joiner took");
	conndata_now(cd);
	ct_check(cd[12] == 0x09u && cd[13] == 0x01u,
		 "conndata[12:14] = 09 01 -- the real joiner's own accept");
}

static void check_resumed_barrier(void)
{
	const struct vms_fixture *want = fixture("cm-eb3-f6-first-step");
	uint32_t k = g.n_sent - 1u;

	ct_check(sent_is(k, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER),
		 "the re-sent GO runs the barrier: step 1 goes out");
	ct_check(want != NULL &&
		 le16_at(g.sent[k].body, VMS_OFB_CM_SEND_MSG) ==
			 le16_at(want->bytes + VMS_OFF_SYSAP_BODY,
				 VMS_OFB_CM_SEND_MSG) &&
		 le16_at(g.sent[k].body, VMS_OFB_CM_ACK_MSG) ==
			 le16_at(want->bytes + VMS_OFF_SYSAP_BODY,
				 VMS_OFB_CM_ACK_MSG),
		 "...as send-msg# 95, ack 266: the real joiner's own step 1 on "
		 "the re-established connection");
}

static void test_f6_replay(void)
{
	char name[40];
	uint32_t n;

	printf("\n-- oracle F6: frozen after Phase 1, the GO lost, 14 s --\n");
	bed_init();
	deliver("cm-eb3-f6-membership-263");
	deliver("cm-eb3-f6-phase1-open");
	check_phase1_answer();
	deliver("cm-eb3-f6-cat04-265");
	ct_check_eq_u32(g.coord->cm_phase1_named, 1u,
			"the coordinator is named in the answered transition");

	/* The GO (266) was dropped; the node froze; the connection went. */
	freeze_and_reconnect();
	ct_check_eq_u32(g.b.state, CNXMAN_BARRIER_OPEN,
			"the transition stands across the loss");
	check_carried();

	deliver("cm-eb3-f6-go-resent");
	check_resumed_barrier();
	for (n = 1u; n <= CNXMAN_BARRIER_STEPS; n++) {
		snprintf(name, sizeof(name), "cm-eb3-f6-release-%u", n);
		deliver(name);
	}
	ct_check_eq_u32(g.b.state, CNXMAN_BARRIER_COMPLETE,
			"the coordinator's twelve real releases complete it");
	ct_check_eq_u32(g.coord->cm_phase1_named, 0u,
			"and the Phase-1 record ends with it");
	ct_check_eq_u32(g.b.transitions_abandoned, 0u, "nothing was abandoned");
}

/*
 * THE OTHER HALF: the same loss, the same window, with NO transition answered.
 * The block is not SELECTED and not named, so the conversation restarts (E77)
 * -- which is what makes the carry above a consequence of Phase 1 and not of
 * the harness.
 */
static void test_no_transition_no_carry(void)
{
	printf("\n-- control: the same loss outside any transition --\n");
	bed_init();
	freeze_and_reconnect();
	ct_check(g.reconnects >= 1u, "the ladder still reconnects");
	ct_check_eq_u32(g.coord->cm_dialogues_carried, 0u, "nothing is carried");
	ct_check_eq_u32(g.coord->cm_send_msg + g.coord->cm_ack_msg, 0u,
			"the conversation restarts at 1/0");
}

int main(void)
{
	char err[256];

	printf("cnxman_joiner_frozen_across_the_go (rd vms-eb3, rung R2)\n");
	g_nfx = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				     g_fx, VMS_FIXTURE_MAX_FILES, err,
				     sizeof(err));
	if (g_nfx < 0) {
		printf("  FAIL could not load fixtures: %s\n", err);
		return 1;
	}
	ct_check(fixture("cm-eb3-f6-go-resent") != NULL,
		 "the F6 re-sent GO specimen is present");
	ct_check(fixture("cm-eb3-f6-release-12") != NULL,
		 "and all twelve releases");

	test_f6_replay();
	test_no_transition_no_carry();
	return ct_summary("cnxman_joiner_frozen_across_the_go");
}
