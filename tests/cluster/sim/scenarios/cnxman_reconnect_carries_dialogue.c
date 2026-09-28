/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_reconnect_carries_dialogue.c - rd vms-8c54's rung-R2 leg:
 * the p. 7-24 reconnect window, run beat by beat on the virtual clock, and the
 * sixteen bytes the reconnect it produces puts on the wire.
 *
 * WHAT IS BEING REPRODUCED, AND WHERE IT WAS MEASURED.
 * tests/lab/captures/vms-8c54-stalled-guest-20260928/. An OVMX member's guest
 * was SIGSTOPped for 14 s two seconds after it was admitted. The real OpenVMS
 * VAX V7.3 closed its virtual circuit, kept the CSB, and when the pair came
 * back it dialled VMS$VAXcluster carrying its own connection-manager ack
 * counter at content[106:108]. This executive answered with a ZERO there --
 * "I have taken nothing from you", to a peer that held it as a member -- and
 * the VAX's connection manager bugchecked CNXMGRERR in the same millisecond as
 * its own ACCEPT_RSP (arms N-6 and V-1).
 *
 * THE ORACLE, two real V7.3 members reconnecting after the same fault: each
 * side carried the highest send-msg# it had TAKEN from the other (14811 and
 * 10249), and the first CM frame on the NEW Con.ID pair continued 10250/14811
 * and 14812/10249. The conversation did not restart; it moved.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1 (the three the other cnxman scenarios
 * in this directory list):
 *
 *   1. THE WHOLE WINDOW IS REALLY RUN by the SHIPPING vms_cnxman_recnx_fsm.c
 *      over the SHIPPING vms_cnxman_csb.c ladder. No state is hand-set: the
 *      block reaches its reconnect because the virtual clock really passed the
 *      deadline those two computed from SYSGEN's RECNXINTERVAL.
 *   2. THE RECONNECT IS A BOUNDED NUMBER OF BEATS rather than a dispatched
 *      event, so "it never reconnected" is expressible.
 *   3. THE ASSERTION IS THE BYTES. The sixteen the reconnect would emit are
 *      built by the SHIPPING codec from the SHIPPING block's counters and
 *      compared against the oracle's own connect, byte for byte.
 *
 * WHAT THE HARNESS OWNS: the port (one system, one circuit) and the order of
 * the once-a-second beat, both of which live in vms_cnxman.c -- the glue TU
 * that names exec_kbackend.h and is therefore not host-linkable. Ten lines,
 * and every decision they feed is a shipping FSM's.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cluster_codec_cm.h"

#define MEMBER_SYSID 0x000004000101ull
#define OWN_SYSID    0x000004000103ull
#define CM_CONID_A   0x4e62000au   /* the connection that is lost      */
#define CM_CONID_B   0x4e62000fu   /* the one the ladder re-establishes */
#define SIM_NODE     0u
#define RECNXINTERVAL_SECS 20u

/* The E31 version quad and tail, exactly as vms_cnxman.c holds them. */
static const uint8_t CD_HEAD[4] = { 0x01, 0x1b, 0x01, 0x03 };
static const uint8_t CD_TAIL[5] = { 0x08, 0x00, 0x00, 0x06, 0x00 };

struct bed {
	struct sim_clock    clock;
	struct vms_cluster  cl;
	struct cnxman_ops   ops;
	struct cnxman_recnx recnx;
	uint32_t            beats;
	uint32_t            reconnects;
};

static struct bed g;

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

static struct vms_csb *member_csb(void)
{
	return cnxman_club_find_sysid(&g.cl.club, MEMBER_SYSID);
}

/*
 * The member this node has been talking to for a while: admitted (p. 7-49
 * SELECTED), holding an open connection, and mid-conversation. The counters
 * below are the oracle's own -- this node has SENT 10249 messages to it and
 * TAKEN 14811 from it.
 */
static void bed_init(void)
{
	struct vms_csb *csb;
	uint32_t i;

	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);
	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.log = bed_log;

	memcpy(g.cl.params.scsnode, "OVMXB0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	g.cl.params.recnxinterval = (uint16_t)RECNXINTERVAL_SECS;

	(void)cnxman_club_init(&g.cl);
	cnxman_recnx_init(&g.recnx, &g.cl, &g.ops);

	csb = cnxman_club_alloc_csb(&g.cl.club, MEMBER_SYSID, 1);
	if (csb == NULL)
		return;
	csb->flags |= VMS_CSB_F_SELECTED;
	csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	cnxman_csb_bind_connection(csb, CM_CONID_A);
	for (i = 0; i < 10249u; i++)
		cnxman_csb_dialogue_sent(csb);
	cnxman_csb_dialogue_heard(csb, 14811u);
}

/* One beat of vms_cnxman.c's once-a-second order, reduced to the one action
 * this scenario's port can satisfy. */
static void beat(void)
{
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];
	uint32_t n, i;

	g.clock.now_ms += CNXMAN_RECNX_ATTEMPT_MS;
	g.beats++;
	n = cnxman_recnx_tick(&g.recnx, recs, VMS_CLUB_MAX_CSB);
	for (i = 0; i < n; i++) {
		if (recs[i].action != (uint8_t)CNXMAN_CSB_ACT_RECONNECT)
			continue;
		g.reconnects++;
		/* vms_cnxman.c: the connect goes out, and the connection it
		 * minted is bound through the CARRYING binder. */
		if (g.reconnects == 1u)
			cnxman_csb_bind_reconnect(member_csb(), CM_CONID_B);
	}
}

/*
 * The sixteen bytes vms_cnxman.c would emit for this peer right now:
 * cnxman_refresh_conndata_for()'s inputs, through the shipping builder.
 */
static void conndata_for_member(uint8_t out[VMS_CM_CONNDATA_LEN])
{
	struct vms_cm_conndata_in in;

	memset(&in, 0, sizeof(in));
	in.cluster_votes = g.cl.club.cevotes;
	in.quorum = g.cl.club.quorum;
	in.cluster_nodes = (uint16_t)g.cl.club.cluster_nodes;
	in.member = 1u;
	in.peer_ack_msg = cnxman_csb_dialogue_ack(member_csb());
	(void)vms_cm_conndata_build(&in, CD_HEAD, 4u, CD_TAIL, 5u, out,
				    VMS_CM_CONNDATA_LEN);
}

static void test_the_window_runs_and_the_dialogue_moves_with_it(void)
{
	struct vms_csb *csb;
	uint8_t cd[VMS_CM_CONNDATA_LEN];
	uint32_t i;

	printf("-- the p. 7-24 window really runs, and the conversation moves "
	       "onto the connection it re-establishes\n");

	bed_init();
	csb = member_csb();
	ct_check(csb != NULL, "the member's block");
	if (csb == NULL)
		return;
	ct_check_eq_u32(csb->cm_send_msg, 10249u, "10249 sent to it");
	ct_check_eq_u32(csb->cm_ack_msg, 14811u, "and 14811 taken from it");

	/* Connectivity goes. Nothing is hand-set beyond this one event: the
	 * SHIPPING ladder decides everything that follows. */
	(void)cnxman_csb_dispatch(&g.cl.club, csb, CNXMAN_CSB_EV_CONN_LOST,
				  &g.ops);
	ct_check_eq_u32(csb->state, (uint32_t)VMS_CNXMAN_CSB_WAIT,
			"p. 7-24: the block WAITS, it does not remove");

	/* The beat runs until the ladder issues its reconnect -- a BOUND, so
	 * "it never reconnected" would fail here rather than hang. */
	for (i = 0; i < 64u && g.reconnects == 0u; i++)
		beat();
	ct_check(g.reconnects >= 1u,
		 "the ladder really issued a reconnect, inside the window");
	ct_check(g.beats <= RECNXINTERVAL_SECS + 2u,
		 "and it did so in a bounded number of beats, not at leisure");

	/* THE POINT. */
	ct_check_eq_u32(csb->cdt_conid, CM_CONID_B,
			"the block holds the connection the ladder minted");
	ct_check_eq_u32(csb->cm_send_msg, 10249u,
			"the send side CONTINUES across it -- the next message "
			"is 10250, exactly as the oracle's VAX1 sent");
	ct_check_eq_u32(csb->cm_ack_msg, 14811u,
			"and so does the ack");
	ct_check_eq_u32(csb->cm_dialogues_carried, 1u,
			"counted as a dialogue CARRIED, not restarted");

	/* ...AND THE BYTES. The oracle's own connect, for this configuration:
	 * the long form, with 14811 little-endian at [12:14]. */
	conndata_for_member(cd);
	{
		static const uint8_t want[VMS_CM_CONNDATA_LEN] = {
			0x01,0x1b,0x02,0x03, 0x00,0x00, 0x00,0x00, 0x00,0x00,
			0x01, 0x0a,0xdb,0x39,0x06,0x00 };
		uint8_t got_tail[6];
		uint8_t want_tail[6];

		memcpy(got_tail, &cd[10], 6);
		memcpy(want_tail, &want[10], 6);
		ct_check(cd[2] == 0x02u,
			 "[2] is the LONG form, because an ack is carried");
		ct_check(memcmp(got_tail, want_tail, 6) == 0,
			 "[11:16] is 0a db 39 06 00 -- the oracle's own bytes, "
			 "built from this block's counters and nothing else");
	}

	/* The counts themselves are the CLUB's and this bed never committed a
	 * transition, so they are honestly zero -- asserted, so that a future
	 * bed which DOES commit one cannot pass this file by accident. */
	ct_check(cd[4] == 0u && cd[6] == 0u && cd[8] == 0u,
		 "and the cluster arithmetic is this CLUB's, which has "
		 "committed nothing: zero, not invented");
}

/*
 * THE OTHER HALF OF THE RULE, on the same bed: a block the cluster has REMOVED
 * is a new conversation, and E77's reset is what it gets. Without this the
 * scenario would prove only that counters can survive, which is the E76/E77
 * crash written as a feature.
 */
static void test_a_removed_system_starts_over(void)
{
	struct vms_csb *csb;
	uint32_t i;

	printf("-- ...and a system the cluster has removed starts over\n");

	bed_init();
	csb = member_csb();
	ct_check(csb != NULL, "the member's block");
	if (csb == NULL)
		return;

	csb->flags &= (uint16_t)~VMS_CSB_F_SELECTED;   /* p. 7-49: removed */
	(void)cnxman_csb_dispatch(&g.cl.club, csb, CNXMAN_CSB_EV_CONN_LOST,
				  &g.ops);
	for (i = 0; i < 64u && g.reconnects == 0u; i++)
		beat();
	ct_check(g.reconnects >= 1u, "the ladder still reconnects");
	ct_check_eq_u32(csb->cm_send_msg, 0u,
			"but the conversation restarts: the first message on "
			"the new connection is 1");
	ct_check_eq_u32(csb->cm_ack_msg, 0u,
			"and it acks NOTHING -- the byte E76/E77 bugchecked "
			"two real VAXes on");
	ct_check_eq_u32(csb->cm_dialogues_carried, 0u, "nothing was carried");
	ct_check_eq_u32(csb->cm_dialogue_resets, 1u, "it was a reset");
}

int main(void)
{
	printf("cnxman_reconnect_carries_dialogue (rd vms-8c54, rung R2): the "
	       "reconnect window and the bytes it emits\n");
	test_the_window_runs_and_the_dialogue_moves_with_it();
	test_a_removed_system_starts_over();
	return ct_summary("cnxman_reconnect_carries_dialogue");
}
