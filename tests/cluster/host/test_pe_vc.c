/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_vc.c - EVERY cell of the FC-P1.2 virtual-circuit table is a test,
 * and the sequencing invariant that three campaign stalls came down to has a
 * test of its own.
 *
 * Plan FC-P1.2's done-condition, rung R1: "every transition a test; a
 * loss/reorder unit scenario never freezes recv_ack". This file is both.
 *
 * THE HEADLINE TEST is test_recv_ack_never_freezes(). A frozen recv_ack -- a
 * circuit that is UP, whose peer keeps sending, and whose acknowledgement
 * stops advancing -- is what spec §4(O.14)/§4(O.15)/§4(O.19) traced three
 * separate join stalls to, with a real member's recv_seq stuck at 10, 19 and
 * 20 while the joiner's stream ran on past it. That test drives loss, peer
 * retransmission, duplicates and a genuine reorder through this FSM and
 * asserts, after EVERY frame, that either
 *
 *   - the circuit is OPEN and its acknowledgement equals the highest
 *     contiguous sequence the link actually delivered (computed independently
 *     by the test, not read back from the code under test), or
 *   - the circuit is NOT open, because it broke and is re-forming.
 *
 * There is deliberately no third outcome, and the assertion is made against
 * the 0x48 frame that went out on the wire as well as against the executive
 * cell -- because "the counter advanced" and "the peer was told" are two
 * different claims (INV-6).
 *
 * Every stimulus is a real codec-built SCA frame fed to pe_fsm_rx(), every
 * assertion re-parses an emitted frame through the codec, and no test reaches
 * inside the FSM.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "pe_fake_vc.h"

#define OVMX_SYSID 1030u
#define VAX1_SYSID 1025u

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

/* SYSGEN CLUSTER_CREDITS on the lab's VAXes (§3 decoder ring, byte-exact at
 * abs 95 of the formation body). */
#define LAB_CREDITS 10u

/* ------------------------------------------------------------------ *
 * The layer above: a recorder, so "SCS was told" is an assertion and not an
 * assumption. Deliberately unbindable -- half these tests run with it NULL.
 * ------------------------------------------------------------------ */
struct fake_upper {
	uint32_t    messages, datagrams, ups, downs;
	vms_conid_t last_conid;
	uint32_t    last_len;
	uint32_t    last_down_reason;
};

static void fu_message(void *ctx, vms_scs_sysid_t from, vms_conid_t conid,
		       const uint8_t *body, uint32_t len)
{
	struct fake_upper *u = (struct fake_upper *)ctx;

	(void)from; (void)body;
	u->messages++;
	u->last_conid = conid;
	u->last_len = len;
}

static void fu_datagram(void *ctx, vms_scs_sysid_t from, const uint8_t *body,
			uint32_t len)
{
	struct fake_upper *u = (struct fake_upper *)ctx;

	(void)from; (void)body; (void)len;
	u->datagrams++;
}

static void fu_vc_up(void *ctx, vms_scs_sysid_t peer)
{
	(void)peer;
	((struct fake_upper *)ctx)->ups++;
}

static void fu_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	struct fake_upper *u = (struct fake_upper *)ctx;

	(void)peer;
	u->downs++;
	u->last_down_reason = reason;
}

/* ------------------------------------------------------------------ *
 * The node under test
 * ------------------------------------------------------------------ */
struct vc_env {
	struct pe_fsm        fsm;
	struct pe_ops        ops;
	struct fake_pe       fake;
	struct fake_peer     peer;
	struct fake_upper    upper_rec;
	struct pe_upper_ops  upper;
	struct pe_vc         vcs[2];
	uint8_t              buf[VMS_HELLO_PADDED_MAX_FRAME];
};

static struct vc_env g_env;   /* ~20 KB: static, not a stack frame */

/*
 * The two SYSGEN-derived timings env_init() puts in pe_identity. They are
 * knobs rather than literals because the RELATION between them is what decides
 * which detector ends a stalled circuit (vms_pe_fsm.h: the ladder must fit
 * inside TIMVCFAIL for exhaustion to be the reason). The defaults are the
 * lab's: TIMVCFAIL 1600 read as centiseconds, and the 2 s cadence FC-P1.2's
 * tests were written against -- with which the 25-rung ladder outlasts
 * TIMVCFAIL, so those tests still end in TIMVCFAIL exactly as they did.
 */
static uint32_t g_timvcfail_ms = 16000;
static uint32_t g_vc_retransmit_ms = 2000;

static void env_timings(uint32_t timvcfail_ms, uint32_t retransmit_ms)
{
	g_timvcfail_ms = timvcfail_ms;
	g_vc_retransmit_ms = retransmit_ms;
}

/*
 * E57: the two formation-body fields the GLUE fills out of loaded SYSGEN state
 * (vms_pe.c pe_build_identity, through cluster_sysgen_sw_version/_credits) --
 * abs 72-79 software version and abs 95 CLUSTER_CREDITS. A boot that loaded no
 * parameters leaves both invalid, which is a different case from
 * `with_identity == 0` (no boot time at all, which forms no circuit): here the
 * circuit still forms and the body still goes out, honestly empty.
 */
static int g_omit_advertised;

static void env_omit_advertised(int omit)
{
	g_omit_advertised = omit;
}

/*
 * E60: how many RECEIVE BUFFERS the port owns. On a booted node the glue reads
 * this off the fork context's real pool (cf_rx_pool_bufs()); here it is a knob,
 * because the whole point of the ledger is that abs 95 MOVES WITH IT. The
 * default is CF_RX_BUFS_DEFAULT, the pool a real port allocates.
 */
#define FAKE_RX_POOL_BUFS 64u

static uint32_t g_rx_pool_bufs = FAKE_RX_POOL_BUFS;

static void env_rx_pool(uint32_t bufs)
{
	g_rx_pool_bufs = bufs;
}

/* The VMS absolute-time clock the formation body needs. A monotonic function
 * of the injected millisecond clock, so it is LIVE (a different value per
 * frame, which is the only thing §4(g) grounds about abs 112) and still
 * perfectly reproducible. */
static uint64_t fake_now_vms(void *ctx)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	return 0x00bc055269000000ull + (uint64_t)f->now_ms * 10000ull;
}

/* This node's boot time. A fixed, REAL-shaped quadword: it stands for a value
 * the executive read, and the test's job is to prove it is what goes on the
 * wire -- never a template constant chosen inside the FSM. */
#define OVMX_BOOT_TIME 0x00bc0552690a0000ull

static void env_init(struct vc_env *e, int with_identity, int with_upper)
{
	struct pe_identity id;
	size_t i;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);
	e->ops.now_vms = with_identity ? fake_now_vms : NULL;

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = 1500;
	if (with_identity) {
		memcpy(id.hw_type, "X86 ", 4);
		id.hw_type_valid = 1;
		id.incarnation_time = OVMX_BOOT_TIME;
		id.incarnation_time_valid = 1;
		if (!g_omit_advertised) {
			/* What cluster_sysgen_sw_version()/_credits() hand the
			 * glue out of a COMMITTED SYSGEN record. The credits
			 * one is a REQUEST for buffers, not the wire value. */
			memcpy(id.sw_version, "VMX V0.6", 8);
			id.sw_version_valid = 1;
			id.credits_requested = LAB_CREDITS;
			id.credits_requested_valid = 1;
		}
	}
	/* The buffers the port really owns -- set even when the SYSGEN record
	 * is omitted, so "no request" and "no buffers" stay separable. */
	id.rx_pool_bufs = g_rx_pool_bufs;
	id.timvcfail_ms = g_timvcfail_ms;
	id.vc_retransmit_ms = g_vc_retransmit_ms;

	(void)pe_fsm_init(&e->fsm, &id, OVMX_SYSID, &e->ops);
	pe_fsm_bind_vcs(&e->fsm, e->vcs, 2);
	if (with_upper) {
		e->upper.message = fu_message;
		e->upper.datagram = fu_datagram;
		e->upper.vc_up = fu_vc_up;
		e->upper.vc_down = fu_vc_down;
		e->upper.ctx = &e->upper_rec;
		pe_fsm_set_upper(&e->fsm, &e->upper);
	}
	pe_fsm_start(&e->fsm);

	fake_peer_init(&e->peer, VAX1_SYSID, vax1_hw, "VAX1");
	for (i = 0; i < VMS_DISC_NONCE_LEN; i++)
		e->peer.nonce[i] = 0;
}

static void ovmx_lavc(uint8_t out[6])
{
	vms_cluster_lavc_addr_build(OVMX_SYSID, out);
}

/* ------------------------------------------------------------------ *
 * Feeding the port
 * ------------------------------------------------------------------ */

static void rx_hello(struct vc_env *e, int directed, uint8_t word,
		     uint16_t incarnation)
{
	uint8_t dst_lavc[6];
	uint32_t len;

	if (directed)
		ovmx_lavc(dst_lavc);
	else
		memcpy(dst_lavc, group1, 6);
	len = fake_peer_hello(&e->peer, directed ? ovmx_hw : group1, dst_lavc,
			      word, incarnation, 0, e->buf, sizeof(e->buf));
	if (len != 0)
		(void)pe_fsm_rx(&e->fsm, e->buf, len);
}

static void rx_frame(struct vc_env *e, uint32_t len)
{
	if (len != 0)
		(void)pe_fsm_rx(&e->fsm, e->buf, len);
}

static void rx_start(struct vc_env *e, uint16_t round, uint16_t send_seq,
		     uint16_t recv_ack)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_start(&e->peer, VAX1_SYSID, ovmx_hw, dst_lavc,
				    round, send_seq, recv_ack, LAB_CREDITS,
				    e->buf, sizeof(e->buf)));
}

static void rx_vc_ack(struct vc_env *e)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_vc_ack(&e->peer, ovmx_hw, dst_lavc, 1, 0,
				     e->buf, sizeof(e->buf)));
}

static void rx_credit(struct vc_env *e, uint16_t acked)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_credit(&e->peer, ovmx_hw, dst_lavc, acked,
				     e->buf, sizeof(e->buf)));
}

static void rx_seqmsg(struct vc_env *e, uint16_t seq, uint16_t recv_ack)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_seqmsg(&e->peer, ovmx_hw, dst_lavc, seq,
				     recv_ack, 0x62c50009u, 0x33580008u,
				     e->buf, sizeof(e->buf)));
}

/* ------------------------------------------------------------------ *
 * Reaching each circuit state, through the public surface only
 * ------------------------------------------------------------------ */

/* Walk the channel to b4 with directed frames, which is the path a real
 * formation takes and the path that leaves a recorded incarnation behind. */
static void channel_to_b4(struct vc_env *e, uint16_t incarnation)
{
	rx_hello(e, 1, PE_PFW_VERIFY_B2, incarnation);
	rx_hello(e, 1, PE_PFW_VERIFY_B4, incarnation);
}

static struct pe_vc *the_vc(struct vc_env *e)
{
	return pe_fsm_vc_at(&e->fsm, 0);
}

static void drive_vc_to(struct vc_env *e, enum vms_pe_vc_state want)
{
	env_init(e, 1, 1);
	channel_to_b4(e, 1);          /* b4 => CHANNEL_UP => START SENT */
	switch (want) {
	case VMS_PE_VC_START_SENT:
		break;
	case VMS_PE_VC_STACK_SENT:
		rx_start(e, 0, 1, 0);
		break;
	case VMS_PE_VC_OPEN:
		rx_start(e, 0, 1, 0);
		rx_vc_ack(e);
		break;
	case VMS_PE_VC_CLOSED:
	default:
		pe_fsm_vc_event(&e->fsm, 0, PE_EV_CHANNEL_DOWN);
		break;
	}
	fake_pe_clear_frames(&e->fake);
	memset(&e->upper_rec, 0, sizeof(e->upper_rec));
}

/* ------------------------------------------------------------------ *
 * The expectation table -- a SECOND, independent statement of the machine.
 * If somebody adds an edge to pe_vc_table[][] without adding it here, the
 * "empty cell must be ignored" assertion turns red.
 * ------------------------------------------------------------------ */
struct vc_edge {
	uint8_t event;
	uint8_t state_after;
};

static const struct vc_edge edges_closed[] = {
	{ PE_EV_CHANNEL_UP,       VMS_PE_VC_START_SENT },
	{ PE_EV_RX_START,         VMS_PE_VC_STACK_SENT },
	{ PE_EV_RX_SEQMSG,        VMS_PE_VC_CLOSED },
	{ PE_EV_RX_DATAGRAM,      VMS_PE_VC_CLOSED },
	{ PE_EV_RX_CREDIT,        VMS_PE_VC_CLOSED },
	{ PE_EV_TIMER_RETRANSMIT, VMS_PE_VC_CLOSED },
	{ PE_EV_TIMER_VCFAIL,     VMS_PE_VC_CLOSED },
	{ PE_EV_CHANNEL_DOWN,     VMS_PE_VC_CLOSED },
	{ PE_EV_LINK_DOWN,        VMS_PE_VC_CLOSED },
	{ PE_EV_RX_LAST_GASP,     VMS_PE_VC_CLOSED },
	{ PE_EV_SHUTDOWN,         VMS_PE_VC_CLOSED },
};

static const struct vc_edge edges_start_sent[] = {
	{ PE_EV_RX_START,         VMS_PE_VC_STACK_SENT },  /* p. 2-14 */
	{ PE_EV_RX_STACK,         VMS_PE_VC_OPEN },
	{ PE_EV_RX_SEQMSG,        VMS_PE_VC_START_SENT },
	{ PE_EV_RX_DATAGRAM,      VMS_PE_VC_START_SENT },
	{ PE_EV_RX_CREDIT,        VMS_PE_VC_START_SENT },
	{ PE_EV_TIMER_RETRANSMIT, VMS_PE_VC_START_SENT },
	{ PE_EV_TIMER_VCFAIL,     VMS_PE_VC_START_SENT },
	{ PE_EV_CHANNEL_DOWN,     VMS_PE_VC_CLOSED },
	{ PE_EV_LINK_DOWN,        VMS_PE_VC_CLOSED },
	{ PE_EV_RX_LAST_GASP,     VMS_PE_VC_CLOSED },
	{ PE_EV_SHUTDOWN,         VMS_PE_VC_CLOSED },
};

static const struct vc_edge edges_stack_sent[] = {
	{ PE_EV_RX_ACK,           VMS_PE_VC_OPEN },
	{ PE_EV_RX_STACK,         VMS_PE_VC_OPEN },
	{ PE_EV_RX_START,         VMS_PE_VC_STACK_SENT },
	{ PE_EV_RX_SEQMSG,        VMS_PE_VC_OPEN },   /* p. 2-16 implied */
	{ PE_EV_RX_DATAGRAM,      VMS_PE_VC_OPEN },
	{ PE_EV_RX_CREDIT,        VMS_PE_VC_OPEN },
	{ PE_EV_TIMER_RETRANSMIT, VMS_PE_VC_STACK_SENT },
	{ PE_EV_TIMER_VCFAIL,     VMS_PE_VC_STACK_SENT },
	{ PE_EV_CHANNEL_DOWN,     VMS_PE_VC_CLOSED },
	{ PE_EV_LINK_DOWN,        VMS_PE_VC_CLOSED },
	{ PE_EV_RX_LAST_GASP,     VMS_PE_VC_CLOSED },
	{ PE_EV_SHUTDOWN,         VMS_PE_VC_CLOSED },
};

static const struct vc_edge edges_open[] = {
	{ PE_EV_RX_SEQMSG,        VMS_PE_VC_OPEN },
	{ PE_EV_RX_CREDIT,        VMS_PE_VC_OPEN },
	{ PE_EV_RX_DATAGRAM,      VMS_PE_VC_OPEN },
	{ PE_EV_RX_START,         VMS_PE_VC_STACK_SENT },
	{ PE_EV_RX_STACK,         VMS_PE_VC_OPEN },
	{ PE_EV_RX_ACK,           VMS_PE_VC_OPEN },     /* p. 2-12 discard */
	{ PE_EV_TIMER_RETRANSMIT, VMS_PE_VC_OPEN },
	{ PE_EV_TIMER_VCFAIL,     VMS_PE_VC_OPEN },
	{ PE_EV_CHANNEL_DOWN,     VMS_PE_VC_CLOSED },
	{ PE_EV_LINK_DOWN,        VMS_PE_VC_CLOSED },
	{ PE_EV_RX_LAST_GASP,     VMS_PE_VC_CLOSED },
	{ PE_EV_SHUTDOWN,         VMS_PE_VC_CLOSED },
};

struct vc_state_expect {
	uint8_t               state;
	const struct vc_edge *edges;
	unsigned              n;
	const char           *name;
};

static const struct vc_state_expect vc_expect[] = {
	{ VMS_PE_VC_CLOSED, edges_closed,
	  sizeof(edges_closed) / sizeof(edges_closed[0]), "CLOSED" },
	{ VMS_PE_VC_START_SENT, edges_start_sent,
	  sizeof(edges_start_sent) / sizeof(edges_start_sent[0]), "START SENT" },
	{ VMS_PE_VC_STACK_SENT, edges_stack_sent,
	  sizeof(edges_stack_sent) / sizeof(edges_stack_sent[0]), "STACK SENT" },
	{ VMS_PE_VC_OPEN, edges_open,
	  sizeof(edges_open) / sizeof(edges_open[0]), "OPEN" },
};

/* Post one event, by the route the event's own evidence requires. */
static void post_event(struct vc_env *e, enum pe_event ev)
{
	switch (ev) {
	case PE_EV_RX_START:  rx_start(e, 0, 1, 0); break;
	case PE_EV_RX_STACK:  rx_start(e, 1, 1, 0); break;
	case PE_EV_RX_ACK:    rx_vc_ack(e);         break;
	case PE_EV_RX_SEQMSG: rx_seqmsg(e, 1, 0);   break;
	case PE_EV_RX_CREDIT: rx_credit(e, 0);      break;
	default:              pe_fsm_vc_event(&e->fsm, 0, ev); break;
	}
}

static const struct vc_edge *edge_for(const struct vc_state_expect *s,
				      unsigned ev)
{
	unsigned i;

	for (i = 0; i < s->n; i++) {
		if (s->edges[i].event == ev)
			return &s->edges[i];
	}
	return NULL;
}

static void test_every_cell(void)
{
	unsigned si, ev;

	printf("-- every [vc state][event] cell\n");
	for (si = 0; si < sizeof(vc_expect) / sizeof(vc_expect[0]); si++) {
		const struct vc_state_expect *s = &vc_expect[si];

		for (ev = 0; ev < (unsigned)PE_EV__COUNT; ev++) {
			const struct vc_edge *edge = edge_for(s, ev);
			uint32_t ignored_before;
			struct pe_vc *vc;
			char what[128];

			drive_vc_to(&g_env, (enum vms_pe_vc_state)s->state);
			vc = the_vc(&g_env);
			if (vc == NULL || vc->state != s->state) {
				snprintf(what, sizeof(what),
					 "[%s] reached before the walk", s->name);
				ct_check(0, what);
				continue;
			}
			ignored_before = g_env.fsm.vc_ignored_events;
			post_event(&g_env, (enum pe_event)ev);
			vc = the_vc(&g_env);

			if (edge != NULL) {
				snprintf(what, sizeof(what),
					 "[%s] + event %u -> %s", s->name, ev,
					 pe_vc_state_name(
						(enum vms_pe_vc_state)
						edge->state_after));
				ct_check(vc != NULL &&
					 vc->state == edge->state_after, what);
			} else {
				snprintf(what, sizeof(what),
					 "[%s] + event %u: no edge, ignored+1",
					 s->name, ev);
				ct_check(vc != NULL &&
					 vc->state == s->state &&
					 g_env.fsm.vc_ignored_events ==
						 ignored_before + 1u, what);
			}
		}
	}
}

/* ------------------------------------------------------------------ *
 * Formation
 * ------------------------------------------------------------------ */

static void test_formation_joiner_path(void)
{
	struct fake_vc_decoded d;
	struct pe_vc *vc;

	printf("-- formation: our START, the peer's STACK, our ACK (p. 2-14)\n");
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);

	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check(d.ok && !d.is_ack, "a verified channel emits a 106-byte START");
	ct_check_eq_u32(d.config_round, 0, "config round 0 (spec 4(g))");
	ct_check_eq_u32(d.send_seq, 1, "send_seq 1 on a fresh circuit (4(i).B)");
	ct_check_eq_u32(d.recv_ack, 0, "recv_ack 0 on a fresh circuit");
	ct_check_eq_u32(d.start.scssystemid, OVMX_SYSID,
			"our OWN SCSSYSTEMID at abs 60");
	ct_check(memcmp(d.start.software_version, "VMX V0.6", 8) == 0,
		 "our OWN software version, not a captured VMS string");
	ct_check_eq_u32(d.start.credits, LAB_CREDITS,
			"abs 95 grants the full request: 64 buffers back 10");
	ct_check_eq_u32(d.start.credits, the_vc(&g_env)->recv_credit_max,
			"and it IS the circuit's reservation, byte for byte");
	ct_check_eq_u32(g_env.fsm.credit.reserved, d.start.credits,
			"withdrawn from the port's pool, which now shows it");
	ct_check_eq_u32(g_env.fsm.vc_sw_version_absent, 0,
			"nothing was omitted from the body");
	ct_check_eq_u32(g_env.fsm.vc_credits_absent, 0,
			"and no credit grant was omitted either");
	ct_check(d.start.incarnation_time == OVMX_BOOT_TIME,
		 "the executive's boot time at abs 80, verbatim");
	ct_check(d.start.message_time != 0 &&
		 d.start.message_time != d.start.incarnation_time,
		 "a LIVE composition time at abs 112, distinct from it");
	ct_check(memcmp(d.start.node_name, "OVMX    ", 8) == 0,
		 "SCSNODE in the fixed 8-byte blank-padded field");

	fake_pe_clear_frames(&g_env.fake);
	rx_start(&g_env, 1, 1, 1);           /* the peer's STACK */
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_OPEN, "a STACK opens the circuit");
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check(d.ok && d.is_ack, "and is answered with the 46-byte ACK");
	ct_check_eq_u32(d.config_round, 2, "config round 2 on the ACK");
	ct_check_eq_u32(g_env.upper_rec.ups, 1, "SCS was told the VC is up");
	ct_check_eq_u32(vc->send_credit_max, LAB_CREDITS,
			"the peer's GRANT was read from its START body");
}

static void test_formation_member_path(void)
{
	struct fake_vc_decoded d;
	struct pe_vc *vc;

	printf("-- formation: the peer's START, our STACK, its ACK\n");
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	fake_pe_clear_frames(&g_env.fake);

	rx_start(&g_env, 0, 1, 0);
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_STACK_SENT,
			"a START is answered with a STACK (p. 2-14)");
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check(d.ok && !d.is_ack, "the STACK re-supplies the identity body");
	ct_check_eq_u32(d.config_round, 1, "config round 1 on the STACK");

	rx_vc_ack(&g_env);
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_OPEN, "the ACK opens the circuit");
	ct_check(memcmp(vc->peer_name, "VAX1    ", 8) == 0,
		 "the peer's node name was LEARNED from its body");
	ct_check_eq_u32((uint32_t)vc->peer_sysid, VAX1_SYSID,
			"and its SCSSYSTEMID with it");
}

/*
 * §4(i).B, THE JOIN GATE. The number stamped at abs 36 is the one the MEMBER
 * advertised for us in its directed HELLO -- 1, then 2, then 3 across
 * incarnations. A joiner that hard-codes 1 against a member advertising 2
 * stalls at config round 0 forever (vms-691).
 */
static void test_incarnation_echo_is_read_not_chosen(void)
{
	struct fake_vc_decoded d;

	printf("-- the incarnation echo is READ from the peer (4(i).B)\n");
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 2);
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check_eq_u32(d.incarnation, 2,
			"the member advertised 2, so the START carries 2");

	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 3);
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check_eq_u32(d.incarnation, 3,
			"the member advertised 3, so the START carries 3");
}

/*
 * INV-6, the other half of the same rule: with NO advertised incarnation
 * there is nothing honest to stamp, so no circuit is formed at all. Reached
 * through the public surface by verifying the channel with MULTICAST-addressed
 * frames, which carry no incarnation for this node (§4(i).B: the field is what
 * the sender attributes to the RECEIVER, and a multicast frame attributes it
 * to nobody).
 */
static void test_no_echo_no_start(void)
{
	printf("-- no advertised incarnation => NO START (INV-6)\n");
	env_init(&g_env, 1, 1);
	rx_hello(&g_env, 0, PE_PFW_VERIFY_B3, 0);
	rx_hello(&g_env, 0, PE_PFW_VERIFY_B4, 0);

	ct_check_eq_u32(pe_fsm_channel_at(&g_env.fsm, 0)->state,
			VMS_PE_CH_B4, "the channel still verifies");
	ct_check_eq_u32(fake_vc_count(&g_env.fake, FAKE_VC_START), 0,
			"but not one 0x41 frame was built");
	ct_check_eq_u32(g_env.fsm.vc_no_incarnation, 1,
			"and the refusal is COUNTED, not silent");
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_CLOSED,
			"the circuit stays closed");
}

/* INV-6 again: the incarnation quadword at abs 80 is this system's BOOT TIME
 * and the one at abs 112 must be live. Without either the port forms nothing
 * rather than advertising 17-NOV-1858 (spec §4(g) CORRECTION, vms-2f3). */
static void test_no_boot_time_no_circuit(void)
{
	printf("-- no boot time / no absolute clock => NO circuit (INV-6)\n");
	env_init(&g_env, 0, 1);
	channel_to_b4(&g_env, 1);
	ct_check_eq_u32(fake_vc_count(&g_env.fake, FAKE_VC_START), 0,
			"no formation frame was built");
	ct_check_eq_u32(g_env.fsm.vc_no_identity, 1,
			"and the refusal is COUNTED");
}

/*
 * E57, the other half of the same rule. The software version at abs 72-79 and
 * the credit grant at abs 95 are ADVERTISEMENTS, not identity gates: a boot
 * that loaded no SYSGEN record still forms a circuit, but it must advertise
 * ZERO in both -- never a version this file invented and never the peer's own
 * "VMS V7.3" -- and the omission must be COUNTED, or it is invisible (which is
 * exactly how E57 shipped: eight zero bytes on the wire, VAX1 rendering the
 * software column as "?", and nothing in the executive saying so).
 */
static void test_unadvertised_identity_is_zero_and_counted(void)
{
	struct fake_vc_decoded d;
	static const uint8_t zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	printf("-- nothing loaded => honest zeros at abs 72/95, COUNTED (E57)\n");
	env_omit_advertised(1);
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	env_omit_advertised(0);

	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check(d.ok && !d.is_ack, "the circuit still forms: a START goes out");
	ct_check(memcmp(d.start.software_version, zeros, 8) == 0,
		 "abs 72-79 is ZERO -- no invented version, no borrowed one");
	ct_check_eq_u32(d.start.credits, 0,
			"abs 95 grants nothing, rather than a default window");
	ct_check_eq_u32(g_env.fsm.vc_sw_version_absent, 1,
			"and the missing version is COUNTED");
	ct_check_eq_u32(g_env.fsm.vc_credits_absent, 1,
			"and so is the missing credit grant");
	ct_check(memcmp(d.start.node_name, "OVMX    ", 8) == 0,
		 "the fields that WERE loaded are unaffected");
}

/*
 * E60, THE FAITHFULNESS GATE. abs 95 is a promise of RECEIVE BUFFERS
 * (p. 2-43: "the local SYSAP requests SCS to allocate a certain number of
 * buffers ... then the local SYSAP is said to have extended 10 Send Credits").
 * The peer will send exactly that many messages without waiting, so the byte
 * has to be the count of buffers this port actually committed -- never the
 * number the operator asked for, and never a plausible-looking constant.
 *
 * These four cases hold the wire byte against the executive state it is
 * supposed to equal, and MOVE THE STATE: the same configured request over
 * three different pools puts three different bytes on the wire. A future
 * change that decouples them -- an id.credits_requested written straight into
 * the frame, say -- fails here rather than on a live cluster.
 */
static void test_credit_is_the_reservation_not_the_request(void)
{
	struct fake_vc_decoded d;
	struct pe_vc *vc;

	printf("-- abs 95 is the RESERVATION, and it tracks the pool (E60)\n");

	/* (a) A pool too small for the request. The node asks for 10 and owns
	 * 4, so it promises 4 -- the promise it can keep. */
	env_rx_pool(4u);
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	vc = the_vc(&g_env);
	ct_check_eq_u32(d.start.credits, 4,
			"4 buffers owned, 10 requested => 4 promised");
	ct_check_eq_u32(vc->recv_credit_max, d.start.credits,
			"the circuit holds exactly what it advertised");
	ct_check_eq_u32(pe_credit_available(&g_env.fsm.credit), 0,
			"and the pool is spent, not overdrawn");
	ct_check_eq_u32(g_env.fsm.vc_credits_absent, 0,
			"a smaller REAL grant is not an omission");

	/* (b) Same request, a bigger pool: the byte moves with the buffers. */
	env_rx_pool(7u);
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check_eq_u32(d.start.credits, 7,
			"7 buffers owned => 7 promised: the wire tracks state");
	ct_check_eq_u32(the_vc(&g_env)->recv_credit_max, d.start.credits,
			"still byte-for-byte the reservation");

	/* (c) A port that owns nothing promises nothing -- and says so. This
	 * is the E57 re-fire's zero, now with a REASON attached. */
	env_rx_pool(0u);
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check(d.ok && !d.is_ack, "the circuit still forms");
	ct_check_eq_u32(d.start.credits, 0, "no buffers => no promise");
	ct_check_eq_u32(g_env.fsm.vc_credits_absent, 1,
			"and the omission is COUNTED, never silent");

	/* (d) A closed circuit gives its buffers back: a promise nobody is
	 * holding must not keep the pool locked up (p. 2-43's deallocate). */
	env_rx_pool(FAKE_RX_POOL_BUFS);
	env_init(&g_env, 1, 1);
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	ct_check_eq_u32(g_env.fsm.credit.reserved, LAB_CREDITS,
			"an open circuit holds its share");
	g_env.fake.now_ms += 25000;             /* §4(M) listen timeout */
	(void)pe_fsm_tick(&g_env.fsm, NULL, 0);
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_CLOSED,
			"the channel went and took the circuit with it");
	ct_check_eq_u32(the_vc(&g_env)->recv_credit_max, 0,
			"a closed circuit advertises nothing");
	ct_check_eq_u32(pe_credit_available(&g_env.fsm.credit),
			FAKE_RX_POOL_BUFS,
			"and every buffer is back in the pool");
}

/*
 * §4(i).A: an ESTABLISHED member's round-0 START carries its prior circuit's
 * continuation (11974 was measured). "A correct joiner must not treat the
 * member's send_seq != 1 as an error, and must not copy it into its own
 * send_seq/recv_ack" -- 40 of the 41 sequence gaps ever measured on the lab
 * wire were OVMX doing exactly that.
 */
static void test_member_continuation_is_not_copied(void)
{
	struct fake_vc_decoded d;
	struct pe_vc *vc;

	printf("-- an established member's send_seq 11974 is not copied (4(i).A)\n");
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	fake_pe_clear_frames(&g_env.fake);

	rx_start(&g_env, 0, 11974, 0);
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_STACK_SENT,
			"the large send_seq is tolerated, not an error");
	d = fake_vc_last(&g_env.fake, FAKE_VC_START);
	ct_check_eq_u32(d.send_seq, 1, "our STACK still carries send_seq 1");
	ct_check_eq_u32(d.recv_ack, 0, "and recv_ack 0");
	ct_check_eq_u32(vc->send_seq, 1, "the circuit's own counter is 1");
	ct_check_eq_u32(vc->recv_seq, 0, "and its recv_seq is 0");
}

/* p. 2-16: in START RECEIVED any packet requiring a circuit opens it, and is
 * then processed -- which is the point of the rule. */
static void test_implied_ack(void)
{
	struct pe_vc *vc;

	printf("-- the implied ACK opens the circuit and processes the packet\n");
	drive_vc_to(&g_env, VMS_PE_VC_STACK_SENT);
	rx_seqmsg(&g_env, 1, 0);
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_OPEN, "the circuit opened");
	ct_check_eq_u32(vc->implied_acks, 1, "by implied acknowledgement");
	ct_check_eq_u32(vc->recv_seq, 1, "and the message was taken, not lost");
	ct_check_eq_u32(g_env.upper_rec.messages, 1, "and delivered upward");
}

/* ------------------------------------------------------------------ *
 * Sequencing -- the item's core
 * ------------------------------------------------------------------ */

/* What this node last told the peer on the wire. Read back off the emitted
 * 0x48, not out of the FSM: "the counter advanced" and "the peer was told"
 * are two different claims. */
static int last_wire_ack(struct vc_env *e, uint16_t *out)
{
	struct fake_vc_decoded d = fake_vc_last(&e->fake, FAKE_VC_CREDIT);

	if (!d.ok)
		return 0;
	*out = d.recv_ack;
	return 1;
}

static void test_ack_advances_per_message(void)
{
	struct pe_vc *vc;
	uint16_t wire = 0;
	uint16_t n;

	printf("-- recv_ack advances once per sequenced message (4(h)(4))\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	for (n = 1; n <= 5; n++) {
		fake_pe_clear_frames(&g_env.fake);
		rx_seqmsg(&g_env, n, 0);
		vc = the_vc(&g_env);
		ct_check_eq_u32(vc->recv_seq, n, "recv_seq took the message");
		ct_check(last_wire_ack(&g_env, &wire) && wire == n,
			 "and the 0x48 on the wire carries that same number");
	}
	ct_check_eq_u32(the_vc(&g_env)->credit_tx, 5,
			"one credit-return per message, strict 1-for-1");
}

/*
 * THE STRUCTURAL GUARANTEE (vms_pe_fsm.h §3b(a)): acknowledgement is a
 * TRANSPORT fact. With no upper layer bound at all -- nothing to deliver to,
 * nobody to consent -- the ack still goes out, because that is the difference
 * between a port and a SYSAP.
 */
static void test_ack_without_an_upper_layer(void)
{
	uint16_t wire = 0;
	uint16_t n;

	printf("-- with NO upper layer bound the ack still advances (3b(a))\n");
	env_init(&g_env, 1, 0);        /* upper deliberately unbound */
	channel_to_b4(&g_env, 1);
	rx_start(&g_env, 0, 1, 0);
	rx_vc_ack(&g_env);
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_OPEN,
			"the circuit opens with nobody listening");

	for (n = 1; n <= 4; n++) {
		fake_pe_clear_frames(&g_env.fake);
		rx_seqmsg(&g_env, n, 0);
		ct_check(last_wire_ack(&g_env, &wire) && wire == n,
			 "every message is acknowledged on the wire");
	}
	ct_check_eq_u32(the_vc(&g_env)->recv_seq, 4, "recv_seq reached 4");
	ct_check(g_env.fsm.vc_rx_undelivered == 4,
		 "and the undeliverable ones are COUNTED, not hidden");
}

/* A duplicate is the peer retransmitting because OUR ack was lost. It must
 * not be scored as a gap (§4(h)(4a) measured 506 of them on real wire), must
 * not be delivered twice, and must be RE-acknowledged -- otherwise the peer
 * never learns and retransmits forever. */
static void test_duplicate_is_absorbed_and_reacked(void)
{
	struct pe_vc *vc;
	uint16_t wire = 0;

	printf("-- a duplicate is absorbed, not delivered twice, and re-acked\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	rx_seqmsg(&g_env, 1, 0);
	rx_seqmsg(&g_env, 2, 0);
	fake_pe_clear_frames(&g_env.fake);
	memset(&g_env.upper_rec, 0, sizeof(g_env.upper_rec));

	rx_seqmsg(&g_env, 2, 0);        /* the peer resends 2 */
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_OPEN, "the circuit is untouched");
	ct_check_eq_u32(vc->rx_dups, 1, "the duplicate is recognised");
	ct_check_eq_u32(vc->rx_gaps, 0, "and is NOT a gap");
	ct_check_eq_u32(g_env.upper_rec.messages, 0, "not delivered twice");
	ct_check(last_wire_ack(&g_env, &wire) && wire == 2,
		 "but re-acknowledged, so the peer stops retransmitting");
}

/*
 * THE FC-P1.9 CORRECTION, at rung 1.
 *
 * This test used to be `test_gap_breaks_the_circuit` and asserted that a
 * sequence gap tore the circuit down with PE_VC_DOWN_SEQ_GAP. THAT ASSERTION
 * ENCODED THE BUG design §3.2.5 ruled on: p. 2-31 governs the delivery/order
 * GUARANTEE and its consequence, not the detection mechanism, and a port
 * satisfies the guarantee under loss by RETRANSMITTING (the 0x7b retransmit
 * msgtype, §4(L)'s sequence reuse and §4(k)'s ~25-retry ladder are all wire
 * evidence of a real VAX doing exactly that before it breaks anything).
 *
 * The contract asserted now is the ruling's receiver row: DISCARD, do not
 * advance recv_seq, COUNT the gap, and IMMEDIATELY re-send the cumulative ack
 * -- with the circuit untouched. The duplicate ack is the signal that drives
 * the sender's go-back-N, so it is asserted on the WIRE, not just in the cell.
 */
static void test_gap_is_discarded_and_reacked(void)
{
	struct pe_vc *vc;
	uint16_t wire = 0;

	printf("-- a sequence gap is DISCARDED and re-acked, never broken "
	       "(3.2.5)\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	rx_seqmsg(&g_env, 1, 0);
	fake_pe_clear_frames(&g_env.fake);
	memset(&g_env.upper_rec, 0, sizeof(g_env.upper_rec));

	rx_seqmsg(&g_env, 3, 0);        /* 2 never arrived */

	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->rx_gaps, 1, "the gap was counted");
	ct_check_eq_u32(vc->state, VMS_PE_VC_OPEN,
			"and the circuit is STILL OPEN -- a gap is not a break");
	ct_check_eq_u32(vc->recv_seq, 1, "recv_seq did NOT advance past the hole");
	ct_check_eq_u32(g_env.upper_rec.messages, 0,
			"the out-of-order frame was discarded, not delivered");
	ct_check_eq_u32(g_env.upper_rec.downs, 0, "SCS was told nothing");
	ct_check_eq_u32(g_env.fsm.vc_reformations, 0, "and nothing re-formed");
	ct_check(last_wire_ack(&g_env, &wire) && wire == 1,
		 "but the cumulative ack of 1 went out again IMMEDIATELY -- "
		 "the duplicate ack that tells the sender where the hole is");

	/* And the hole fills: the sender's go-back-N re-sends 2, then 3. */
	rx_seqmsg(&g_env, 2, 0);
	rx_seqmsg(&g_env, 3, 0);
	ct_check_eq_u32(vc->recv_seq, 3, "the re-sent tail was taken, in order");
	ct_check_eq_u32(g_env.upper_rec.messages, 2,
			"and delivered upward exactly once each");
	ct_check_eq_u32(vc->rx_gaps, 1, "with no further gap");
	ct_check_eq_u32(vc->downs, 0, "and the circuit never went down");
}

/* ------------------------------------------------------------------ *
 * ***  THE HEADLINE: recv_ack can never freeze  ***
 * ------------------------------------------------------------------ */

/*
 * The scenario is a script of what the LINK did, and the expectation is
 * computed by the test from that script alone -- the highest contiguous
 * sequence actually delivered. After every step exactly one of two things
 * must be true, and the checker below is the whole point of this file.
 */
struct wire_step {
	uint16_t seq;
	int      delivered;   /* 0 = the link dropped it */
	const char *what;
};

static void check_never_frozen(struct vc_env *e, uint16_t expect_ack,
			       const char *what)
{
	struct pe_vc *vc = the_vc(e);
	uint16_t wire = 0;
	char msg[160];

	if (vc == NULL) {
		ct_check(0, "the circuit object survived");
		return;
	}
	if (vc->state != VMS_PE_VC_OPEN) {
		/* The other admissible outcome: it broke and is re-forming.
		 * A broken circuit is a LOUD failure, and the campaign's
		 * pathology was precisely a circuit that stayed up. */
		snprintf(msg, sizeof(msg),
			 "%s: circuit not open (re-forming) -- not a freeze",
			 what);
		ct_check(vc->downs > 0, msg);
		return;
	}
	snprintf(msg, sizeof(msg),
		 "%s: OPEN circuit acknowledges %u (highest contiguous)",
		 what, (unsigned)expect_ack);
	ct_check(vc->recv_seq == expect_ack, msg);

	if (expect_ack == 0)
		return;
	snprintf(msg, sizeof(msg), "%s: and the WIRE carries %u", what,
		 (unsigned)expect_ack);
	ct_check(last_wire_ack(e, &wire) && wire == expect_ack, msg);
}

static void test_recv_ack_never_freezes(void)
{
	/*
	 * Phase 1 -- LOSS, and the peer's own retransmit ladder filling it.
	 * A real port drops frames and re-sends them at the SAME sequence
	 * (§4(L)); what must never happen is this node taking the retransmit
	 * and leaving its acknowledgement one short.
	 */
	static const struct wire_step phase1[] = {
		{ 1, 1, "msg 1 arrives" },
		{ 2, 0, "msg 2 is LOST on the link" },
		{ 3, 0, "msg 3 is LOST too" },
		{ 2, 1, "the peer retransmits 2 (same seq)" },
		{ 3, 1, "the peer retransmits 3" },
		{ 4, 1, "msg 4 arrives" },
		{ 4, 1, "msg 4 again: OUR ack was lost" },
		{ 5, 1, "msg 5 arrives" },
	};
	uint16_t highest = 0;
	unsigned i;

	printf("-- ***  recv_ack never freezes: loss, retransmit, duplicate  ***\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);

	for (i = 0; i < sizeof(phase1) / sizeof(phase1[0]); i++) {
		const struct wire_step *s = &phase1[i];

		if (!s->delivered)
			continue;               /* the link ate it */
		fake_pe_clear_frames(&g_env.fake);
		rx_seqmsg(&g_env, s->seq, 0);
		if (s->seq == (uint16_t)(highest + 1u))
			highest = s->seq;       /* the contiguous frontier */
		check_never_frozen(&g_env, highest, s->what);
	}
	ct_check_eq_u32(the_vc(&g_env)->recv_seq, 5,
			"the acknowledgement reached the peer's highest "
			"contiguous send_seq");
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_OPEN,
			"having never broken the circuit over pure loss");
	ct_check_eq_u32(the_vc(&g_env)->rx_gaps, 0, "and never seen a gap");

	/*
	 * Phase 2 -- REORDER, and the FC-P1.9 correction.
	 *
	 * The link delivers 7 before 6. This phase used to assert that the
	 * circuit BROKE on it; that assertion encoded the bug design §3.2.5
	 * ruled on. With a receive window of 1 the reordered frame is
	 * DISCARDED, the acknowledgement stays at 5, and it is RE-SENT -- which
	 * is still the invariant this test exists for, because the third
	 * outcome (an open circuit whose acknowledgement silently stops) is what
	 * check_never_frozen() forbids. The ack does not advance past a hole,
	 * and it does not stop being told to the peer either.
	 */
	printf("--   ... and a reorder is discarded and re-acked, not a break\n");
	fake_pe_clear_frames(&g_env.fake);
	rx_seqmsg(&g_env, 7, 0);
	check_never_frozen(&g_env, 5, "reordered msg 7 before 6");
	ct_check_eq_u32(the_vc(&g_env)->rx_gaps, 1, "scored as a gap");
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_OPEN,
			"the circuit is untouched: a gap is a counter, not a "
			"reason (3.2.5)");
	ct_check_eq_u32(the_vc(&g_env)->downs, 0, "nothing was torn down");

	/*
	 * Phase 3 -- the sender goes back N and the frontier moves again. 6 is
	 * re-sent, then 7 behind it, and the acknowledgement follows the
	 * contiguous frontier the whole way. No re-formation was needed: the
	 * port absorbed the loss, which is precisely what makes a VC break a
	 * REAL event for the layers above (design §3.2.5's FC-P2.2 contract).
	 */
	printf("--   ... and go-back-N fills the hole without a re-formation\n");
	fake_pe_clear_frames(&g_env.fake);
	rx_seqmsg(&g_env, 6, 0);
	check_never_frozen(&g_env, 6, "the sender re-sent 6");
	rx_seqmsg(&g_env, 7, 0);
	check_never_frozen(&g_env, 7, "and 7 behind it, in order");
	ct_check_eq_u32(g_env.fsm.vc_reformations, 0,
			"the whole loss/reorder scenario cost ZERO circuit "
			"re-formations");
	ct_check_eq_u32(g_env.upper_rec.downs, 0, "and SCS was never told of a "
			"break it would have had to close every CDT for");
}

/* ------------------------------------------------------------------ *
 * Sending: one contiguous sequence, retransmit reusing it
 * ------------------------------------------------------------------ */

/* A frame for this node to send: built exactly as the layer above would,
 * addressed from the circuit's OWN learned addressing (pe_vc_addr). */
static uint32_t our_seqmsg(struct vc_env *e, uint32_t conid, uint8_t *out,
			   uint32_t cap)
{
	struct vms_scs_addr a;
	struct vms_frame_info fi;
	struct vms_sca_hdr h;
	vms_wire_buf_t w;
	uint32_t written = 0;

	if (pe_vc_addr(&e->fsm, VAX1_SYSID, &a) != 0 || cap < FAKE_VC_MSG_LEN)
		return 0;
	memset(out, 0, FAKE_VC_MSG_LEN);
	memset(&h, 0, sizeof(h));
	memcpy(h.eth_dst, a.dst_mac, 6);
	memcpy(h.eth_src, a.src_mac, 6);
	memcpy(h.dst_lavc, a.dst_logical, 6);
	memcpy(h.src_lavc, a.src_logical, 6);
	h.sca_len_field = (uint16_t)(FAKE_VC_MSG_SCA - 2u);
	h.connect_flag = 0x0001u;
	h.word30 = (uint16_t)(VMS_SCS_MT_MSG |
			      ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	if (vms_sca_hdr_build(&h, out, cap, &written) != VMS_CODEC_OK)
		return 0;
	if (vms_frame_classify(out, FAKE_VC_MSG_LEN, &fi) != VMS_CODEC_OK)
		return 0;
	vms_wire_buf_init(&w, out, FAKE_VC_MSG_LEN);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, conid);
	if (!vms_wire_buf_ok(&w))
		return 0;
	return FAKE_VC_MSG_LEN;
}

static void test_send_seq_is_one_contiguous_counter(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	struct fake_vc_decoded d;
	uint32_t len;
	int rc;

	printf("-- ONE contiguous send_seq across all connections (4(O.14))\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);

	/* Three messages on TWO different Con.ID pairs: the sequence is the
	 * CIRCUIT's, so it must run 1,2,3 with no per-connection restart. */
	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	ct_check(len == FAKE_VC_MSG_LEN, "the caller built a sendable frame");
	rc = pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	ct_check_eq_u32((unsigned)-rc, 0, "send 1 accepted");
	d = fake_vc_last(&g_env.fake, FAKE_VC_SEQ);
	ct_check_eq_u32(d.send_seq, 1, "first message carries send_seq 1");

	len = our_seqmsg(&g_env, 0x33580008u, msg, sizeof(msg));
	(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	d = fake_vc_last(&g_env.fake, FAKE_VC_SEQ);
	ct_check_eq_u32(d.send_seq, 2,
			"a DIFFERENT Con.ID takes the next number, not 1");

	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	d = fake_vc_last(&g_env.fake, FAKE_VC_SEQ);
	ct_check_eq_u32(d.send_seq, 3, "and the first Con.ID continues at 3");
	ct_check_eq_u32(the_vc(&g_env)->unacked, 3, "all three are unacked");

	/* And the ack we piggyback is this circuit's real recv_seq. */
	rx_seqmsg(&g_env, 1, 0);
	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	d = fake_vc_last(&g_env.fake, FAKE_VC_SEQ);
	ct_check_eq_u32(d.recv_ack, 1,
			"an outbound message carries the REAL recv_seq");
}

static void test_retransmit_reuses_the_sequence(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	struct fake_vc_decoded d;
	uint32_t len;

	printf("-- a retransmit reuses the seq and refreshes the ack (4(L))\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);

	/* The peer sends us something (so our recv_seq moves) but does not
	 * acknowledge ours. The retransmit must carry the OLD sequence and the
	 * NEW acknowledgement -- a retransmit with a stale ack would be the
	 * freeze arriving by the back door. */
	rx_seqmsg(&g_env, 1, 0);
	rx_seqmsg(&g_env, 2, 0);
	fake_pe_clear_frames(&g_env.fake);

	g_env.fake.now_ms += 2500;             /* past the retransmit cadence */
	pe_fsm_vc_timer(&g_env.fsm, 0);

	d = fake_vc_last(&g_env.fake, FAKE_VC_SEQ);
	ct_check(d.ok, "the unacked message went out again");
	ct_check_eq_u32(d.send_seq, 1, "at the SAME sequence");
	ct_check_eq_u32(d.recv_ack, 2, "with a FRESH acknowledgement");
	ct_check_eq_u32(d.msgtype, VMS_SCS_MT_ALT,
			"marked 0x7b, the wire's own retransmit form");
	ct_check_eq_u32(the_vc(&g_env)->retransmits, 1, "and it is counted");
	ct_check_eq_u32(the_vc(&g_env)->send_seq, 2,
			"the circuit's next number did NOT advance");
}

/* Walk everything the port emitted since the last clear and read the sequenced
 * messages out of it, in emission order, through the codec. Returns how many
 * there were and fills `seq`/`retransmit_marked`. */
struct resend_run {
	unsigned n;
	uint16_t seq[8];
	int      all_marked_retransmit;
};

static struct resend_run collect_resends(struct vc_env *e)
{
	struct resend_run r;
	uint32_t i;

	memset(&r, 0, sizeof(r));
	r.all_marked_retransmit = 1;
	for (i = 0; i < e->fake.n_frames; i++) {
		struct fake_vc_decoded d = fake_vc_decode(&e->fake, i);

		if (!d.ok || !d.is_seqmsg)
			continue;
		if (d.msgtype != VMS_SCS_MT_ALT)
			r.all_marked_retransmit = 0;
		if (r.n < sizeof(r.seq) / sizeof(r.seq[0]))
			r.seq[r.n] = d.send_seq;
		r.n++;
	}
	return r;
}

/*
 * GO-BACK-N (design §3.2.5). The receiver's window is 1, so everything after
 * a hole was discarded and the SENDER must re-send the whole outstanding tail,
 * in sequence order, starting at the oldest unacked entry. Proved with no
 * randomness: three messages go out, nothing is acknowledged, the ladder beat
 * fires once, and what came out is read back off the wire.
 */
static void test_go_back_n_resends_the_tail_in_order(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	struct resend_run r;
	uint32_t len;
	unsigned i;

	printf("-- go-back-N: the whole unacked tail goes again, IN ORDER\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	for (i = 0; i < 3; i++) {
		len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
		(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	}
	ct_check_eq_u32(the_vc(&g_env)->unacked, 3, "three outstanding");

	/* The peer acknowledges NOTHING, so the hole is at sequence 1. */
	fake_pe_clear_frames(&g_env.fake);
	g_env.fake.now_ms += 2500;             /* past the ladder cadence */
	pe_fsm_vc_timer(&g_env.fsm, 0);

	r = collect_resends(&g_env);
	ct_check_eq_u32(r.n, 3, "the whole tail went again, not just the head");
	ct_check(r.n == 3 && r.seq[0] == 1 && r.seq[1] == 2 && r.seq[2] == 3,
		 "1, 2, 3 in that order -- a window-1 receiver takes them in "
		 "no other");
	ct_check(r.all_marked_retransmit,
		 "each marked 0x7b, the wire's own retransmit form (4(h))");
	ct_check_eq_u32(the_vc(&g_env)->send_seq, 4,
			"and NOT ONE retransmit consumed a new sequence (4(L))");
	ct_check_eq_u32(the_vc(&g_env)->retransmits, 3, "three counted");

	/* A cumulative ack MOVES the hole: 1 is released, so the next round
	 * starts at 2 and 1 is never sent again. */
	rx_credit(&g_env, 1);
	fake_pe_clear_frames(&g_env.fake);
	g_env.fake.now_ms += 2500;
	pe_fsm_vc_timer(&g_env.fsm, 0);

	r = collect_resends(&g_env);
	ct_check_eq_u32(r.n, 2, "only what is still outstanding goes again");
	ct_check(r.n == 2 && r.seq[0] == 2 && r.seq[1] == 3,
		 "starting at the NEW oldest entry, 2, and in order");
}

/*
 * THE LADDER'S BOUND (design §3.2.5). A peer that never acknowledges gets
 * PE_VC_RETRANSMIT_TRIES transmissions of the same bytes at the same sequence
 * and then the circuit is broken with PE_VC_DOWN_RETRANSMIT_EXHAUSTED --
 * p. 2-31's "the guarantee of message delivery cannot be satisfied", as a
 * MEASUREMENT rather than a guess. TIMVCFAIL is set well beyond the ladder
 * here so that the ladder is unambiguously the detector that fired.
 */
static void test_retransmit_ladder_exhaustion_breaks_the_circuit(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	struct pe_vc *vc;
	uint32_t len;
	unsigned i;

	printf("-- the ladder is BOUNDED: %u tries, then the circuit breaks\n",
	       (unsigned)PE_VC_RETRANSMIT_TRIES);
	env_timings(600000, 1000);             /* ladder 25 s, TIMVCFAIL 600 s */
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);

	/* One beat per cadence interval; the peer answers nothing at all. */
	for (i = 0; i < PE_VC_RETRANSMIT_TRIES + 1u; i++) {
		fake_pe_clear_frames(&g_env.fake);
		g_env.fake.now_ms += 1000;
		pe_fsm_vc_timer(&g_env.fsm, 0);
	}

	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->retransmits, PE_VC_RETRANSMIT_TRIES,
			"exactly the ladder's worth of retransmissions, then "
			"it stopped");
	ct_check_eq_u32(vc->last_down_reason, PE_VC_DOWN_RETRANSMIT_EXHAUSTED,
			"the circuit broke for the LADDER, not for TIMVCFAIL");
	ct_check_eq_u32(g_env.upper_rec.last_down_reason,
			PE_VC_DOWN_RETRANSMIT_EXHAUSTED,
			"and SCS was raised vc_down() with that reason -- the "
			"seam FC-P2.2 binds (design 3.2.5)");
	ct_check_eq_u32(g_env.upper_rec.downs, 1, "exactly once");
	ct_check_eq_u32(vc->downs, 1, "one break, not a storm of them");
	ct_check_eq_u32(vc->state, VMS_PE_VC_START_SENT,
			"and it is already re-forming over the live channel");
	ct_check_eq_u32(vc->unacked, 0,
			"the old ring went with the old circuit");
	env_timings(16000, 2000);              /* back to the lab's values */
}

static void test_peer_ack_releases_the_ring(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	uint32_t len;
	unsigned i;

	printf("-- the peer's CUMULATIVE ack releases the unacked ring\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	for (i = 0; i < 3; i++) {
		len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
		(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	}
	ct_check_eq_u32(the_vc(&g_env)->unacked, 3, "three outstanding");

	rx_credit(&g_env, 2);       /* one 0x48 acknowledging up to 2 */
	ct_check_eq_u32(the_vc(&g_env)->unacked, 1,
			"one ack for 2 releases 1 AND 2 -- it is cumulative");
	ct_check_eq_u32(the_vc(&g_env)->peer_recv_ack, 2,
			"and the peer's position is recorded");

	rx_credit(&g_env, 3);
	ct_check_eq_u32(the_vc(&g_env)->unacked, 0, "then the last one");
}

/* p. 2-43: the window is the PEER's grant, read from its formation body. This
 * node never invents one for itself, and it refuses rather than overrunning. */
static void test_credit_window(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	uint32_t len;
	unsigned i;
	int rc = 0;

	printf("-- the send window is the peer's GRANT, and it is enforced\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	ct_check_eq_u32(the_vc(&g_env)->send_credit, LAB_CREDITS,
			"CLUSTER_CREDITS 10, read off the peer's START");

	for (i = 0; i < LAB_CREDITS; i++) {
		len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
		rc = pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
		if (rc != PE_VC_SEND_OK)
			break;
	}
	ct_check_eq_u32((unsigned)-rc, 0, "ten messages fit the window");
	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	rc = pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);
	ct_check_eq_u32((unsigned)(-rc), (unsigned)(-PE_VC_SEND_NOCREDIT),
			"the eleventh is REFUSED, not sent on credit we lack");
	ct_check_eq_u32(the_vc(&g_env)->send_seq, 11,
			"and the refused message consumed NO sequence number");

	rx_credit(&g_env, 1);
	ct_check_eq_u32(the_vc(&g_env)->send_credit, 1,
			"one 0x48 returns exactly one credit (4(h)(3))");
}

/*
 * TIMVCFAIL is the answer to the campaign's actual pathology: a peer whose
 * recv_ack stops advancing while this node keeps sending. The circuit dies
 * and re-forms instead of stalling forever.
 */
static void test_timvcfail_breaks_and_reforms(void)
{
	uint8_t msg[FAKE_VC_MSG_LEN];
	uint32_t len;
	struct pe_vc *vc;

	printf("-- a peer that stops acknowledging => TIMVCFAIL, then re-form\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	len = our_seqmsg(&g_env, 0x62c50009u, msg, sizeof(msg));
	(void)pe_vc_send_frame(&g_env.fsm, VAX1_SYSID, msg, len);

	/* The peer keeps TALKING (so the channel stays alive) but never
	 * acknowledges -- exactly the shape of §4(O.19)'s frozen member. */
	g_env.fake.now_ms += 8000;
	pe_fsm_vc_timer(&g_env.fsm, 0);
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_OPEN,
			"before TIMVCFAIL it just keeps retransmitting");
	ct_check(the_vc(&g_env)->retransmits >= 1, "and it does retransmit");

	g_env.fake.now_ms += 9000;             /* past TIMVCFAIL 16 s */
	pe_fsm_vc_timer(&g_env.fsm, 0);
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->last_down_reason, PE_VC_DOWN_TIMVCFAIL,
			"the circuit failed for TIMVCFAIL");
	ct_check_eq_u32(g_env.upper_rec.last_down_reason, PE_VC_DOWN_TIMVCFAIL,
			"SCS was told, with the reason");
	ct_check_eq_u32(vc->state, VMS_PE_VC_START_SENT,
			"and it is already re-forming");
	ct_check_eq_u32(vc->unacked, 0, "the old ring went with the old circuit");
	ct_check_eq_u32(vc->send_credit, 0,
			"and so did the old grant -- no invented window");
}

/* Design §3.4: the circuit rides the channel. A channel that stops being
 * verified takes its circuit with it, and the next verify re-forms it. */
static void test_channel_loss_tears_the_circuit_down(void)
{
	struct pe_vc *vc;

	printf("-- a channel that goes unverified tears the circuit down\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);

	/* §4(M): silence past the listen timeout. The channel gives up, and
	 * the port beat carries the news to the circuit. */
	g_env.fake.now_ms += 25000;
	(void)pe_fsm_tick(&g_env.fsm, NULL, 0);

	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_CLOSED, "the circuit closed");
	ct_check_eq_u32(vc->last_down_reason, PE_VC_DOWN_CHANNEL,
			"because the channel went, not because of TIMVCFAIL");
	ct_check_eq_u32(g_env.upper_rec.downs, 1, "SCS was told");

	/* And it comes back when the channel does. */
	channel_to_b4(&g_env, 1);
	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_START_SENT,
			"a re-verified channel re-forms the circuit");
}

/* §4(O.30) / p. 7-29: on a last gasp the port "immediately closes the virtual
 * circuit … then notifies all SYSAPs" -- and does NOT re-form, because the
 * node said it was leaving. */
static void test_last_gasp_closes_without_reforming(void)
{
	printf("-- a last gasp closes the circuit and does not re-form it\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	rx_hello(&g_env, 0, PE_PFW_LAST_GASP, 0);

	ct_check_eq_u32(the_vc(&g_env)->state, VMS_PE_VC_CLOSED,
			"the circuit closed");
	ct_check_eq_u32(the_vc(&g_env)->last_down_reason, PE_VC_DOWN_PEER_GONE,
			"with the departure reason");
	ct_check_eq_u32(g_env.fsm.vc_reformations, 0, "and no re-formation");
}

/* An OPEN circuit whose peer sends a START has a peer that re-formed: the old
 * circuit is finished and the counters reset (§4(h)(4a)). */
static void test_peer_restart_resets_the_circuit(void)
{
	struct pe_vc *vc;

	printf("-- a START on an OPEN circuit is the peer re-forming\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	rx_seqmsg(&g_env, 1, 0);
	rx_seqmsg(&g_env, 2, 0);
	ct_check_eq_u32(the_vc(&g_env)->recv_seq, 2, "the circuit had run");

	rx_start(&g_env, 0, 1, 0);
	vc = the_vc(&g_env);
	ct_check_eq_u32(vc->state, VMS_PE_VC_STACK_SENT, "we answer with STACK");
	ct_check_eq_u32(vc->recv_seq, 0, "and the counters reset (4(h)(4a))");
	ct_check_eq_u32(vc->send_seq, 1, "both of them");
	ct_check_eq_u32(g_env.upper_rec.last_down_reason,
			PE_VC_DOWN_PEER_RESTART, "SCS was told why");
}

/* The snapshot is a projection of executive state and nothing else: a value
 * never learned stays zero (INV-6). */
static void test_projection(void)
{
	struct vms_pe_vc_view v;

	printf("-- the snapshot projects what the executive holds, and no more\n");
	env_init(&g_env, 1, 1);
	channel_to_b4(&g_env, 1);
	pe_fsm_vc_project(&g_env.fsm, the_vc(&g_env), &v);
	ct_check_eq_u32(v.state, VMS_PE_VC_START_SENT, "state projected");
	ct_check_eq_u32(v.incarnation_lo, 0,
			"no formation body yet => no peer incarnation claimed");
	ct_check_eq_u32(v.credits_send, 0, "and no credit claimed");
	ct_check_eq_u32(v.rx_gaps, 0, "no traffic yet => no gap");
	ct_check_eq_u32(v.down_reason, 0, "never down since allocation");

	rx_start(&g_env, 0, 1, 0);
	rx_vc_ack(&g_env);
	rx_seqmsg(&g_env, 1, 0);
	pe_fsm_vc_project(&g_env.fsm, the_vc(&g_env), &v);
	ct_check_eq_u32(v.state, VMS_PE_VC_OPEN, "open");
	ct_check_eq_u32(v.recv_seq, 1, "recv_seq projected");
	ct_check_eq_u32(v.recv_ack, v.recv_seq,
			"recv_ack IS recv_seq -- one cell, read twice");
	ct_check_eq_u32(v.credits_send, LAB_CREDITS, "the peer's real grant");
	ct_check(vms_cluster_snapshot_q(v.incarnation_lo, v.incarnation_hi) ==
		 0x00bc05526906b4a1ull,
		 "the peer's incarnation, as ITS body carried it");

	/* FC-P1.6: rx_gaps is vc->rx_gaps read straight through -- feed a real
	 * ahead-of-window frame and confirm the SAME cell test_gap_is_
	 * discarded_and_reacked() asserts directly is what the wire row
	 * reports. */
	rx_seqmsg(&g_env, 3, 0);        /* 2 never arrived: a gap */
	pe_fsm_vc_project(&g_env.fsm, the_vc(&g_env), &v);
	ct_check_eq_u32(v.rx_gaps, the_vc(&g_env)->rx_gaps,
			"the projected gap counter IS pe_vc.rx_gaps");
	ct_check_eq_u32(v.rx_gaps, 1, "and the gap really happened");
	ct_check_eq_u32(v.down_reason, 0,
			"a gap alone never breaks the circuit (3.2.5)");
}

/* down_reason is pe_vc.last_down_reason, unwidened -- proved against a REAL
 * teardown (channel loss), not asserted from the enum alone. */
static void test_projection_down_reason(void)
{
	struct vms_pe_vc_view v;
	struct pe_vc *vc;

	printf("-- the snapshot's down_reason is the FSM's own last_down_reason\n");
	drive_vc_to(&g_env, VMS_PE_VC_OPEN);
	vc = the_vc(&g_env);
	pe_fsm_vc_project(&g_env.fsm, vc, &v);
	ct_check_eq_u32(v.down_reason, 0, "open, never down");

	/* §4(M): silence past the listen timeout tears the channel, and the
	 * port beat carries the news to the circuit -- the same trigger
	 * test_channel_loss_tears_the_circuit_down() uses. */
	g_env.fake.now_ms += 25000;
	(void)pe_fsm_tick(&g_env.fsm, NULL, 0);
	ct_check_eq_u32(vc->last_down_reason, PE_VC_DOWN_CHANNEL,
			"the FSM recorded why");
	pe_fsm_vc_project(&g_env.fsm, vc, &v);
	ct_check_eq_u32(v.down_reason, PE_VC_DOWN_CHANNEL,
			"and the snapshot reports the SAME reason, not a copy");
}

/* Without a bound circuit table the port is FC-P0.8's port: channels, no
 * circuits, and every SCS frame honestly counted. */
static void test_no_table_no_circuits(void)
{
	printf("-- with no circuit table bound the port forms none\n");
	env_init(&g_env, 1, 1);
	pe_fsm_bind_vcs(&g_env.fsm, NULL, 0);
	channel_to_b4(&g_env, 1);
	rx_start(&g_env, 0, 1, 0);

	ct_check(pe_fsm_vc_at(&g_env.fsm, 0) == NULL, "no circuit exists");
	ct_check_eq_u32(fake_vc_count(&g_env.fake, FAKE_VC_START), 0,
			"and no 0x41 frame was built");
	ct_check_eq_u32(g_env.fsm.vc_rx_no_circuit, 1,
			"the peer's START is counted, not answered");
}

int main(void)
{
	test_every_cell();
	test_formation_joiner_path();
	test_formation_member_path();
	test_incarnation_echo_is_read_not_chosen();
	test_no_echo_no_start();
	test_no_boot_time_no_circuit();
	test_unadvertised_identity_is_zero_and_counted();
	test_credit_is_the_reservation_not_the_request();
	test_member_continuation_is_not_copied();
	test_implied_ack();
	test_ack_advances_per_message();
	test_ack_without_an_upper_layer();
	test_duplicate_is_absorbed_and_reacked();
	test_gap_is_discarded_and_reacked();
	test_recv_ack_never_freezes();
	test_send_seq_is_one_contiguous_counter();
	test_retransmit_reuses_the_sequence();
	test_go_back_n_resends_the_tail_in_order();
	test_retransmit_ladder_exhaustion_breaks_the_circuit();
	test_peer_ack_releases_the_ring();
	test_credit_window();
	test_timvcfail_breaks_and_reforms();
	test_channel_loss_tears_the_circuit_down();
	test_last_gasp_closes_without_reforming();
	test_peer_restart_resets_the_circuit();
	test_projection();
	test_projection_down_reason();
	test_no_table_no_circuits();
	return ct_summary("test_pe_vc");
}
