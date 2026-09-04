/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_join.c - FC-P3.3's rung-R2 leg: replay the reference JOIN
 * choreography at one simulated OVMX node and assert the SEQUENCE it emits.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1. Three things, and they are the whole
 * difference:
 *
 *   1. THE INBOUND FRAMES ARE THE MANIFEST-HASHED SPECIMENS, not frames this
 *      test built. `cm-params.spec` is a real `VMS$VAXcluster` op-0x01 body
 *      from formation-ci1-joinwindow.pcap; `cm-open-add-req.spec`,
 *      `cm-commit-req.spec`, `cm-close-req.spec` and `cm-dlm-op0d-req.spec`
 *      are the transition open, the membership commit, the transaction close
 *      and the DLM rebuild record from the same clean-room chain. They are
 *      loaded through the SAME loader (and the same manifest gate) the R1
 *      codec tests use -- no new decoder, no new fixture format.
 *
 *   2. TIME IS THE SIMULATOR'S VIRTUAL CLOCK AND TIMER WHEEL (sim_clock),
 *      driven by this scenario, so "the join stalled" is an OBSERVABLE (a
 *      timer that fires and a step that does not advance) rather than a
 *      wall-clock hope. The join's own watchdog runs on it.
 *
 *   3. THE ASSERTION IS THE ORDERED SEQUENCE, not a set of counters. The
 *      reference sequence of spec sec 4(L)(a)-(e) + sec 4(o) rows 1-4 is
 *      written out once, in order, and compared element by element against
 *      what the SHIPPING FSM actually emitted -- including that it reaches
 *      the barrier hand-off (op-0x0a XITGO) with the SHIPPING barrier FSM
 *      really taking the transition.
 *
 * WHAT IS LAB-DEFERRED, AND SAID PLAINLY (integration note E12). The plan row
 * names "R2 replay of the vax3 reference join". The raw pcap for that capture
 * (`vax3-2to3-established-join-20260730.pcap`) is LAB-HOST-ONLY -- pcaps are
 * never committed (docs/clean-room/PROVENANCE.md) -- and no decoded fixture for
 * it exists in this tree. So the inbound frames replayed here are the
 * formation-window specimens that ARE decoded and manifest-hashed, which carry
 * the same wire choreography this FSM drives. Swapping in a decoded vax3
 * fixture list is a one-line change to the array below; decoding it is the lab
 * lane's, and until it lands this leg is HONESTLY PARTIAL.
 *
 * TWO ELEMENTS OF THE SEQUENCE ARE NECESSARILY CODEC-BUILT, and both say so at
 * the call: the op-0x0a GO (no GO specimen is decoded in this tree, so it is
 * built through the grounded coordinator recipe vms_cm_go_build) and the MSCP
 * END messages (no MSCP fixture is in this tree either; they are built through
 * the FC-P6.2 codec that test_codec_mscp.c already proves reproduces a real
 * server's answer byte-exact).
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
#include "vms_cluster_codec_mscp.h"

/* ==========================================================================
 * The reference sequence, written down ONCE and in order
 *
 * Spec sec 4(L)(a)-(e): "(a) open its own SCS$DIRECTORY CLIENT connection;
 * (b) look up each SYSAP on the member as a client BEFORE connecting to it --
 * MSCP$DISK and VMS$VAXcluster; (c) only THEN open the MSCP$DISK client
 * connection; (d) open the VMS$VAXcluster VC; (e) send the add-member burst",
 * then sec 4(o) rows 1/2/4: op 0x14, op 0x01, and -- after the disk-client
 * discovery run -- op 0x02, "this starts admission".
 * ========================================================================== */
enum ref_step {
	R_LOOKUP_MSCP = 0,
	R_LOOKUP_VAXCLUSTER,
	R_CONNECT_MSCP,
	R_CONNECT_VAXCLUSTER,
	R_CM_MODEL,
	R_CM_PARAMS,
	R_MSCP_SCC1,
	R_MSCP_SCC2,
	R_MSCP_GUS,
	R_CM_CONFIG,
	R_HANDOFF,
	R_STEP__COUNT
};

static const char *const ref_name[R_STEP__COUNT] = {
	"lookup MSCP$DISK",
	"lookup VMS$VAXcluster",
	"connect VMS$DISK_CL_DRVR -> MSCP$DISK",
	"connect VMS$VAXcluster -> VMS$VAXcluster",
	"cat 0x01 op 0x14  (model)",
	"cat 0x01 op 0x01  (parameters)",
	"MSCP SET CONTROLLER CHARACTERISTICS #1",
	"MSCP SET CONTROLLER CHARACTERISTICS #2",
	"MSCP GET UNIT STATUS (NEXT-UNIT walk)",
	"cat 0x01 op 0x02  (config -- admission starts)",
	"XITGO -> the barrier FSM owns the wire"
};

/* The reference sequence as an ordered list. The GUS walk is one element here
 * because its LENGTH is the peer's business (the walk ends on the peer's own
 * Unit-Offline terminator, sec 4(n)); its ORDER is not. */
static const enum ref_step reference[] = {
	R_LOOKUP_MSCP, R_LOOKUP_VAXCLUSTER,
	R_CONNECT_MSCP, R_CONNECT_VAXCLUSTER,
	R_CM_MODEL, R_CM_PARAMS,
	R_MSCP_SCC1, R_MSCP_SCC2, R_MSCP_GUS,
	R_CM_CONFIG,
	R_HANDOFF
};

/* ==========================================================================
 * The bed: one simulated OVMX node on the virtual clock
 * ========================================================================== */

#define MEMBER_SYSID 0x000004000101ull
#define OWN_SYSID    0x000004000103ull
#define MEMBER_CSID  0x00010001u
#define MSCP_CONID   0x4e620008u
#define CM_CONID     0x4e620009u
#define SIM_NODE     0u

#define MAX_OBS 64

struct bed {
	struct sim_clock        clock;
	struct vms_cluster      cl;
	struct cnxman_ops       ops;
	struct cnxman_join_ops  jops;
	struct cnxman_join      j;
	struct cnxman_barrier   b;
	struct vms_csb         *member_csb;

	enum ref_step obs[MAX_OBS];
	uint32_t      n_obs;
	/* E71: the simulated transient -- this node's own SCS refusing to put
	 * the VMS$VAXcluster connect on the wire, which is what happened on the
	 * live join-e70refire run. */
	int           refuse_cm_connect;
	uint32_t      gus_cmds;      /* collapsed into one R_MSCP_GUS element */
	uint32_t      logs;
	char          last_log[160];
};

static struct bed g;

static void obs(enum ref_step s)
{
	/* The GUS walk is ONE sequence element however many units the peer
	 * reports (see the reference[] comment). */
	if (s == R_MSCP_GUS) {
		g.gus_cmds++;
		if (g.gus_cmds > 1u)
			return;
	}
	if (g.n_obs < MAX_OBS)
		g.obs[g.n_obs++] = s;
}

/* ---- the injected ops ---------------------------------------------------- */

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
	size_t n;

	(void)ctx;
	g.logs++;
	if (msg == NULL)
		return;
	n = strlen(msg);
	if (n >= sizeof(g.last_log))
		n = sizeof(g.last_log) - 1;
	memcpy(g.last_log, msg, n);
	g.last_log[n] = '\0';
}

static int bed_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx;
	(void)dst;
	if (memcmp(name, cnxman_join_name_mscp_disk, VMS_SCS_PROCNAME_LEN) == 0)
		obs(R_LOOKUP_MSCP);
	else if (memcmp(name, cnxman_join_name_vaxcluster,
			VMS_SCS_PROCNAME_LEN) == 0)
		obs(R_LOOKUP_VAXCLUSTER);
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
		obs(R_CONNECT_MSCP);
		*out_conid = MSCP_CONID;
	} else {
		if (g.refuse_cm_connect)
			return -1;   /* nothing went out: no step to observe */
		obs(R_CONNECT_VAXCLUSTER);
		*out_conid = CM_CONID;
		/* Mirror cnxman_jop_connect(): the Con.ID SCS minted goes into
		 * the destination's CSB at that instant, which is what binds
		 * that block's dialogue counters to THIS connection (E77). */
		cnxman_csb_bind_connection(g.member_csb, CM_CONID);
	}
	return 0;
}

static int bed_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len)
{
	(void)ctx;
	if (conid == MSCP_CONID) {
		/* The MSCP command's own opcode, read out of the body this FSM
		 * just built -- not inferred from the call order. */
		uint8_t opcd = body[VMS_OFF_MSCP_OPCD - VMS_OFF_SYSAP_BODY];

		if (opcd == VMS_MSCP_OP_SCC)
			obs(g.gus_cmds == 0u &&
			    g.n_obs > 0u &&
			    g.obs[g.n_obs - 1u] == R_MSCP_SCC1
				    ? R_MSCP_SCC2 : R_MSCP_SCC1);
		else if (opcd == VMS_MSCP_OP_GUS)
			obs(R_MSCP_GUS);
		return 0;
	}
	if (len != VMS_CM_BODY_LEN)
		return 0;
	switch (body[VMS_OFB_CM_OPCODE]) {
	case VMS_CM_OP_MODEL:  obs(R_CM_MODEL);  break;
	case VMS_CM_OP_PARAMS: obs(R_CM_PARAMS); break;
	case VMS_CM_OP_CONFIG: obs(R_CM_CONFIG); break;
	default: break;
	}
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

static void bed_init(void)
{
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

	memcpy(g.cl.params.scsnode, "OVMXS0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;

	(void)cnxman_club_init(&g.cl);
	g.member_csb = cnxman_club_alloc_csb(&g.cl.club, MEMBER_SYSID, 1);
	cnxman_csb_set_csid(g.member_csb, MEMBER_CSID);

	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);

	/* This simulated node's OWN identity -- the harness's, not a capture's
	 * (the honest-identity ruling; sim_node.h says the same at its own
	 * pe_identity). */
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
 * E73: feed the join the SYSAP BODY the executive delivers (design sec 3.2.4),
 * sliced out of the captured FRAME the fixtures hold, and tell it which CSB's
 * connection carried it -- the member's.
 */
static int32_t member_csb_index(void)
{
	return (int32_t)cnxman_club_csb_index(&g.cl.club, g.member_csb);
}

static enum cnxman_join_rx join_feed(const uint8_t *frame, uint32_t len)
{
	return cnxman_join_rx_body(&g.j, frame + VMS_OFF_SYSAP_BODY,
				   len - VMS_OFF_SYSAP_BODY, MEMBER_CSID, 1,
				   member_csb_index());
}

static int feed_fixture(const char *name)
{
	const struct vms_fixture *f = fixture(name);

	if (f == NULL)
		return -1;
	(void)join_feed(f->bytes, f->wire_len);
	return 0;
}

/* ---- the two necessarily codec-built elements (see the file header) ----- */

static uint8_t g_synth[VMS_CM_FRAME_LEN];

static uint32_t mk_go_frame(uint32_t epoch)
{
	const struct vms_fixture *tmpl = fixture("cm-open-add-req");
	uint8_t body[VMS_CM_BODY_LEN];
	uint32_t written = 0;

	/*
	 * No op-0x0a specimen is decoded in this tree, so the GO's BODY is
	 * built through the GROUNDED coordinator recipe (vms_cm_go_build,
	 * spec sec 4(p)/(r)) rather than typed out, and it is carried on the
	 * abs [0,72) span of a REAL specimen -- the open the coordinator sent
	 * on the same connection an instant earlier. Nothing here invents a
	 * port or SCS header.
	 */
	memset(g_synth, 0, sizeof(g_synth));
	if (tmpl != NULL)
		memcpy(g_synth, tmpl->bytes, VMS_OFF_SYSAP_BODY);
	if (vms_cm_go_build(VMS_CM_CLASS_ADD, epoch, body, sizeof(body),
			    &written) != VMS_CODEC_OK)
		return 0;
	memcpy(g_synth + VMS_OFF_SYSAP_BODY, body, VMS_CM_BODY_LEN);
	return VMS_CM_FRAME_LEN;
}

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

/* ==========================================================================
 * Running the clock
 *
 * Advance the virtual clock to the next armed timer and deliver it. Used to
 * prove the join is NOT waiting on one: the reference run below completes with
 * every step driven by a peer answer, and the only timers that ever fire are
 * the instrumentation watchdogs.
 * ========================================================================== */
static int run_one_timer(void)
{
	struct sim_timer t;
	uint32_t slot;

	if (!sim_clock_next(&g.clock, &slot))
		return 0;
	if (!sim_clock_fire(&g.clock, slot, &t))
		return 0;
	if (t.due_ms > g.clock.now_ms)
		g.clock.now_ms = t.due_ms;
	if (t.which == (uint8_t)CNXMAN_TIMER_JOIN)
		cnxman_join_timer(&g.j);
	return 1;
}

/* ==========================================================================
 * The replay
 * ========================================================================== */

static void replay(void)
{
	uint32_t len;

	bed_init();

	(void)cnxman_join_start(&g.j);

	/* The member answers both inquiries affirmatively -- exactly what the
	 * reference member does (spec sec 4(L)(b): "each answered
	 * affirmatively by the member on that dir-client connection"). */
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);

	cnxman_join_opened(&g.j, MSCP_CONID);
	cnxman_join_opened(&g.j, CM_CONID);

	/* sec 4(o) row 3: the peer reciprocates in kind within ~1 ms. THIS is
	 * the manifest-hashed op-0x01 specimen. */
	(void)feed_fixture("cm-params");

	len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u, VMS_MSCP_ST_AVAILABLE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 1u), 2u,
			 VMS_MSCP_ST_OFFLINE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);

	/* sec 4(o) rows 5-10, from the manifest-hashed specimens. */
	(void)feed_fixture("cm-commit-req");
	(void)feed_fixture("cm-dlm-op0d-req");
	(void)feed_fixture("cm-close-req");

	/* The transition: a real op-0x09 open, then the GO. */
	(void)feed_fixture("cm-open-add-req");
	len = mk_go_frame(0x0000000eu);
	if (len != 0u)
		(void)join_feed(g_synth, len);
	if (cnxman_join_handed_off(&g.j))
		obs(R_HANDOFF);
}

static void test_sequence_matches_the_reference(void)
{
	uint32_t i;
	uint32_t n_ref = (uint32_t)(sizeof(reference) / sizeof(reference[0]));
	int ok = 1;

	printf("\n-- the emitted sequence vs the reference joiner drive --\n");
	replay();

	for (i = 0; i < n_ref; i++) {
		const char *got = (i < g.n_obs) ? ref_name[g.obs[i]] : "(none)";

		printf("  %2u  want %-42s got %s\n", i, ref_name[reference[i]],
		       got);
		if (i >= g.n_obs || g.obs[i] != reference[i])
			ok = 0;
	}
	ct_check(ok, "the join emits the reference sequence, in order");
	ct_check_eq_u32(g.n_obs, n_ref,
			"... and emits nothing else in that window");
	/* Two GUS commands: unit 1 (the seed -- never 0, sec 4(n)) and unit 2
	 * (the previous END's own returned unit + 1), the second answered
	 * Unit-Offline. The walk length is the PEER's, so this pins the cursor
	 * advancing off the peer's answers, not a local counter. */
	ct_check_eq_u32(g.gus_cmds, 2u,
			"the NEXT-UNIT walk ran to the peer's own terminator");
	ct_check_eq_u32(g.j.units_found, 1u,
			"and enumerated exactly the units the peer reported");
}

static void test_reaches_the_barrier_handoff(void)
{
	printf("\n-- the hand-off: FC-P3.5 really takes the transition --\n");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_BARRIER,
			"the join state is BARRIER after XITGO");
	ct_check(cnxman_join_handed_off(&g.j) != 0, "handed_off() agrees");
	ct_check_eq_u32(g.b.opens_answered, 1u,
			"the barrier answered the Phase 1 open");
	ct_check(cnxman_barrier_phase2_committed(&g.b) != 0,
		 "the barrier committed Phase 2 (book p. 7-42)");
	ct_check(g.j.handoffs >= 2u,
		 "the open and the GO were both forwarded to it");
	ct_check_eq_u32(g.b.rebuild_records, 1u,
			"the interleaved cat-0x02 op-0x0d record reached it");
}

static void test_no_step_waited_on_a_timer(void)
{
	uint32_t fired = 0;

	printf("\n-- the drive is answer-driven, not clock-driven --\n");
	/* Every element of the sequence above was produced with the virtual
	 * clock standing still: nothing in the reference run waits for a
	 * timer. Draining the wheel now must not add a single sequence
	 * element -- the watchdogs instrument, they do not drive. */
	{
		uint32_t before = g.n_obs;

		while (fired < 8u && run_one_timer())
			fired++;
		ct_check_eq_u32(g.n_obs, before,
				"draining the timer wheel emits no new step");
	}
	ct_check(fired > 0u, "the join really did arm its watchdog");
	ct_check_eq_u32(g.clock.overflows, 0u, "no timer was silently dropped");
}

static void test_honest_omissions_survive_the_replay(void)
{
	printf("\n-- E8/E24/FC-P3.2: the omissions are still omissions --\n");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"no CSID was learned or invented across the whole "
			"replay (integration note E8)");
	ct_check_eq_u32(g.j.conndata_omitted, 1u,
			"the 16-byte connect data went out empty and counted");
	ct_check_eq_u32(g.j.dir_descriptor_omitted, 1u,
			"the VMS$VAXcluster directory descriptor was not "
			"declared (integration note E24)");
	ct_check(g.j.lockdirwt_unpinned >= 1u,
		 "LOCKDIRWT's absence is counted on every PARAMS (FC-P3.2)");
	ct_check_eq_u32(g.j.codec_failures, 0u, "no codec refusal");
	ct_check_eq_u32(g.j.send_failures, 0u, "no send refusal");
}

/* ==========================================================================
 * E71 -- THE SAME REFERENCE DRIVE, THROUGH A TRANSIENT
 *
 * This is the R2 leg the live runs asked for, and it is R2 rather than a
 * second R1 for the same reason the rest of this file is: the RECOVERY IS
 * DRIVEN BY THE VIRTUAL TIMER WHEEL. p. 7-30's reconnect cadence is "once a
 * second", so the only way to prove the retry really happens is to advance a
 * clock and deliver the timer -- which is exactly what sim_clock does here,
 * in microseconds and with no wire.
 *
 * The transient injected is the one join-e70refire really hit: this node's own
 * SCS refusing to put the VMS$VAXcluster connect on the wire, at the instant
 * the disk-client step handed the drive on. Before E71 that ended the join two
 * seconds before the cluster offered it membership.
 * ========================================================================== */
static void replay_through_a_transient(uint32_t *timers_fired)
{
	uint32_t len;
	uint32_t fired = 0;

	bed_init();
	g.refuse_cm_connect = 1;

	(void)cnxman_join_start(&g.j);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	cnxman_join_opened(&g.j, MSCP_CONID);
	/* ... and the VMS$VAXcluster connect that follows it is refused. */

	/* The wheel: beats pass with the refusal standing, then it lifts. */
	while (fired < 3u && run_one_timer())
		fired++;
	g.refuse_cm_connect = 0;
	while (fired < 4u && run_one_timer())
		fired++;
	*timers_fired = fired;

	cnxman_join_opened(&g.j, CM_CONID);

	(void)feed_fixture("cm-params");

	len = mk_scc_end(VMS_MSCP_CL_SCC_MSGID0);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_scc_end((uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u));
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end(VMS_MSCP_CL_GUS_MSGID0, 1u, VMS_MSCP_ST_AVAILABLE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);
	len = mk_gus_end((uint16_t)(VMS_MSCP_CL_GUS_MSGID0 + 1u), 2u,
			 VMS_MSCP_ST_OFFLINE);
	cnxman_join_rx_mscp(&g.j, MSCP_CONID, g_mscp, len);

	(void)feed_fixture("cm-commit-req");
	(void)feed_fixture("cm-dlm-op0d-req");
	(void)feed_fixture("cm-close-req");
	(void)feed_fixture("cm-open-add-req");
	len = mk_go_frame(0x0000000eu);
	if (len != 0u)
		(void)join_feed(g_synth, len);
	if (cnxman_join_handed_off(&g.j))
		obs(R_HANDOFF);
}

static void test_a_transient_does_not_end_the_join(void)
{
	uint32_t n_ref = (uint32_t)(sizeof(reference) / sizeof(reference[0]));
	uint32_t fired = 0;
	uint32_t i;
	int ok = 1;

	printf("\n-- E71: a transient connect refusal, and the drive that "
	       "survives it --\n");
	replay_through_a_transient(&fired);

	ct_check(fired >= 4u,
		 "the reconnect really ran on the virtual clock: four "
		 "once-a-second beats were delivered");
	ct_check_eq_u32(g.j.cm_connect_refused, 4u,
			"the first attempt and three beats were all refused");
	ct_check_eq_u32(g.j.cm_reattempts, 4u,
			"... and each beat made a real, counted attempt "
			"(p. 7-30's cadence)");

	for (i = 0; i < n_ref; i++) {
		if (i >= g.n_obs || g.obs[i] != reference[i])
			ok = 0;
	}
	ct_check(ok,
		 "the whole reference sequence is still emitted, in order, "
		 "AFTER the transient");
	ct_check_eq_u32(g.n_obs, n_ref, "... and nothing extra");
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_BARRIER,
			"... and the join still reaches the barrier hand-off");

	/* INV-6: surviving a transient is not becoming a member. */
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"no CSID was learned or invented across the retry");
	ct_check_eq_u32(g.j.csid_unpinned, 0u,
			"... and none was even looked for outside a real "
			"op-0x06");
	ct_check_eq_u32(g.clock.overflows, 0u, "no timer was silently dropped");
}

int main(void)
{
	char err[512];

	printf("cnxman_join (FC-P3.3, rung R2): replay the reference join\n");

	g_nfx = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				     g_fx, VMS_FIXTURE_MAX_FILES, err,
				     sizeof(err));
	if (g_nfx < 0) {
		printf("  FAIL could not load fixtures: %s\n", err);
		return 1;
	}
	printf("  loaded %d manifest-hashed specimens from %s\n", g_nfx,
	       OVMX_FIXTURE_DIR);
	ct_check(fixture("cm-params") != NULL, "cm-params specimen present");
	ct_check(fixture("cm-open-add-req") != NULL,
		 "cm-open-add-req specimen present");
	ct_check(fixture("cm-commit-req") != NULL,
		 "cm-commit-req specimen present");
	ct_check(fixture("cm-close-req") != NULL,
		 "cm-close-req specimen present");
	ct_check(fixture("cm-dlm-op0d-req") != NULL,
		 "cm-dlm-op0d-req specimen present");

	test_sequence_matches_the_reference();
	test_reaches_the_barrier_handoff();
	test_no_step_waited_on_a_timer();
	test_honest_omissions_survive_the_replay();
	test_a_transient_does_not_end_the_join();

	printf("\n  NOTE (integration note E12): the plan row's named "
	       "vax3-2to3-established-join\n"
	       "  capture is lab-host-only and has no decoded fixture in this "
	       "tree, so the\n"
	       "  inbound frames above are the formation-window specimens that "
	       "ARE decoded.\n"
	       "  Swapping in a decoded vax3 fixture list is a one-line change "
	       "here.\n");

	return ct_summary("cnxman_join");
}
