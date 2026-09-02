/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_channel.c - EVERY cell of the FC-P0.8 channel table is a test.
 *
 * Plan FC-P0.8's done-condition, rung R1: "table-driven transitions each a
 * test". This file takes that literally. It walks all
 * VMS_PE_CH_STATE__COUNT x PE_EV__COUNT cells of pe_table[][] and asserts, for
 * each one:
 *
 *   - a POPULATED cell produces the documented resulting state and the
 *     documented action;
 *   - an EMPTY cell changes nothing and increments f->ignored_events by exactly
 *     one -- which is the auditable record that the FSM ignored an event the
 *     wire spec does not connect to that state, rather than guessing at it.
 *
 * The expectation lists below are therefore a SECOND, independent statement of
 * the machine: if somebody adds an edge to vms_pe_fsm.c without adding it here,
 * the "empty cell must be ignored" assertion turns red. That is the property a
 * hand-written per-transition test does not have.
 *
 * Every event is driven through the PUBLIC surface only: the seven
 * discovery-family RX_* events by feeding a real, codec-built SCA frame to
 * pe_fsm_rx(), and the rest by posting them with pe_fsm_event(). No test
 * reaches inside the FSM, and no test asserts on a byte offset.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "pe_fake_ops.h"

/* ------------------------------------------------------------------ *
 * The node under test, and the stations it hears
 *
 * SS3's decoder ring: VAX1 is SCSSYSTEMID 1025 / aa-00-04-00-01-04 with HW MAC
 * 08-00-2b-4a-b7-15, and the cluster group 1 is AB-00-04-01-01-01.
 * ------------------------------------------------------------------ */

#define OVMX_SYSID 1030u

static const uint8_t ovmx_hw[6]  = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6]  = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]   = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

struct pe_env {
	struct pe_fsm    fsm;
	struct pe_ops    ops;
	struct fake_pe   fake;
	struct fake_peer peer;      /* a real VAX: publishes a LOGICAL address */
	struct fake_peer mute;      /* a station that publishes none (SS4(a).0)  */
	uint8_t          buf[VMS_HELLO_PADDED_MAX_FRAME];
};

static struct pe_env g_env;   /* ~13 KB of FSM: static, not a stack frame */

static void env_init(struct pe_env *e)
{
	struct pe_identity id;
	size_t i;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = 1500;      /* NISCS_MAX_PKTSZ 1498 + 2 (SS4(k)) */

	(void)pe_fsm_init(&e->fsm, &id, OVMX_SYSID, &e->ops);
	pe_fsm_start(&e->fsm);

	fake_peer_init(&e->peer, 1025, vax1_hw, "VAX1");

	/* The same station, but its HELLOs carry its HARDWARE address where the
	 * cluster-LOGICAL one belongs, so there is no address SS4(a).0 lets us
	 * put at abs 16 of a reply. */
	fake_peer_init(&e->mute, 1025, vax1_hw, "VAX1");
	for (i = 0; i < 6; i++)
		e->mute.lavc[i] = vax1_hw[i];
}

static void ovmx_lavc(uint8_t out[6])
{
	vms_cluster_lavc_addr_build(OVMX_SYSID, out);
}

/* Feed one HELLO from `p`, directed at this node or sent to the group. */
static enum pe_channel_action rx_hello(struct pe_env *e,
				       const struct fake_peer *p, int directed,
				       uint8_t word, uint16_t incarnation,
				       uint16_t padded_sca)
{
	uint8_t dst_lavc[6];
	uint32_t len;

	if (directed)
		ovmx_lavc(dst_lavc);
	else
		memcpy(dst_lavc, group1, 6);

	len = fake_peer_hello(p, directed ? ovmx_hw : group1, dst_lavc, word,
			      incarnation, padded_sca, e->buf, sizeof(e->buf));
	if (len == 0)
		return PE_CH_ACT_NONE;
	return pe_fsm_rx(&e->fsm, e->buf, len);
}

static enum pe_channel_action rx_solicit(struct pe_env *e,
					 const struct fake_peer *p)
{
	uint32_t len = fake_peer_solicit(p, group1, e->buf, sizeof(e->buf));

	if (len == 0)
		return PE_CH_ACT_NONE;
	return pe_fsm_rx(&e->fsm, e->buf, len);
}

/* ------------------------------------------------------------------ *
 * Reaching each state
 *
 * Every path here is one the wire produces, except the "directed HELLO carrying
 * the multicast word a0" used to seed a channel with a recorded incarnation:
 * that word is not seen on a directed frame in any capture, and feeding it is
 * itself a check that an UNRECOGNISED abs-30 word is treated as a plain HELLO
 * rather than guessed at.
 * ------------------------------------------------------------------ */
static void drive_to(struct pe_env *e, enum vms_pe_channel_state want)
{
	env_init(e);
	switch (want) {
	case VMS_PE_CH_SEEN:
		/* A station with no LOGICAL address: nothing to direct at. */
		(void)rx_hello(e, &e->mute, 1, PE_PFW_MULTICAST, 1, 0);
		break;
	case VMS_PE_CH_B2:
		(void)rx_hello(e, &e->peer, 1, PE_PFW_VERIFY_B2, 1, 0);
		break;
	case VMS_PE_CH_B3:
		(void)rx_hello(e, &e->peer, 1, PE_PFW_MULTICAST, 1, 0);
		break;
	case VMS_PE_CH_B4:
		(void)rx_hello(e, &e->peer, 1, PE_PFW_VERIFY_B2, 1, 0);
		(void)rx_hello(e, &e->peer, 1, PE_PFW_VERIFY_B4, 1, 0);
		break;
	case VMS_PE_CH_CLOSED:
	default:
		(void)rx_hello(e, &e->peer, 1, PE_PFW_MULTICAST, 1, 0);
		(void)pe_fsm_event(&e->fsm, 0, PE_EV_SHUTDOWN);
		break;
	}
	fake_pe_clear_frames(&e->fake);
}

/* Which station a given state's channel belongs to. */
static const struct fake_peer *state_peer(struct pe_env *e,
					  enum vms_pe_channel_state s)
{
	return (s == VMS_PE_CH_SEEN) ? &e->mute : &e->peer;
}

/* ------------------------------------------------------------------ *
 * The expectation table -- the second statement of the machine
 * ------------------------------------------------------------------ */
struct edge {
	uint8_t event;
	uint8_t state_after;
	uint8_t action;
};

/* [CLOSED]: identity retained, ladder gone. Ticks and link events are no-ops. */
static const struct edge edges_closed[] = {
	{ PE_EV_RX_HELLO,           VMS_PE_CH_B3,   PE_CH_ACT_NONE },
	{ PE_EV_RX_SOLICIT,         VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B2,       VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B3,       VMS_PE_CH_B3,   PE_CH_ACT_NONE },
	{ PE_EV_RX_NEW_INCARNATION, VMS_PE_CH_B3,   PE_CH_ACT_RESET },
	{ PE_EV_TIMER_CHANNEL,      VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
	{ PE_EV_LINK_DOWN,          VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
	{ PE_EV_SHUTDOWN,           VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
};

/* [SEEN]: this channel's station published no LOGICAL address, so every reply
 * this FSM would like to send is withheld and the ladder cannot advance. */
static const struct edge edges_seen[] = {
	{ PE_EV_RX_HELLO,           VMS_PE_CH_SEEN, PE_CH_ACT_NONE },
	{ PE_EV_RX_SOLICIT,         VMS_PE_CH_SEEN, PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B2,       VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B3,       VMS_PE_CH_SEEN, PE_CH_ACT_NONE },
	{ PE_EV_RX_NEW_INCARNATION, VMS_PE_CH_SEEN, PE_CH_ACT_RESET },
	{ PE_EV_RX_LAST_GASP,       VMS_PE_CH_CLOSED, PE_CH_ACT_DEPARTED },
	{ PE_EV_TIMER_CHANNEL,      VMS_PE_CH_SEEN, PE_CH_ACT_NONE },
	{ PE_EV_LINK_DOWN,          VMS_PE_CH_CLOSED, PE_CH_ACT_LOST },
	{ PE_EV_SHUTDOWN,           VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
};

/* [B2] and [B3] differ only in how they were reached, so they carry the same
 * edge set; the b4 CONFIRM is what makes either usable. */
static const struct edge edges_b2[] = {
	{ PE_EV_RX_HELLO,           VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_SOLICIT,         VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B2,       VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B3,       VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B4,       VMS_PE_CH_B4,   PE_CH_ACT_VERIFIED },
	{ PE_EV_RX_NEW_INCARNATION, VMS_PE_CH_B3,   PE_CH_ACT_RESET },
	{ PE_EV_RX_LAST_GASP,       VMS_PE_CH_CLOSED, PE_CH_ACT_DEPARTED },
	{ PE_EV_TIMER_CHANNEL,      VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_LINK_DOWN,          VMS_PE_CH_CLOSED, PE_CH_ACT_LOST },
	{ PE_EV_SHUTDOWN,           VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
};

static const struct edge edges_b3[] = {
	{ PE_EV_RX_HELLO,           VMS_PE_CH_B3,   PE_CH_ACT_NONE },
	{ PE_EV_RX_SOLICIT,         VMS_PE_CH_B3,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B2,       VMS_PE_CH_B2,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B3,       VMS_PE_CH_B3,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B4,       VMS_PE_CH_B4,   PE_CH_ACT_VERIFIED },
	{ PE_EV_RX_NEW_INCARNATION, VMS_PE_CH_B3,   PE_CH_ACT_RESET },
	{ PE_EV_RX_LAST_GASP,       VMS_PE_CH_CLOSED, PE_CH_ACT_DEPARTED },
	{ PE_EV_TIMER_CHANNEL,      VMS_PE_CH_B3,   PE_CH_ACT_NONE },
	{ PE_EV_LINK_DOWN,          VMS_PE_CH_CLOSED, PE_CH_ACT_LOST },
	{ PE_EV_SHUTDOWN,           VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
};

/* [B4] verified. A second b2 is the INFERRED peer re-INIT and drops the ladder
 * back; a further b4 is just the steady oscillation's ack. */
static const struct edge edges_b4[] = {
	{ PE_EV_RX_HELLO,           VMS_PE_CH_B4,   PE_CH_ACT_NONE },
	{ PE_EV_RX_SOLICIT,         VMS_PE_CH_B4,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B2,       VMS_PE_CH_B2,   PE_CH_ACT_RESET },
	{ PE_EV_RX_VERIFY_B3,       VMS_PE_CH_B4,   PE_CH_ACT_NONE },
	{ PE_EV_RX_VERIFY_B4,       VMS_PE_CH_B4,   PE_CH_ACT_NONE },
	{ PE_EV_RX_NEW_INCARNATION, VMS_PE_CH_B3,   PE_CH_ACT_RESET },
	{ PE_EV_RX_LAST_GASP,       VMS_PE_CH_CLOSED, PE_CH_ACT_DEPARTED },
	{ PE_EV_TIMER_CHANNEL,      VMS_PE_CH_B4,   PE_CH_ACT_NONE },
	{ PE_EV_LINK_DOWN,          VMS_PE_CH_CLOSED, PE_CH_ACT_LOST },
	{ PE_EV_SHUTDOWN,           VMS_PE_CH_CLOSED, PE_CH_ACT_NONE },
};

struct state_edges {
	const struct edge *e;
	unsigned n;
	const char *name;
};

#define SE(arr, nm) { arr, (unsigned)(sizeof(arr) / sizeof((arr)[0])), nm }

static const struct state_edges g_states[VMS_PE_CH_STATE__COUNT] = {
	[VMS_PE_CH_CLOSED] = SE(edges_closed, "CLOSED"),
	[VMS_PE_CH_SEEN]   = SE(edges_seen,   "SEEN"),
	[VMS_PE_CH_B2]     = SE(edges_b2,     "B2"),
	[VMS_PE_CH_B3]     = SE(edges_b3,     "B3"),
	[VMS_PE_CH_B4]     = SE(edges_b4,     "B4"),
};

static const struct edge *edge_for(enum vms_pe_channel_state s, unsigned ev)
{
	const struct state_edges *se = &g_states[s];
	unsigned i;

	for (i = 0; i < se->n; i++) {
		if (se->e[i].event == ev)
			return &se->e[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ *
 * Driving one event, whatever kind it is
 * ------------------------------------------------------------------ */
static int drive_event(struct pe_env *e, enum vms_pe_channel_state s,
		       unsigned ev, enum pe_channel_action *act)
{
	const struct fake_peer *p = state_peer(e, s);

	switch (ev) {
	case PE_EV_RX_HELLO:
		*act = rx_hello(e, p, 0, PE_PFW_MULTICAST, 0, 0);
		return 1;
	case PE_EV_RX_SOLICIT:
		*act = rx_solicit(e, p);
		return 1;
	case PE_EV_RX_VERIFY_B2:
		*act = rx_hello(e, p, 1, PE_PFW_VERIFY_B2, 1, 0);
		return 1;
	case PE_EV_RX_VERIFY_B3:
		*act = rx_hello(e, p, 1, PE_PFW_VERIFY_B3, 1, 0);
		return 1;
	case PE_EV_RX_VERIFY_B4:
		*act = rx_hello(e, p, 1, PE_PFW_VERIFY_B4, 1, 0);
		return 1;
	case PE_EV_RX_LAST_GASP:
		*act = rx_hello(e, p, 0, PE_PFW_LAST_GASP, 0, 0);
		return 1;
	case PE_EV_RX_NEW_INCARNATION:
		/* SS4(i).B: the SAME peer, now attributing a different
		 * incarnation to this node. */
		*act = rx_hello(e, p, 1, PE_PFW_MULTICAST, 2, 0);
		return 1;
	default:
		*act = pe_fsm_event(&e->fsm, 0, (enum pe_event)ev);
		return 1;
	}
}

static void test_every_cell(void)
{
	unsigned s, ev;

	printf("-- every [state][event] cell of the channel table\n");
	for (s = 0; s < (unsigned)VMS_PE_CH_STATE__COUNT; s++) {
		for (ev = 0; ev < (unsigned)PE_EV__COUNT; ev++) {
			const struct edge *want =
				edge_for((enum vms_pe_channel_state)s, ev);
			enum pe_channel_action act = PE_CH_ACT_NONE;
			uint32_t ignored_before;
			struct pe_channel *ch;
			char what[128];

			drive_to(&g_env, (enum vms_pe_channel_state)s);
			ignored_before = g_env.fsm.ignored_events;
			(void)drive_event(&g_env,
					  (enum vms_pe_channel_state)s, ev, &act);
			ch = &g_env.fsm.ch[0];

			if (want != NULL) {
				snprintf(what, sizeof(what),
					 "[%s][%u] -> %s / %s",
					 g_states[s].name, ev,
					 pe_channel_state_name(
						 (enum vms_pe_channel_state)
						 want->state_after),
					 pe_channel_action_name(
						 (enum pe_channel_action)
						 want->action));
				ct_check(ch->state == want->state_after &&
					 (uint8_t)act == want->action, what);
				continue;
			}

			/* No grounded edge: nothing changes and the FSM says so
			 * by counting it, rather than inventing a response. */
			snprintf(what, sizeof(what),
				 "[%s][%u] has no grounded edge: ignored+counted",
				 g_states[s].name, ev);
			ct_check(ch->state == (uint8_t)s &&
				 act == PE_CH_ACT_NONE &&
				 g_env.fsm.ignored_events == ignored_before + 1u,
				 what);
		}
	}
}

/* ------------------------------------------------------------------ *
 * The ladder's meaning, beyond the shape of the table
 * ------------------------------------------------------------------ */

static void test_ovmx_never_originates_b2(void)
{
	unsigned i;

	printf("-- SS4(a).1: the joiner NEVER originates b2 (0 of 213 in the "
	       "reference)\n");
	env_init(&g_env);
	for (i = 0; i < 8; i++) {
		(void)rx_hello(&g_env, &g_env.peer, 0, PE_PFW_MULTICAST, 0, 0);
		(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B2, 1, 0);
		(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B4, 1, 0);
		(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B3, 1, 0);
		g_env.fake.now_ms += 2000;
		(void)pe_fsm_tick(&g_env.fsm, NULL, 0);
	}
	ct_check_eq_u32(fake_pe_count_word(&g_env.fake, PE_PFW_VERIFY_B2), 0,
			"OVMX emitted zero b2 frames");
	ct_check(fake_pe_count_word(&g_env.fake, PE_PFW_VERIFY_B3) > 0,
		 "OVMX emits b3 REQUESTs");
	ct_check(fake_pe_count_word(&g_env.fake, PE_PFW_VERIFY_B4) > 0,
		 "OVMX emits b4 CONFIRMs");
	ct_check_eq_u32(g_env.fake.frames_dropped, 0,
			"the recorder captured every emitted frame");
}

static void test_b4_only_from_our_own_request(void)
{
	struct pe_channel *ch;

	printf("-- an unmatched b4 confirms nothing (INV-6)\n");
	drive_to(&g_env, VMS_PE_CH_SEEN);
	(void)rx_hello(&g_env, &g_env.mute, 1, PE_PFW_VERIFY_B4, 1, 0);
	ch = &g_env.fsm.ch[0];
	ct_check(ch->state == (uint8_t)VMS_PE_CH_SEEN,
		 "a b4 with no outstanding b3 does NOT make the channel usable");
	ct_check(g_env.fsm.ignored_events > 0,
		 "and it is counted, not silently dropped");
}

static void test_response_rule(void)
{
	struct fake_pe_decoded d;

	printf("-- SS4(a).1 response rule: b2 -> b3, b3 -> b4\n");
	drive_to(&g_env, VMS_PE_CH_B3);
	fake_pe_clear_frames(&g_env.fake);

	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B2, 1, 0);
	ct_check_eq_u32(g_env.fake.n_frames, 1, "b2 draws exactly one reply");
	d = fake_pe_decode(&g_env.fake, 0);
	ct_check(d.ok, "the reply decodes through the codec");
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B3, "b2 -> b3");

	fake_pe_clear_frames(&g_env.fake);
	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B3, 1, 0);
	ct_check_eq_u32(g_env.fake.n_frames, 1, "b3 draws exactly one reply");
	d = fake_pe_decode(&g_env.fake, 0);
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B4, "b3 -> b4");

	fake_pe_clear_frames(&g_env.fake);
	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B4, 1, 0);
	/* b4 is TERMINAL -- there is no b5, so nothing ANSWERS it. What does
	 * follow is the SS4(k) size verification the newly usable channel now
	 * starts: a PADDED b3 REQUEST, which is a new verify and not a reply. */
	ct_check_eq_u32(g_env.fake.n_frames, 1, "b4 draws no b5");
	d = fake_pe_decode(&g_env.fake, 0);
	ct_check(d.ok && d.fi.cls == (uint8_t)VMS_FCLS_HELLO_PADDED,
		 "the one frame that follows is the SS4(k) size probe");
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B3,
			"and it carries b3: a padded HELLO is a verify REQUEST");
}

static void test_incarnation_is_recorded_not_invented(void)
{
	struct pe_channel *ch;

	printf("-- SS4(i).B: the peer's advertisement is RECORDED; a change "
	       "resets the channel\n");
	env_init(&g_env);
	ch = NULL;

	/* A multicast HELLO carries incarnation 0 and says nothing about us. */
	(void)rx_hello(&g_env, &g_env.peer, 0, PE_PFW_MULTICAST, 0, 0);
	ch = pe_fsm_channel_at(&g_env.fsm, 0);
	ct_check(ch != NULL && ch->peer_incarnation_valid == 0,
		 "a multicast HELLO advertises no incarnation for us");

	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B2, 2, 0);
	ct_check(ch->peer_incarnation_valid == 1 && ch->peer_incarnation == 2,
		 "the member's directed advertisement (2) is recorded verbatim");

	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B4, 2, 0);
	ct_check(ch->state == (uint8_t)VMS_PE_CH_B4, "the channel verifies");

	/* Now the member says 3: the generation we verified is over. */
	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B4, 3, 0);
	ct_check(ch->peer_incarnation == 3, "the new advertisement replaces it");
	ct_check(ch->state != (uint8_t)VMS_PE_CH_B4,
		 "and the channel is NOT still claimed as verified");
	ct_check_eq_u32(ch->resets, 1, "the reset is counted");
}

static void test_our_own_directed_incarnation_is_one(void)
{
	struct fake_pe_decoded d;
	unsigned i, directed = 0, multicast = 0;

	printf("-- SS4(i).B: our directed HELLO carries incarnation 1, our "
	       "multicast 0\n");
	env_init(&g_env);
	(void)rx_hello(&g_env, &g_env.peer, 1, PE_PFW_VERIFY_B2, 7, 0);
	g_env.fake.now_ms += 2000;
	(void)pe_fsm_tick(&g_env.fsm, NULL, 0);

	for (i = 0; i < g_env.fake.n_frames; i++) {
		d = fake_pe_decode(&g_env.fake, i);
		if (!d.ok)
			continue;
		if (d.h.poller_sweep == PE_POLLER_SWEEP_DIRECTED) {
			directed++;
			ct_check_eq_u32(d.h.incarnation, 1,
					"directed HELLO incarnation");
		} else {
			multicast++;
			ct_check_eq_u32(d.h.incarnation, 0,
					"multicast HELLO incarnation");
		}
	}
	ct_check(directed > 0 && multicast > 0,
		 "both frame flavours were emitted and checked");
	ct_check(g_env.fsm.ch[0].peer_incarnation == 7,
		 "the peer's 7 is remembered for FC-P1.2's START echo, not "
		 "echoed into our own HELLO");
}

static void test_no_identity_no_frames(void)
{
	struct pe_identity id;
	struct pe_ops ops;
	static struct pe_fsm fsm;
	static struct fake_pe fake;

	printf("-- INV-6: no cluster-LOGICAL address, no frames at all\n");
	fake_pe_ops_init(&ops, &fake);
	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;

	/* A SCSSYSTEMID that does not fit the two bytes SS4(a) grounds. */
	(void)pe_fsm_init(&fsm, &id, 0x1234567ull, &ops);
	pe_fsm_start(&fsm);
	fake.now_ms = 5000;
	(void)pe_fsm_tick(&fsm, NULL, 0);
	ct_check_eq_u32(fsm.id.lavc_valid, 0,
			"an unrepresentable SCSSYSTEMID leaves the address INVALID");
	ct_check_eq_u32(fake.n_frames, 0,
			"and the port emits nothing rather than a truncated one");
}

static void test_projection_blanks_what_is_unknown(void)
{
	struct vms_pe_channel_view v;

	printf("-- the snapshot blanks what the executive never learned\n");
	drive_to(&g_env, VMS_PE_CH_SEEN);
	pe_fsm_channel_project(pe_fsm_channel_at(&g_env.fsm, 0), &v);
	ct_check_eq_u32(v.remote_sysid_valid, 0,
			"no LOGICAL address => no sysid claimed");
	ct_check_eq_u32(v.remote_sysid_lo, 0, "and the field stays zero");

	drive_to(&g_env, VMS_PE_CH_B4);
	pe_fsm_channel_project(pe_fsm_channel_at(&g_env.fsm, 0), &v);
	ct_check_eq_u32(v.remote_sysid_valid, 1, "a real LOGICAL address =>");
	ct_check_eq_u32(v.remote_sysid_lo, 1025, "SS3's VAX1 SCSSYSTEMID 1025");
	ct_check_eq_u32(v.state, (unsigned)VMS_PE_CH_B4, "state B4 projected");
}

int main(void)
{
	test_every_cell();
	test_ovmx_never_originates_b2();
	test_b4_only_from_our_own_request();
	test_response_rule();
	test_incarnation_is_recorded_not_invented();
	test_our_own_directed_incarnation_is_one();
	test_no_identity_no_frames();
	test_projection_blanks_what_is_unknown();
	return ct_summary("test_pe_channel");
}
